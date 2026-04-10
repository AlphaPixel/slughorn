#pragma once

// ================================================================================================
// NanoSVG backend for slughorn
//
// Parses SVG files/strings into slughorn `Atlas` shapes, producing a `CompositeShape` with one
// `Layer` per filled SVG shape, back-to-front order preserved.
//
// Local coords are used by default; every shape is decomposed in its own tight bounding box, so
// atlas bands are sized to the geometry rather than the full SVG canvas. `Layer::transform` carries
// the canvas-space offset back to the caller (TODO: NOT IDEAL, investigate...)
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_NANOSVG_IMPLEMENTATION
//   #include "slughorn-nanosvg.hpp"
//
// All other translation units include it without the define.
//
// NanoSVG must be available as "nanosvg.h", and "nanosvgrast.h" is not used.
//
// WHAT IS SUPPORTED
// -----------------
//   - Filled shapes (solid color only)
//   - All path segment types: lines, cubics (split to quadratics), and NanoSVG's pre-flattened
//     cubic chains
//   - Fill color unpacked from NanoSVG's packed ABGR uint32
//   - Per-shape local coordinate decomposition (tight bands, zero offset waste)
//   - Auto-scale from SVG viewBox width (pass scale=0 to use this)
//
// WHAT IS NOT (YET) SUPPORTED
// ---------------------------
//   - Stroked shapes (stroke-to-fill expansion not yet wired up)
//   - Gradients (first stop color used as flat approximation, like COLRv1)
//   - Clip paths, masks, opacity, transforms on groups
//   - Text elements
//   - ZWJ / multi-codepoint keys (same limitation as emoji support)
//
// NanoSVG parses all paths as cubic Bezier chains. Each cubic is split at its midpoint into two
// quadratics via `CurveDecomposer::cubicTo`, the same approximation used by slughorn-skia.hpp and
// slughorn-cairo.hpp.
// ================================================================================================

#include "slughorn.hpp"

// NanoSVG is a single-header library. The implementation is compiled in exactly one translation
// unit via SLUGHORN_NANOSVG_IMPLEMENTATION (see below).
#ifdef SLUGHORN_NANOSVG_IMPLEMENTATION
#   define NANOSVG_IMPLEMENTATION
#endif

#include "nanosvg.h"

#include <string>

namespace slughorn {
namespace nanosvg {

// ================================================================================================
// colorFromNSVG
//
// NanoSVG packs fill/stroke color as 0xAABBGGRR (little-endian ABGR).
//
// Note: NanoSVG stores colors in sRGB. For correct compositing you should convert to linear; for
// now we pass through as-is (same pragmatic choice made by the FT2 and Cairo backends).
// ================================================================================================

inline Color colorFromNSVG(unsigned int packed) {
	return {
		cv((packed & 0xFF)) / 255.0_cv, // R
		cv(((packed >> 8) & 0xFF)) / 255.0_cv, // G
		cv(((packed >> 16) & 0xFF)) / 255.0_cv, // B
		cv(((packed >> 24) & 0xFF)) / 255.0_cv, // A
	};
}

// ================================================================================================
// Decomposition
// ================================================================================================

// Decompose a single `NSVGshape` path into slughorn curves, appending to @p curves. @p scale is
// applied to every coordinate.
void decomposeShape(const NSVGshape* shape, Atlas::Curves& curves, slug_t scale=1.0_cv);

// Decompose a single `NSVGshape` in local coordinate space (tight bounding box origin), appending
// to @p curves. The canvas-space translation is written to @p outTransform (TODO: Investigate...)
//
// Returns false if the shape produces no curves or has a zero bounding box.
bool decomposeShapeLocal(
	const NSVGshape* shape,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t scale=1.0_cv
);

// ================================================================================================
// Atlas Integration
// ================================================================================================

// Decompose @p shape in local coords and register it in @p atlas under @p key. The canvas-space
// offset is written to @p outTransform (TODO: Investigate...)
//
// Returns true if at least one curve was produced and the shape was added.
bool loadShape(
	const NSVGshape* shape,
	Atlas& atlas,
	uint32_t key,
	Matrix& outTransform,
	slug_t scale=1.0_cv
);

// Parse an entire NSVGimage into a CompositeShape - one `Layer` per filled `NSVGshape`,
// back-to-front order preserved.
//
// @p scale normalises SVG canvas coordinates into slughorn's [0, 1] em-space. Pass scale = 0.0 to
// auto-compute from the SVG's viewBox width (equivalent to 1.0 / image->width).
//
// Keys are allocated sequentially from @p baseKey. On return, @p baseKey is advanced past the last
// key used; pass a fresh namespace each call (e.g. 0xD0000).
//
// Shapes with no fill (fillRule == NSVG_FILLRULE_NONZERO but color alpha==0, or paint type !=
// solid) are skipped with a warning to stderr.
CompositeShape loadImage(
	const NSVGimage* image,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale=0.0_cv
);

// Convenience: parse an SVG file and load it in one call.  @p dpi controls NanoSVG's unit
// conversion (96.0 is a sensible default).  Returns an empty CompositeShape on parse failure.
CompositeShape loadFile(
	const std::string& path,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale=0.0_cv,
	float dpi=96.0f
);

// Convenience helper to parse an SVG string and load it in one call. The string is copied
// internally (nsvgParse requires a mutable buffer).
CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale=0.0_cv,
	float dpi=96.0f
);

}
}

// ================================================================================================
// IMPLEMENTATION
// ================================================================================================
#ifdef SLUGHORN_NANOSVG_IMPLEMENTATION

#include <cstring>
#include <iostream>

