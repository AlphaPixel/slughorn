#pragma once

// ================================================================================================
// Decomposes Blend2D path data into slughorn Atlas shapes. No OSG, VSG, or other graphics
// library dependency.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_BLEND2D_IMPLEMENTATION
//   #include <slughorn/blend2d.hpp>
//
// All other translation units include it without the define.
//
// PATH CONVENTIONS
// ----------------
// BLPath coordinates are taken as-is (user space). Apply scale to normalise them into the
// em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit space).
//
// decomposePath() shifts curves to local origin when autoMetrics=true (tight atlas bands) and
// returns both the curves and the offset that was subtracted as a Transform. When autoMetrics=false,
// curves are stored as-is and the returned Transform is zero. If you are compositing multiple shapes,
// store the returned Transform in Layer::transform so the renderer can restore the correct canvas
// position at draw time.
//
// COMMAND STREAM ENCODING
// -----------------------
// Blend2D's BLPathView stores one vertex per command entry. Multi-vertex primitives use
// consecutive command tags:
//   QUAD   -> [BL_PATH_CMD_QUAD, BL_PATH_CMD_ON]              -- 2 vertices (cp, end)
//   CUBIC  -> [BL_PATH_CMD_CUBIC, BL_PATH_CMD_CUBIC,
//              BL_PATH_CMD_ON]                                -- 3 vertices (cp1, cp2, end)
//   CONIC  -> [BL_PATH_CMD_CONIC, BL_PATH_CMD_WEIGHT,
//              BL_PATH_CMD_ON]                                -- 3 vertices (cp, weight, end)
// CONIC curves are approximated as quadratics (weight ignored). Conics appear only when a
// BLPath contains arc geometry built via arc_to; for typical authored vector paths this is rare.
//
// STROKE SUPPORT
// --------------
// loadStrokedShape() calls bl_path_add_stroked_path() to expand a stroke to a filled outline
// before registering it in the atlas. This is the capability Cairo lacks and the main reason
// to prefer this backend for stroked shapes.
//
// CUBIC CURVES
// ------------
// Blend2D supports cubic Beziers natively; CurveDecomposer::cubicTo splits each cubic at its
// midpoint into two quadratics for slughorn's quad-only curve representation.
// ================================================================================================

#include "slughorn.hpp"

#include <blend2d/blend2d.h>

#include <optional>
#include <utility>

