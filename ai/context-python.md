# slughorn — Python Context

## Files

    ext/slughorn-python.cpp       — pybind11 bindings
    test/slughorn_render.py       — software shader emulator + image output
    test/slughorn_example.py      — end-to-end harness (pure pybind11, no serial.py)
    test/slughorn_serial.py       — pure-Python .slug/.slugb reader/writer (DEFERRED)

Raw URLs:
    https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/ext/slughorn-python.cpp
    https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_render.py
    https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_example.py
    https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_serial.py


## pybind11 bindings — current surface (slughorn-python.cpp)

Module: `slughorn`

Types exposed at module level (flat — see scoping note in file header):

    slughorn.Key              fromCodepoint(uint32_t) / from_string(str)
                              __hash__ / __eq__ — usable as dict key
    slughorn.KeyType          Codepoint / Name enum
    slughorn.Color            r, g, b, a  (default opaque black)
    slughorn.Matrix           xx, yx, xy, yy, dx, dy  (default identity)
                              identity(), is_identity(), apply(x,y), __mul__
    slughorn.Layer            key, color, transform, scale, effect_id
    slughorn.CompositeShape   layers, advance,  __len__
    slughorn.Curve            x1,y1, x2,y2, x3,y3  (quadratic Bézier)
    slughorn.ShapeInfo        curves, auto_metrics, bearing_x/y, width, height,
                              advance, num_bands_x, num_bands_y
    slughorn.Shape            (read-only) all band/metric fields +
                              em_origin, em_size, em_to_uv(ex, ey)
    slughorn.TextureData      width, height, format ("RGBA32F"/"RGBA16UI"),
                              bytes (zero-copy memoryview — keep Atlas alive)
    slughorn.Atlas            add_shape, add_composite_shape, build, is_built,
                              get_shape, get_composite_shape, has_key,
                              get_shapes, get_composite_shapes,
                              curve_texture, band_texture
    slughorn.CurveDecomposer  move_to, line_to, quad_to, cubic_to,
                              get_curves, clear, __len__
    slughorn.emoji            submodule — Unicode 15.1 CLDR lookup table

Serial functions (only present when built with SLUGHORN_SERIAL=ON):

    slughorn.read(path)       → shared_ptr<Atlas> (is_built immediately)
    slughorn.write(atlas, path)  extension determines format (.slug / .slugb)

Backend submodules (ft2, skia, cairo) are stubbed but not yet bound.


## slughorn_render.py — architecture

Three levels, each building on the one below:

**Level 1 — Pure math** (no slughorn import):
- `calc_root_code`, `solve_horiz_poly`, `solve_vert_poly`, `calc_coverage`
- `render_sample(curves, render_coord, pixels_per_em)` — ground truth, all curves, no bands
- `render_sample_banded(curves, hbands_idx, vbands_idx, ...)` — mirrors GPU shader exactly

**Level 2 — Atlas bridge**:
- `AtlasView(atlas, key)` — decodes a built Atlas into Python-native structures
  that `render_sample_banded` can consume. Constructed once per (atlas, key) pair.
  Attributes: `shape`, `curves` (flat tuples), `curve_list` (Curve objects),
  `hbands_idx`, `vbands_idx`

**Level 3 — Grid samplers + image output**:
- `sample_grid(curves, size, margin)` — reference path, pure Curve list
- `sample_grid_from_atlas(atlas, key, size, margin, banded)` — uses AtlasView
- `save_image(grid, filename, flip_y)`
- `save_curves_debug(curves, shape, filename, scale)` — geometry diagram PNG
- `print_grid(grid)` — ASCII art to stdout


## AtlasView — why it exists

`render_sample_banded` is a Python emulator of the GPU fragment shader. The
shader does hardware texture fetches; Python cannot. `AtlasView.__init__` decodes
the raw texture bytes once into Python-native lists:

- `self.curves` — flat list of `(x1,y1,x2,y2,x3,y3)` tuples (curve texture unpacked)
- `self.hbands_idx` / `self.vbands_idx` — lists-of-lists of int indices (band texture decoded)

Without this one-time decode, `render_sample_banded` would re-parse both textures
via numpy at every pixel call — 65,536 `frombuffer` calls for a 256×256 render.

**`AtlasView` is NOT like `SlugShape` / `SlugAtlasData` in `slughorn_serial.py`.**
It is a shader simulator bridge, not a data wrapper or Atlas substitute.

**Long-term migration path:** Add `Atlas.get_decoded_curves()` and
`Atlas.get_decoded_bands(key)` to the pybind11 bindings. C++ decodes its own
textures once and hands Python clean lists. `AtlasView` then becomes a thin
wrapper or disappears. The render math in `slughorn_render.py` stays untouched.


## AtlasView._decode_band_texture — hidden assumption

`loc_to_index` computes curve index algebraically:

    def loc_to_index(cx: int, cy: int) -> int:
        return (cy * BAND_TEX_WIDTH + cx) // 2

This assumes curves are densely packed at texel positions 0, 2, 4, 6... in
scan order — true for current `packTextures()` which always places curve `i`
at texel `i*2`. When non-uniform band offsets are implemented, the curve layout
may shift and this inverse mapping would silently return wrong indices.

**Fix when that time comes:** decode the curve texture first and build an explicit
`(cx, cy) → index` dict rather than computing it algebraically.


## slughorn_serial.py — status: DEFERRED

Pure-Python reader/writer for .slug/.slugb with no compiled module dependency.
Useful for tooling on machines without the C++ extension built.

Currently deferred because:
- It duplicates all C++ types as shadow Python classes (SlugShape, SlugColor,
  SlugMatrix, SlugKey, SlugLayer, SlugCompositeShape, SlugAtlasData)
- The duck-typing gymnastics (`isinstance(atlas, SlugAtlasData)` vs real Atlas)
  add noise before the pybind11 path is even proven to work
- It has fallen behind the current API (Key type enum access uses `.type == 0`
  instead of `== Key.Type.Codepoint`, etc.)

**Do not use or update until slughorn_example.py runs end-to-end.**
When reviving: audit every `cpp_key.type == 0` comparison — should use the
`KeyType` enum. Audit Layer fields — `scale` was missing from the bindings
until Day 7.5 and serial.py predates that fix.


## Known bugs fixed this session (Day 7.5)

1. `slughorn-python.cpp` — `Layer` was missing `scale` field. Now exposed as
   `effect_id` (already present) + `scale` (added).

2. `slughorn_render.py` self-test (Test 2) — `info.num_bands = 2` silently set
   a non-existent attribute; pybind11 ignored it and built with auto band
   selection. Fixed to `info.num_bands_x = 2` / `info.num_bands_y = 2`.


## Forward-looking: the debugging / inspector goal

The ultimate Python goal is not just rendering — it's an interactive debugger
for the band-building algorithm:

- Visualize which band cell covers which screen region
- Show per-cell curve membership (which curves are in band (3,2)?)
- Heatmap of iteration counts (already in render_sample_banded's return dict)
- **Experiment with band XY dimensions and offsets without recompiling**
  (right now bands are uniformly spaced — quick to implement but not optimal)
- Eventually: `build(optimize=True)` — sweep band configs, minimize padding ratio

This is why `render_sample_banded` stays as close as possible to the GLSL shader.
Every deviation is a debugging hazard. The Python code is the reference against
which shader bugs get diagnosed.

Planned architecture for the inspector:

    slughorn_render.py      ← pure math, mirrors GLSL as closely as possible
    AtlasView               ← GPU memory decoder (temporary until bindings grow)
    slughorn_example.py     ← basic end-to-end harness
    slughorn_inspector.py   ← [future] interactive band debugger


## CompositeShape rendering — design questions (UNRESOLVED)

`slughorn_render.py` has NO CompositeShape support. `sample_grid_from_atlas` and
`AtlasView` both call `get_shape(key)` and fall over if the key resolves to a
CompositeShape instead (returns None).

A CompositeShape is an ordered layer stack — each `Layer` has its own `Key`
(pointing to a `Shape`), `Color`, `transform`, and `scale`. The following design
questions must be answered before implementation:

**Q1 — Compositing model:**
Each layer produces a `(color * fill)` value at each pixel. How do we combine
them? The natural answer is Porter-Duff `src-over` in layer order (bottom to top),
which matches what the GPU shader does with blending enabled. But should the Python
path support other blend modes, or just `src-over` for now?

**Q2 — Output granularity:**
Option A: render each layer to its own grid, then composite in a final pass.
  - More debuggable — you can inspect each layer's contribution independently.
  - More memory, more passes.
Option B: accumulate directly into a single RGBA grid in one pass over all layers.
  - Simpler, closer to what the GPU does.
  - Less useful for debugging individual layers.
Likely want both: a `render_composite_layers()` that returns one grid per layer,
and a `composite_layers()` that combines them — so the caller can choose.

**Q3 — Transform handling:**
Each `Layer.transform` has `dx/dy` offset (where that layer's curves originated
on the source canvas). For a CompositeShape, sampling a pixel at em-space `(px, py)`
for layer N means the effective sample coordinate for that layer's Shape is
`(px - layer.transform.dx, py - layer.transform.dy)` (accounting for scale too).
This must be correct from day one — without it, multi-layer shapes will render
all layers on top of each other at the wrong position.

Exact formula (mirrors `computeQuad` logic):

    effective_x = (px - layer.transform.dx * layer.scale) / layer.scale
    effective_y = (py - layer.transform.dy * layer.scale) / layer.scale

For SVG content `layer.scale = 1.0` so this simplifies to a straight offset.

**Q4 — Grid coordinate space:**
A single Shape samples over its own em-space `[0,1]` bounding box. A
CompositeShape may span multiple shapes with different offsets — the grid needs
to cover the *union* bounding box of all layers. How do we compute that?
`computeQuad(layer.transform, layer.scale)` gives the world-space quad for each
layer — the union of all those quads is the composite bounding box.

**Q5 — `save_curves_debug` for composites:**
Currently takes a single `curves` list and `shape`. For a composite we'd want to
draw all layers' curves in different colors, with their transforms applied, on a
single canvas. Nice-to-have for debugging but not required for correctness.

**Proposed new API surface** (not yet implemented):

    # One grid per layer (RGBA — color * fill)
    render_composite_layers(atlas, key, size, margin) → List[List[List[tuple]]]

    # Composite all layers into one RGBA grid (src-over)
    render_composite(atlas, key, size, margin) → List[List[tuple]]

    # Save an RGBA grid as PNG
    save_image_rgba(grid, filename, flip_y=True)

    # AtlasView equivalent for composites
    CompositeView(atlas, key)  — one AtlasView per layer, plus union bbox


## TODOs — Python layer

- [x] Run `slughorn_example.py --selftest` and confirm passes cleanly
- [ ] Run `slughorn_example.py <real.slug> axolotl` end-to-end
- [ ] Resolve CompositeShape rendering design questions (see section above)
- [ ] Implement CompositeShape rendering in slughorn_render.py
- [ ] Migrate `AtlasView` decode into pybind11 bindings:
      `Atlas.get_decoded_curves()` and `Atlas.get_decoded_bands(key)`
- [ ] Fix `loc_to_index` assumption before non-uniform band offsets land
- [ ] Revive and audit `slughorn_serial.py` (after pybind11 path is proven)
- [ ] Add `#ifdef SLUGHORN_HAS_SERIAL` guard to CMake for Python module build
- [ ] Bind FT2, Cairo, NanoSVG backend submodules
- [ ] nanobind port (listed in core TODOs — Python context tracks it here)
- [ ] `slughorn_inspector.py` — interactive band dimension/offset explorer
