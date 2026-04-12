#pragma once

// ================================================================================================
// Decomposes Skia SkPath objects into slughorn Atlas shapes, with optional stroke-to-fill expansion
// via skpathutils::FillPathWithPaint. No OSG, VSG, or other graphics library dependency.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_SKIA_IMPLEMENTATION
//   #include "slughorn-skia.hpp"
//
// All other translation units include it without the define.
//
// The Skia headers must be on your include path. Link against skia.
//
// Y-UP CONVENTION
// ---------------
// slughorn uses Y-up coordinates throughout. Skia uses Y-down. All decompose/load functions in
// this header convert automatically: pass canvasHeight (in the same pre-scale units as the path
// coordinates) and the flip is applied for you. For the local-coords variants canvasHeight is
// the full canvas height (not the shape height); the shape-local height is derived internally.
//
// For the raw decomposePath() overload, canvasHeight defaults to 0.0 which means "pass through
// unchanged"; use this only when your path coordinates are already Y-up (e.g. pre-flipped by
// another step). Prefer passing an explicit canvasHeight.
//
// CONIC HANDLING
// --------------
// Skia paths may contain rational quadratic (conic) segments with a weight w. When w == 1 the
// conic is an ordinary quadratic and is passed through as-is. When w != 1 (e.g. circular arcs,
// where w = cos(A / 2)) the segment is split at t=0.5 into two ordinary quadratics using the
// standard rational-to-polynomial formula:
//
// mid = (P0 + 2w * P1 + P2) / (2(1 + w)) - point on curve at t=0.5
// ctrl0 = (P0 + w * P1) / (1+w) - control point for first half
// ctrl1 = (P2 + w * P1) / (1+w) - control point for second half
//
// One split is sufficient in practice because Skia's own iterator already subdivides conics before
// yielding them. See slughorn::skia::detail::splitConic() if you need to inspect or extend the
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

// Decompose @p path into slughorn curves and append them to @p curves.
//
// @p scale is applied uniformly to every coordinate - use it to normalize path coordinates into the
// [0, 1] em-square that slughorn expects. Pass 1.0 if your coordinates are already normalized.
//
// @p canvasHeight is the height of the coordinate space in pre-scale source units. When nonzero,
// every Y coordinate is flipped (y_up = canvasHeight - y_down) to convert from Skia's Y-down
// convention to slughorn's Y-up convention. Pass 0.0 (default) only if the path is already Y-up.
//
// Conic segments (kConic_Verb) are split into two ordinary quadratics. Cubic segments
// (kCubic_Verb) are split at their midpoint into two quadratics via CurveDecomposer::cubicTo.
void decomposePath(
	const SkPath& path,
	Atlas::Curves& curves,
	slug_t scale=1.0_cv,
	slug_t canvasHeight=0.0_cv
);

// Decompose @p path in local coordinate space (tight bounding box origin), appending curves to @p
// curves.
//
// The path is translated so that its bounding box bottom-left (Y-up) maps to the origin before
// decomposition. This means the atlas shape gets bands sized to its own geometry rather than the
// full canvas.
//
// The translation that maps local space back to the original canvas space is returned in @p
// outTransform as a pure translation Matrix (dx/dy only, xx/yy = 1, xy/yx = 0). Store this in
// Layer::transform so that ShapeDrawable::compile() can apply it when building the quad position.
// outTransform.dy is in Y-up em-space (i.e. the Y-up bottom of the shape in canvas coordinates).
//
// @p canvasHeight is the height of the coordinate space in pre-scale source units, required to
// convert the Y-down bounding box into Y-up canvas coordinates for outTransform.dy.
//
// @p scale is applied after the local translation, normalizing the local bounding box into
// slughorn's em-space exactly as decomposePath() does for the full canvas.
//
// Returns false (and leaves @p curves and @p outTransform untouched) if @p path is empty or has a
// zero-size bounding box.
bool decomposePathLocal(
	const SkPath& path,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale=1.0_cv
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
// @p canvasHeight is forwarded to decomposePath() for Y-down -> Y-up conversion; see that
// function's documentation. Pass 0.0 only for already-Y-up paths.
//
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty.
bool loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t canvasHeight,
	slug_t scale=1.0_cv,
	bool autoMetrics=true
);

// Decompose @p path in local coordinate space and register the result in @p atlas under @p key. The
// canvas-space translation is written to @p outTransform; store it in the corresponding
// Layer::transform.
//
// This is the local-coords counterpart to loadShape(). Use it for any layer whose geometry occupies
// only a small portion of the full canvas, so the atlas allocates tight bands rather than
// canvas-sized ones.
//
// @p canvasHeight is forwarded to decomposePathLocal() for Y-down -> Y-up conversion.
//
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty or has a zero-size bounding box.
bool loadShapeLocal(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale=1.0_cv,
	bool autoMetrics=true
);

