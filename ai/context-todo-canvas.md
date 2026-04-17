# slughorn-canvas - Status, Design, and TODO

## What exists (as of Day 8)

`slughorn-canvas.hpp` - a slughorn-native HTML Canvas-style drawing context.
No external dependencies. Pure C++ header, no implementation guard needed.

### Files
    slughorn-canvas.hpp       - the drawing context (new, Day 8)
    slughorn.hpp              - CurveDecomposer upgraded (adaptive cubicTo, Day 8)

### Key types
    slughorn::canvas::Canvas  - stateful CompositeShape builder + path accumulator

### Canvas API (implemented)
    beginPath()
    moveTo(x, y)
    lineTo(x, y)
    quadTo(cx, cy, x, y)
    bezierTo(c1x, c1y, c2x, c2y, x, y)   - cubic, adaptively subdivided
    closePath()

    arc(cx, cy, r, startAngle, endAngle, ccw=false)
    arcTo(x1, y1, x2, y2, r)              - tangential arc, HTML Canvas semantics

    rect(x, y, w, h)
    roundedRect(x, y, w, h, r)
    circle(cx, cy, r)
    ellipse(cx, cy, rx, ry)

    fill(color, scale=1.0)                - commits path as a Layer, auto-key
    defineShape(key, scale=1.0)           - commits path as geometry only, no Layer

    beginComposite()                      - discard accumulated layers, start fresh
    setAdvance(advance)
    finalize()                            - return CompositeShape, reset state
    finalizeAs(key)                       - register in Atlas + reset

    decomposer()                          - access internal CurveDecomposer (tune tolerance)
    layerCount()
    hasPendingPath()

### CurveDecomposer upgrade (slughorn.hpp)
    // Tolerance constants (all on CurveDecomposer as static constexpr):
    TOLERANCE_DRAFT    = 1e-2f   - fast, noticeable only at large sizes
    TOLERANCE_BALANCED = 1e-3f   - recommended default for screen work
    TOLERANCE_FINE     = 1e-4f   - high-DPI / print / export
    TOLERANCE_EXACT    = numeric_limits<slug_t>::max()  - classic: always exactly 2 quads

    tolerance field defaults to TOLERANCE_BALANCED (changed from original 1e-4f)
    cubicTo() uses adaptive De Casteljau subdivision
    _flatEnough() uses squared point-to-chord distance (no sqrt in hot path)
    _emitTwoQuads() is the leaf - identical to old cubicTo, two quads always
    MAX_DEPTH = 8

All backends (Cairo, NanoSVG, Skia, Canvas) route cubics through
CurveDecomposer::cubicTo and benefit from the upgrade automatically.
No backend source changes required. Default of TOLERANCE_BALANCED is a
silent upgrade for all existing backends.

TOLERANCE_EXACT works because _flatEnough compares distSq <= tolerance*tolerance;
squaring numeric_limits::max() gives infinity, so _flatEnough always returns true
immediately and falls through to _emitTwoQuads. No special-casing needed.

### arc() / arcTo() implementation notes
arc() does NOT call beginPath(). It appends to the current path, enabling
composition with lineTo() for pie slices, stadium shapes, etc.

arc() decomposes into segments of at most ?/2 each via _arcSegments(), using:
    k = (4/3) * tan(segSweep / 4)
as the cubic control point offset. Each segment is emitted via bezierTo() ->
CurveDecomposer::cubicTo() -> adaptive subdivision. Quality scales automatically.

arcTo() implements the full HTML Canvas spec including all degenerate cases:
zero-length legs, collinear points, zero radius -> fallback to lineTo(x1,y1).
Winding direction determined by cross product sign of the two tangent legs.

Practical curve count with TOLERANCE_BALANCED (confirmed in testing):
    triangle  ->   6 curves  (3 edges x 2 quads, no cubics involved)
    circle    -> ~32 curves  (4 cubics x ~8 quads at BALANCED)
    stadium   -> ~60 curves  (arcTo corners, reasonable)
With TOLERANCE_EXACT all of the above drop to minimum (2 quads per cubic).
With TOLERANCE_FINE (original default) counts explode - 2578 for a simple
3-shape scene. TOLERANCE_BALANCED is the correct default for screen work.

### Compound paths / winding - confirmed working (Day 8)
Tested via osgslug-shape-cairo.cpp: outer CCW contour + inner CW contour
as a single bridged path produces correct holes (ring shape, rounded rect
stroke). The Slug fragment shader implements nonzero winding correctly for
compound paths. No special fillRule support needed for this use case.

The working pattern (confirmed): outer CCW -> lineTo bridge -> inner CW,
all as one continuous path with a single closePath(). The bridge seam is
a straight lineTo connecting the outer contour's start point to the inner
contour's start point. See buildPseudostrokeRoundedRectPath() in
osgslug-shape-cairo.cpp for the reference implementation.

Debug mode 1 (checkerboard per band cell) confirmed tight band allocation
for annular shapes. Debug mode 4 (iteration heatmap) showed reasonable
per-band curve costs for the ring shape.