namespace slughorn {
namespace nanosvg {

void decomposeShape(const NSVGshape* shape, Atlas::Curves& curves, slug_t scale) {
	CurveDecomposer decomposer(curves);

	for(const NSVGpath* path = shape->paths; path; path = path->next) {
		if(path->npts < 4) continue;

		// NanoSVG stores paths as a flat array of cubic Bezier control points:
		// [x0,y0, cx0,cy0, cx1,cy1, x1,y1,  cx0,cy0, ...] repeating. Each cubic uses 4 points
		// (8 floats), sharing the end point with the next segment's start.

		const float* p = path->pts;

		decomposer.moveTo(cv(p[0]) * scale, cv(p[1]) * scale);

		for(int i = 0; i < path->npts - 1; i += 3) {
			p = path->pts + i * 2;

			decomposer.cubicTo(
				cv(p[2]) * scale, cv(p[3]) * scale, // control 1
				cv(p[4]) * scale, cv(p[5]) * scale, // control 2
				cv(p[6]) * scale, cv(p[7]) * scale  // end point
			);
		}

		if(path->closed) {
			// Close back to the path start with a line if needed.  CurveDecomposer::lineTo will
			// emit a degenerate quadratic.
			const float* start = path->pts;

			decomposer.lineTo(cv(start[0]) * scale, cv(start[1]) * scale);
		}
	}
}

bool decomposeShapeLocal(
	const NSVGshape* shape,
	Atlas::Curves& curves,
	Matrix& outTransform,
	slug_t scale
) {
	// NanoSVG pre-computes the bounding box for each shape.
	const float minX = shape->bounds[0];
	const float minY = shape->bounds[1];
	const float maxX = shape->bounds[2];
	const float maxY = shape->bounds[3];

	if(maxX <= minX || maxY <= minY) return false;

	// Decompose with a local-origin translation baked into scale application. We can't easily
	// pre-translate an NSVGshape in place, so instead we subtract the bounding box origin from
	// every point during decomposition.

	CurveDecomposer decomposer(curves);

	const size_t curvesBefore = curves.size();

	for(const NSVGpath* path = shape->paths; path; path = path->next) {
		if(path->npts < 4) continue;

		const float* p = path->pts;

		decomposer.moveTo(cv(p[0] - minX) * scale, cv(p[1] - minY) * scale);

		for(int i = 0; i < path->npts - 1; i += 3) {
			p = path->pts + i * 2;

			decomposer.cubicTo(
				cv(p[2] - minX) * scale, cv(p[3] - minY) * scale,
				cv(p[4] - minX) * scale, cv(p[5] - minY) * scale,
				cv(p[6] - minX) * scale, cv(p[7] - minY) * scale
			);
		}

		if(path->closed) {
			const float* start = path->pts;

			decomposer.lineTo(cv(start[0] - minX) * scale, cv(start[1] - minY) * scale);
		}
	}

	if(curves.size() == curvesBefore) return false;

	// Return canvas-space offset in scaled coordinates.
	outTransform = Matrix::identity();
	outTransform.dx = cv(minX) * scale;
	outTransform.dy = cv(minY) * scale;

	return true;
}

bool loadShape(
	const NSVGshape* shape,
	Atlas& atlas,
	uint32_t key,
	Matrix& outTransform,
	slug_t scale
) {
	Atlas::ShapeInfo info;

	info.autoMetrics = true;

	if(!decomposeShapeLocal(shape, info.curves, outTransform, scale)) return false;

	atlas.addShape(key, info);

	return true;
}

CompositeShape loadImage(
	const NSVGimage* image,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale
) {
	CompositeShape composite;

	if(!image) return composite;

	// Auto-scale from viewBox width if scale not provided.
	if(scale <= 0.0_cv) {
		if(image->width > 0.0f) scale = 1.0_cv / cv(image->width);

		else {
			std::cerr
				<< "slughorn::nanosvg::loadImage: image width is zero, "
				<< "cannot auto-compute scale. Pass scale explicitly." << std::endl
			;

			return composite;
		}
	}

	composite.advance = cv(image->width) * scale; // normalised width = 1.0

	for(const NSVGshape* shape = image->shapes; shape; shape = shape->next) {
		// Skip invisible shapes.
		if(!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

		// Only solid fills supported for now.
		if(shape->fill.type != NSVG_PAINT_COLOR) {
			std::cerr
				<< "slughorn::nanosvg: skipping shape with non-solid fill "
				<< "(gradient/pattern support not yet implemented)" << std::endl
			;

			continue;
		}

		const Color color = colorFromNSVG(shape->fill.color);

		// Skip fully transparent shapes.
		if(color.a < 1e-4_cv) continue;

		Matrix xform;

		if(!loadShape(shape, atlas, baseKey, xform, scale)) continue;

		Layer layer;

		layer.key = baseKey++;
		layer.color = color;
		layer.transform = xform;

		composite.layers.push_back(layer);
	}

	return composite;
}

CompositeShape loadFile(
	const std::string& path,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale,
	float dpi
) {
	// nsvgParseFromFile requires a mutable char* path.
	NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", dpi);

	if(!image) {
		std::cerr
			<< "slughorn::nanosvg::loadFile: failed to parse '"
			<< path << "'" << std::endl
		;

		return {};
	}

	CompositeShape result = loadImage(image, atlas, baseKey, scale);

	nsvgDelete(image);

	return result;
}

CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	uint32_t& baseKey,
	slug_t scale,
	float dpi
) {
	// nsvgParse requires a mutable buffer, so we copy the string.
	std::string buf = svg;

	NSVGimage* image = nsvgParse(buf.data(), "px", dpi);

	if(!image) {
		std::cerr << "slughorn::nanosvg::loadString: failed to parse SVG" << std::endl;

		return {};
	}

	CompositeShape result = loadImage(image, atlas, baseKey, scale);

	nsvgDelete(image);

	return result;
}

}
}

#endif
