#pragma once

// =============================================================================
// slughorn-cairo.hpp — Cairo path backend for slughorn
//
// Decomposes Cairo path data into slughorn Atlas shapes.
// No OSG, VSG, or other graphics library dependency.
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
// Cairo is Y-down by default.  slughorn expects Y-up coordinates (the same
// convention as FreeType and em-space in general).  The caller is responsible
// for applying a flip transform to the cairo_t before building paths:
//
//   cairo_translate(cr, 0.0, emSize);   // shift origin to bottom-left
//   cairo_scale(cr, 1.0, -1.0);         // flip Y
//
// This bakes the flip into the path coordinates returned by cairo_copy_path(),
// so decomposePath() receives Y-up data without needing to know about it.
//
// STROKE LIMITATION
// -----------------
// cairo_stroke_to_path() is not part of Cairo's public stable API (it exists
// internally but is not exported).  For stroke-to-fill expansion use the Skia
// backend (slughorn-skia.hpp) or pre-expand strokes in your path construction.
//
// CUBIC CURVES
// ------------
// Cairo's native curve primitive is the cubic Bézier (CAIRO_PATH_CURVE_TO).
// There are no conics or rational quadratics to handle.  CurveDecomposer::
// cubicTo splits each cubic at its midpoint into two quadratics — sufficient
// for smooth results and consistent with the FreeType and Skia backends.
// =============================================================================

#include "slughorn.hpp"

#include <cairo/cairo.h>

namespace slughorn {
namespace cairo {

// =============================================================================
// Core decomposition
// =============================================================================

// Decompose the current path on @p cr into slughorn curves and append them
// to @p curves.
//
// Uses cairo_copy_path() (cubic-preserving) rather than cairo_copy_path_flat()
// (which would pre-subdivide arcs into line segments and inflate curve counts).
//
// The path is copied and then destroyed internally — the path on @p cr is
// left unchanged.
//
// @p scale is applied uniformly to every coordinate.  Use it to normalise
// path coordinates into the [0, 1] em-square slughorn expects (e.g. 1/100
// if your path is built in a 100-unit space).  Pass 1.0 if coordinates are
// already normalised.
void decomposePath(
    cairo_t*       cr,
    Atlas::Curves& curves,
    slug_t         scale = 1.0_cv
);

// =============================================================================
// Atlas integration
// =============================================================================

// Decompose the current path on @p cr and register the result in @p atlas
// under @p key.
//
// If autoMetrics is true (default) slughorn derives width/height/bearing/
// advance from the curve bounding box.  Set it to false and populate the
// ShapeInfo fields manually if you need precise control.
//
// Returns true if at least one curve was produced and the shape was added.
// Returns false (and does NOT call addShape) if the path is empty.
bool loadShape(
    cairo_t*  cr,
    Atlas&    atlas,
    uint32_t  key,
    slug_t    scale       = 1.0_cv,
    bool      autoMetrics = true
);

} // namespace cairo
} // namespace slughorn


// =============================================================================
// IMPLEMENTATION
// =============================================================================
#ifdef SLUGHORN_CAIRO_IMPLEMENTATION

namespace slughorn {
namespace cairo {

// =============================================================================
// decomposePath — implementation
// =============================================================================

void decomposePath(
    cairo_t*       cr,
    Atlas::Curves& curves,
    slug_t         scale
) {
    CurveDecomposer decomposer(curves);

    cairo_path_t* path = cairo_copy_path(cr);

    for(int i = 0; i < path->num_data; i += path->data[i].header.length) {
        const cairo_path_data_t* d = &path->data[i];

        switch(d->header.type) {

            case CAIRO_PATH_MOVE_TO:
                decomposer.moveTo(
                    cv(d[1].point.x) * scale,
                    cv(d[1].point.y) * scale
                );
                break;

            case CAIRO_PATH_LINE_TO:
                decomposer.lineTo(
                    cv(d[1].point.x) * scale,
                    cv(d[1].point.y) * scale
                );
                break;

            case CAIRO_PATH_CURVE_TO:
                decomposer.cubicTo(
                    cv(d[1].point.x) * scale, cv(d[1].point.y) * scale,
                    cv(d[2].point.x) * scale, cv(d[2].point.y) * scale,
                    cv(d[3].point.x) * scale, cv(d[3].point.y) * scale
                );
                break;

            // The implicit close line (back to the subpath start) has already
            // been emitted as a CAIRO_PATH_LINE_TO by Cairo before this token,
            // so no geometric action is needed here.
            case CAIRO_PATH_CLOSE_PATH:
                break;
        }
    }

    cairo_path_destroy(path);
}

// =============================================================================
// loadShape — implementation
// =============================================================================

bool loadShape(
    cairo_t*  cr,
    Atlas&    atlas,
    uint32_t  key,
    slug_t    scale,
    bool      autoMetrics
) {
    Atlas::ShapeInfo info;
    info.autoMetrics = autoMetrics;

    decomposePath(cr, info.curves, scale);

    if(info.curves.empty()) return false;

    atlas.addShape(key, info);

    return true;
}

} // namespace cairo
} // namespace slughorn

#endif // SLUGHORN_CAIRO_IMPLEMENTATION
