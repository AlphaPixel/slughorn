#pragma once

// ================================================================================================
// render.hpp - CPU-side Slug coverage emulator
//
// Mirrors the GPU fragment shader analytically, enabling:
//
// - Software rendering / visual validation without a GPU
// - SDF/MSDF tile baking (render::field(), see Atlas::SDF)
// - Post-build curve access for strokeText / glyphOutline
// - CPU, GL-free hit testing (Sampler::pick(), authored-space only - see its own comment)
//
// Usage:
//
// slughorn::render::Sampler s = slughorn::render::decode(atlas, key);
// slughorn::render::Grid g = s.renderGrid(128);
// // g.data is row-major float32: g.data[row * g.width + col]
//
// decode() reads the packed curve/band textures produced by Atlas::build() and
// Atlas::packTextures(). It can be called any time after packTextures() returns.
//
// For a one-shot pick against a single Shape, skip decode() entirely:
//
// bool hit = slughorn::render::Sampler(shape).pick(transform, scale, worldX, worldY);
//
// No GPU, no Python, no external dependencies beyond slughorn.hpp.
// ================================================================================================

#include "slughorn.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>


#ifdef SLUGHORN_HAS_SDF
#include <msdfgen.h>
#endif

namespace slughorn {
namespace render {

// ================================================================================================
// Sample - coverage result for a single em-space point
// ================================================================================================

struct Sample {
	slug_t fill = 0_cv;
	slug_t xcov = 0_cv;
	slug_t ycov = 0_cv;
	slug_t xwgt = 0_cv;
	slug_t ywgt = 0_cv;

	uint32_t iters = 0;
};

// ================================================================================================
// Grid - 2-D coverage result from renderGrid()
//
// data is row-major: data[row * width + col], values in [0, 1].
// ================================================================================================

struct Grid {
	uint32_t width = 0;
	uint32_t height = 0;

	std::vector<slug_t> data = {};

	slug_t at(uint32_t row, uint32_t col) const { return data[row * width + col]; }
};

// ================================================================================================
// Sampler - holds a decoded shape and provides coverage evaluation
// ================================================================================================

// How each ray crossing ramps from outside to inside.
//
// Axis: Lengyel's original - a ramp 1 pixel wide along the ray's own axis (x for the horizontal
// ray, y for the vertical one). Across an edge at angle theta that is 1 / (|cos| + |sin|) pixels
// wide, close to an exact box filter's coverage.
//
// Edge (the default): each crossing's distance is converted to its distance across the edge in
// screen pixels (through the EmJacobian), divided by the edge's ramp width |cos| + |sin|, so both
// rays report the same linear ramp |cos| + |sin| pixels wide across the edge (the box filter's
// full support) at any rotation. This is the ramp osgSlug::PathDrawable's Sluggit mode produces;
// softer than Axis on angled edges, identical on axis-aligned ones. It costs more per crossing
// and walks bands 2 pixels deep instead of 0.5.
enum class AARamp {
	Axis,
	Edge
};

// How em space maps onto one screen pixel: dx is the em-space change for a one pixel step along
// screen x, dy for a step along screen y (a shader's dFdx/dFdy of its em coordinate). Only
// AARamp::Edge reads the full matrix; both modes read the per-axis pixels-per-em it implies
// (1 / fwidth, as a shader computes it).
struct EmJacobian {
	slug_t dxX = 0_cv;
	slug_t dxY = 0_cv;
	slug_t dyX = 0_cv;
	slug_t dyY = 0_cv;

	// Em space axis-aligned with the screen, ppeX/ppeY pixels per em.
	static EmJacobian fromPixelsPerEm(slug_t ppeX, slug_t ppeY) {
		return {1_cv / ppeX, 0_cv, 0_cv, 1_cv / ppeY};
	}

	// Em space rotated by radians on screen (counterclockwise, Y-up), ppe pixels per em.
	static EmJacobian fromRotation(slug_t radians, slug_t ppe) {
		const slug_t c = std::cos(radians) / ppe;
		const slug_t s = std::sin(radians) / ppe;

		return {c, -s, s, c};
	}