---

## Design invariants (DO NOT VIOLATE)

### Scale contract
The `scale` parameter to fill() / defineShape() is backend normalization only.
It is NEVER stored in Layer::scale.
Layer::scale is reserved for FreeType2 font-size-to-world-units conversion.

### Local origin shift
fill() shifts curves to local origin (bounding box min -> 0,0) before calling
atlas.addShape(). The subtracted offset is stored in Layer::transform (dx/dy).
This matches the convention in slughorn-cairo.hpp and slughorn-nanosvg.hpp exactly.

### Two-quad leaf
_emitTwoQuads() always emits exactly two quadratics per leaf cubic.
This matches NanoSVG's output convention (e.g. a triangle -> 6 curves, not 3)
and keeps curve-texture alignment predictable. Do NOT change this to one quad.

### arc() does not call beginPath()
arc() appends to the current path. This is intentional and matches HTML Canvas.
Callers that want a standalone arc must call beginPath() first themselves.
This enables arc() composition with lineTo() for pie slices, annuli, etc.

### No NanoVG dependency
slughorn-canvas.hpp has zero dependency on the NanoVG library.
The name "Canvas" refers to the HTML Canvas API vocabulary, not NanoVG.
Do NOT add a NanoVG submodule or #include for this backend.

### CompositeShape is always in progress
Canvas always has a composite in progress from construction.
beginComposite() resets it. There is no "closed" state between composites.

### TOLERANCE_EXACT semantics
TOLERANCE_EXACT = numeric_limits<slug_t>::max() replicates pre-adaptive behavior.
It does NOT mean "maximum quality" - it means "exactly 2 quads per cubic, always."
This is the fastest option and matches what Cairo/NanoSVG/Skia produced before
the CurveDecomposer upgrade. Name chosen to signal "read the comment."

---

## TODO - Near term

### arcTo() - investigate bridge seam artifact
Confirmed bug (Day 8): arcTo() produces visually detached corner circles when
used for axis-aligned rounded rectangles, even with correct proportions.
Root cause: the bridging lineTo() inside arcTo() that connects the current
point to the arc's start tangent point is not landing flush with the prior
edge endpoint. The tangent point math may be slightly off, or the implicit
lineTo() is creating a visible micro-gap at the seam.

Workaround: use roundedRect() for axis-aligned rounded rectangles - confirmed
working correctly (Day 8). arcTo() is safe for irregular paths where edges
meet at non-right angles.

Debug approach: write a minimal test case - single right-angle corner via
two lineTo() calls followed by one arcTo() - and verify that:
  1. The tangent point on leg 0 (current->p1) lands exactly at the end of
     the prior lineTo()
  2. The bridging lineTo() is zero-length (i.e. current point IS already
     at the tangent point)
  3. The arc end point lands exactly where the next lineTo() begins
Compare against HTML Canvas arcTo() reference implementation for the same
coordinates. The KAPPA90 corner math in roundedRect() is the proven baseline.

### Tests
Tests were not written during Day 8 - this is the most important near-term task.
See test cases section at the bottom of this file.

### fillRule
Nonzero vs even-odd winding for subpath interpretation. Compound path holes
are already confirmed working via nonzero winding (see above). Before
implementing fillRule as an API: verify whether the Slug fragment shader
supports even-odd at all. Check osgSlug-frag.glsl. If nonzero-only, this
is moot. If both are supported, fillRule should be a Canvas state field
passed through ShapeInfo (which may need a new field).

### beginHole() convenience helper
Given that compound paths with inner CW contours work correctly, a Canvas-level
helper that manages the bridge seam automatically would be useful:

    canvas.beginPath();
    canvas.rect(0, 0, 1, 1);           // outer CCW
    canvas.beginHole();                 // bridge + start inner CW subpath
    canvas.circle(0.5, 0.5, 0.3);      // inner CW (auto-reversed)
    canvas.fill(color);

Implementation: beginHole() emits a lineTo() to the inner contour's start
point (bridge), then sets a flag that causes subsequent path commands to
wind in reverse. Requires tracking current winding state.

---

## TODO - Medium term

### Transform stack (save / restore / CTM)

A Matrix CTM stack enabling:
    canvas.save()
    canvas.translate(dx, dy)
    canvas.scale(sx, sy)             // or scale(s) for uniform
    canvas.rotate(radians)
    canvas.transform(matrix)         // multiply CTM by arbitrary Matrix
    canvas.restore()

Implementation: maintain a std::vector<Matrix> _ctmStack. On moveTo / lineTo /
quadTo / bezierTo, pre-multiply coordinates by the current CTM before passing
to _decomposer. save() pushes current CTM, restore() pops.

Matrix already has operator* for concatenation. translate / scale / rotate are
just Matrix constructors. This is largely mechanical.

Key constraint: the CTM affects path coordinates only, NOT Layer::transform.
Layer::transform continues to carry only the local-origin shift from
_toLocalOrigin(). The CTM is baked into the curve coordinates before the
local-origin shift is applied.

