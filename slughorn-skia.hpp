#pragma once

// =============================================================================
// slughorn-skia.hpp — Skia path backend for slughorn
//
// Decomposes Skia SkPath objects into slughorn Atlas shapes, with optional
// stroke-to-fill expansion via skpathutils::FillPathWithPaint.
// No OSG, VSG, or other graphics library dependency.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
//   #define SLUGHORN_SKIA_IMPLEMENTATION
//   #include "slughorn-skia.hpp"
//
// All other translation units include it without the define.
//
// The Skia headers must be on your include path. Link against skia.
//
// CONIC HANDLING
// --------------
// Skia paths may contain rational quadratic (conic) segments with a weight w.
// When w == 1 the conic is an ordinary quadratic and is passed through as-is.
// When w != 1 (e.g. circular arcs, where w = cos(θ/2)) the segment is split
// at t=0.5 into two ordinary quadratics using the standard rational-to-
// polynomial formula:
//
//   mid   = (P0 + 2w·P1 + P2) / (2(1+w))   — point on curve at t=0.5
//   ctrl0 = (P0 + w·P1) / (1+w)             — control point for first half
//   ctrl1 = (P2 + w·P1) / (1+w)             — control point for second half
//
// One split is sufficient in practice because Skia's own iterator already
// subdivides conics before yielding them.  See slughorn::skia::detail::
// splitConic() if you need to inspect or extend the logic.
// =============================================================================

#include "slughorn.hpp"

#include "include/core/SkPath.h"
#include "include/core/SkPaint.h"
#include "include/pathops/SkPathOps.h"

namespace slughorn {
namespace skia {

// =============================================================================
// Core decomposition
// =============================================================================

// Decompose @p path into slughorn curves and append them to @p curves.
//
// @p scale is applied uniformly to every coordinate — use it to normalise
// path coordinates into the [0, 1] em-square that slughorn expects.
// Pass 1.0 if your coordinates are already normalised.
//
// Conic segments (kConic_Verb) are split into two ordinary quadratics.
// Cubic segments (kCubic_Verb) are split at their midpoint into two
// quadratics via CurveDecomposer::cubicTo.
void decomposePath(
    const SkPath&   path,
    Atlas::Curves&  curves,
    slug_t          scale = 1.0_cv
);

// Decompose @p path in local coordinate space (tight bounding box origin),
// appending curves to @p curves.
//
// The path is translated so that its bounding box top-left moves to the
// origin before decomposition. This means the atlas shape gets bands sized
// to its own geometry rather than the full canvas — directly addressing the
// "every layer shares the full em-square" waste visible in the heatmap.
//
// The translation that maps local space back to the original canvas space is
// returned in @p outTransform as a pure translation Matrix (dx/dy only,
// xx/yy = 1, xy/yx = 0). Store this in Layer::transform so that
// ShapeDrawable::compile() can apply it when building the quad position.
//
// @p scale is applied after the local translation, normalising the local
// bounding box into slughorn's em-space exactly as decomposePath() does for
// the full canvas.
//
// Returns false (and leaves @p curves and @p outTransform untouched) if
// @p path is empty or has a zero-size bounding box.
bool decomposePathLocal(
    const SkPath&    path,
    Atlas::Curves&   curves,
    Matrix&          outTransform,
    slug_t           scale = 1.0_cv
);

// =============================================================================
// Stroke expansion
// =============================================================================

// Expand @p src from a stroked outline into a filled path using Skia's
// skpathutils::FillPathWithPaint.  The returned path can be passed directly
// to decomposePath() or loadStrokedShape().
//
// join and cap default to round — the most common choice for smooth shapes.
SkPath strokeToFill(
    const SkPath&    src,
    float            strokeWidth,
    SkPaint::Join    join = SkPaint::kRound_Join,
    SkPaint::Cap     cap  = SkPaint::kRound_Cap
);

// =============================================================================
// Atlas integration
// =============================================================================

// Decompose @p path and register the result in @p atlas under @p key.
//
// If autoMetrics is true (default) slughorn derives width/height/bearing/
// advance from the curve bounding box.  Set it to false and fill in the
// ShapeInfo fields below if you need precise control.
//
// Returns true if at least one curve was produced and the shape was added.
// Returns false (and does NOT call addShape) if the path is empty.
bool loadShape(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    slug_t        scale       = 1.0_cv,
    bool          autoMetrics = true
);

// Decompose @p path in local coordinate space and register the result in
// @p atlas under @p key. The canvas-space translation is written to
// @p outTransform — store it in the corresponding Layer::transform.
//
// This is the local-coords counterpart to loadShape(). Use it for any layer
// whose geometry occupies only a small portion of the full canvas, so the
// atlas allocates tight bands rather than canvas-sized ones.
//
// Returns true if at least one curve was produced and the shape was added.
// Returns false (and does NOT call addShape) if the path is empty or has a
// zero-size bounding box.
bool loadShapeLocal(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    Matrix&       outTransform,
    slug_t        scale       = 1.0_cv,
    bool          autoMetrics = true
);

// Convenience: stroke-expand then load.  Equivalent to:
//   loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, scale)
bool loadStrokedShape(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    float         strokeWidth,
    slug_t        scale     = 1.0_cv,
    SkPaint::Join join      = SkPaint::kRound_Join,
    SkPaint::Cap  cap       = SkPaint::kRound_Cap
);

} // namespace skia
} // namespace slughorn