	slug_t ppeX() const { return 1_cv / (std::abs(dxX) + std::abs(dyX)); }
	slug_t ppeY() const { return 1_cv / (std::abs(dxY) + std::abs(dyY)); }
};

// ================================================================================================
// distanceToCurve - em distance from a point to a quadratic curve (centerline strokes)
//
// With the point moved to the origin, the curve is B(t) = p1 + 2t * A + t^2 * C for t in [0, 1],
// where A = p2 - p1 and C = p1 - 2 * p2 + p3. The nearest point is where the curve's direction
// B'(t) = 2 * (A + t * C) is perpendicular to B(t), i.e. B(t) . B'(t) = 0, which expands to the
// cubic
//
//     (C . C) t^3 + 3 (A . C) t^2 + (2 (A . A) + p1 . C) t + p1 . A = 0
//
// Its real roots, clamped to [0, 1], are the candidates; the clamp also covers the endpoints,
// which are the nearest points past either end (round caps). A root error moves the result only
// to second order, because the distance is stationary at its minimum.
//
// A straight segment stored the slughorn way (lineTo() commits p2 = p3) keeps C = p1 - p3, so it
// is an ordinary cubic. Only C ~ 0 (control point at the chord midpoint) is degenerate: the curve
// is then the segment p1 -> p3, measured directly.
// ================================================================================================

inline slug_t distanceToCurve(const Atlas::Curve& c, Point point) {
	const Point p1 = c.p1() - point;
	const Point A = c.p2() - c.p1();
	const Point C = c.p1() - 2_cv * c.p2() + c.p3();

	auto distanceAt = [&](slug_t t) { return length(p1 + t * (2_cv * A + t * C)); };

	const slug_t cc = dot(C, C);
	const slug_t aa = dot(A, A);

	// Degenerate: a segment p1 -> p1 + 2A (or a single point when A is also zero).
	if(cc <= 1e-12_cv * aa || cc == 0_cv) {
		const Point d = 2_cv * A;
		const slug_t len2 = dot(d, d);
		const slug_t t = len2 > 0_cv ? std::clamp(-dot(p1, d) / len2, 0_cv, 1_cv) : 0_cv;

		return length(p1 + t * d);
	}

	// Monic cubic t^3 + a t^2 + b t + k, then depressed with t = u - a / 3: u^3 + p u + q = 0.
	const slug_t a = 3_cv * dot(A, C) / cc;
	const slug_t b = (2_cv * aa + p1.x * C.x + p1.y * C.y) / cc;
	const slug_t k = dot(p1, A) / cc;
	const slug_t p = b - a * a / 3_cv;
	const slug_t q = 2_cv * a * a * a / 27_cv - a * b / 3_cv + k;
	const slug_t shift = -a / 3_cv;
	const slug_t disc = q * q / 4_cv + p * p * p / 27_cv;

	slug_t best = std::min(distanceAt(0_cv), distanceAt(1_cv));

	auto consider = [&](slug_t u) {
		best = std::min(best, distanceAt(std::clamp(u + shift, 0_cv, 1_cv)));
	};

	if(disc > 0_cv) {
		// One real root. The cube root takes the sign that adds like signs (no cancellation); the
		// other term is -p / (3 * w), from (w + v)^3 with w * v = -p / 3.
		const slug_t s = std::sqrt(disc);
		const slug_t w = std::cbrt(q >= 0_cv ? -q / 2_cv - s : -q / 2_cv + s);

		consider(w != 0_cv ? w - p / (3_cv * w) : 0_cv);
	}

	else {
		// Three real roots (p <= 0): the trigonometric form.
		const slug_t m = 2_cv * std::sqrt(std::max(-p / 3_cv, 0_cv));
		const slug_t arg = m > 0_cv ? std::clamp(3_cv * q / (p * m), -1_cv, 1_cv) : 0_cv;
		const slug_t theta = std::acos(arg) / 3_cv;
		const slug_t third = 2.0943951023931953_cv; // 2 * pi / 3

		consider(m * std::cos(theta));
		consider(m * std::cos(theta - third));
		consider(m * std::cos(theta - 2_cv * third));
	}

	return best;
}

struct Sampler;

// Forward-declared here so Sampler's atlas+key ctor below can call it from a complete-class
// context; defined after Sampler, next to its 3-arg sibling (see decode()'s own section).
inline Sampler decode(const Atlas& atlas, Key key);

struct Sampler {
	// shape.curves is always left empty here, deliberately - whichever ctor built this Sampler
	// already moved/reconstructed the real curve data into `curves` below, and nothing in Sampler
	// ever reads shape.curves. Keeping it populated would just be a second live copy of the same
	// (or, on the decode() path, DIFFERENT - see curves' own comment) data for free.
	Atlas::Shape shape;

	// The curve list renderSample()/renderSampleBanded()/pick() actually walk. What's IN here
	// depends entirely on which ctor built this Sampler - the two are not interchangeable:
	//
	// - Sampler(shape) (cheap ctor): moved straight out of Shape::curves - same values, same
	//   order, full original precision. No decode() call.
	// - Sampler(atlas, key) (via decode()): reconstructed from the atlas's PACKED curve/band
	//   textures - a different representation, not just a copy. May be half-float-rounded (if
	//   the curve texture is RGBA16F), is reordered/deduped by packed texel address rather than
	//   authoring order, and - critically - hbandIndices/vbandIndices are remapped to point at
	//   THIS specific reconstruction. This is intentional: decode() exists to mirror exactly what
	//   the GPU shader reads off its own textures, packing/precision effects included, not to
	//   hand back the original authored curves a second time.
	Atlas::Curves curves;

	std::vector<uint32_t> hbandOffsets;
	std::vector<uint32_t> hbandIndices;
	std::vector<uint32_t> vbandOffsets;
	std::vector<uint32_t> vbandIndices;
	std::array<uint8_t, Atlas::INDIRECTION_SIZE> indirY{};
	std::array<uint8_t, Atlas::INDIRECTION_SIZE> indirX{};

	AARamp aaRamp = AARamp::Edge;

	Sampler() = default;

	// Cheap path - curves moved straight off Shape::curves, no decode(). shape.curves is cleared
	// right after so there is exactly one live copy of the data, not two (shape_ taken by value
	// so an rvalue caller gets a free move instead of a copy). pick() stays on the unbanded scan
	// below (hbandOffsets/vbandOffsets stay empty).
	explicit Sampler(Atlas::Shape shape_) : curves(std::move(shape_.curves)) {
		shape = std::move(shape_);
	}

	// Full path - reconstructs bands via decode(). pick() switches to the banded scan once this
	// ctor runs (hbandOffsets.size() >= 2 is the tell, see pick() below).
	Sampler(const Atlas& atlas, Key key) { *this = decode(atlas, key); }

	// -------------------------------------------------------------------------
	// Em-space helpers
	// -------------------------------------------------------------------------

	Point emOrigin() const {
		const slug_t ox = shape.bandScaleX != 0_cv ? -shape.bandOffsetX / shape.bandScaleX : 0_cv;
		const slug_t oy = shape.bandScaleY != 0_cv ? -shape.bandOffsetY / shape.bandScaleY : 0_cv;

		return {ox, oy};
	}

	Point emSize() const {
		const slug_t sx = shape.bandScaleX != 0_cv
			? cv(Atlas::INDIRECTION_SIZE) / shape.bandScaleX
			: 0_cv
		;

		const slug_t sy = shape.bandScaleY != 0_cv
			? cv(Atlas::INDIRECTION_SIZE) / shape.bandScaleY
			: 0_cv
		;

		return {sx, sy};
	}

	std::pair<uint32_t, uint32_t> computeRenderSize(uint32_t sizeHint) const {
		const slug_t w = shape.width;
		const slug_t h = shape.height;

		if(w <= 0_cv || h <= 0_cv)
			throw std::runtime_error("Invalid shape dimensions for renderGrid()")
		;

		const slug_t scale = cv(sizeHint) / std::max(w, h);
		const auto outW = static_cast<uint32_t>(std::max(1_cv, cv(std::round(w * scale))));
		const auto outH = static_cast<uint32_t>(std::max(1_cv, cv(std::round(h * scale))));

		return {outW, outH};
	}

