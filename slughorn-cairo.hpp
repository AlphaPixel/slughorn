#pragma once

// ================================================================================================
// Decomposes Cairo path data into slughorn Atlas shapes. No OSG, VSG, or other graphics library
// dependency. NOTE: Cairo is the EASIEST "backend" usable by slughorn, but lacks a public-facing
// `stroke_to_path` function (it has been stubbed out as "NYI" FOREVER...)
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_CAIRO_IMPLEMENTATION
//   #include "slughorn-cairo.hpp"
//
// All other translation units include it without the define.
//
// Cairo must be on your include path. Link against cairo.
//
// Y-UP CONVENTION
// ---------------
// slughorn uses Y-up coordinates throughout. Cairo uses Y-down. All decompose/load functions in
// this header convert automatically: pass canvasHeight (in the same pre-scale units as the path
// coordinates) and the flip is applied for you. There is no need to apply a Y-flip CTM to the
// cairo_t before building paths.
//
// For the raw decomposePath() overload, canvasHeight defaults to 0.0 which means "pass through
// unchanged" — use this only when your path coordinates are already Y-up. Prefer passing an
// explicit canvasHeight.
//
// STROKE LIMITATION
// -----------------
// cairo_stroke_to_path() is not part of Cairo's public stable API (it exists internally but is not
// exported). For stroke-to-fill expansion use the Skia backend (slughorn-skia.hpp) or author
// stroked shapes as explicit filled paths at construction time.
//
// CUBIC CURVES
// ------------
// Cairo's native curve primitive is the cubic Bezier (CAIRO_PATH_CURVE_TO). There are no conics or
// rational quadratics to handle. CurveDecomposer::cubicTo splits each cubic at its midpoint into
// two quadratics - sufficient for smooth results and consistent with the FreeType and Skia
// backends.
// ================================================================================================

#include "slughorn.hpp"
#include <cairo/cairo.h>

