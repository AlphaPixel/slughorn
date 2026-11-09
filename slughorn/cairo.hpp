#pragma once

#include <optional>
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
//   #include <slughorn/cairo.hpp>
//
// All other translation units include it without the define.
//
// PATH CONVENTIONS
// ----------------
// cairo_copy_path() returns coordinates in user space, unaffected by the CTM. Author path
// coordinates directly and pass the appropriate scale to normalize them.
//
// decomposePath() shifts curves to local origin when autoMetrics=true (tight atlas bands) and
// returns both the curves and the offset that was subtracted as a Transform. When autoMetrics=false,
// curves are stored as-is and the returned Transform is zero. If you are compositing multiple shapes,
// store the returned Transform in Layer::transform so the renderer can restore the correct canvas
// position at draw time.
//
// STROKE LIMITATION
// -----------------
// cairo_stroke_to_path() is not part of Cairo's public stable API. For stroke-to-fill
// expansion use the Skia backend (slughorn/skia.hpp) or author stroked shapes as explicit
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
// The bounding box minimum (x1, y1) is subtracted from every curve point. Returns a ShapeInfo
// (with curves and origin pre-set) and a Transform (x/y only). Store the Transform
// in Layer::transform.
//
// The returned transform depends on @p origin:
//
// Origin::Default - transform.x/y = bbox corner (x1, y1) * scale.
// Origin::Centered - transform.x/y = bbox center * scale; computeQuad will still place the
// quad at the correct canvas position, and the transform acts as a pivot.
//
// @p scale is applied uniformly to every coordinate after the local shift. Use it to normalize path
// coordinates into the em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit
// space). Pass 1.0 if coordinates are already normalized.
// When autoMetrics=false and canvasExtent={widthEm, heightEm} is supplied, the returned ShapeInfo
// has its band metrics (bearing, width, height, autoMetrics) pre-populated. Callers that use
// decomposePath directly and then call atlas.addShape() should pass canvasExtent here.
std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	cairo_t* cr,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
);

// Decompose the current path on @p cr and register the result in @p atlas under @p key.
//
// Returns the local-origin offset as a Transform (see decomposePath). Store it in Layer::transform
// for correct composite positioning.
//
// Returns a zero Transform and does NOT call addShape if the path is empty.
//
// When autoMetrics=false, supply canvasExtent={widthEm, heightEm} to declare the band extent.
// Without it, loadShape silently falls back to tight-bbox behavior (autoMetrics=true).
Transform loadShape(
	cairo_t* cr,
	Atlas& atlas,
	Key key,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_CAIRO_IMPLEMENTATION

namespace slughorn {
namespace cairo {

std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	cairo_t* cr,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent
) {
	double x1, y1, x2, y2;

	cairo_path_extents(cr, &x1, &y1, &x2, &y2);

	const slug_t ox = cv(x1) * scale;
	const slug_t oy = cv(y1) * scale;
	const slug_t offX = autoMetrics ? ox : 0_cv;
	const slug_t offY = autoMetrics ? oy : 0_cv;

	Atlas::Curves curves;

	CurveDecomposer decomposer(curves);

	const auto point = [scale](const cairo_path_data_t& d) {
		return Point{cv(d.point.x) * scale, cv(d.point.y) * scale};
	};

	cairo_path_t* path = cairo_copy_path(cr);

	for(int i = 0; i < path->num_data; i += path->data[i].header.length) {
		const cairo_path_data_t* d = &path->data[i];

		switch(d->header.type) {
			case CAIRO_PATH_MOVE_TO:
				decomposer.moveTo(point(d[1]));

				break;

			case CAIRO_PATH_LINE_TO:
				decomposer.lineTo(point(d[1]));

				break;

			case CAIRO_PATH_CURVE_TO:
				decomposer.cubicTo(point(d[1]), point(d[2]), point(d[3]));

				break;

			case CAIRO_PATH_CLOSE_PATH:
				decomposer.close();

				break;
		}
	}

	cairo_path_destroy(path);

	if(autoMetrics) {
		for(auto& c : curves) {
			c.x1 -= ox; c.x2 -= ox; c.x3 -= ox;
			c.y1 -= oy; c.y2 -= oy; c.y3 -= oy;
		}
	}

	Atlas::ShapeInfo::Origin infoOrigin = origin;

	if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
		infoOrigin.point = origin.point * scale - Point{offX, offY};
	}

	else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
		infoOrigin.point = origin.point * scale;
	}

	const Transform transform = !autoMetrics ? Transform{} :
		(origin.type == Atlas::ShapeInfo::Origin::Type::Centered)
		? Transform{ cv(x1 + x2) * 0.5_cv * scale, cv(y1 + y2) * 0.5_cv * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Pivot)
		? Transform{ origin.point.x * scale, origin.point.y * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Custom)
		? Transform{ ox + origin.point.x * scale, oy + origin.point.y * scale }
		: Transform{ ox, oy }
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

Transform loadShape(cairo_t* cr, Atlas& atlas, Key key, slug_t scale, Atlas::ShapeInfo::Origin origin, bool autoMetrics, std::optional<std::pair<slug_t, slug_t>> canvasExtent) {
	// Fall back to tight-bbox if autoMetrics=false but no canvas extent is declared.
	const bool effectiveShift = autoMetrics || !canvasExtent.has_value();
	auto [info, transform] = decomposePath(cr, scale, origin, effectiveShift, canvasExtent);

	if(info.curves.empty()) return {};

	if(effectiveShift) info.autoMetrics = true;

	atlas.addShape(key, info);

	return transform;
}

}
}

#endif