	// -------------------------------------------------------------------------
	// Coverage evaluation
	// -------------------------------------------------------------------------

	Sample renderSample(Point r, slug_t ppeX, slug_t ppeY) const {
		return _renderSample(r, {EmJacobian::fromPixelsPerEm(ppeX, ppeY), ppeX, ppeY});
	}

	Sample renderSample(Point r, const EmJacobian& em) const {
		return _renderSample(r, {em, em.ppeX(), em.ppeY()});
	}

	Sample renderSampleBanded(Point r, slug_t ppeX, slug_t ppeY) const {
		return _renderSampleBanded(r, {EmJacobian::fromPixelsPerEm(ppeX, ppeY), ppeX, ppeY});
	}

	Sample renderSampleBanded(Point r, const EmJacobian& em) const {
		return _renderSampleBanded(r, {em, em.ppeX(), em.ppeY()});
	}

	Sample renderSample(slug_t rx, slug_t ry, slug_t ppeX, slug_t ppeY) const {
		return renderSample({rx, ry}, ppeX, ppeY);
	}

	Sample renderSample(slug_t rx, slug_t ry, const EmJacobian& em) const {
		return renderSample({rx, ry}, em);
	}

	Sample renderSampleBanded(slug_t rx, slug_t ry, slug_t ppeX, slug_t ppeY) const {
		return renderSampleBanded({rx, ry}, ppeX, ppeY);
	}

	Sample renderSampleBanded(slug_t rx, slug_t ry, const EmJacobian& em) const {
		return renderSampleBanded({rx, ry}, em);
	}


	// CPU-side, GL-free hit test against this Sampler's analytic fill - answers "does this
	// world-space point land inside the shape's real fill" without a GPU, a camera, or an FBO
	// readback. Coverage-exact (same Slug math the GPU fragment shader runs), not a bbox
	// approximation. transform/scale are the same placement args as Shape::computeQuad() - the
	// inverse of that function's own mapping.
	//
	// This is an AUTHORED-space query: it tests the shape exactly as it was built. It does not
	// know about anything a shader could have done to it (VertexHook deformation, masks, fragment
	// discards) - that is a rendered-space question, answered by a GPU id-buffer pick instead.
	//
	// Dispatches on hbandOffsets.size() >= 2, which doubles as proof of which ctor built this
	// Sampler - no separate stored flag needed. The cheap Sampler(shape) ctor never pays for
	// decode(), so there is no caching question on that path either.
	bool pick(const Transform& transform, slug_t scale, slug_t worldX, slug_t worldY) const {
		if(curves.empty() || scale == 0_cv) return false;

		const slug_t localX = worldX / scale - transform.x + shape.origin.point.x;
		const slug_t localY = worldY / scale - transform.y + shape.origin.point.y;

		// Cheap bbox pre-test in local space, mirroring computeQuad()'s own authored bounds - skips
		// the curve walk for the overwhelmingly common "nowhere near this layer" case.
		if(
			localX < shape.bearing.x || localX > shape.bearing.x + shape.width ||
			localY < shape.bearing.y - shape.height || localY > shape.bearing.y
		) return false;

		// No AA ramp wanted for a hit test - a huge "pixels per em" collapses the 1-pixel AA ramp
		// to a sharp inside/outside step. The 0.5 threshold is edge-exact either way.
		constexpr slug_t INFINITE_PPE = 1e6_cv;

		const auto result = hbandOffsets.size() >= 2
			? renderSampleBanded({localX, localY}, INFINITE_PPE, INFINITE_PPE)
			: renderSample({localX, localY}, INFINITE_PPE, INFINITE_PPE)
		;

		return result.fill >= 0.5_cv;
	}

	Grid renderGrid(
		uint32_t sizeHint=128,
		slug_t margin=0_cv,
		bool banded=true,
		bool parallel=false
	) const {
		const auto [width, height] = computeRenderSize(sizeHint);

		auto [ox, oy] = emOrigin();
		auto [sx, sy] = emSize();

		ox -= margin * sx;
		oy -= margin * sy;
		sx *= (1_cv + 2_cv * margin);
		sy *= (1_cv + 2_cv * margin);

		Grid grid{width, height, std::vector<slug_t>(width * height, 0_cv)};

		const slug_t ppeX = cv(width) / sx;
		const slug_t ppeY = cv(height) / sy;

		auto renderRow = [&](uint32_t j) {
			for(uint32_t i = 0; i < width; i++) {
				const slug_t u = (cv(i) + 0.5_cv) / cv(width);
				const slug_t v = (cv(j) + 0.5_cv) / cv(height);
				const slug_t ex = ox + u * sx;
				const slug_t ey = oy + v * sy;

				const auto result = banded
					? renderSampleBanded(ex, ey, ppeX, ppeY)
					: renderSample(ex, ey, ppeX, ppeY)
				;

				grid.data[j * width + i] = result.fill;
			}
		};

#ifdef SLUGHORN_HAS_PARALLEL
		if(parallel) {
			#pragma omp parallel for schedule(static)
			for(uint32_t j = 0; j < height; j++) renderRow(j);
			return grid;
		}
#else
		static_cast<void>(parallel);
#endif

		for(uint32_t j = 0; j < height; j++) renderRow(j);

		return grid;
	}

private:
	static constexpr slug_t EPS = 1_cv / 65536_cv;

	// Measured with test/slughorn-test-aa: 0.25 keeps straight edges on the linear ramp exactly from
	// 20 to 45 degrees (mean error <= 0.006 below that) with a 2 pixel reach; 0.5 bends edges up to
	// 0.13 off the ramp at 25-35 degrees.
	static constexpr slug_t EDGE_SCALE_FLOOR = 0.25_cv;

	// A sample's pixel footprint: the Jacobian, plus the per-axis pixels per em it implies (kept
	// as given by the ppeX/ppeY entry points, so Axis results are unchanged by the round trip).
	struct _Pixel {
		EmJacobian em;
		slug_t ppeX;
		slug_t ppeY;
	};