// Convenience: stroke-expand then load. Equivalent to:
// loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, canvasHeight, scale)
bool loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t canvasHeight,
	slug_t scale=1.0_cv,
	SkPaint::Join join=SkPaint::kRound_Join,
	SkPaint::Cap cap=SkPaint::kRound_Cap
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
	slug_t p0x, slug_t p0y, // current pen P0 (already Y-up)
	slug_t p1x, slug_t p1y, // control point P1 (already Y-up)
	slug_t p2x, slug_t p2y, // end point P2 (already Y-up)
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

void decomposePath(const SkPath& path, Atlas::Curves& curves, slug_t scale, slug_t canvasHeight) {
	CurveDecomposer decomposer(curves);

	// Flip helper: when canvasHeight > 0, converts Y-down to Y-up.
	const bool doFlip = (canvasHeight > 0.0_cv);

	auto flipY = [&](slug_t y) -> slug_t { return doFlip ? (canvasHeight - y) : y; };

	// Force-close open contours so the winding rule works correctly.
	SkPath::Iter iter(path, true);
	SkPoint pts[4];
	SkPath::Verb verb;

	while((verb = iter.next(pts)) != SkPath::kDone_Verb) {
		switch(verb) {
		case SkPath::kMove_Verb:
			decomposer.moveTo(cv(pts[0].x()) * scale, flipY(cv(pts[0].y())) * scale);

			break;

		case SkPath::kLine_Verb:
			decomposer.lineTo(cv(pts[1].x()) * scale, flipY(cv(pts[1].y())) * scale);

			break;

		case SkPath::kQuad_Verb:
			decomposer.quadTo(
				cv(pts[1].x()) * scale, flipY(cv(pts[1].y())) * scale,
				cv(pts[2].x()) * scale, flipY(cv(pts[2].y())) * scale
			);

			break;

		case SkPath::kConic_Verb: {
			const slug_t w = cv(iter.conicWeight());

			// w == 1.0: ordinary quadratic, no split needed.
			if(std::abs(w - 1.0_cv) < 1e-5_cv) decomposer.quadTo(
				cv(pts[1].x()) * scale, flipY(cv(pts[1].y())) * scale,
				cv(pts[2].x()) * scale, flipY(cv(pts[2].y())) * scale
			);

			else detail::splitConic(
				decomposer,
				cv(pts[0].x()) * scale, flipY(cv(pts[0].y())) * scale,
				cv(pts[1].x()) * scale, flipY(cv(pts[1].y())) * scale,
				cv(pts[2].x()) * scale, flipY(cv(pts[2].y())) * scale,
				w
			);

			break;
		}

		case SkPath::kCubic_Verb:
			decomposer.cubicTo(
				cv(pts[1].x()) * scale, flipY(cv(pts[1].y())) * scale,
				cv(pts[2].x()) * scale, flipY(cv(pts[2].y())) * scale,
				cv(pts[3].x()) * scale, flipY(cv(pts[3].y())) * scale
			);

			break;

		// forceClose=true means the iterator inserts an implicit lineTo back to the start
		// before emitting kClose_Verb, so we don't need to do anything here.
		case SkPath::kClose_Verb:
		case SkPath::kDone_Verb:
			break;
		}
	}
}

bool decomposePathLocal(
	const SkPath& path,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale
) {
	if(path.isEmpty()) return false;

	const SkRect bounds = path.getBounds();

	if(bounds.isEmpty()) return false;

	// Translate so the Y-down bottom-left of the bounding box moves to the local origin.
	// Combined with the Y-flip in decomposePath (using shapeHeight as the local canvasHeight),
	// this maps: Y-down bottom -> Y-up 0, Y-down top -> Y-up shapeHeight.
	const slug_t shapeHeight = cv(bounds.height());
	const SkPath local = path.makeTransform(SkMatrix::Translate(-bounds.left(), -bounds.bottom()));

	// Decompose using shape height as the local canvas height for the Y-flip.
	const size_t curvesBefore = curves.size();

	decomposePath(local, curves, scale, shapeHeight);

	if(curves.size() == curvesBefore) return false;

	// outTransform.dy is the Y-up canvas position of the shape's bottom edge:
	//
	// y_up_bottom = canvasHeight - bounds.bottom() (Y-down bottom -> Y-up)
	outTransform = Matrix::identity();
	outTransform.dx = cv(bounds.left()) * scale;
	outTransform.dy = (canvasHeight - cv(bounds.bottom())) * scale;

	return true;
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

bool loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t canvasHeight,
	slug_t scale,
	bool autoMetrics
) {
	Atlas::ShapeInfo info;

	info.autoMetrics = autoMetrics;

	decomposePath(path, info.curves, scale, canvasHeight);

	if(info.curves.empty()) return false;

	atlas.addShape(key, info);

	return true;
}

bool loadShapeLocal(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale,
	bool autoMetrics
) {
	Atlas::ShapeInfo info;

	info.autoMetrics = autoMetrics;

	if(!decomposePathLocal(path, info.curves, outTransform, canvasHeight, scale)) return false;

	atlas.addShape(key, info);

	return true;
}

bool loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t canvasHeight,
	slug_t scale,
	SkPaint::Join join,
	SkPaint::Cap cap
) {
	return loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, canvasHeight, scale);
}

}
}

#endif
