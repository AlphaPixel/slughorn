#pragma once

// ================================================================================================
// tessellate.hpp - polygon-with-holes triangulation and linear extrusion for Atlas shapes
//
// tessellate() flattens each contour returned by Atlas::getShapeContours() to line segments
// (reusing the same midpoint-subdivision flattening canvas.hpp uses for stroking/sampling),
// groups the resulting rings into exterior/hole sets, and triangulates each group via
// mapbox/earcut.hpp. A shape may have any number of disjoint exterior rings (e.g. the two dots
// of an umlaut) each with zero or more holes (e.g. the counters of a lowercase "e"); rings are
// classified by signed area (CCW = exterior, CW = hole -- slughorn's standing winding
// convention, see render.hpp's msdfgen-orientation note) and assigned to their owning exterior
// via point-in-polygon containment.
//
// extrude() reuses that same triangulation for the top and bottom caps of a closed 3D solid,
// and generates the connecting side walls directly from each ring's own flattened point loop --
// earcut is never involved in wall generation. A ring's point order already encodes which side
// is "outward" (exterior rings wind one way, holes the other), so the identical wall-quad
// pattern produces correctly outward-facing normals for both without any special-casing.
//
// Requires SLUGHORN_TESSELLATE=ON (vendors ext/earcut.hpp; header-only, no link dependency).
// ================================================================================================

#include "slughorn.hpp"