	Sample _renderSample(Point r, const _Pixel& px) const {
		if(shape.strokeRadius > 0_cv) return _renderStroke(r, px, false);

		Sample out;

		for(const auto& c : curves) {
			out.iters++;

			const Point p1 = c.p1() - r;
			const Point p2 = c.p2() - r;
			const Point p3 = c.p3() - r;

			_addHorizCrossings(out, p1, p2, p3, px);
			_addVertCrossings(out, p1, p2, p3, px);
		}

		out.fill = _calcCoverage(out.xcov, out.ycov, out.xwgt, out.ywgt);

		return out;
	}

	Sample _renderSampleBanded(Point r, const _Pixel& px) const {
		if(shape.strokeRadius > 0_cv) return _renderStroke(r, px, true);

		Sample out;

		if(hbandOffsets.size() < 2 || vbandOffsets.size() < 2) return out;

		const uint32_t bandX = _lookupBandIndir(r.x * shape.bandScaleX + shape.bandOffsetX, indirX);
		const uint32_t bandY = _lookupBandIndir(r.y * shape.bandScaleY + shape.bandOffsetY, indirY);

		auto process = [&](uint32_t ci, bool horizontal) -> bool {
			out.iters++;

			const auto& c = curves[ci];
			const Point p1 = c.p1() - r;
			const Point p2 = c.p2() - r;
			const Point p3 = c.p3() - r;

			if(horizontal) {
				if(std::max({p1.x, p2.x, p3.x}) * px.ppeX < -_rampReach()) return false;

				_addHorizCrossings(out, p1, p2, p3, px);
			}

			else {
				if(std::max({p1.y, p2.y, p3.y}) * px.ppeY < -_rampReach()) return false;

				_addVertCrossings(out, p1, p2, p3, px);
			}

			return true;
		};

		if(bandY + 1 < hbandOffsets.size()) {
			for(uint32_t i = hbandOffsets[bandY]; i < hbandOffsets[bandY + 1]; i++) {
				if(!process(hbandIndices[i], true)) break;
			}
		}

		if(bandX + 1 < vbandOffsets.size()) {
			for(uint32_t i = vbandOffsets[bandX]; i < vbandOffsets[bandX + 1]; i++) {
				if(!process(vbandIndices[i], false)) break;
			}
		}

		out.fill = _calcCoverage(out.xcov, out.ycov, out.xwgt, out.ywgt);

		return out;
	}

	// A stroke shape (shape.strokeRadius > 0): its curves are a centerline, and a point is covered
	// within strokeRadius em of the nearest one (distanceToCurve()). Coverage ramps linearly over
	// one pixel centered on that boundary, the pixel's em size taken as the mean of its two axes.
	//
	// Banded, only ONE band list is walked: the builder files each curve in every band within
	// strokeRadius of it, so the pixel's h-band and its v-band each hold every curve that can reach
	// it, and the shorter is taken. Lists are sorted by descending max x (h-bands) / max y
	// (v-bands), so once a curve's max is farther behind the pixel than the reach plus a pixel, no
	// later curve can reach it either. Curves far AHEAD of the pixel are still walked.
	//
	// The coverage ramp reaches half a pixel past strokeRadius, but the band padding is only
	// strokeRadius (em, fixed at build time). Every band of the chosen axis within half a pixel of
	// the point is walked, so a curve filed only in a neighboring band still contributes there;
	// walking a curve twice cannot change the minimum.
	Sample _renderStroke(Point r, const _Pixel& px, bool banded) const {
		Sample out;

		const slug_t radius = shape.strokeRadius;
		const slug_t emPerPixel = 0.5_cv * (1_cv / px.ppeX + 1_cv / px.ppeY);

		slug_t d = std::numeric_limits<slug_t>::infinity();

		if(!banded) {
			for(const auto& c : curves) {
				out.iters++;

				d = std::min(d, distanceToCurve(c, r));
			}
		}

		else {
			if(hbandOffsets.size() < 2 || vbandOffsets.size() < 2) return out;

			const uint32_t bandX = _lookupBandIndir(r.x * shape.bandScaleX + shape.bandOffsetX, indirX);
			const uint32_t bandY = _lookupBandIndir(r.y * shape.bandScaleY + shape.bandOffsetY, indirY);

			if(bandY + 1 >= hbandOffsets.size() || bandX + 1 >= vbandOffsets.size()) return out;

			const uint32_t hcount = hbandOffsets[bandY + 1] - hbandOffsets[bandY];
			const uint32_t vcount = vbandOffsets[bandX + 1] - vbandOffsets[bandX];
			const bool horizontal = hcount <= vcount;
			const auto& offsets = horizontal ? hbandOffsets : vbandOffsets;
			const auto& indices = horizontal ? hbandIndices : vbandIndices;
			const slug_t cutoff = (horizontal ? r.x : r.y) - radius - emPerPixel;

			// The band axis is perpendicular to the sort axis: h-bands slice y, v-bands slice x.
			const slug_t coord = horizontal ? r.y : r.x;
			const slug_t bandScale = horizontal ? shape.bandScaleY : shape.bandScaleX;
			const slug_t bandOffset = horizontal ? shape.bandOffsetY : shape.bandOffsetX;
			const auto& indir = horizontal ? indirY : indirX;
			const slug_t fringe = 0.5_cv * emPerPixel;
			const uint32_t first = _lookupBandIndir((coord - fringe) * bandScale + bandOffset, indir);
			const uint32_t last = _lookupBandIndir((coord + fringe) * bandScale + bandOffset, indir);

			for(uint32_t b = first; b <= last && b + 1 < offsets.size(); b++) {
				for(uint32_t i = offsets[b]; i < offsets[b + 1]; i++) {
					const auto& c = curves[indices[i]];
					const slug_t maxCoord = horizontal
						? std::max({c.x1, c.x2, c.x3})
						: std::max({c.y1, c.y2, c.y3})
					;

					if(maxCoord < cutoff) break;

					out.iters++;

					d = std::min(d, distanceToCurve(c, r));
				}
			}
		}

		out.fill = emPerPixel > 0_cv
			? _clamp((radius - d) / emPerPixel + 0.5_cv, 0_cv, 1_cv)
			: (d <= radius ? 1_cv : 0_cv)
		;

		return out;
	}

