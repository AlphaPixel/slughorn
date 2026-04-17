#pragma once

// ================================================================================================
// NanoSVG backend for slughorn
//
// Parses SVG files/strings into slughorn Atlas shapes, producing a CompositeShape with one
// Layer per filled SVG shape, back-to-front order preserved.
//
// API mirrors slughorn-cairo.hpp exactly:
//
// decomposePath() - low-level: NSVGshape -> (curves, transform)
// loadShape() - mid-level: decompose + register in atlas
// loadImage() - high-level: full NSVGimage -> CompositeShape
// loadFile() - convenience: parse file + loadImage
// loadString() - convenience: parse string + loadImage
//
// Scale is always auto-computed from image->width (1/image->width), normalizing
// all coordinates to [0,1] em-space. It is never exposed as a caller parameter.
// World sizing is the caller's responsibility via MatrixTransform or equivalent.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
// #define SLUGHORN_NANOSVG_IMPLEMENTATION
// #include "slughorn-nanosvg.hpp"
//
// All other translation units include it without the define.
//
// WHAT IS SUPPORTED
// -----------------
// - Filled shapes (solid color only)
// - All path segment types: lines, cubics (split to quadratics), and NanoSVG's
//   pre-flattened cubic chains
// - Fill color unpacked from NanoSVG's packed ABGR uint32
// - Per-shape local coordinate decomposition (tight bands, zero offset waste)
// - Auto-scale from SVG viewBox width (always 1/image->width)
//
// WHAT IS NOT (YET) SUPPORTED
// ---------------------------
// - Stroked shapes (stroke-to-fill expansion not yet wired up)
// - Gradients (first stop color used as flat approximation)
// - Clip paths, masks, opacity, transforms on groups
// - Text elements
// ================================================================================================

#include "slughorn.hpp"

#ifdef SLUGHORN_NANOSVG_IMPLEMENTATION
#   define NANOSVG_IMPLEMENTATION
#endif

_Pragma("GCC diagnostic push")
_Pragma("GCC diagnostic ignored \"-Wsign-conversion\"")
_Pragma("GCC diagnostic ignored \"-Wshadow\"")

#include "nanosvg.h"

_Pragma("GCC diagnostic pop")

#include <string>
#include <utility>

