# slughorn / osgSlug - Core Context

## What this project is

A GPU-native vector shape renderer based on the Slug algorithm (Lengyel 2017).
Renders closed quadratic Bezier outlines per-fragment - no tessellation,
resolution-independent, correct under perspective.

Built in two layers:
- **slughorn** - pure C++, no graphics dependencies. Builds curve + band textures
  as raw byte buffers. Public output is `TextureData` for the caller to upload.
- **osgSlug** - OSG adapter. Adds texture upload, ShapeDrawable, Text, and Font.

## Repository

    https://github.com/AlphaPixel/slughorn

Raw file base: `https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/`

Key files:
    slughorn.hpp
    slughorn.cpp
    slughorn-ft2.hpp
    slughorn-skia.hpp
    slughorn-cairo.hpp
    slughorn-nanosvg.hpp
    slughorn-serial.hpp
    ext/slughorn-python.cpp
    test/slughorn_render.py
    test/slughorn_serial.py
    test/slughorn_example.py

AI/LLM context bootstrap files:
    ai/context-core.md (THIS FILE)
    ai/context-python.md (Python-centric)

## Key types (slughorn.hpp)

- `Atlas` - add shapes/composites, call `build()`, get `TextureData` back
- `Atlas::Shape` - output: band texture coords, band transform, metrics
- `Atlas::Shape::computeQuad(transform, scale=1, expand=0)` - world-space quad
- `Atlas::TextureData` - raw pixel buffer (RGBA32F curve tex, RGBA16UI band tex)
- `Atlas::PackingStats` - build-time diagnostic
- `Atlas::ShapeInfo` - input descriptor: curves, autoMetrics, numBandsX/Y
- `Quad` - `{ x0, y0, x1, y1 }` bottom-left and top-right corners
- `CurveDecomposer` - path sink: moveTo/lineTo/quadTo/cubicTo/close -> Curves
- `Color` - `{ r, g, b, a }`
- `Matrix` - 6-float column-major affine: xx, yx, xy, yy, dx, dy
- `Key` - discriminated union: `fromCodepoint(uint32_t)` or `fromString(string)`
- `Layer` - `{ Key, Color, Matrix transform, slug_t scale, uint32_t effectId }`
- `CompositeShape` - `{ vector<Layer>, advance }`
- `slug_t = float`, `_cv` UDL, `cv()` cast helper ("cv" means "curve value")


## Scale contract - CRITICAL

Two distinct uses of "scale" that must never be conflated:

**Backend normalization scale** - internal to each backend, never stored on Layer:
- Cairo: scale param to `decomposePath()` normalizes canvas pixels to em-space
- NanoSVG: always `1/image->width` internally, never exposed to caller
- FreeType2: `1/unitsPerEM`, internal only

**Layer scale** - stored on `Layer::scale`, read by `computeQuad()` and `compile()`:
- SVG / Cairo / NanoSVG: always `1.0` (already normalized to em-space)
- Text / FreeType2: `_fontSize` - scales em-space glyphs to world units

The two must never be conflated. `Layer::scale` is not the normalization factor.
If Text / FreeType2 continues to be the ONLY way `scale` is used, it might be
worth REMOVING IT from `Layer` and punting the reponsibility to the caller.


## SLUG_EXPAND - CRITICAL

`expand` in `compile()` must NEVER be derived from scale. It is a fixed constant:

    // Drawable.hpp
    static constexpr float SLUG_EXPAND = 0.01f;

    // Drawable.cpp compile()
    const slughorn::Quad q = shape->computeQuad(layer.transform, layer.scale, cv(SLUG_EXPAND));

`expand` is the AA fringe in em-space units. Small and fixed. Not related to
world size or font size. The old `expand = 1/scale` was always wrong.


## computeQuad - current signature

    Quad computeQuad(
        const Matrix& transform,
        slug_t scale = 1.0_cv,
        slug_t expand = 0.0_cv
    ) const {
        const slug_t ox = transform.dx * scale;
        const slug_t oy = transform.dy * scale;
        return {
            ox + (bearingX - expand) * scale,
            oy + (bearingY - height - expand) * scale,
            ox + (bearingX + width  + expand) * scale,
            oy + (bearingY          + expand) * scale
        };
    }