	static uint32_t _floatBitsToUint32(slug_t x) { return std::bit_cast<uint32_t>(x); }

	static slug_t _clamp(slug_t x, slug_t lo, slug_t hi) {
		return x < lo ? lo : (x > hi ? hi : x);
	}

	static uint32_t _calcRootCode(slug_t y1, slug_t y2, slug_t y3) {
		const uint32_t i1 = _floatBitsToUint32(y1) >> 31;
		const uint32_t i2 = _floatBitsToUint32(y2) >> 30;
		const uint32_t i3 = _floatBitsToUint32(y3) >> 29;

		uint32_t shift = (i2 & 0x2u) | (i1 & ~0x2u);

		shift = (i3 & 0x4u) | (shift & ~0x4u);

		return (0x2E74u >> shift) & 0x0101u;
	}

	// Roots t1 = (b - d) / a and t2 = (b + d) / a of a*t^2 - 2*b*t + c, for |a| >= EPS. Computed
	// directly, the root whose numerator subtracts nearly equal values loses every significant
	// digit once |a| is small next to |b| - a nearly straight curve, or a straight edge whose
	// float32 midpoint control point leaves a at one ulp of the coordinate magnitude (2^-16 at
	// 128). That root is taken in its equal form c / (b + d) or c / (b - d) instead, whichever
	// denominator adds like signs: q = b + sign(b) * d never cancels, so the roots are c / q and
	// q / a. The result is continuous across the EPS linear fallback, where c / q -> c / (2b).
	// A clamped (negative) discriminant keeps both roots at b / a so a grazing curve's paired
	// contributions still cancel.
	static std::pair<slug_t, slug_t> _solveRoots(slug_t a, slug_t b, slug_t c) {
		const slug_t disc = b * b - a * c;
		const slug_t d = std::sqrt(std::max(disc, 0_cv));
		const slug_t q = b >= 0_cv ? b + d : b - d;
		const slug_t tFar = q / a;
		const slug_t tNear = disc > 0_cv ? c / q : tFar;

		return b >= 0_cv ? std::pair{tNear, tFar} : std::pair{tFar, tNear};
	}

	// One ray crossing: r is the root's signed distance along the ray's axis (em units); tangent is
	// the curve's direction there, B'(t) / 2 = a * t - b, unnormalized.
	struct Crossing {
		slug_t r;
		Point tangent;
	};

	// p1, p2, p3 are the curve's points relative to the sample.
	static std::pair<Crossing, Crossing> _solveHorizPoly(Point p1, Point p2, Point p3) {
		const Point a = p1 - 2_cv * p2 + p3;
		const Point b = p1 - p2;

		auto at = [&](slug_t t) { return Crossing{(a.x * t - 2_cv * b.x) * t + p1.x, a * t - b}; };

		if(std::abs(a.y) < EPS) {
			const auto c = at(std::abs(b.y) >= EPS ? p1.y * (0.5_cv / b.y) : 0_cv);

			return {c, c};
		}

		const auto [t1, t2] = _solveRoots(a.y, b.y, p1.y);

		return {at(t1), at(t2)};
	}

	static std::pair<Crossing, Crossing> _solveVertPoly(Point p1, Point p2, Point p3) {
		const Point a = p1 - 2_cv * p2 + p3;
		const Point b = p1 - p2;

		auto at = [&](slug_t t) { return Crossing{(a.y * t - 2_cv * b.y) * t + p1.y, a * t - b}; };

		if(std::abs(a.x) < EPS) {
			const auto c = at(std::abs(b.x) >= EPS ? p1.x * (0.5_cv / b.x) : 0_cv);

			return {c, c};
		}

		const auto [t1, t2] = _solveRoots(a.x, b.x, p1.x);

		return {at(t1), at(t2)};
	}

	// Under AARamp::Edge, the factor a crossing's axis distance (pixels) is scaled by so it becomes
	// the crossing's distance across the edge in screen pixels, divided by the edge's ramp width
	// |cos| + |sin| (the box filter's support across the edge's screen-space direction). With n the
	// curve's em-space normal there (the tangent rotated: n = (ty, -tx)) and the em axis distance
	// r, the edge's offset is n.axis * r em and one screen pixel step changes it by n . dx and
	// n . dy, so the scale is |n.axis| / (ppe.axis * (|n . dx| + |n . dy|)). It is floored at
	// EDGE_SCALE_FLOOR, which bounds how far behind the sample a crossing can still contribute (see
	// _rampReach()); the floor only bends edges within about 18 degrees of parallel to this ray,
	// where the other ray carries most of the weight. A degenerate tangent keeps the axis ramp.
	slug_t _rampScale(const Crossing& c, bool horizontal, const _Pixel& px) const {
		if(aaRamp == AARamp::Axis) return 1_cv;

		const slug_t nx = c.tangent.y;
		const slug_t ny = -c.tangent.x;
		const slug_t acrossX = std::abs(nx * px.em.dxX + ny * px.em.dxY);
		const slug_t acrossY = std::abs(nx * px.em.dyX + ny * px.em.dyY);
		const slug_t across = (acrossX + acrossY) * (horizontal ? px.ppeX : px.ppeY);

		if(across <= 0_cv) return 1_cv;

		return std::max(EDGE_SCALE_FLOOR, std::abs(horizontal ? nx : ny) / across);
	}

	// How far (pixels) behind the sample along a ray a crossing can still add coverage: the
	// banded walk stops at the first curve entirely beyond it.
	slug_t _rampReach() const {
		return aaRamp == AARamp::Axis ? 0.5_cv : 0.5_cv / EDGE_SCALE_FLOOR;
	}

