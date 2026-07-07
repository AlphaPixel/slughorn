//vimrun! ./slughorn-test-tessellate
//
// Smoke-test for slughorn/tessellate.hpp.
//
// Builds three shapes exercising the three grouping cases tessellate() has to get right:
//   - "triangle": a single exterior ring, no holes (straight lines; exact area).
//   - "donut":    one exterior ring with one hole (the ring-grouping + earcut path).
//   - "two_dots": two disjoint exterior rings, no holes (multi-component grouping).
//
// For each, triangulates via tessellate() and checks the summed triangle area against the
// known analytic area (a correctness check that doesn't require any visualization). Also runs
// extrude() on each and checks basic mesh invariants (index bounds, multiple-of-3 sizes).

#include "slughorn/canvas.hpp"
#include "slughorn/tessellate.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <numbers>

using namespace slughorn::literals;

using slughorn::Atlas;
using slughorn::Key;
using slughorn::slug_t;

static slug_t triangleArea(
	slug_t x0, slug_t y0,
	slug_t x1, slug_t y1,
	slug_t x2, slug_t y2
) {
	return 0.5_cv * std::abs(x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1));
}

static slug_t meshArea(const slughorn::tessellate::Mesh2D& mesh) {
	slug_t area = 0_cv;

	for(size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
		const uint32_t a = mesh.indices[i], b = mesh.indices[i + 1], c = mesh.indices[i + 2];

		area += triangleArea(
			mesh.positions[a * 2], mesh.positions[a * 2 + 1],
			mesh.positions[b * 2], mesh.positions[b * 2 + 1],
			mesh.positions[c * 2], mesh.positions[c * 2 + 1]
		);
	}

	return area;
}

static void checkMesh2D(const char* label, const slughorn::tessellate::Mesh2D& mesh, slug_t expectedArea) {
	const size_t vertexCount = mesh.positions.size() / 2;

	std::cout
		<< label << ": " << vertexCount << " vertices, " << (mesh.indices.size() / 3) << " triangles\n"
	;

	assert(!mesh.indices.empty() && "tessellate() produced no triangles");
	assert(mesh.indices.size() % 3 == 0 && "index count is not a multiple of 3");

	for(uint32_t idx : mesh.indices) assert(idx < vertexCount && "index out of bounds");

	const slug_t area = meshArea(mesh);

	std::cout << "  area=" << area << " expected=" << expectedArea << '\n';

	assert(std::abs(area - expectedArea) < 0.01_cv && "triangulated area does not match analytic area");
}

static void checkMesh3D(const char* label, const slughorn::tessellate::Mesh3D& mesh) {
	const size_t vertexCount = mesh.positions.size() / 3;

	std::cout
		<< label << " (extruded): " << vertexCount << " vertices, "
		<< (mesh.indices.size() / 3) << " triangles\n"
	;

	assert(!mesh.indices.empty() && "extrude() produced no triangles");
	assert(mesh.indices.size() % 3 == 0 && "index count is not a multiple of 3");

	for(uint32_t idx : mesh.indices) assert(idx < vertexCount && "index out of bounds");
}

int main() {
	Atlas atlas;
	slughorn::canvas::Canvas canvas(atlas);

	// "triangle" - single exterior, no holes.
	canvas.beginPath();
	canvas.moveTo(0.5_cv, 0.1_cv);
	canvas.lineTo(0.9_cv, 0.9_cv);
	canvas.lineTo(0.1_cv, 0.9_cv);
	canvas.closePath();
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("triangle"));
	canvas.finalize(Key("triangle_comp"));

	// "donut" - one exterior circle (CCW) + one square hole (CW).
	canvas.beginPath();
	canvas.circle(0.5_cv, 0.5_cv, 0.4_cv);
	canvas.moveTo(0.35_cv, 0.35_cv);
	canvas.lineTo(0.35_cv, 0.65_cv);
	canvas.lineTo(0.65_cv, 0.65_cv);
	canvas.lineTo(0.65_cv, 0.35_cv);
	canvas.closePath();
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("donut"));
	canvas.finalize(Key("donut_comp"));

	// "two_dots" - two disjoint exteriors, no holes.
	canvas.beginPath();
	canvas.circle(0.3_cv, 0.5_cv, 0.15_cv);
	canvas.circle(0.7_cv, 0.5_cv, 0.15_cv);
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("two_dots"));
	canvas.finalize(Key("two_dots_comp"));

	atlas.build();

	const slug_t pi = std::numbers::pi_v<slug_t>;

	{
		auto contours = atlas.getShapeContours(Key("triangle"));
		auto mesh = slughorn::tessellate::tessellate(contours);

		checkMesh2D("triangle", mesh, triangleArea(0.5_cv, 0.1_cv, 0.9_cv, 0.9_cv, 0.1_cv, 0.9_cv));
		checkMesh3D("triangle", slughorn::tessellate::extrude(contours, 0.2_cv));
	}

	{
		auto contours = atlas.getShapeContours(Key("donut"));
		auto mesh = slughorn::tessellate::tessellate(contours);
		const slug_t expected = pi * 0.4_cv * 0.4_cv - 0.3_cv * 0.3_cv;

		checkMesh2D("donut", mesh, expected);
		checkMesh3D("donut", slughorn::tessellate::extrude(contours, 0.2_cv));
	}

	{
		auto contours = atlas.getShapeContours(Key("two_dots"));
		auto mesh = slughorn::tessellate::tessellate(contours);
		const slug_t expected = 2_cv * pi * 0.15_cv * 0.15_cv;

		checkMesh2D("two_dots", mesh, expected);
		checkMesh3D("two_dots", slughorn::tessellate::extrude(contours, 0.2_cv));
	}

	std::cout << "OK\n";

	return 0;
}