### stroke() / stroke-to-fill expansion

fill() covers filled shapes. stroke() would expand a stroked path into a filled
outline before handing to the Atlas.

Options:
  A) Implement a minimal stroke expander in slughorn-canvas.hpp directly.
  B) Use Skia's stroke-to-path expansion and feed the result into Canvas.
  C) Defer - Cairo already handles stroked paths. Use Cairo for stroked paths
     for now.

Recommendation: defer until Skia backend redesign is complete.
Skia approach (B) is cleanest long-term.

---

## TODO - Longer term

### Layer::scale - evaluate for removal
Currently only meaningful for FT2/text. All geometry backends leave it at 1.0.
If osgSlug::Font / osgSlug::Text take full ownership of font-size-to-world
scaling, Layer::scale becomes dead weight and computeQuad() could take scale
as a call-site parameter instead. Defer until text pipeline stabilizes.
See context-core.md TODO comment on the same topic.

### Python bindings
ext/slughorn-python.cpp already exposes slughorn. Extend it to wrap
slughorn::canvas::Canvas. The Canvas API maps naturally to Python:

    canvas = slughorn.Canvas(atlas, key_base)
    canvas.begin_path()
    canvas.circle(0.5, 0.5, 0.4)
    canvas.fill(slughorn.Color(1, 0, 0, 1))
    canvas.finalize_as(slughorn.Key.from_string("my_scene"))

Method naming: follow Python convention (snake_case). bezierTo -> bezier_to, etc.
See ai/context-python.md for Python binding conventions in this project.

### Skia backend redesign
context-core.md flags Skia as "known issues, pending redesign to match
Cairo/NanoSVG API." slughorn-canvas.hpp now serves as the reference for what
a clean slughorn backend looks like. When redesigning slughorn-skia.hpp,
match the decomposePath / loadShape / loadImage pattern from slughorn-cairo.hpp
and slughorn-nanosvg.hpp. Skia backends can opt in to better arc quality by
setting decomposer.tolerance = CurveDecomposer::TOLERANCE_BALANCED.

### ai/context-todo-canvas.md (this file) - keep updated
Re-generate this file at the end of each session that modifies
slughorn-canvas.hpp or CurveDecomposer. The canonical prompt is:
    "Please generate an updated ai/context-todo-canvas.md reflecting
     today's changes."

---

## Test cases

### Basic shapes
- Triangle (3 lineTo + close) -> expect 6 curves (2 per segment)
- Rect -> expect 8 curves (4 edges x 2 quads each)
- Circle via circle() at TOLERANCE_EXACT -> expect 8 curves (4 cubics x 2 quads)
- Circle via circle() at TOLERANCE_BALANCED -> expect ~32 curves

### arc() / arcTo()
- arc() full circle (0 -> 2?) -> same curve count as circle()
- arc() semicircle (0 -> ?) -> roughly half the curves of a full circle
- arc() with ccw=true -> mirror image winding of ccw=false
- arcTo() right-angle corner -> single quarter-circle arc
- arcTo() degenerate (r=0) -> falls back to lineTo(x1,y1), no crash
- arcTo() collinear points -> falls back to lineTo(x1,y1), no crash
- arcTo() zero-length leg -> falls back to lineTo(x1,y1), no crash
- arcTo() right-angle corner -> bridge lineTo() must be zero-length when
  current point is already on the tangent (regression test for Day 8 artifact)

### Adaptive subdivision
- High-curvature cubic (tight S-curve) -> more than 2 quads at BALANCED
- Nearly-straight cubic -> exactly 2 quads at BALANCED (no subdivision)
- TOLERANCE_EXACT -> always exactly 2 quads regardless of curvature
- TOLERANCE_DRAFT vs TOLERANCE_FINE -> measurable curve count difference
- MAX_DEPTH clamp: degenerate cubic -> at most 2^8 x 2 = 512 quads

### Scale contract
- fill() with scale=2.0 -> Layer::scale must still be 1.0
- Curve coordinates in Layer's Shape must be in [0,1] after normalization

### Local origin
- Shape at canvas (100,200) with scale=0.01 -> Layer::transform.dx?1.0, dy?2.0
- Shape's Atlas::Shape bearingX/Y must be near 0 (local, not canvas origin)

### CompositeShape accumulation
- Three fill() calls -> layerCount() == 3
- finalize() -> returns composite with 3 layers, layerCount() resets to 0
- finalizeAs(key) -> composite registered in atlas, state reset
- beginComposite() mid-session -> discards accumulated layers cleanly

### Compound paths / winding
- Outer CCW rect + inner CW circle (bridged) -> correct hole rendering
- Confirmed working via osgSlug renderer (Day 8 screenshot evidence)

### Tolerance constants
- TOLERANCE_EXACT -> same curve count as pre-adaptive CurveDecomposer
- TOLERANCE_DRAFT -> fewer curves than BALANCED, faster atlas build
- Ghostscript Tiger SVG -> should stay well under 300 curves at TOLERANCE_BALANCED