	void _addHorizCrossings(Sample& out, Point p1, Point p2, Point p3, const _Pixel& px) const {
		const uint32_t code = _calcRootCode(p1.y, p2.y, p3.y);

		if(!code) return;

		const auto [c1, c2] = _solveHorizPoly(p1, p2, p3);

		if(code & 0x01u) {
			const slug_t r = c1.r * px.ppeX;

			out.xcov += _clamp(r * _rampScale(c1, true, px) + 0.5_cv, 0_cv, 1_cv);
			out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r) * 2_cv, 0_cv, 1_cv));
		}

		if(code & 0x100u) {
			const slug_t r = c2.r * px.ppeX;

			out.xcov -= _clamp(r * _rampScale(c2, true, px) + 0.5_cv, 0_cv, 1_cv);
			out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r) * 2_cv, 0_cv, 1_cv));
		}
	}

	void _addVertCrossings(Sample& out, Point p1, Point p2, Point p3, const _Pixel& px) const {
		const uint32_t code = _calcRootCode(p1.x, p2.x, p3.x);

		if(!code) return;

		const auto [c1, c2] = _solveVertPoly(p1, p2, p3);

		if(code & 0x01u) {
			const slug_t r = c1.r * px.ppeY;

			out.ycov -= _clamp(r * _rampScale(c1, false, px) + 0.5_cv, 0_cv, 1_cv);
			out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r) * 2_cv, 0_cv, 1_cv));
		}

		if(code & 0x100u) {
			const slug_t r = c2.r * px.ppeY;

			out.ycov += _clamp(r * _rampScale(c2, false, px) + 0.5_cv, 0_cv, 1_cv);
			out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r) * 2_cv, 0_cv, 1_cv));
		}
	}

	static slug_t _calcCoverage(slug_t xcov, slug_t ycov, slug_t xwgt, slug_t ywgt) {
		const slug_t weighted = std::abs(xcov * xwgt + ycov * ywgt) / std::max(xwgt + ywgt, EPS);
		const slug_t conservative = std::min(std::abs(xcov), std::abs(ycov));

		return _clamp(std::max(weighted, conservative), 0_cv, 1_cv);
	}

	static uint32_t _lookupBandIndir(
		slug_t coordScaled,
		const std::array<uint8_t, Atlas::INDIRECTION_SIZE>& indir
	) {
		const auto q = static_cast<uint32_t>(_clamp(coordScaled, 0_cv, cv(Atlas::INDIRECTION_SIZE - 1)));

		return indir[q];
	}
};

// ================================================================================================
// decode() - reconstruct a Sampler from packed atlas textures
//
// Reads the curve and band textures produced by Atlas::build() + packTextures().
// Throws std::out_of_range if the key is not found, std::runtime_error on texture
// format or bounds violations.
// ================================================================================================

inline Sampler decode(
	const Atlas::Shape& shape,
	const Atlas::TextureData& curveTex,
	const Atlas::TextureData& bandTex
);

inline Sampler decode(const Atlas& atlas, Key key) {
	const auto info = atlas.getShape(key);

	if(!info) throw std::out_of_range("Key not found in atlas (or atlas not built yet)");

	return decode(*info, atlas.getCurveTextureData(), atlas.getBandTextureData());
}