#include <mapbox/earcut.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace slughorn {
namespace tessellate {

// Flat triangle mesh in the shape's own XY (em-space) plane. `positions` is xy-interleaved;
// `indices` is a flat triangle list (3 per triangle, CCW winding in a y-up coordinate system).
// Raw data out -- caller owns any GPU upload.
struct Mesh2D {
	std::vector<slug_t> positions;
	std::vector<uint32_t> indices;
};

// Flat triangle mesh in 3D, produced by extrude(). `positions` is xyz-interleaved.
struct Mesh3D {
	std::vector<slug_t> positions;
	std::vector<uint32_t> indices;
};

namespace detail {

using Point = std::array<slug_t, 2>;
using Ring = std::vector<Point>;

// Midpoint-subdivision curve flattening, identical in spirit to canvas.hpp's
// detail::flattenCurve. Duplicated here (rather than included) so this header has no
// dependency on canvas.hpp for one small pure-math helper.
inline void flattenCurve(
	slug_t p0x, slug_t p0y,
	slug_t p1x, slug_t p1y,
	slug_t p2x, slug_t p2y,
	slug_t tol, size_t depth,
	Ring& pts
) {
	const slug_t dx = p2x - p0x, dy = p2y - p0y;
	const slug_t lenSq = dx * dx + dy * dy;
	const slug_t cross = (p1x - p0x) * dy - (p1y - p0y) * dx;

	if(lenSq < 1e-12_cv || (cross * cross) <= (tol * tol * lenSq) || depth >= 8) {
		pts.push_back({p2x, p2y});

		return;
	}

	const slug_t m01x = (p0x + p1x) * 0.5_cv, m01y = (p0y + p1y) * 0.5_cv;
	const slug_t m12x = (p1x + p2x) * 0.5_cv, m12y = (p1y + p2y) * 0.5_cv;
	const slug_t mx = (m01x + m12x) * 0.5_cv, my = (m01y + m12y) * 0.5_cv;

	flattenCurve(p0x, p0y, m01x, m01y, mx, my, tol, depth + 1, pts);
	flattenCurve(mx, my, m12x, m12y, p2x, p2y, tol, depth + 1, pts);
}

// Flattens one closed contour (a single Bezier loop, as returned per-entry by
// Atlas::getShapeContours()) into a polygon ring with no duplicated closing vertex.
inline Ring flattenContour(const Atlas::Curves& contour, slug_t tolerance) {
	Ring pts;

	if(contour.empty()) return pts;

	pts.push_back({contour.front().x1, contour.front().y1});

	for(const auto& c : contour) flattenCurve(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3, tolerance, 0, pts);

	if(pts.size() > 1) {
		const auto& first = pts.front();
		const auto& last = pts.back();

		if(std::abs(first[0] - last[0]) < 1e-6_cv && std::abs(first[1] - last[1]) < 1e-6_cv) {
			pts.pop_back();
		}
	}

	return pts;
}

// Twice the signed area (shoelace formula). Positive = CCW = exterior; negative = CW = hole.
inline slug_t signedArea2(const Ring& ring) {
	slug_t area = 0_cv;

	for(size_t i = 0; i < ring.size(); i++) {
		const auto& a = ring[i];
		const auto& b = ring[(i + 1) % ring.size()];

		area += a[0] * b[1] - b[0] * a[1];
	}

	return area;
}

// Standard ray-cast point-in-polygon test (even-odd crossing count).
inline bool pointInRing(const Ring& ring, const Point& p) {
	bool inside = false;

	for(size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
		const auto& a = ring[i];
		const auto& b = ring[j];

		if(
			((a[1] > p[1]) != (b[1] > p[1])) &&
			(p[0] < (b[0] - a[0]) * (p[1] - a[1]) / (b[1] - a[1]) + a[0])
		) inside = !inside;
	}

	return inside;
}

struct Group {
	size_t exteriorIndex;
	std::vector<size_t> holeIndices;
};

// Flattens `contours` and classifies/groups the resulting rings: one Group per exterior ring,
// carrying the indices (into `rings`) of the holes it owns. Shared by tessellate() and
// extrude()'s cap generation.
inline std::vector<Group> groupContours(const std::vector<Ring>& rings) {
	std::vector<Group> groups;

	for(size_t i = 0; i < rings.size(); i++) {
		if(signedArea2(rings[i]) > 0_cv) groups.push_back({i, {}});
	}

	for(size_t i = 0; i < rings.size(); i++) {
		if(signedArea2(rings[i]) >= 0_cv) continue;

		// Assign to the smallest-area exterior that contains this hole's first point -- handles
		// nested exteriors (e.g. one glyph component sitting inside another's counter).
		slug_t bestArea = std::numeric_limits<slug_t>::max();
		int bestGroup = -1;

		for(size_t g = 0; g < groups.size(); g++) {
			const Ring& ext = rings[groups[g].exteriorIndex];

			if(!pointInRing(ext, rings[i][0])) continue;

			const slug_t area = signedArea2(ext);

			if(area < bestArea) {
				bestArea = area;
				bestGroup = static_cast<int>(g);
			}
		}

		if(bestGroup >= 0) groups[static_cast<size_t>(bestGroup)].holeIndices.push_back(i);
	}

	return groups;
}

} // namespace detail

// Flattens `contours` (as returned by Atlas::getShapeContours()) and triangulates them via
// earcut. See the file banner above for the exterior/hole grouping rules.
//
// Output vertex order matches the order rings were fed to earcut (exterior ring first, then
// its holes, per group); indices are offset to index into the single concatenated `positions`
// array.
inline Mesh2D tessellate(const Atlas::Contours& contours, slug_t tolerance = TOLERANCE_BALANCED) {
	using namespace detail;

	std::vector<Ring> rings;

	rings.reserve(contours.size());

	for(const auto& contour : contours) {
		Ring ring = flattenContour(contour, tolerance);

		if(ring.size() >= 3) rings.push_back(std::move(ring));
	}

	Mesh2D mesh;
	uint32_t vertexOffset = 0;

	for(const auto& group : groupContours(rings)) {
		std::vector<Ring> polygon;

		polygon.push_back(rings[group.exteriorIndex]);

		for(size_t hi : group.holeIndices) polygon.push_back(rings[hi]);

		std::vector<uint32_t> localIndices = mapbox::earcut<uint32_t>(polygon);

		for(uint32_t idx : localIndices) mesh.indices.push_back(idx + vertexOffset);

		for(const auto& ring : polygon) {
			for(const auto& p : ring) {
				mesh.positions.push_back(p[0]);
				mesh.positions.push_back(p[1]);
			}

			vertexOffset += static_cast<uint32_t>(ring.size());
		}
	}

	return mesh;
}

// Extrudes `contours` (as returned by Atlas::getShapeContours()) into a closed 3D solid: a top
// cap at z=depth, a bottom cap at z=0 (reversed winding so both caps face outward), and a
// ruled quad-strip wall connecting every ring -- exterior boundaries and hole boundaries alike
// -- between the two.
inline Mesh3D extrude(
	const Atlas::Contours& contours,
	slug_t depth,
	slug_t tolerance = TOLERANCE_BALANCED
) {
	using namespace detail;

	Mesh2D cap = tessellate(contours, tolerance);

	Mesh3D mesh;

	const auto capVertexCount = static_cast<uint32_t>(cap.positions.size() / 2);

	mesh.positions.reserve(capVertexCount * 2 * 3);

	// Top cap (z = depth), same winding as the 2D triangulation -> faces +Z.
	for(uint32_t i = 0; i < capVertexCount; i++) {
		mesh.positions.push_back(cap.positions[i * 2 + 0]);
		mesh.positions.push_back(cap.positions[i * 2 + 1]);
		mesh.positions.push_back(depth);
	}

	for(uint32_t idx : cap.indices) mesh.indices.push_back(idx);

	// Bottom cap (z = 0), winding reversed so it faces -Z.
	for(uint32_t i = 0; i < capVertexCount; i++) {
		mesh.positions.push_back(cap.positions[i * 2 + 0]);
		mesh.positions.push_back(cap.positions[i * 2 + 1]);
		mesh.positions.push_back(0_cv);
	}

	for(size_t i = 0; i + 2 < cap.indices.size(); i += 3) {
		mesh.indices.push_back(capVertexCount + cap.indices[i + 0]);
		mesh.indices.push_back(capVertexCount + cap.indices[i + 2]);
		mesh.indices.push_back(capVertexCount + cap.indices[i + 1]);
	}

	// Walls: re-flatten each contour (cheap; keeps tessellate() and extrude() independently
	// simple rather than threading ring storage through both) and connect top/bottom per ring.
	for(const auto& contour : contours) {
		Ring ring = flattenContour(contour, tolerance);

		if(ring.size() < 3) continue;

		const auto base = static_cast<uint32_t>(mesh.positions.size() / 3);

		for(const auto& p : ring) {
			mesh.positions.push_back(p[0]);
			mesh.positions.push_back(p[1]);
			mesh.positions.push_back(depth);
		}

		for(const auto& p : ring) {
			mesh.positions.push_back(p[0]);
			mesh.positions.push_back(p[1]);
			mesh.positions.push_back(0_cv);
		}

		const auto n = static_cast<uint32_t>(ring.size());

		for(uint32_t i = 0; i < n; i++) {
			const uint32_t next = (i + 1) % n;
			const uint32_t top0 = base + i;
			const uint32_t top1 = base + next;
			const uint32_t bot0 = base + n + i;
			const uint32_t bot1 = base + n + next;

			mesh.indices.push_back(bot0);
			mesh.indices.push_back(bot1);
			mesh.indices.push_back(top0);

			mesh.indices.push_back(top0);
			mesh.indices.push_back(bot1);
			mesh.indices.push_back(top1);
		}
	}

	return mesh;
}

} // namespace tessellate
} // namespace slughorn
