# slughorn

`slughorn` is modern C++20 library implementing the recent OSS release of the
"Slug" GPU vector graphics rendering technique by [Eric Lengyel](https://terathon.com/blog/).
It makes no assumptions about what graphics environment being used (OpenGL,
Vulkan, WebGL, WebGPU, DirectX, etc) and instead focuses *only* on simplifying
the process of creating/ingesting vector data from various backends--or,
alternatively, by using an HTML "Canvas-like" API directly in code--so it can be
directly uploaded to the GPU as easily as possible. A backend is simply an
implementation that produces Bezier curves for `slughorn` to process to
facilitate drawing. Further code outside of `slughorn` draws the curves with a
graphics API like OpenGL, Vulkan, WebGL, WebGPU, Metal, DirectX or similar,
potentially using toolkits like but not limited to OSG / OpenSceneGraph or VSG /
VulkanSceneGraph. Companion projects ( https://github.com/AlphaPixel/osgSlug )
assist with this integration.

Furthermore, `slughorn` provides tools for traditional "offline asset"
processing (primarily in Python), as well as a [glTF](#)-compatible JSON file
format.

# Current Supported Backends

- NanoSVG
- Skia
- Cairo
- FreeType

In addition to the above backends, `slughorn` provides a "native" API for
authoring vector graphics inspired by the standard HTML `Canvas` element.

Adding support for other backends is generally as easy using a single helper
class: `slughorn::CurveDecomposer`. If your vector data can be reduced into
simple quadratic Bezier curves, `slughorn` can make it render.

## Supported Backends: FreeType

The FreeType2 backend supports standard OTF/TTF glyphs, all of the COLRv0
specification, and a large portion of the COLRv1 spec!

# Demos

> **Note on demos:** All video demonstrations below are captured from
> [osgSlug](https://github.com/AlphaPixel/osgSlug), the intentionally separate
> OpenSceneGraph-based testbed developed alongside `slughorn`. `slughorn` itself
> has no graphics dependencies, and `osgSlug` exists to prove the integration
> story and serve as a reference implementation for other graphics backends.

<table>
  <thead>
    <tr>
      <th align="center">Preview</th>
      <th align="left">Description</th>
    </tr>
  </thead>

  <tbody>
    <tr>
      <td align="center">
        <img
          src="https://ambaince.com/osgSlug/emojis.png"
          alt="Emoji Demo"
          width="300">
      </td>
      <td>
        <strong>Emoji</strong><br><br>
        Demonstrates loading **COLRv0** and **COLRv1** emojis using the
        `slughorn-freetype.hpp` backend. Each "layer" of the emoji as composited
        into its own quad and positioned relative to the "base."<p>*NOTE*: Not
        all features of **COLRv1** are currently supported, but will be soon.
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/glyph-animate.mp4">
          <img
            src="https://ambaince.com/osgSlug/glyph-animate.webp"
            alt="Animated Glyphs Demo"
            width="300">
        </a>
      </td>
      <td>
        <strong>Animated Glyphs</strong><br><br>
        Shader-driven animation applied to Slug-rendered glyph geometry,
        accomplished by adjusting the output positions in the vertex shader.
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/logo.mp4">
          <img
            src="https://ambaince.com/osgSlug/logo.webp"
            alt="Layer Effects Demo"
            width="300">
        </a>
      </td>
      <td>
        <strong>Layer Effects</strong><br><br>
        Shows how individual layers in a `CompositeShape` (the rectangular
        regions) can not only be distinguished in the shader pipeline, but also
        how each layer can have its own unique fragment output. From left to
        right: basic fill, two GLSL algorihmic fills, traditional texture lookup
        and finally an animated GLSL algorithmic fill.
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/morph.mp4">
          <img
            src="https://ambaince.com/osgSlug/morph.webp"
            alt="Morphing Demo"
            width="300">
        </a>
      </td>
      <td>
        <strong>Morphing</strong><br><br>
        Similar to the "animated glyphs" demo, this example shows both the
        "shape" (the simple triangle) and the "debugging bounding box" (using
        the `OSGSLUG_DEBUG=3` environment variable).
      </td>
    </tr>

    <tr>
      <td align="center">
        <img
          src="https://ambaince.com/osgSlug/project2d.png"
          alt="2D Demo"
          width="300">
      </td>
      <td>
        <strong>2D Projection</strong><br><br>
        The standard orthographic-style 2D placement/rendering.
      </td>
    </tr>

    <tr>
      <td align="center">
        <img
          src="https://ambaince.com/osgSlug/project3d.png"
          alt="3D Demo"
          width="300">
      </td>
      <td>
        <strong>3D Projection</strong><br><br>
        The same scene as the 2D Projection above, but with a traditional
        perspective-style view.
      </td>
    </tr>

    <tr>
      <td align="center">
        <img
          src="https://ambaince.com/osgSlug/shapes-compositeshapes.png"
          alt="Shapes/CompositeShapes"
          width="300">
      </td>
      <td>
        <strong>Shapes/CompositeShapes</strong><br><br>
        Each backend has its own examples of how to create both simple `Shape`
        and `CompositeShape` objects (groups of `Shape` instances layered
        together). The last screenshot demonstrates the `slughorn-canvas.hpp`
        backend, in addition to showing how the traditional "punch out" effect
        can be achieved by manually using opposite winding directions in order
        to "cut out" one closed path from another.<p>*NOTE*: The `skia` backend
        is currently the **only** backend that supports a true "stroke to path"
        feature (the jigsaw puzzle piece).
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/sphere3d.mp4">
          <img
            src="https://ambaince.com/osgSlug/sphere3d.webp"
            alt="3D Objects"
            width="300">
        </a>
      </td>
      <td>
        <strong>3D Objects</strong><br><br>
        Slug isn't restricted to simple quads; any 3D object/mesh can be
        assigned compatible `slughorn` coordiante mappings (such as spheres,
        cubes, curved surfaces, etc).
      </td>
    </tr>

    <tr>
      <td align="center">
        <img
          src="https://ambaince.com/osgSlug/svgs.png"
          alt="SVG Demo"
          width="300">
      </td>
      <td>
        <strong>SVG</strong><br><br>
        SVG content fits easily within the `slughorn` ecosystem via the
        `slughorn-nanosvg.hpp` backend.<p>*NOTE*: Similar to the emoji support,
        gradients are not currently supported.
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/text-mix.mp4">
          <img
            src="https://ambaince.com/osgSlug/text-mix.webp"
            alt="Mixed Text Demo"
            width="300">
        </a>
      </td>
      <td>
        <strong>Mixed Text</strong><br><br>
        Text glyphs are simply an instance of `Shape`, and can be freely mixed
        with any **other** `Shape/CompositeShape` object. This example
        demonstrates replacing the character `F` with a simple triangle, which
        fits seamlessly into the layout process.
      </td>
    </tr>

    <tr>
      <td align="center">
        <a href="https://ambaince.com/osgSlug/text.mp4">
          <img
            src="https://ambaince.com/osgSlug/text.webp"
            alt="Text Demo"
            width="300">
        </a>
      </td>
      <td>
        <strong>Text</strong><br><br>
        Text was the original inspiration for Slug, and will always be
        incredibly well-supported. As mentioned above, each glyph in a text
        layout is nothing more than an instance of `Shape`, and can be
        manipulated in any way you can imagine. :)
      </td>
    </tr>
  </tbody>
</table>

# TODO

## Curve/Band Texture Magic Number Sentinel

Unbound/mismatched band/curve textures cause runaway per-fragment iteration,
leading to GPU hang and system lock. A magic number at texel (0,0) or at the END
of each row (where there is almost ALWAYS extra bytes)
checked by the shader would allow clean discard instead of GPU DoS.

This would require:

- Writing 0x534C5547 ("SLUG") from C++ during packTextures()
- Adding guard in the fragment shader before any band reads

Avoidable by careful uniform naming. High value as a robustness/debug aid.

## Soon

- [ ] Add `KeyIterator` to `slughorn::nanosvg::load` (instead of forcing numeric
  codepoint)
- [ ] Emphasize `osgSlug` as the "testbed" for visualization (when ready)
- [x] Change `slughorn_render.py` to dump SVG for the `save_curves_debug`
- [ ] Add CMake helpers for compiling in each backend
- [ ] Slug "mip-mapping"! Starting with osgSlug, introduce the ability to
  "short-circuit" the Slug quad based on some "level of detail" rule; when the
  shape is some threshold of distance AWAY, revert to simple texture lookup
  approximations. NOTE: `slughorn` MIGHT be able to participate in this
  optmization as well ... somehow
- [ ] Fix `numBands*` auto-calculation and account for spatial curve
  distribution, not just count
- [x] pybind11 wrapper
- [ ] Change `composites` to `compositeShapes` in serialization, etc
- [x] Change the `autoMetrics` default to `true`
- [x] Rename `slughorn-ft2.hpp` to `slughorn-freetype.hpp`
- [ ] Enforce VERSION compatibility in backends
- [ ] UDL types for `slughorn::Key::from{String,Codepoint}`, potentially as
  `_ukey`, `_skey` or similar?

## Medium Term

- [ ] Remove `slughorn::Key::from*` helpers (if the ctors really cover
  everything)
- [ ] Move `slug_t` into `slughorn::literals`, and introduce:
  ```
  using slugf_t = float;
  using slugi_t = std::uint16_t;
  using slugid_t = std::uint32_t;
  ```
- [ ] Introduce `slughorn-harfbuzz.hpp` text API, using Harfbuzz to "shape" it
  properly. Note: it will necessarily NEED to be built on top of the FreeType2
  backend (`slughorn-freetype.hpp`)!
- [ ] Qt6 QPainterPath provides moveTo / lineTo / quadTo / cubicTo /
  closeSubpath via elementAt() iteration; maps cleanly to CurveDecomposer.
  Quadratics are native to Qt6 (unlike Cairo which works in cubics). QFont /
  QRawFont provide glyph outline extraction as a potential FreeType2 complement.
  QSvgRenderer provides SVG loading as a potential NanoSVG complement.
  Structure to match slughorn-cairo.hpp: decomposePath(QPainterPath, Atlas&).
- [ ] Allow `serial::writeJSON` for ANY object (not JUST `Atlas`)
- [x] Simulate a fragment processing frame in Python and inspect the math
- [ ] Tooling for "optimizing" `Atlas::build`
- [ ] Generate tight carrier mesh from Slug
  coverage via sampling + contour extraction + triangulation (offline). Preserve
  em-space UVs.
- [ ] Generate SDF/MSDF texture data with Python emulator

## When Ready

- [x] `Atlas::Key` type conversion (`uint32_t` -> `Codepoint | Name`
  discriminated union)
- [x] Conic subdivision for Skia circular geometry (`iter.conicWeight()`)
- [x] Minimal Skia `args.gn` build config (trim from 25GB)
- [x] Non-square band grids (`bandMaxX != bandMaxY`)

## Someday / Fun

- [ ] Live `numBands` slider tool with real-time rebuild + heatmap feedback
- [ ] VSG adapter (trivial now given `slughorn` separation)

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

The key word is *tessellation*; Skia converts your curves into triangle meshes.
This means:

- Quality is **resolution-dependent**; you have to choose a tessellation
  tolerance, and if you zoom in far enough you'll see faceting
- Every time the path **transforms** (scale, rotation, perspective), you either
  re-tessellate or accept quality loss
- **Perspective projection** of curves is fundamentally broken with
  tessellation; a cubic Bezier projected through a perspective matrix is no
  longer a cubic Bezier

### What Slug Does Differently

Slug evaluates the **exact curve equations per-fragment** in the shader. This
means:

- Quality is **resolution-independent**; zoom in infinitely, edges stay perfect
- Transforms are **free**; the vertex shader handles them, the fragment shader
  doesn't care
- **Perspective is correct**; because you're evaluating the curve at each
  pixel, not approximating it with triangles
- No CPU tessellation step; the atlas is built once and never rebuilt
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

- Skia wants to **own the OpenGL context**; it manages its own state, its own
  FBOs, its own texture atlases. OSG also wants to own the context. Getting them
  to share without stomping each other's state is a known headache.
- Skia's GPU API is **not designed to be embedded**; it's designed to be the
  renderer, not a component of one.
- You'd essentially be running two renderers side by side and compositing, which
  introduces synchronization, blending, and depth buffer problems.

Slug, on the other hand, is just **a shader and two textures**; it slots into
OSG's existing pipeline as naturally as any other `osg::Program`.

### The Real Answer to "Are You Reinventing It?"

For **2D UI / document rendering at fixed resolution**; Skia is better. It's
battle-hardened, handles edge cases you haven't thought of yet, and the
tessellation quality is fine for screen-resolution work.

For **3D scene graph rendering with arbitrary transforms and perspective**,
Slug is genuinely superior. The moment you want text or shapes on a billboard, a
HUD, a curved surface, or viewed through a perspective camera, Skia's approach
starts showing cracks and Slug's approach shines.

Slug is solving a different problem that Skia deliberately doesn't address.

# AI Disclosure Declaration

AI tools such as ChatGPT and Claude have been used for research and some code
development. However, this is not a "fire and forget" AI project. It was
carefully designed, planned, architected and implemented by Jeremy 'Cubicool'
Moles, an expert in both GPU development and typography and glyph rendering.
AI tools were also used for producing documentation and ancillary data products
to accelerate development.

# Sponsorship

This project was created and implemented by Jeremy 'Cubicool' Moles, author of
the osgCairo and osgPango, as well as numerous SDF-rendering and other
glyph-centric projects. Jeremy produced this work for AlphaPixel
([AlphaPixelDev.com](https://alphapixeldev.com/)) based on the 2026 release
of the Slug algorithm ( https://terathon.com/blog/decade-slug.html) patent
by its author, Eric Lengyel, for whom the whole Slug-using community is
extremely grateful.

If you are interested in assistance with Slug, slughorn, OpenGL, OpenSceneGraph,
Vulkan, VulkanSceneGraph, AR, VR, xR, 3d mapping, or almost any other
performance-centric computing, please contact AlphaPixel. We are mercenary
programmers who can provide rapid assistance to any challenge, and our motto is
"We Solve Your Difficult Problems."