inline Sampler decode(
	const Atlas::Shape& shape,
	const Atlas::TextureData& curveTex,
	const Atlas::TextureData& bandTex
) {

	const bool curveIsHalf = curveTex.format == Atlas::TextureData::Format::RGBA16F;

	if(!curveIsHalf && curveTex.format != Atlas::TextureData::Format::RGBA32F)
		throw std::runtime_error("Unexpected curve texture format")
	;

	const bool bandIsLegacy = bandTex.format == Atlas::TextureData::Format::RGBA16UI;

	if(!bandIsLegacy && bandTex.format != Atlas::TextureData::Format::RG16UI)
		throw std::runtime_error("Unexpected band texture format")
	;

	// RG16UI (current) is 2 uint16_t/texel; RGBA16UI (legacy read-compat) is 4 - callers only
	// ever read indices 0/1 either way (B/A were never consumed even in the legacy format).
	const uint32_t bandStride = bandIsLegacy ? 4 : 2;

	Sampler out;

	out.shape = shape;
	out.shape.curves.clear(); // see Sampler::shape's own comment - never read, never kept live here

	if(shape.bandScaleX == 0_cv || shape.bandScaleY == 0_cv) {
		out.hbandOffsets = {0, 0};
		out.vbandOffsets = {0, 0};

		return out;
	}

	const auto* curveBytes = curveTex.bytes.data();
	const auto* bandData = reinterpret_cast<const uint16_t*>(bandTex.bytes.data());

	// Curve texel values are either raw float32 or half-float, per curveIsHalf above --
	// always returned as float32 here so the rest of decode() stays format-agnostic.
	auto readCurveTexel = [&](uint32_t texelIndex) -> std::array<float, 4> {
		if(texelIndex >= curveTex.width * curveTex.height)
			throw std::runtime_error("Curve texture read out of bounds")
		;

		std::array<float, 4> v;

		if(curveIsHalf) {
			const auto* p = reinterpret_cast<const uint16_t*>(curveBytes) + size_t{texelIndex} * 4;

			for(size_t i = 0; i < 4; i++) v[i] = detail::halfToFloat(p[i]);
		}

		else {
			const auto* p = reinterpret_cast<const float*>(curveBytes) + size_t{texelIndex} * 4;

			for(size_t i = 0; i < 4; i++) v[i] = p[i];
		}

		return v;
	};

	const uint32_t shapeStart = shape.bandTexY * bandTex.width + shape.bandTexX;
	const uint32_t numHBands = shape.bandMaxY + 1;
	const uint32_t numVBands = shape.bandMaxX + 1;
	const uint32_t numBandHdrs = numHBands + numVBands;
	const uint32_t indirSize = numBandHdrs > 0 ? 2 * Atlas::INDIRECTION_SIZE : 0;

	auto readBandTexel = [&](uint32_t texelIndex) -> const uint16_t* {
		if(texelIndex >= bandTex.width * bandTex.height)
			throw std::runtime_error("Band texture read out of bounds")
		;

		return bandData + size_t{texelIndex} * bandStride;
	};

	for(uint32_t q = 0; q < Atlas::INDIRECTION_SIZE; q++) {
		out.indirY[q] = static_cast<uint8_t>(readBandTexel(shapeStart + q)[0]);
		out.indirX[q] = static_cast<uint8_t>(readBandTexel(shapeStart + Atlas::INDIRECTION_SIZE + q)[0]);
	}

	struct Header { uint32_t count = 0, offset = 0; };

	std::vector<Header> headers(numBandHdrs);

	for(uint32_t i = 0; i < numBandHdrs; i++) {
		const auto* texel = readBandTexel(shapeStart + indirSize + i);

		headers[i].count = texel[0];
		headers[i].offset = texel[1];
	}

	std::vector<uint32_t> globalIndices;

	globalIndices.reserve(64);

	auto decodeBandList = [&](
		uint32_t headerIndex,
		uint32_t numBands,
		std::vector<uint32_t>& offsets,
		std::vector<uint32_t>& indices
	) {
		offsets.clear();
		indices.clear();
		offsets.push_back(0);

		for(uint32_t i = 0; i < numBands; i++) {
			const auto& h = headers[headerIndex + i];

			for(uint32_t j = 0; j < h.count; j++) {
				const auto* texel = readBandTexel(shapeStart + h.offset + j);
				const uint32_t cx = texel[0];
				const uint32_t cy = texel[1];

				// Raw texel address of the curve's first texel, used directly as the dedup/remap
				// key - NOT divided down into a "curve index" via an assumed 2-texels-per-curve
				// stride. That assumption breaks once endpoint-shared packing lets a curve's
				// first texel land at an odd offset (a shared texel isn't always curve-aligned).
				const uint32_t idx = cy * curveTex.width + cx;

				indices.push_back(idx);
				globalIndices.push_back(idx);
			}

			offsets.push_back(static_cast<uint32_t>(indices.size()));
		}
	};

	decodeBandList(0, numHBands, out.hbandOffsets, out.hbandIndices);
	decodeBandList(numHBands, numVBands, out.vbandOffsets, out.vbandIndices);

	std::sort(globalIndices.begin(), globalIndices.end());

	globalIndices.erase(
		std::unique(globalIndices.begin(), globalIndices.end()),
		globalIndices.end()
	);

	std::unordered_map<uint32_t, uint32_t> remap;

	remap.reserve(globalIndices.size());

	out.curves.reserve(globalIndices.size());

	for(uint32_t globalIndex : globalIndices) {
		const uint32_t texel0 = globalIndex;
		const uint32_t texel1 = texel0 + 1;

		const auto t0 = readCurveTexel(texel0);
		const auto t1 = readCurveTexel(texel1);

		remap[globalIndex] = static_cast<uint32_t>(out.curves.size());

		out.curves.push_back({t0[0], t0[1], t0[2], t0[3], t1[0], t1[1]});
	}

	for(auto& idx : out.hbandIndices) idx = remap.at(idx);
	for(auto& idx : out.vbandIndices) idx = remap.at(idx);

	return out;
}

// ================================================================================================
// SDF baking - only available when built with -DSLUGHORN_SDF=ON (msdfgen)
// ================================================================================================

#ifdef SLUGHORN_HAS_SDF

// One baked distance-field tile, straight out of msdfgen. See Atlas::SDF (slughorn.hpp) for the
// full tile contract: uniform scale, em-space frame, row 0 = BOTTOM, values clamped to [0, 1]
// with edge = 0.5. `width == 0` means the shape has no geometry (there is nothing to bake).
//
// Reconstruct in a shader: channels == 1 -> sd = data.r; channels == 3 -> sd = median(r, g, b).
struct Field {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t channels = 0; // 1 = Type::SDF, 3 = Type::MSDF

	slug_t range = 0_cv; // em-space half-range this field was baked with
	slug_t texelsPerEm = 0_cv;
	Point emOrigin = {}; // em-space point at the bottom-left corner

	std::vector<float> data = {}; // row-major, `channels` floats per texel

	float at(uint32_t row, uint32_t col, uint32_t channel=0) const {
		return data[(size_t{row} * width + col) * channels + channel];
	}
};

// Build an msdfgen::Shape from the atlas contours for key.
// Coordinates are in em-space (same space as Atlas::Curves).
// Returns an empty Shape if the key has no geometry.
//
// Slughorn's curve winding appears CW to msdfgen (Y-up, CCW=filled convention).
// Each contour is reversed so that filled regions become CCW, then orientContours()
// assigns the correct fill/hole role for compound shapes.
inline msdfgen::Shape toMsdfgenShape(const Atlas& atlas, Key key) {
	msdfgen::Shape shape;

	for(const auto& contour : atlas.getShapeContours(key)) {
		auto& c = shape.addContour();

		// Each Atlas::Curve is a quadratic Bezier: p1=start, p2=off-curve, p3=end.
		for(const auto& curve : contour) c.addEdge(msdfgen::EdgeHolder(
			{curve.x1, curve.y1},
			{curve.x2, curve.y2},
			{curve.x3, curve.y3}
		));

		c.reverse();
	}

	if(!shape.contours.empty()) shape.orientContours();

	return shape;
}

