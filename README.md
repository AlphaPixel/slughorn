# SlugHorn

Library for shoehorning the Slug text/graphics GPU rendering library
(https://sluglibrary.com) into projects.

# Skia

## Compilation

```
cd ~/dev/skia
rm -rf out/Release
bin/gn gen out/Release
ninja -C out/Release skia
```

## Why Slug Instead of Skia's GPU Backend?

### What Skia Actually Does on the GPU

Skia has two GPU backends: **Ganesh** (the older one, OpenGL/Vulkan/Metal) and
**Graphite** (the newer one, designed for modern explicit APIs). Both work by:

1. **Tessellating** paths into triangles on the CPU (or via compute shaders)
2. Uploading those triangles to the GPU
3. Rendering with relatively simple shaders

The key word is *tessellation* — Skia converts your curves into triangle meshes.
This means:

- Quality is **resolution-dependent** — you have to choose a tessellation
  tolerance, and if you zoom in far enough you'll see faceting
- Every time the path **transforms** (scale, rotation, perspective), you either
  re-tessellate or accept quality loss
- **Perspective projection** of curves is fundamentally broken with tessellation
  — a cubic Bézier projected through a perspective matrix is no longer a cubic
  Bézier

### What Slug Does Differently

Slug evaluates the **exact curve equations per-fragment** in the shader. This
means:

- Quality is **resolution-independent** — zoom in infinitely, edges stay perfect
- Transforms are **free** — the vertex shader handles them, the fragment shader
  doesn't care
- **Perspective is correct** — because you're evaluating the curve at each
  pixel, not approximating it with triangles
- No CPU tessellation step — the atlas is built once and never rebuilt
  regardless of transform

### Comparison

| | Skia GPU | Slug |
|---|---|---|
| Edge quality at scale | Depends on tolerance | Perfect always |
| Perspective correctness | Approximate | Exact |
| CPU work per frame | Re-tessellate on change | Nothing |
| GPU fragment cost | Cheap (just triangles) | More expensive |
| Setup complexity | High (full GPU framework) | Moderate |
| OSG integration | Very difficult | Natural |

### OSG Integration Specifically

Getting Skia's GPU backend working inside OSG would be genuinely painful:

- Skia wants to **own the OpenGL context** — it manages its own state, its own
  FBOs, its own texture atlases. OSG also wants to own the context. Getting them
  to share without stomping each other's state is a known headache.
- Skia's GPU API is **not designed to be embedded** — it's designed to be the
  renderer, not a component of one.
- You'd essentially be running two renderers side by side and compositing, which
  introduces synchronization, blending, and depth buffer problems.

Slug, on the other hand, is just **a shader and two textures** — it slots into
OSG's existing pipeline as naturally as any other `osg::Program`.

### The Real Answer to "Are You Reinventing It?"

For **2D UI / document rendering at fixed resolution** — Skia is better. It's
battle-hardened, handles edge cases you haven't thought of yet, and the
tessellation quality is fine for screen-resolution work.

For **3D scene graph rendering with arbitrary transforms and perspective** —
Slug is genuinely superior. The moment you want text or shapes on a billboard, a
HUD, a curved surface, or viewed through a perspective camera, Skia's approach
starts showing cracks and Slug's approach shines.

The fact that this library is built *inside OSG* is itself the answer — you're
in the domain where Slug's properties matter most. A shape rotating in
perspective with pixel-perfect edges at any zoom level is something Skia simply
cannot match without re-tessellating every frame.

**Slug is not reinventing what Skia solved — it's solving a different problem
that Skia deliberately doesn't address.**

# TODO

## Soon

- [ ] Debug dump of curve counts per glyph
- [ ] Test `numBands = 2-4` on simple arrow shape (3 curves)
- [ ] Fix `numBands` auto-calculation — account for spatial curve distribution,
  not just count
- [x] pybind11 wrapper

## Medium Term

- [ ] `Atlas::createDefaultStateSet()` member instead of free function in
  `Drawable.hpp`
- [ ] Sync `TEX_WIDTH` / `kLogBandTextureWidth` — uniform or shader preamble
  injection
- [ ] Premultiplied alpha
- [ ] Proper depth testing and render order
- [ ] Remove `slug_color` uniform remnant from shaders (color is pure vertex
  attribute now)

## When Ready

- [ ] `Atlas::Key` type conversion (`uint32_t` → `Codepoint | Name`
  discriminated union)
- [x] Conic subdivision for Skia circular geometry (`iter.conicWeight()`)
- [x] Minimal Skia `args.gn` build config (trim from 25GB)
- [ ] Non-square band grids (`bandMaxX != bandMaxY`)

## Someday / Fun

- [ ] Live `numBands` slider tool with real-time rebuild + heatmap feedback
- [ ] VSG adapter (trivial now given `slughorn` separation)
