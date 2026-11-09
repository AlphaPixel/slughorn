#pragma once

#include <optional>
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
// and a Transform (x/y only). Store the Transform in Layer::transform.
//
// The returned transform depends on @p origin:
//
// Origin::Default - transform.x/y = bounds.left/top * scale (bbox corner). Pass directly to
// Layer::transform; computeQuad will reconstruct the correct world position.
//
// Origin::Centered - transform.x/y = bounds.centerX/Y * scale (bbox center). Pass directly to
// Layer::transform; computeQuad subtracts origin.point (= rangeX/2, rangeY/2) and the quad still lands
// at the correct canvas position. Use this when the transform should act as a pivot point for
// GPU-side rotation.
//
// @p scale is applied uniformly to every coordinate after the local shift. Pass 1.0 if coordinates
// are already normalized.
//
// Conic segments (kConic_Verb) are split into two ordinary quadratics. Cubic segments
// (kCubic_Verb) are split at their midpoint into two quadratics via CurveDecomposer::cubicTo.
//
// Returns a ShapeInfo with an empty Curves vector and a zero Transform if @p path is empty or
// has a zero-size bounding box.
//
// When autoMetrics=false and canvasExtent={widthEm, heightEm} is supplied, the returned ShapeInfo
// has its band metrics (bearing, width, height, autoMetrics) pre-populated. Callers that use
// decomposePath directly and then call atlas.addShape() should pass canvasExtent here.
std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	const SkPath& path,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
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
// bounding box. When autoMetrics=false, supply canvasExtent={widthEm, heightEm} to declare the
// band extent explicitly; without it, loadShape falls back to tight-bbox behavior.
//
// Returns the local-origin offset as a Transform (see decomposePath). Store it in Layer::transform
// for correct composite positioning.
//
// Returns a zero Transform and does NOT call addShape if the path is empty or has a zero-size
// bounding box.
Transform loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
);

// Convenience: stroke-expand then load. Equivalent to: loadShape(strokeToFill(path, strokeWidth,
// join, cap), atlas, key, scale)
Transform loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t scale=1_cv,
	SkPaint::Join join=SkPaint::kRound_Join,
	SkPaint::Cap cap=SkPaint::kRound_Cap,
	Atlas::ShapeInfo::Origin origin={}
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
	Point p0, // current pen P0
	Point p1, // control point P1
	Point p2, // end point P2
	slug_t w // conic weight
) {
	const slug_t denom = 1_cv + w;
	const slug_t inv = 1_cv / denom;

	// Midpoint on the curve
	const Point mid = (p0 + 2_cv * w * p1 + p2) * (0.5_cv * inv);

	// Control points for each half
	const Point c0 = (p0 + w * p1) * inv;
	const Point c1 = (p2 + w * p1) * inv;

	decomposer.quadTo(c0, mid);
	decomposer.quadTo(c1, p2);
}

}

std::pair<Atlas::ShapeInfo, Transform> decomposePath(const SkPath& path, slug_t scale, Atlas::ShapeInfo::Origin origin, bool autoMetrics, std::optional<std::pair<slug_t, slug_t>> canvasExtent) {
	if(path.isEmpty()) return { {}, {} };

	const SkRect bounds = path.getBounds();

	if(bounds.isEmpty()) return { {}, {} };

	// Translate path so its bounding box top-left sits at the origin.
	// All curve coordinates then live in [0, width] x [0, height]; tight
	// bands, no wasted em-space from canvas offset. Skipped when autoMetrics=false
	// so that curves are stored exactly as authored.
	const SkPath local = autoMetrics
		? path.makeTransform(SkMatrix::Translate(-bounds.left(), -bounds.top()))
		: path
	;

	Atlas::Curves curves;

	CurveDecomposer decomposer(curves);

	const auto point = [scale](const SkPoint& p) { return Point{cv(p.x()) * scale, cv(p.y()) * scale}; };

	// Force-close open contours so the winding rule works correctly.
	SkPath::Iter iter(local, true);

	SkPoint pts[4];

	SkPath::Verb verb;

	while((verb = iter.next(pts)) != SkPath::kDone_Verb) {
		switch(verb) {
			case SkPath::kMove_Verb:
				decomposer.moveTo(point(pts[0]));

				break;

			case SkPath::kLine_Verb:
				decomposer.lineTo(point(pts[1]));

				break;

			case SkPath::kQuad_Verb:
				decomposer.quadTo(point(pts[1]), point(pts[2]));

				break;

			case SkPath::kConic_Verb: {
				const slug_t w = cv(iter.conicWeight());

				// w == 1.0: ordinary quadratic, no split needed.
				if(std::abs(w - 1_cv) < 1e-5_cv) decomposer.quadTo(point(pts[1]), point(pts[2]));

				else detail::splitConic(decomposer, point(pts[0]), point(pts[1]), point(pts[2]), w);

				break;
			}

			case SkPath::kCubic_Verb:
				decomposer.cubicTo(point(pts[1]), point(pts[2]), point(pts[3]));

				break;

			// forceClose=true means the iterator inserts an implicit lineTo back to the start
			// before emitting kClose_Verb, so we don't need to do anything here.
			case SkPath::kClose_Verb:
			case SkPath::kDone_Verb:
				break;
		}
	}

	const slug_t bx = cv(bounds.left()) * scale;
	const slug_t by = cv(bounds.top()) * scale;
	const slug_t offX = autoMetrics ? bx : 0_cv;
	const slug_t offY = autoMetrics ? by : 0_cv;

	Atlas::ShapeInfo::Origin infoOrigin = origin;

	if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
		infoOrigin.point = origin.point * scale - Point{offX, offY};
	}
	else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
		infoOrigin.point = origin.point * scale;
	}

	const Transform transform = !autoMetrics ? Transform{} :
		(origin.type == Atlas::ShapeInfo::Origin::Type::Centered)
		? Transform{ cv(bounds.centerX()) * scale, cv(bounds.centerY()) * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Pivot)
		? Transform{ origin.point.x * scale, origin.point.y * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Custom)
		? Transform{ bx + origin.point.x * scale, by + origin.point.y * scale }
		: Transform{ bx, by }
	;

	Atlas::ShapeInfo info;

	info.curves = std::move(curves);
	info.origin = infoOrigin;

	if(!autoMetrics && canvasExtent) {
		info.bearing = {0_cv, canvasExtent->second};
		info.width = canvasExtent->first;
		info.height = canvasExtent->second;
		info.autoMetrics = false;
	}

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

Transform loadShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent
) {
	// Fall back to tight-bbox if autoMetrics=false but no canvas extent is declared.
	const bool effectiveShift = autoMetrics || !canvasExtent.has_value();
	auto [info, transform] = decomposePath(path, scale, origin, effectiveShift, canvasExtent);

	if(info.curves.empty()) return {};

	if(effectiveShift) info.autoMetrics = true;

	atlas.addShape(key, info);

	return transform;
}

Transform loadStrokedShape(
	const SkPath& path,
	Atlas& atlas,
	uint32_t key,
	float strokeWidth,
	slug_t scale,
	SkPaint::Join join,
	SkPaint::Cap cap,
	Atlas::ShapeInfo::Origin origin
) {
	return loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, scale, origin);
}

}
}

#endif
