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
// PATH CONVENTIONS
// ----------------
// Cairo is Y-down by default. slughorn expects Y-up coordinates (the same convention as FreeType
// and em-space in general). The caller is responsible for applying a flip transform to the cairo_t
// before building paths:
//
//   cairo_translate(cr, 0.0, emSize); // shift origin to bottom-left
//   cairo_scale(cr, 1.0, -1.0); // flip Y
//
// This bakes the flip into the path coordinates returned by cairo_copy_path(), so decomposePath()
// receives Y-up data without needing to know about it.
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
// The path is copied and then destroyed internally - the path on @p cr is left unchanged.
//
// @p scale is applied uniformly to every coordinate. Use it to normalize path coordinates into the
// [0, 1] em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit space). Pass
// 1.0 if coordinates are already normalized.
void decomposePath(cairo_t* cr, Atlas::Curves& curves, slug_t scale = 1.0_cv);

// Decompose the current path on @p cr into slughorn curves in local coordinate space.
//
// The tight bounding box of the path is computed via cairo_path_extents(). The path is then
// translated so its origin sits at (0, 0) before decomposition, producing tight atlas bands with
// no wasted offset space.
//
// The canvas-space translation that was subtracted is returned in @p outTransform (dx/dy only;
// xx/yy remain identity). The caller should store this in Layer::transform so that
// ShapeDrawable::compile() can restore the correct canvas position at draw time.
//
// @p canvasHeight is required to correctly convert the Y-down extents returned by
// cairo_path_extents() into the Y-up offset that slughorn and ShapeDrawable expect. Pass the
// same height value used in the cairo_translate(cr, 0, height) + cairo_scale(cr, 1, -1) flip
// applied to the cairo_t before building paths.
//
// @p scale is applied after the local translation, consistent with decomposePath().
void decomposePathLocal(
	cairo_t* cr,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale = 1.0_cv
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
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty.
bool loadShape(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	slug_t scale = 1.0_cv,
	bool autoMetrics = true
);

// Decompose the current path on @p cr in local coordinate space and register the result in
// @p atlas under @p key.
//
// The canvas-space offset is returned in @p outTransform — store it in Layer::transform so
// ShapeDrawable::compile() can restore the correct position at draw time.
//
// Returns true if at least one curve was produced and the shape was added. Returns false (and does
// NOT call addShape) if the path is empty.
bool loadShapeLocal(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	Matrix& outTransform,
	slug_t canvasHeight,
	slug_t scale = 1.0_cv,
	bool autoMetrics = true
);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_CAIRO_IMPLEMENTATION

namespace slughorn {
namespace cairo {

void decomposePath(cairo_t* cr, Atlas::Curves& curves, slug_t scale) {
	CurveDecomposer decomposer(curves);

	cairo_path_t* path = cairo_copy_path(cr);

	for(int i = 0; i < path->num_data; i += path->data[i].header.length) {
		const cairo_path_data_t* d = &path->data[i];

		switch(d->header.type) {
			case CAIRO_PATH_MOVE_TO:
				decomposer.moveTo(cv(d[1].point.x) * scale, cv(d[1].point.y) * scale);

				break;

			case CAIRO_PATH_LINE_TO:
				decomposer.lineTo(cv(d[1].point.x) * scale, cv(d[1].point.y) * scale);

				break;

			case CAIRO_PATH_CURVE_TO:
				decomposer.cubicTo(
					cv(d[1].point.x) * scale, cv(d[1].point.y) * scale,
					cv(d[2].point.x) * scale, cv(d[2].point.y) * scale,
					cv(d[3].point.x) * scale, cv(d[3].point.y) * scale
				);

				break;

			// The implicit close line (back to the subpath start) has already been emitted as a
			// CAIRO_PATH_LINE_TO by Cairo before this token, so no geometric action is needed here.
			case CAIRO_PATH_CLOSE_PATH: break;
		}
	}

	cairo_path_destroy(path);
}

#if 0
void decomposePathLocal(cairo_t* cr, Atlas::Curves& curves, Matrix& outTransform, slug_t canvasHeight, slug_t scale) {
	outTransform = Matrix::identity();

	double x1, y1, x2, y2;

	cairo_path_extents(cr, &x1, &y1, &x2, &y2);

	// cairo_path_extents() returns coordinates in Y-down user space (even with the flip CTM
	// active). Translate by the Y-down top-left corner so Cairo's CTM handles the rest correctly
	// during decomposition.
	cairo_save(cr);
	cairo_translate(cr, -x1, -y1);
	// cairo_translate(cr, -x1, -(canvasHeight - cv(y2)));

	decomposePath(cr, curves, scale);

	cairo_restore(cr);

	// Convert the Y-down extents to a Y-up canvas offset for ShapeDrawable::compile().
	// In Y-up space the bottom of the shape sits at (canvasHeight - y2).
	outTransform.dx = cv(x1) * scale;
	outTransform.dy = (canvasHeight - cv(y2)) * scale;
}
#endif

void decomposePathLocal(cairo_t* cr, Atlas::Curves& curves, Matrix& outTransform, slug_t canvasHeight, slug_t scale) {
	outTransform = Matrix::identity();

	double x1, y1, x2, y2;

	cairo_path_extents(cr, &x1, &y1, &x2, &y2);

	// Y-up bottom-left of the shape in normalized space.
	const slug_t ox = cv(x1)                  * scale;
	const slug_t oy = (canvasHeight - cv(y2)) * scale;

	// Decompose as normal — cairo_copy_path() returns already-transformed
	// coordinates so CTM manipulation after the fact has no effect.
	const size_t priorCount = curves.size();

	decomposePath(cr, curves, scale);

	// Shift all newly added curves to local origin.
	for(auto it = curves.begin() + priorCount; it != curves.end(); ++it) {
		it->x1 -= ox; it->x2 -= ox; it->x3 -= ox;
		it->y1 -= oy; it->y2 -= oy; it->y3 -= oy;
	}

	outTransform.dx = ox;
	outTransform.dy = oy;
}

bool loadShape(cairo_t* cr, Atlas& atlas, Key key, slug_t scale, bool autoMetrics) {
	Atlas::ShapeInfo info;
	info.autoMetrics = autoMetrics;

	decomposePath(cr, info.curves, scale);

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
