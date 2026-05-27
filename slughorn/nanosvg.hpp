#pragma once

// ================================================================================================
// NanoSVG backend for slughorn
//
// Parses SVG files/strings into slughorn Atlas shapes, producing a CompositeShape with one
// Layer per filled SVG shape, back-to-front order preserved.
//
// API mirrors slughorn/cairo.hpp exactly:
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
// #include <slughorn/nanosvg.hpp>
//
// All other translation units include it without the define.
//
// WHAT IS SUPPORTED
// -----------------
// - Filled shapes (solid color and linear/radial gradients)
// - All path segment types: lines, cubics (split to quadratics), and NanoSVG's
//   pre-flattened cubic chains
// - Fill color unpacked from NanoSVG's packed ABGR uint32
// - Per-shape local coordinate decomposition (tight bands, zero offset waste)
// - Auto-scale from SVG viewBox width (always 1/image->width)
//
// WHAT IS NOT (YET) SUPPORTED
// ---------------------------
// - Stroked shapes (stroke-to-fill expansion not yet wired up)
// - Sweep/conic gradients (SVG has no native sweep gradient type)
// - Gradient spreadMethod reflect/repeat (clamped to pad)
// - Radial focal point offset / cone gradients (g->fx/fy != 0 is the SVG cone case)
// - Clip paths, masks, opacity, transforms on groups
// - Text elements
// ================================================================================================

#include "slughorn.hpp"

#ifdef SLUGHORN_NANOSVG_IMPLEMENTATION
#define NANOSVG_IMPLEMENTATION
#endif

SLUGHORN_DIAGNOSTIC_PUSH()

#if defined(__clang__)
SLUGHORN_IGNORE("-Wimplicit-float-conversion")
SLUGHORN_IGNORE("-Wfloat-conversion")
SLUGHORN_IGNORE("-Wimplicit-int-conversion")
#endif

SLUGHORN_IGNORE("-Wshadow")
SLUGHORN_IGNORE("-Wsign-conversion")

#include "nanosvg.h"

SLUGHORN_DIAGNOSTIC_POP()

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
		cv((packed & 0xFF)) / 255_cv, // R
		cv(((packed >> 8) & 0xFF)) / 255_cv, // G
		cv(((packed >> 16) & 0xFF)) / 255_cv, // B
		cv(((packed >> 24) & 0xFF)) / 255_cv, // A
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
// Returns a ShapeInfo (curves and origin pre-set) and a Matrix (dx/dy only; xx/yy are identity).
// Store the Matrix in Layer::transform for correct composite positioning. Returns an empty ShapeInfo
// if the shape has no paths or a zero bounding box.
//
// The returned transform depends on @p origin:
//
// Origin::Default - transform.dx/dy = bbox corner * scale.
// Origin::Centered - transform.dx/dy = bbox center * scale; computeQuad still places the quad at
// the correct canvas position, and the transform acts as a pivot point.
// ================================================================================================
std::pair<Atlas::ShapeInfo, Matrix> decomposePath(
	const NSVGshape* shape,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={}
);

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
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={}
);

// ================================================================================================
// loadImage
//
// Parse an entire NSVGimage into a CompositeShape - one Layer per filled
// NSVGshape, back-to-front order preserved.
//
// Scale is always auto-computed as 1/image->width, normalizing the SVG canvas
// to [0,1] em-space. Keys are allocated via @p keys (KeyIterator); on return
// keys.counter is advanced past the last key used. Pass a named KeyIterator
// (e.g. KeyIterator("logo")) to produce named keys like "logo_0", "logo_1", …
//
// Shapes with no solid fill are skipped with a warning to stderr.
// ================================================================================================
CompositeShape loadImage(
	const NSVGimage* image,
	Atlas& atlas,
	KeyIterator& keys
);

// ================================================================================================
// loadFile / loadString - convenience wrappers
// ================================================================================================
CompositeShape loadFile(
	const std::string& path,
	Atlas& atlas,
	KeyIterator& keys,
	float dpi=96.0f
);

CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	KeyIterator& keys,
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

std::pair<Atlas::ShapeInfo, Matrix> decomposePath(const NSVGshape* shape, slug_t scale, Atlas::ShapeInfo::Origin origin) {
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

	if(origin.type == Atlas::ShapeInfo::Origin::Type::Centered) {
		transform.dx = (minX + maxX) * 0.5_cv;
		transform.dy = (minY + maxY) * 0.5_cv;
	}

	else {
		transform.dx = minX;
		transform.dy = minY;
	}

	Atlas::ShapeInfo info;

	info.curves = std::move(curves);
	info.origin = origin;

	return { std::move(info), transform };
}

Matrix loadShape(const NSVGshape* shape, Atlas& atlas, Key key, slug_t scale, Atlas::ShapeInfo::Origin origin) {
	auto [info, transform] = decomposePath(shape, scale, origin);

	if(info.curves.empty()) return Matrix::identity();

	atlas.addShape(key, info);

	return transform;
}

CompositeShape loadImage(const NSVGimage* image, Atlas& atlas, KeyIterator& keys) {
	CompositeShape composite;

	if(!image) return composite;

	if(image->width <= 0.0f) {
		std::cerr
			<< "slughorn::nanosvg::loadImage: image width is zero, cannot normalize."
			<< std::endl
		;

		return composite;
	}

	const slug_t scale = 1_cv / cv(image->width);

	// Normalized width is always 1.0...
	composite.advance = 1_cv;

	for(const NSVGshape* shape = image->shapes; shape; shape = shape->next) {
		if(!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

		Color color = { 1_cv, 1_cv, 1_cv, 1_cv };
		uint32_t gradientId = 0;

		if(shape->fill.type == NSVG_PAINT_COLOR) {
			color = colorFromNSVG(shape->fill.color);

			if(color.a < 1e-4_cv) continue;
		}
		else if(
			shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT ||
			shape->fill.type == NSVG_PAINT_RADIAL_GRADIENT
		) {
			NSVGgradient* g = shape->fill.gradient;

			if(!g || g->nstops == 0) {
				std::cerr << "slughorn::nanosvg: skipping gradient with no stops." << std::endl;
				continue;
			}

			std::vector<GradientStop> stops;
			stops.reserve(static_cast<size_t>(g->nstops));

			for(int i = 0; i < g->nstops; i++) {
				stops.push_back({ cv(g->stops[i].offset), colorFromNSVG(g->stops[i].color) });
			}

			// NanoSVG stores g->xform as the INVERSE of the gradient -> pixel transform.
			// nsvg__scaleToViewbox() builds the forward transform then inverts it for its software
			// rasterizer. We invert back to get the forward transform so we can extract canonical
			// endpoints.
			//
			// Gradient endpoints are shifted into local em-space to match _toLocalOrigin() applied
			// to the path curves in decomposePath.
			float fwd[6];
			nsvg__xformInverse(fwd, g->xform);

			const slug_t minX_em = cv(shape->bounds[0]) * scale;
			const slug_t minY_em = cv(shape->bounds[1]) * scale;

			GradientInfo info;
			info.stops = std::move(stops);

			if(shape->fill.type == NSVG_PAINT_LINEAR_GRADIENT) {
				// In NanoSVG's convention, the gradient t-axis is the second
				// column of the forward transform: (0,0) -> start, (0,1) - end.
				float px0, py0, px1, py1;

				nsvg__xformPoint(&px0, &py0, 0.0f, 0.0f, fwd);
				nsvg__xformPoint(&px1, &py1, 0.0f, 1.0f, fwd);

				info.type = GradientInfo::Type::Linear;
				info.transform = buildLinearGradientMatrix(
					cv(px0) * scale - minX_em, cv(py0) * scale - minY_em,
					cv(px1) * scale - minX_em, cv(py1) * scale - minY_em
				);
			}

			else {
				// Center is at the origin of the forward transform.
				float pcx, pcy;

				nsvg__xformPoint(&pcx, &pcy, 0.0f, 0.0f, fwd);

				// Extract the full 2x2 from fwd to support elliptical (non-square bbox) radials.
				// fwd maps gradient-unit-space -> SVG-pixel-space; xformPoint convention:
				//
				// px = x*fwd[0] + y*fwd[2] + fwd[4]
				// py = x*fwd[1] + y*fwd[3] + fwd[5]
				//
				// 2x2 matrix A (row-major): [[fwd[0], fwd[2]], [fwd[1], fwd[3]]]
				// In em-space: A_em = A * scale_f
				// B = A_em^{-1} maps em-space deltas to gradient space; length(B*d)=1 at outer ellipse.
				const float scale_f = float(scale);
				const float det = fwd[0] * fwd[3] - fwd[2] * fwd[1];

				if(std::abs(det) < 1e-10f) {
					std::cerr << "slughorn::nanosvg: degenerate radial gradient transform, skipping." << std::endl;
					continue;
				}

				const float invSDet = 1.0f / (scale_f * det);

				float b00 = fwd[3] * invSDet; // B[0,0]
				float b01 = -fwd[2] * invSDet; // B[0,1]
				float b10 = -fwd[1] * invSDet; // B[1,0]
				float b11 = fwd[0] * invSDet; // B[1,1]

				// Ensure b11 > 0 for the shader discriminator (w > 0 == radial).
				// Negating the whole matrix is safe: length(B*d) == length(-B*d).
				if(b11 < 0.0f) { b00=-b00; b01=-b01; b10=-b10; b11=-b11; }

				// NanoSVG computes objectBoundingBox radial gradients with an isotropic
				// radius sl = sqrt(pow(sw,2) + pow(sh, 2)) / sqrt(2) instead of separate
				// rx= r * sw, ry = r * sh.
				// Correct B_correct = diag(sl / sw, sl / sh) * B_current.
				if(g->units == NSVG_OBJECT_SPACE) {
					const float sw_px = shape->bounds[2] - shape->bounds[0];
					const float sh_px = shape->bounds[3] - shape->bounds[1];

					if(sw_px > 0.0f && sh_px > 0.0f) {
						const float sl_px = sqrtf(sw_px * sw_px + sh_px * sh_px) / sqrtf(2.0f);
						const float kx = sl_px / sw_px;
						const float ky = sl_px / sh_px;

						b00 *= kx; b01 *= kx;
						b10 *= ky; b11 *= ky;
					}
				}

				info.type = GradientInfo::Type::AffineRadial;
				info.transform = buildAffineRadialGradientMatrix(
					cv(pcx) * scale - minX_em,
					cv(pcy) * scale - minY_em,
					cv(b00), cv(b01), cv(b10), cv(b11)
				);
				info.innerRadius = 0_cv;
			}

			gradientId = atlas.addGradient(info);
		}
		else if(shape->fill.type == NSVG_PAINT_NONE) {
			continue;
		}
		else {
			std::cerr
				<< "slughorn::nanosvg: skipping unsupported fill type "
				<< static_cast<int>(shape->fill.type)
				<< std::endl
			;

			continue;
		}

		const Key key = (!keys.force && shape->id[0] != '\0') ? Key(shape->id) : keys.next();

		Matrix transform = loadShape(shape, atlas, key, scale);

		Layer layer;

		layer.key = key;
		layer.color = color;
		layer.gradientId = gradientId;
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
	KeyIterator& keys,
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

	CompositeShape result = loadImage(image, atlas, keys);

	nsvgDelete(image);

	return result;
}

CompositeShape loadString(
	const std::string& svg,
	Atlas& atlas,
	KeyIterator& keys,
	float dpi
) {
	std::string buf = svg;

	NSVGimage* image = nsvgParse(buf.data(), "px", dpi);

	if(!image) {
		std::cerr << "slughorn::nanosvg::loadString: failed to parse SVG" << std::endl;

		return {};
	}

	CompositeShape result = loadImage(image, atlas, keys);

	nsvgDelete(image);

	return result;
}

}
}

#endif