// =============================================================================
// IMPLEMENTATION
// =============================================================================
#ifdef SLUGHORN_SKIA_IMPLEMENTATION

#include "include/core/SkPathUtils.h"

namespace slughorn {
namespace skia {
namespace detail {

// -----------------------------------------------------------------------------
// splitConic
//
// Split a rational quadratic Bézier (conic) with weight @p w at t=0.5 into
// two ordinary quadratics and append them to @p decomposer.
//
// Formula (standard rational-to-polynomial midpoint split):
//
//   denom = 1 + w
//   mid   = (P0 + 2w·P1 + P2) / (2·denom)
//   ctrl0 = (P0 + w·P1)       /    denom
//   ctrl1 = (P2 + w·P1)       /    denom
//
// Emit: quadTo(ctrl0, mid) then quadTo(ctrl1, P2).
// The current pen position is P0 (already set in the decomposer).
// -----------------------------------------------------------------------------
static void splitConic(
    CurveDecomposer& decomposer,
    slug_t p0x, slug_t p0y,   // current pen — P0
    slug_t p1x, slug_t p1y,   // control point — P1
    slug_t p2x, slug_t p2y,   // end point — P2
    slug_t w                  // conic weight
) {
    const slug_t denom = 1.0_cv + w;
    const slug_t inv   = 1.0_cv / denom;

    // Midpoint on the curve
    const slug_t midX = (p0x + 2.0_cv * w * p1x + p2x) * (0.5_cv * inv);
    const slug_t midY = (p0y + 2.0_cv * w * p1y + p2y) * (0.5_cv * inv);

    // Control points for each half
    const slug_t c0x = (p0x + w * p1x) * inv;
    const slug_t c0y = (p0y + w * p1y) * inv;
    const slug_t c1x = (p2x + w * p1x) * inv;
    const slug_t c1y = (p2y + w * p1y) * inv;

    decomposer.quadTo(c0x, c0y, midX, midY);
    decomposer.quadTo(c1x, c1y, p2x,  p2y);
}

} // namespace detail

// =============================================================================
// decomposePath — implementation
// =============================================================================

void decomposePath(
    const SkPath&  path,
    Atlas::Curves& curves,
    slug_t         scale
) {
    CurveDecomposer decomposer(curves);

    // Force-close open contours so the winding rule works correctly.
    SkPath::Iter iter(path, /*forceClose=*/true);

    SkPoint      pts[4];
    SkPath::Verb verb;

    while((verb = iter.next(pts)) != SkPath::kDone_Verb) {
        switch(verb) {

            case SkPath::kMove_Verb:
                decomposer.moveTo(
                    cv(pts[0].x()) * scale,
                    cv(pts[0].y()) * scale
                );
                break;

            case SkPath::kLine_Verb:
                decomposer.lineTo(
                    cv(pts[1].x()) * scale,
                    cv(pts[1].y()) * scale
                );
                break;

            case SkPath::kQuad_Verb:
                decomposer.quadTo(
                    cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
                    cv(pts[2].x()) * scale, cv(pts[2].y()) * scale
                );
                break;

            case SkPath::kConic_Verb: {
                const slug_t w = cv(iter.conicWeight());

                // w == 1.0: ordinary quadratic — no split needed.
                if(std::abs(w - 1.0_cv) < 1e-5_cv) {
                    decomposer.quadTo(
                        cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
                        cv(pts[2].x()) * scale, cv(pts[2].y()) * scale
                    );
                }
                else {
                    detail::splitConic(
                        decomposer,
                        cv(pts[0].x()) * scale, cv(pts[0].y()) * scale,
                        cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
                        cv(pts[2].x()) * scale, cv(pts[2].y()) * scale,
                        w
                    );
                }
                break;
            }

            case SkPath::kCubic_Verb:
                decomposer.cubicTo(
                    cv(pts[1].x()) * scale, cv(pts[1].y()) * scale,
                    cv(pts[2].x()) * scale, cv(pts[2].y()) * scale,
                    cv(pts[3].x()) * scale, cv(pts[3].y()) * scale
                );
                break;

            // forceClose=true means the iterator inserts an implicit lineTo
            // back to the start before emitting kClose_Verb, so we don't need
            // to do anything here.
            case SkPath::kClose_Verb:
            case SkPath::kDone_Verb:
                break;
        }
    }
}

// =============================================================================
// decomposePathLocal — implementation
// =============================================================================

bool decomposePathLocal(
    const SkPath& path,
    Atlas::Curves& curves,
    Matrix&        outTransform,
    slug_t         scale
) {
    if(path.isEmpty()) return false;

    const SkRect bounds = path.getBounds();

    if(bounds.isEmpty()) return false;

    // Translate path so its bounding box top-left sits at the origin.
    // All curve coordinates then live in [0, width] x [0, height] — tight
    // bands, no wasted em-space from canvas offset.
    const SkPath local = path.makeTransform(
        SkMatrix::Translate(-bounds.left(), -bounds.top())
    );

    decomposePath(local, curves, scale);

    if(curves.empty()) return false;

    // Return the canvas-space offset in slughorn's scaled coordinate system.
    // Layer::transform carries this so ShapeDrawable can add it to pos.
    outTransform = Matrix::identity();
    outTransform.dx = cv(bounds.left())  * scale;
    outTransform.dy = cv(bounds.top())   * scale;

    return true;
}

// =============================================================================
// strokeToFill — implementation
// =============================================================================

SkPath strokeToFill(
    const SkPath& src,
    float         strokeWidth,
    SkPaint::Join join,
    SkPaint::Cap  cap
) {
    SkPaint paint;

    paint.setStyle(SkPaint::kStroke_Style);
    paint.setStrokeWidth(strokeWidth);
    paint.setStrokeJoin(join);
    paint.setStrokeCap(cap);

    return skpathutils::FillPathWithPaint(src, paint);
}

// =============================================================================
// loadShape — implementation
// =============================================================================

bool loadShape(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    slug_t        scale,
    bool          autoMetrics
) {
    Atlas::ShapeInfo info;
    info.autoMetrics = autoMetrics;

    decomposePath(path, info.curves, scale);

    if(info.curves.empty()) return false;

    atlas.addShape(key, info);

    return true;
}

// =============================================================================
// loadShapeLocal — implementation
// =============================================================================

bool loadShapeLocal(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    Matrix&       outTransform,
    slug_t        scale,
    bool          autoMetrics
) {
    Atlas::ShapeInfo info;
    info.autoMetrics = autoMetrics;

    if(!decomposePathLocal(path, info.curves, outTransform, scale)) return false;

    atlas.addShape(key, info);

    return true;
}

// =============================================================================
// loadStrokedShape — implementation
// =============================================================================

bool loadStrokedShape(
    const SkPath& path,
    Atlas&        atlas,
    uint32_t      key,
    float         strokeWidth,
    slug_t        scale,
    SkPaint::Join join,
    SkPaint::Cap  cap
) {
    return loadShape(strokeToFill(path, strokeWidth, join, cap), atlas, key, scale);
}

} // namespace skia
} // namespace slughorn

#endif // SLUGHORN_SKIA_IMPLEMENTATION