namespace slughorn {
namespace blend2d {

// Decompose @p path into slughorn curves, shifted to local origin for tight atlas bands.
//
// The bounding box minimum is subtracted from every curve point when autoMetrics=true.
// Returns a ShapeInfo (with curves and origin pre-set) and a Transform (x/y only). Store
// the Transform in Layer::transform.
//
// @p scale is applied uniformly to every coordinate. Use it to normalise path coordinates
// into the em-square slughorn expects (e.g. 1/100 if your path is built in a 100-unit space).
// Pass 1.0 if coordinates are already normalised.
//
// When autoMetrics=false and canvasExtent={widthEm, heightEm} is supplied, the returned
// ShapeInfo has its band metrics pre-populated. Callers that use decomposePath directly and
// then call atlas.addShape() should pass canvasExtent here.
std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	const BLPath& path,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
);

// Decompose @p path and register the result in @p atlas under @p key.
//
// Returns the local-origin offset as a Transform (see decomposePath). Store it in
// Layer::transform for correct composite positioning.
//
// Returns a zero Transform and does NOT call addShape if the path is empty.
//
// When autoMetrics=false, supply canvasExtent={widthEm, heightEm} to declare the band
// extent. Without it, loadShape silently falls back to tight-bbox behaviour (autoMetrics=true).
Transform loadShape(
	const BLPath& path,
	Atlas& atlas,
	Key key,
	slug_t scale=1_cv,
	Atlas::ShapeInfo::Origin origin={},
	bool autoMetrics=true,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent={}
);

// Expand @p path as a stroke (using @p stroke options) to a filled outline, then register
// the result in @p atlas under @p key. Uses Blend2D's stroke-to-fill expansion -- the
// primary advantage this backend has over Cairo.
//
// @p stroke controls width, join, cap, and dash; use a default-constructed BLStrokeOptions
// for a 1-unit round-capped round-joined stroke.
Transform loadStrokedShape(
	const BLPath& path,
	Atlas& atlas,
	Key key,
	const BLStrokeOptions& stroke={},
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
#ifdef SLUGHORN_BLEND2D_IMPLEMENTATION

namespace slughorn {
namespace blend2d {

std::pair<Atlas::ShapeInfo, Transform> decomposePath(
	const BLPath& path,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent
) {
	BLBox bbox{};
	const bool hasGeometry = path.size() > 0 && path.get_bounding_box(&bbox) == BL_SUCCESS;

	const slug_t ox = hasGeometry ? cv(bbox.x0) * scale : 0_cv;
	const slug_t oy = hasGeometry ? cv(bbox.y0) * scale : 0_cv;
	const slug_t offX = autoMetrics ? ox : 0_cv;
	const slug_t offY = autoMetrics ? oy : 0_cv;

	Atlas::Curves curves;
	CurveDecomposer decomposer(curves);

	const BLPathView v = path.view();
	const uint8_t* cmds = v.command_data;
	const BLPoint* pts = v.vertex_data;
	size_t ci = 0;
	size_t vi = 0;

	while(ci < v.size) {
		switch(cmds[ci]) {
			case BL_PATH_CMD_MOVE:
				decomposer.moveTo(cv(pts[vi].x) * scale, cv(pts[vi].y) * scale);

				ci += 1; vi += 1;

				break;

			case BL_PATH_CMD_ON:
				decomposer.lineTo(cv(pts[vi].x) * scale, cv(pts[vi].y) * scale);

				ci += 1; vi += 1;

				break;

			case BL_PATH_CMD_QUAD:
				// [QUAD, ON] -> pts[vi]=cp, pts[vi+1]=end
				decomposer.quadTo(
					cv(pts[vi].x) * scale, cv(pts[vi].y) * scale,
					cv(pts[vi+1].x) * scale, cv(pts[vi+1].y) * scale
				);

				ci += 2; vi += 2;

				break;

			case BL_PATH_CMD_CUBIC:
				// [CUBIC, CUBIC, ON] -> pts[vi]=cp1, pts[vi+1]=cp2, pts[vi+2]=end
				decomposer.cubicTo(
					cv(pts[vi].x) * scale, cv(pts[vi].y) * scale,
					cv(pts[vi+1].x) * scale, cv(pts[vi+1].y) * scale,
					cv(pts[vi+2].x) * scale, cv(pts[vi+2].y) * scale
				);

				ci += 3; vi += 3;

				break;

			case BL_PATH_CMD_CONIC:
				// [CONIC, WEIGHT, ON] -> pts[vi]=cp, pts[vi+1].x=weight, pts[vi+2]=end
				// Approximate as quadratic (weight ignored for now).
				decomposer.quadTo(
					cv(pts[vi].x) * scale, cv(pts[vi].y) * scale,
					cv(pts[vi+2].x) * scale, cv(pts[vi+2].y) * scale
				);

				ci += 3; vi += 3;

				break;

			case BL_PATH_CMD_CLOSE:
				decomposer.close();

				ci += 1; vi += 1; // skip implicit close vertex stored in stream

				break;

			default:
				// BL_PATH_CMD_WEIGHT -- consumed by CONIC above; skip defensively.
				ci += 1; vi += 1;

				break;
		}
	}

	if(autoMetrics) {
		for(auto& c : curves) {
			c.x1 -= offX; c.x2 -= offX; c.x3 -= offX;
			c.y1 -= offY; c.y2 -= offY; c.y3 -= offY;
		}
	}

	Atlas::ShapeInfo::Origin infoOrigin = origin;

	if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
		infoOrigin.x = origin.x * scale - offX;
		infoOrigin.y = origin.y * scale - offY;
	}

	else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
		infoOrigin.x = origin.x * scale;
		infoOrigin.y = origin.y * scale;
	}

	// YES, code like this a DEAD GIVEAWAY that AI wrote it. :) But I still like it, so... it stays!
	const Transform transform = !autoMetrics ? Transform{} :
		(origin.type == Atlas::ShapeInfo::Origin::Type::Centered)
		? Transform{ cv(bbox.x0 + bbox.x1) * 0.5_cv * scale, cv(bbox.y0 + bbox.y1) * 0.5_cv * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Pivot)
		? Transform{ origin.x * scale, origin.y * scale }
		: (origin.type == Atlas::ShapeInfo::Origin::Type::Custom)
		? Transform{ ox + origin.x * scale, oy + origin.y * scale }
		: Transform{ ox, oy }
	;

	Atlas::ShapeInfo info;

	info.curves = std::move(curves);
	info.origin = infoOrigin;

	if(!autoMetrics && canvasExtent) {
		info.bearingX = 0_cv;
		info.bearingY = canvasExtent->second;
		info.width = canvasExtent->first;
		info.height = canvasExtent->second;
		info.autoMetrics = false;
	}

	return { std::move(info), transform };
}

Transform loadShape(
	const BLPath& path,
	Atlas& atlas,
	Key key,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent
) {
	const bool effectiveShift = autoMetrics || !canvasExtent.has_value();
	auto [info, transform] = decomposePath(path, scale, origin, effectiveShift, canvasExtent);

	if(info.curves.empty()) return {};

	if(effectiveShift) info.autoMetrics = true;

	atlas.addShape(key, info);

	return transform;
}

Transform loadStrokedShape(
	const BLPath& path,
	Atlas& atlas,
	Key key,
	const BLStrokeOptions& stroke,
	slug_t scale,
	Atlas::ShapeInfo::Origin origin,
	bool autoMetrics,
	std::optional<std::pair<slug_t, slug_t>> canvasExtent
) {
	BLPath stroked;

	bl_path_add_stroked_path(&stroked, &path, nullptr, &stroke, nullptr);

	return loadShape(stroked, atlas, key, scale, origin, autoMetrics, canvasExtent);
}

}
}

#endif