// Bake a centerline stroke (Shape::strokeRadius > 0) into a Field without msdfgen: its curves are
// a centerline, not an outline, so the exact signed distance is strokeRadius minus the distance to
// the nearest curve (distanceToCurve()), positive inside. Same tile contract as field() below:
// bounds are the curves' control-point box grown by strokeRadius + range, one uniform scale with
// the longer axis at config.tileSize, texel centers at +0.5, and +/- range em mapped onto [0, 1].
// A round-capped, round-joined stroke has no sharp convex corners, so Type::MSDF stores the same
// value in all three channels (their median is that value).
inline Field strokeField(
	const Atlas::Curves& curves,
	slug_t strokeRadius,
	const Atlas::SDF::Config& config,
	slug_t range
) {
	Field out;

	out.channels = config.type == Atlas::SDF::Type::MSDF ? 3u : 1u;
	out.range = range;

	if(curves.empty()) return out;

	double l = curves[0].x1, b = curves[0].y1, r = l, t = b;

	for(const auto& c : curves) {
		for(const auto& [x, y] : {std::pair{c.x1, c.y1}, std::pair{c.x2, c.y2}, std::pair{c.x3, c.y3}}) {
			l = std::min(l, static_cast<double>(x));
			r = std::max(r, static_cast<double>(x));
			b = std::min(b, static_cast<double>(y));
			t = std::max(t, static_cast<double>(y));
		}
	}

	const double pad = static_cast<double>(strokeRadius + range);

	l -= pad;
	b -= pad;
	r += pad;
	t += pad;

	const double scale = config.tileSize / std::max(r - l, t - b);

	// See field() for the epsilon and the clamp.
	const auto texels = [&](double extent) {
		return static_cast<uint32_t>(
			std::clamp(std::ceil(extent * scale - 1e-4), 1.0, static_cast<double>(config.tileSize))
		);
	};

	const uint32_t w = texels(r - l);
	const uint32_t h = texels(t - b);

	std::vector<float> buf(size_t{w} * h * out.channels);

	for(uint32_t row = 0; row < h; row++) {
		const slug_t y = static_cast<slug_t>(b + (row + 0.5) / scale);

		for(uint32_t col = 0; col < w; col++) {
			const slug_t x = static_cast<slug_t>(l + (col + 0.5) / scale);

			slug_t d = std::numeric_limits<slug_t>::max();

			for(const auto& c : curves) d = std::min(d, distanceToCurve(c, {x, y}));

			const float v = std::clamp(
				static_cast<float>(0.5_cv + (strokeRadius - d) / (2_cv * range)),
				0.0f,
				1.0f
			);

			for(uint32_t ch = 0; ch < out.channels; ch++) {
				buf[(size_t{row} * w + col) * out.channels + ch] = v;
			}
		}
	}

	out.width = w;
	out.height = h;
	out.texelsPerEm = static_cast<slug_t>(scale);
	out.emOrigin = {static_cast<slug_t>(l), static_cast<slug_t>(b)};
	out.data = std::move(buf);

	return out;
}

// Bake one shape into a Field. Centerline strokes go to strokeField().
//
// msdfgen's Projection formula is pixel = scale * (shape + translate) with `translate` in em-space,
// so translating by -bounds.(l, b) puts the bounds' bottom-left corner at pixel (0, 0) - which is
// exactly the emOrigin recorded on the result. ONE scale is used for both axes (the tile keeps the
// shape's aspect ratio), so a tile is (ceil(bw * scale), ceil(bh * scale)) texels, the longer axis
// being config.tileSize.
//
// Bounds are expanded by `range` on every side so the tile's edge is deeply exterior (SDF << 0.5);
// otherwise curves touching the tight bbox leave edge texels at ~0.5, which bilinear filtering (and
// the frontend's AA margin) turns into ghost fringes. Distances of +/- `range` em map onto [0, 1]
// with NO clamp in msdfgen's DistanceMapping, so values overshoot in the padded margin; they are
// clamped here so the stored data is well-defined for any frontend.
//
// No Y flip, deliberately: msdfgen's native row 0 = bottom already agrees with GL/osg::Image
// (row 0 -> V = 0) and with the shape's own Y-up em-space. Flip only where a human-facing image
// needs it (e.g. `bin/slughorn sdf`).
inline Field field(
	const Atlas& atlas,
	Key key,
	const Atlas::SDF::Config& config,
	slug_t range
) {
	const bool msdf = config.type == Atlas::SDF::Type::MSDF;

	Field out;

	out.channels = msdf ? 3u : 1u;
	out.range = range;

	if(const auto info = atlas.getShape(key); info && info->strokeRadius > 0_cv) {
		return strokeField(info->curves, info->strokeRadius, config, range);
	}

	msdfgen::Shape shape = toMsdfgenShape(atlas, key);

	if(shape.contours.empty()) return out;

	if(msdf) {
		if(config.coloring == Atlas::SDF::Coloring::ByDistance) {
			msdfgen::edgeColoringByDistance(shape, 3.0);
		}

		else msdfgen::edgeColoringSimple(shape, 3.0);
	}

	const auto bounds = shape.getBounds(range);
	const double bw = bounds.r - bounds.l;
	const double bh = bounds.t - bounds.b;
	const double scale = config.tileSize / std::max(bw, bh);

	// The tiny epsilon keeps floating-point noise on the longer axis from rounding up to
	// tileSize + 1; the clamp is belt and braces for the same reason.
	const auto texels = [&](double extent) {
		return static_cast<uint32_t>(
			std::clamp(std::ceil(extent * scale - 1e-4), 1.0, static_cast<double>(config.tileSize))
		);
	};

	const uint32_t w = texels(bw);
	const uint32_t h = texels(bh);

	const msdfgen::SDFTransformation transform(
		msdfgen::Projection({scale, scale}, {-bounds.l, -bounds.b}),
		msdfgen::DistanceMapping(msdfgen::Range(2.0 * range)) // [-range, range] em -> [0, 1]
	);

	std::vector<float> buf(size_t{w} * h * out.channels);

	if(msdf) {
		msdfgen::BitmapSection<float, 3> bmp(buf.data(), static_cast<int>(w), static_cast<int>(h));

		msdfgen::generateMSDF(bmp, shape, transform);
	}

	else {
		msdfgen::BitmapSection<float, 1> bmp(buf.data(), static_cast<int>(w), static_cast<int>(h));

		msdfgen::generateSDF(bmp, shape, transform);
	}

	for(float& v : buf) v = std::clamp(v, 0.0f, 1.0f);

	out.width = w;
	out.height = h;
	out.texelsPerEm = static_cast<slug_t>(scale);
	out.emOrigin = {static_cast<slug_t>(bounds.l), static_cast<slug_t>(bounds.b)};
	out.data = std::move(buf);

	return out;
}

#endif

}
}