namespace slughorn {
namespace nanosvg {

// ================================================================================================
// colorFromNSVG
//
// NanoSVG packs fill/stroke color as 0xAABBGGRR (little-endian ABGR).
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
// decomposePath
//
// Decompose a single NSVGshape into slughorn curves, shifted to local origin for tight atlas bands.
// Mirrors slughorn::cairo::decomposePath exactly.
//
// @p scale normalizes SVG canvas coordinates into em-space. For direct use, pass 1/image->width.
// loadImage/loadFile/loadString handle this automatically.
//
// Returns the shifted curves and the canvas-space offset as a Matrix (dx/dy only; xx/yy are
// identity). Store the Matrix in Layer::transform for correct composite positioning. Returns an
// empty curves vector if the shape has no paths or a zero bounding box.
// ================================================================================================
std::pair<Atlas::Curves, Matrix> decomposePath( const NSVGshape* shape, slug_t scale=1.0_cv);

// ================================================================================================
// loadShape
//
// Decompose @p shape and register it in @p atlas under @p key. Mirrors slughorn::cairo::loadShape
// exactly.
//
// Returns the canvas-space offset as a Matrix (see decomposePath). Store it in Layer::transform for
// correct composite positioning. Returns an identity Matrix and does NOT call addShape if the path
// produces no curves.
// ================================================================================================
Matrix loadShape(
	const NSVGshape* shape,
	Atlas& atlas,
	Key key,
	slug_t scale=1.0_cv
);

// ================================================================================================
// loadImage
//
// Parse an entire NSVGimage into a CompositeShape - one Layer per filled
// NSVGshape, back-to-front order preserved.
//
// Scale is always auto-computed as 1/image->width, normalizing the SVG canvas
// to [0,1] em-space. Keys are allocated sequentially from @p baseKey; on
// return baseKey is advanced past the last key used.
//
// Shapes with no solid fill are skipped with a warning to stderr.
// ================================================================================================
CompositeShape loadImage(
	const NSVGimage* image,
	Atlas& atlas,
	uint32_t& baseKey
);

// ================================================================================================
// loadFile / loadString - convenience wrappers
// ================================================================================================
CompositeShape loadFile(
	const std::string& path,
	Atlas& atlas,
	uint32_t& baseKey,
	float dpi=96.0f
);

CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	uint32_t& baseKey,
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

std::pair<Atlas::Curves, Matrix> decomposePath(const NSVGshape* shape, slug_t scale) {
	// NanoSVG pre-computes the bounding box for each shape.
	const slug_t minX = cv(shape->bounds[0]) * scale;
	const slug_t minY = cv(shape->bounds[1]) * scale;
	const slug_t maxX = cv(shape->bounds[2]) * scale;
	const slug_t maxY = cv(shape->bounds[3]) * scale;

	if(maxX <= minX || maxY <= minY) return { {}, Matrix::identity() };

	Atlas::Curves curves;

	CurveDecomposer decomposer(curves);

	for(const NSVGpath* path = shape->paths; path; path = path->next) {
		if(path->npts < 4) continue;

		const float* p = path->pts;

		decomposer.moveTo(cv(p[0]) * scale - minX, cv(p[1]) * scale - minY);

		for(int i = 0; i < path->npts - 1; i += 3) {
			p = path->pts + i * 2;

			decomposer.cubicTo(
				cv(p[2]) * scale - minX, cv(p[3]) * scale - minY,
				cv(p[4]) * scale - minX, cv(p[5]) * scale - minY,
				cv(p[6]) * scale - minX, cv(p[7]) * scale - minY
			);
		}

		if(path->closed) decomposer.close();
	}

	if(curves.empty()) return { {}, Matrix::identity() };

	Matrix transform = Matrix::identity();

	transform.dx = minX;
	transform.dy = minY;

	return { curves, transform };
}

Matrix loadShape(const NSVGshape* shape, Atlas& atlas, Key key, slug_t scale) {
	auto [curves, transform] = decomposePath(shape, scale);

	if(curves.empty()) return Matrix::identity();

	Atlas::ShapeInfo info;

	// info.autoMetrics = true;
	info.curves = std::move(curves);

	atlas.addShape(key, info);

	return transform;
}

CompositeShape loadImage(const NSVGimage* image, Atlas& atlas, uint32_t& baseKey) {
	CompositeShape composite;

	if(!image) return composite;

	if(image->width <= 0.0f) {
		std::cerr
			<< "slughorn::nanosvg::loadImage: image width is zero, cannot normalize."
			<< std::endl
		;

		return composite;
	}

	const slug_t scale = 1.0_cv / cv(image->width);

	// Normalized width is always 1.0...
	composite.advance = 1.0_cv;

	for(const NSVGshape* shape = image->shapes; shape; shape = shape->next) {
		if(!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

		if(shape->fill.type != NSVG_PAINT_COLOR) {
			std::cerr << "slughorn::nanosvg: skipping shape with non-solid fill." << std::endl;

			continue;
		}

		const Color color = colorFromNSVG(shape->fill.color);

		if(color.a < 1e-4_cv) continue;

		const uint32_t key = baseKey++;

		Matrix transform = loadShape(shape, atlas, key, scale);

		Layer layer;

		layer.key = key;
		layer.color = color;
		layer.transform = transform;
		// layer.scale is intentionally not set - world sizing is the caller's
		// responsibility. layer.scale remains at its default of 1.0.

		composite.layers.push_back(layer);
	}

	return composite;
}

CompositeShape loadFile(
	const std::string& path,
	Atlas& atlas,
	uint32_t& baseKey,
	float dpi
) {
	NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", dpi);

	if(!image) {
		std::cerr
			<< "slughorn::nanosvg::loadFile: failed to parse '" << path << "'"
			<< std::endl
		;

		return {};
	}

	CompositeShape result = loadImage(image, atlas, baseKey);

	nsvgDelete(image);

	return result;
}

CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	uint32_t& baseKey,
	float dpi
) {
	std::string buf = svg;

	NSVGimage* image = nsvgParse(buf.data(), "px", dpi);

	if(!image) {
		std::cerr << "slughorn::nanosvg::loadString: failed to parse SVG" << std::endl;

		return {};
	}

	CompositeShape result = loadImage(image, atlas, baseKey);

	nsvgDelete(image);

	return result;
}

}
}

#endif
