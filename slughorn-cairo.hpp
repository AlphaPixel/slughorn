#pragma once

#include <utility>

// ================================================================================================
// Decomposes Cairo path data into slughorn Atlas shapes. No OSG, VSG, or other graphics library
// dependency.
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
// PATH CONVENTIONS
// ----------------
// cairo_copy_path() returns coordinates in user space, unaffected by the CTM. Author path
// coordinates directly and pass the appropriate scale to normalize them.
//
// decomposePath() always shifts curves to local origin (tight atlas bands) and returns both
// the curves and the offset that was subtracted as a Matrix. If you don't need positional
// information, ignore the returned Matrix. If you are compositing multiple shapes, store the
// returned Matrix in Layer::transform so the renderer can restore the correct canvas position
// at draw time.
//
// STROKE LIMITATION
// -----------------
// cairo_stroke_to_path() is not part of Cairo's public stable API. For stroke-to-fill
// expansion use the Skia backend (slughorn-skia.hpp) or author stroked shapes as explicit
// filled paths.
//
// CUBIC CURVES
// ------------
// Cairo's native curve primitive is the cubic Bezier (CAIRO_PATH_CURVE_TO).
// CurveDecomposer::cubicTo splits each cubic at its midpoint into two quadratics.
// ================================================================================================

#include "slughorn.hpp"
#include <cairo/cairo.h>

namespace slughorn {
namespace cairo {

// Decompose the current path on @p cr into slughorn curves, shifted to local origin for
// tight atlas bands.
//
// The bounding box minimum (x1, y1) is subtracted from every curve point. Returns both the shifted
// curves and the subtracted offset as a Matrix (dx/dy only; xx/yy are identity). Store the Matrix
// in Layer::transform if you need to restore the original canvas position at draw time.
//
// @p scale is applied uniformly to every coordinate after the local shift. Use it to normalize path
// coordinates into the em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit
// space). Pass 1.0 if coordinates are already normalized.
std::pair<Atlas::Curves, Matrix> decomposePath(cairo_t* cr, slug_t scale=1.0_cv);

// Decompose the current path on @p cr and register the result in @p atlas under @p key.
//
// Returns the local-origin offset as a Matrix (see decomposePath). Store it in Layer::transform for
// correct composite positioning.
//
// Returns an identity Matrix and does NOT call addShape if the path is empty.
Matrix loadShape(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
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

std::pair<Atlas::Curves, Matrix> decomposePath(cairo_t* cr, slug_t scale) {
	double x1, y1, x2, y2;

	cairo_path_extents(cr, &x1, &y1, &x2, &y2);

	const slug_t ox = cv(x1) * scale;
	const slug_t oy = cv(y1) * scale;

	Atlas::Curves curves;

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

			case CAIRO_PATH_CLOSE_PATH:
				decomposer.close();

				break;
		}
	}

	cairo_path_destroy(path);

	// Shift all curves to local origin.
	for(auto& c : curves) {
		c.x1 -= ox; c.x2 -= ox; c.x3 -= ox;
		c.y1 -= oy; c.y2 -= oy; c.y3 -= oy;
	}

	Matrix transform = Matrix::identity();

	transform.dx = ox;
	transform.dy = oy;

	return { curves, transform };
}

Matrix loadShape(cairo_t* cr, Atlas& atlas, Key key, slug_t scale, bool autoMetrics) {
	auto [curves, transform] = decomposePath(cr, scale);

	if(curves.empty()) return Matrix::identity();

	Atlas::ShapeInfo info;

	info.autoMetrics = autoMetrics;
	info.curves = std::move(curves);

	atlas.addShape(key, info);

	return transform;
}

}
}

#endif