`computeQuad(Matrix{})` with default args = tight bounding box. No separate
`bounds()` method needed.


## autoMetrics behavior

`autoMetrics=true` (default for all non-FT2 backends) sets:

    bearingX = minX   (left edge of bounding box)
    bearingY = maxY   (top edge, Y-up)
    width    = rangeX
    height   = rangeY

In plain terms:

    left   = bearingX
    right  = bearingX + width
    top    = bearingY
    bottom = bearingY - height


## Y convention

slughorn makes no Y convention assumption. Cairo backend is authored in Y-down
space; OSG/OpenGL interprets as Y-up. Shapes render "upside down" relative to
Cairo authoring space. This is expected and consistent.

Use `osg::MatrixTransform::rotate(90deg around X)` to bring SVG content upright
in a Z-up OSG scene.


## Texture packing - key invariants

- Textures grow vertically: always `TEX_WIDTH` (512) wide, height from content
- `alignCursorForSpan()` ensures no band curve list straddles a row boundary
  (required for correct Slug shader behaviour)
- `TEX_WIDTH` (512) must match `kLogBandTextureWidth` (9) in fragment shader
- Curve padding is always 0 (mathematical guarantee - curves pack in spans of
  exactly 2 texels; TEX_WIDTH is even; cursor starts at 0 and advances by 2)
- Band padding is typically very low (<1%) for real SVG content


## PackingStats

Populated by `build()`, accessible via `getPackingStats()`. Build-time diagnostic
only - not included in serialization (serial path bypasses `packTextures()`).

Real-world stats for AlphaPixel logo SVG (~20 layers):

    curve: 1652 used + 0 padding / 2048 total  (80% util, 0% pad)
    band:  3791 used + 29 padding / 4096 total  (92% util, 0.7% pad)


## ShapeDrawable::compile() - current pattern

    for(const auto& layer : _layers) {
        const slughorn::Atlas::Shape* shape = _atlas->getShape(layer.key);
        if(!shape) continue;

        const slughorn::Quad q = shape->computeQuad(
            layer.transform, layer.scale, cv(SLUG_EXPAND));

        const float emX0 = static_cast<float>(shape->bearingX) - SLUG_EXPAND;
        const float emY0 = static_cast<float>(shape->bearingY - shape->height) - SLUG_EXPAND;
        const float emX1 = static_cast<float>(shape->bearingX + shape->width) + SLUG_EXPAND;
        const float emY1 = static_cast<float>(shape->bearingY) + SLUG_EXPAND;
        // ...
    }


## Fragment shader debug modes (osgSlug-frag.glsl)

    0 - normal rendering via slug_ApplyEffect
    1 - checkerboard per band cell
    2 - 1px band edge lines (AA via fwidth/smoothstep)
    3 - colored quad borders + shape fill underneath
    4 - iteration heatmap (blue=cheap, green=moderate, red=expensive)
    5 - heatmap + 1px band grid overlay


## Shader vertex attribute layout

    0: a_position  (vec3)
    1: a_color     (vec4)
    2: a_emCoord   (vec2)
    3: a_bandXform (vec4)  - bandScaleX/Y, bandOffsetX/Y
    4: a_glyphData (vec4)  - bandTexX/Y, bandMaxX/Y
    5: a_effectId  (float) - flat varying

Uniforms: `slug_curveTexture` (unit 0), `slug_bandTexture` (unit 1),
`slug_effectTexture` (unit 2), `slug_debugMode` (int), `osg_SimulationTime` (float)


## Backend status

- **FreeType2** - stable, do not touch
- **Cairo** - stable
- **NanoSVG** - redesigned, tests passing (43/43)
- **Skia** - known issues, pending redesign to match Cairo/NanoSVG API
- **Serialization** - stable


## Serial API (slughorn-serial.hpp)

Requires `SLUGHORN_SERIAL=ON` at CMake time (pulls in nlohmann/json).
CMake propagates `SLUGHORN_HAS_SERIAL` define to all consumers.

    // Write (.slug = JSON+base64, .slugb = binary - extension determines format)
    slughorn::serial::write(atlas, "logo.slug");
    slughorn::serial::write(atlas, "logo.slugb");

    // Read (format auto-detected from file header)
    slughorn::Atlas atlas = slughorn::serial::read("logo.slug");
