#pragma once

#include <utility>

// ================================================================================================
// Decomposes Skia SkPath objects into slughorn Atlas shapes, with optional stroke-to-fill expansion
// via skpathutils::FillPathWithPaint. No OSG, VSG, or other graphics library dependency.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_SKIA_IMPLEMENTATION
//   #include <slughorn/skia.hpp>
//
// All other translation units include it without the define.
//
// The Skia headers must be on your include path. Link against skia.
//
// CONIC HANDLING
// --------------
// Skia paths may contain rational quadratic (conic) segments with a weight w. When w == 1 the
// conic is an ordinary quadratic and is passed through as-is. When w != 1 (e.g. circular arcs,
// where w = cos(A / 2)) the segment is split at t=0.5 into two ordinary quadratics using the
// standard rational-to- polynomial formula:
//
//   mid = (P0 + 2w * P1 + P2) / (2(1 + w)) - point on curve at t=0.5
//   ctrl0 = (P0 + w * P1) / (1+w) = control point for first half
//   ctrl1 = (P2 + w * P1) / (1+w) = control point for second half
//
// One split is sufficient in practice because Skia's own iterator already subdivides conics before
// yielding them. See slughorn::skia::detail:: splitConic() if you need to inspect or extend the
// logic.
// ================================================================================================

#include "slughorn.hpp"

#include "include/core/SkPath.h"
#include "include/core/SkPaint.h"
#include "include/pathops/SkPathOps.h"

namespace slughorn {
namespace skia {

// ================================================================================================
// Decomposition
// ================================================================================================

// Decompose @p path into slughorn curves, shifted to local origin for tight atlas bands.
//
// The bounding box minimum is subtracted from every curve point. Returns both the shifted curves
// and a transform Matrix (dx/dy only; xx/yy are identity). Store the Matrix in Layer::transform.
//
// The returned transform depends on @p origin:
//
// Origin::Defaulf - transform.dx/dy = bounds.left/top * scale (bbox corner). Pass directly to
// Layer::transform; computeQuad will reconstruct the correct world position.
//
// Origin::Centered - transform.dx/dy = bounds.centerX/Y * scale (bbox center). Pass directly to
// Layer::transform; computeQuad subtracts originX/Y (= rangeX/2, rangeY/2) and the quad still lands
// at the correct canvas position. Use this when the transform should act as a pivot point for
// GPU-side rotation.
//
// @p scale is applied uniformly to every coordinate after the local shift. Pass 1.0 if coordinates
// are already normalized.
//
// Conic segments (kConic_Verb) are split into two ordinary quadratics. Cubic segments
// (kCubic_Verb) are split at their midpoint into two quadratics via CurveDecomposer::cubicTo.
//
// Returns a ShapeInfo with an empty Curves vector and an identity Matrix if @p path is empty or
// has a zero-size bounding box.
std::pair<Atlas::ShapeInfo, Matrix> decomposePath(
	const SkPath& path,
	slug_t scale=1.0_cv,
	Atlas::ShapeInfo::Origin origin=Atlas::ShapeInfo::Origin::Default
);

// ================================================================================================
// Stroke Expansion
// ================================================================================================

// Expand @p src from a stroked outline into a filled path using Skia's
// skpathutils::FillPathWithPaint. The returned path can be passed directly to decomposePath() or
// loadStrokedShape().
//
// join and cap default to round, the most common choice for smooth shapes.
SkPath strokeToFill(
	const SkPath& src,
	float strokeWidth,
	SkPaint::Join join=SkPaint::kRound_Join,
	SkPaint::Cap cap=SkPaint::kRound_Cap
);

// ================================================================================================
// Atlas Integration
// ================================================================================================

// Decompose @p path and register the result in @p atlas under @p key.
//
// If autoMetrics is true (default) slughorn derives width/height/bearing/advance from the curve
// bounding box. Set it to false and fill in the ShapeInfo fields below if you need precise control.
//
// Returns the local-origin offset as a Matrix (see decomposePath). Store it in Layer::transform for
// correct composite positioning.
//
// Returns an identity Matrix and does NOT call addShape if the path is empty or has a zero-size
// bounding box.
Matrix loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t scale=1.0_cv,
	bool autoMetrics=true,
	Atlas::ShapeInfo::Origin origin=Atlas::ShapeInfo::Origin::Default
);

// Convenience: stroke-expand then load. Equivalent to: loadShape(strokeToFill(path, strokeWidth,
// join, cap), atlas, key, scale)
Matrix loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t scale=1.0_cv,
	SkPaint::Join join=SkPaint::kRound_Join,
	SkPaint::Cap cap=SkPaint::kRound_Cap,
	Atlas::ShapeInfo::Origin origin=Atlas::ShapeInfo::Origin::Default
);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_SKIA_IMPLEMENTATION

#include "include/core/SkPathUtils.h"