namespace slughorn {
namespace cairo {

// ================================================================================================
// Core decomposition
// ================================================================================================

// Decompose the current path on @p cr into slughorn curves and append them to @p curves.
//
// Uses cairo_copy_path() (cubic-preserving) rather than cairo_copy_path_flat() (which would
// pre-subdivide arcs into line segments and inflate curve counts).
//
// The path is copied and then destroyed internally — the path on @p cr is left unchanged.
//
// @p scale is applied uniformly to every coordinate. Use it to normalize path coordinates into the
// [0, 1] em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit space). Pass
// 1.0 if coordinates are already normalized.
//
// @p canvasHeight is the height of the coordinate space in pre-scale source units. When nonzero,
// every Y coordinate is flipped (y_up = canvasHeight - y_down) to convert from Cairo's Y-down
// convention to slughorn's Y-up convention. Pass 0.0 (default) only if the path is already Y-up.
void decomposePath(
	cairo_t* cr,
	Atlas::Curves& curves,
	slug_t scale=1.0_cv,
	slug_t canvasHeight=0.0_cv
);

// Decompose the current path on @p cr into slughorn curves in local coordinate space.
//
// The tight bounding box of the path is computed via cairo_path_extents(). The path is then
// translated so its Y-up bottom-left corner sits at (0, 0) before decomposition, producing tight
// atlas bands with no wasted offset space.
//
// The canvas-space translation that was subtracted is returned in @p outTransform (dx/dy only;
// xx/yy remain identity). The caller should store this in Layer::transform so that
// ShapeDrawable::compile() can restore the correct canvas position at draw time.
// outTransform.dy is in Y-up em-space (i.e. the Y-up bottom of the shape in canvas coordinates).
//
// @p canvasHeight is the height of the coordinate space in pre-scale source units, required to
// convert the Y-down extents from cairo_path_extents() into a Y-up canvas offset for
// outTransform.dy.
//
// @p scale is applied after the local translation, consistent with decomposePath().
void decomposePathLocal(
	cairo_t* cr,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale=1.0_cv
);

// ================================================================================================
// Atlas integration
// ================================================================================================

// Decompose the current path on @p cr and register the result in @p atlas under @p key.
//
// If autoMetrics is true (default) slughorn derives width/height/bearing/advance from the curve
// bounding box. Set it to false and populate the ShapeInfo fields manually if you need precise
// control.
//
// @p canvasHeight is forwarded to decomposePath() for Y-down -> Y-up conversion; see that
// function's documentation. Pass 0.0 only for already-Y-up paths.
//
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty.
bool loadShape(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	slug_t canvasHeight,
	slug_t scale=1.0_cv,
	bool autoMetrics=true
);

// Decompose the current path on @p cr in local coordinate space and register the result in
// @p atlas under @p key.
//
// The canvas-space offset is returned in @p outTransform — store it in Layer::transform so
// ShapeDrawable::compile() can restore the correct position at draw time.
//
// @p canvasHeight is forwarded to decomposePathLocal() for Y-down -> Y-up conversion.
//
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty.
bool loadShapeLocal(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale=1.0_cv,
	bool autoMetrics=true
);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================

#ifdef SLUGHORN_CAIRO_IMPLEMENTATION

namespace slughorn {
namespace cairo {

void decomposePath(cairo_t* cr, Atlas::Curves& curves, slug_t scale, slug_t canvasHeight) {
	CurveDecomposer decomposer(curves);

	// Flip helper: when canvasHeight > 0, converts Y-down to Y-up.
	const bool doFlip = (canvasHeight > 0.0_cv);

	auto flipY = [&](slug_t y) -> slug_t { return doFlip ? (canvasHeight - y) : y; };

	cairo_path_t* path = cairo_copy_path(cr);

	for(int i = 0; i < path->num_data; i += path->data[i].header.length) {
		const cairo_path_data_t* d = &path->data[i];

		switch(d->header.type) {
		case CAIRO_PATH_MOVE_TO:
			decomposer.moveTo(cv(d[1].point.x) * scale, flipY(cv(d[1].point.y)) * scale);

			break;

		case CAIRO_PATH_LINE_TO:
			decomposer.lineTo(cv(d[1].point.x) * scale, flipY(cv(d[1].point.y)) * scale);

			break;

		case CAIRO_PATH_CURVE_TO:
			decomposer.cubicTo(
				cv(d[1].point.x) * scale, flipY(cv(d[1].point.y)) * scale,
				cv(d[2].point.x) * scale, flipY(cv(d[2].point.y)) * scale,
				cv(d[3].point.x) * scale, flipY(cv(d[3].point.y)) * scale
			);

			break;

		// The implicit close line (back to the subpath start) has already been emitted as a
		// CAIRO_PATH_LINE_TO by Cairo before this token, so no geometric action is needed here.
		case CAIRO_PATH_CLOSE_PATH:
			break;
		}
	}

	cairo_path_destroy(path);
}

void decomposePathLocal(
	cairo_t* cr,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale
) {
	outTransform = Matrix::identity();

	double x1, y1, x2, y2;

	cairo_path_extents(cr, &x1, &y1, &x2, &y2);

	// cairo_path_extents() returns Y-down coordinates. The Y-up bottom of the shape is:
	// y_up_bottom = canvasHeight - y2 (y2 is the Y-down bottom = largest Y-down value)
	const slug_t ox = cv(x1) * scale;
	const slug_t oy = (canvasHeight - cv(y2)) * scale;

	// Decompose as normal with the full canvasHeight for the Y-flip, then shift all newly added
	// curves to local origin.
	const size_t priorCount = curves.size();

	decomposePath(cr, curves, scale, canvasHeight);

	for(auto it = curves.begin() + priorCount; it != curves.end(); ++it) {
		it->x1 -= ox; it->x2 -= ox; it->x3 -= ox;
		it->y1 -= oy; it->y2 -= oy; it->y3 -= oy;
	}

	outTransform.dx = ox;
	outTransform.dy = oy;
}

bool loadShape(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	slug_t canvasHeight,
	slug_t scale,
	bool autoMetrics
) {
	Atlas::ShapeInfo info;

	info.autoMetrics = autoMetrics;

	decomposePath(cr, info.curves, scale, canvasHeight);

	if(info.curves.empty()) return false;

	atlas.addShape(key, info);

	return true;
}

bool loadShapeLocal(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale,
	bool autoMetrics
) {
	Atlas::ShapeInfo info;

	info.autoMetrics = autoMetrics;

	decomposePathLocal(cr, info.curves, outTransform, canvasHeight, scale);

	if(info.curves.empty()) return false;

	atlas.addShape(key, info);

	return true;
}

}
}

#endif