namespace slughorn {
namespace skia {
namespace detail {

static void splitConic(
	CurveDecomposer& decomposer,
	slug_t p0x, slug_t p0y, // current pen P0
	slug_t p1x, slug_t p1y, // control point P1
	slug_t p2x, slug_t p2y, // end point P2
	slug_t w // conic weight
) {
	const slug_t denom = 1.0_cv + w;
	const slug_t inv = 1.0_cv / denom;

	// Midpoint on the curve
	const slug_t midX = (p0x + 2.0_cv * w * p1x + p2x) * (0.5_cv * inv);
	const slug_t midY = (p0y + 2.0_cv * w * p1y + p2y) * (0.5_cv * inv);

	// Control points for each half
	const slug_t c0x = (p0x + w * p1x) * inv;
	const slug_t c0y = (p0y + w * p1y) * inv;
	const slug_t c1x = (p2x + w * p1x) * inv;
	const slug_t c1y = (p2y + w * p1y) * inv;

	decomposer.quadTo(c0x, c0y, midX, midY);
	decomposer.quadTo(c1x, c1y, p2x, p2y);
}

}

std::pair<Atlas::ShapeInfo, Matrix> decomposePath(const SkPath& path, slug_t scale, Atlas::ShapeInfo::Origin origin) {
	if(path.isEmpty()) return { {}, Matrix::identity() };

	const SkRect bounds = path.getBounds();

	if(bounds.isEmpty()) return { {}, Matrix::identity() };

	// Translate path so its bounding box top-left sits at the origin.
	// All curve coordinates then live in [0, width] x [0, height]; tight
	// bands, no wasted em-space from canvas offset.
	const SkPath local = path.makeTransform(
		SkMatrix::Translate(-bounds.left(), -bounds.top())
	);

	Atlas::Curves curves;

	CurveDecomposer decomposer(curves);

	// Force-close open contours so the winding rule works correctly.
	SkPath::Iter iter(local, true);

	SkPoint pts[4];

	SkPath::Verb verb;

	while((verb = iter.next(pts)) != SkPath::kDone_Verb) {
		switch(verb) {
			case SkPath::kMove_Verb:
				decomposer.moveTo(
					cv(pts[0].x()) * scale,
					cv(pts[0].y()) * scale
				);

				break;

			case SkPath::kLine_Verb:
				decomposer.lineTo(
					cv(pts[1].x()) * scale,
					cv(pts[1].y()) * scale
				);

				break;

			case SkPath::kQuad_Verb:
				decomposer.quadTo(
					cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
					cv(pts[2].x()) * scale, cv(pts[2].y()) * scale
				);

				break;

			case SkPath::kConic_Verb: {
				const slug_t w = cv(iter.conicWeight());

				// w == 1.0: ordinary quadratic, no split needed.
				if(std::abs(w - 1.0_cv) < 1e-5_cv) decomposer.quadTo(
					cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
					cv(pts[2].x()) * scale, cv(pts[2].y()) * scale
				);

				else detail::splitConic(
					decomposer,
					cv(pts[0].x()) * scale, cv(pts[0].y()) * scale,
					cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
					cv(pts[2].x()) * scale, cv(pts[2].y()) * scale,
					w
				);

				break;
			}

			case SkPath::kCubic_Verb:
				decomposer.cubicTo(
					cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
					cv(pts[2].x()) * scale, cv(pts[2].y()) * scale,
					cv(pts[3].x()) * scale, cv(pts[3].y()) * scale
				);

				break;

			// forceClose=true means the iterator inserts an implicit lineTo back to the start
			// before emitting kClose_Verb, so we don't need to do anything here.
			case SkPath::kClose_Verb:
			case SkPath::kDone_Verb:
				break;
		}
	}

	Matrix transform = Matrix::identity();

	if(origin == Atlas::ShapeInfo::Origin::Centered) {
		transform.dx = cv(bounds.centerX()) * scale;
		transform.dy = cv(bounds.centerY()) * scale;
	} else {
		transform.dx = cv(bounds.left()) * scale;
		transform.dy = cv(bounds.top()) * scale;
	}

	Atlas::ShapeInfo info;

	info.curves = std::move(curves);
	info.origin = origin;

	return { std::move(info), transform };
}

SkPath strokeToFill(
	const SkPath& src,
	float strokeWidth,
	SkPaint::Join join,
	SkPaint::Cap cap
) {
	SkPaint paint;

	paint.setStyle(SkPaint::kStroke_Style);
	paint.setStrokeWidth(strokeWidth);
	paint.setStrokeJoin(join);
	paint.setStrokeCap(cap);

	return skpathutils::FillPathWithPaint(src, paint);
}

Matrix loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t scale,
	bool autoMetrics,
	Atlas::ShapeInfo::Origin origin
) {
	auto [info, transform] = decomposePath(path, scale, origin);

	if(info.curves.empty()) return Matrix::identity();

	info.autoMetrics = autoMetrics;

	atlas.addShape(key, info);

	return transform;
}

Matrix loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t scale,
	SkPaint::Join join,
	SkPaint::Cap cap,
	Atlas::ShapeInfo::Origin origin
) {
	return loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, scale, true, origin);
}

}
}

#endif
