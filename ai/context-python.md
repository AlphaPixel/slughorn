# slughorn - Python Context

## Files

```
ext/slughorn-python.cpp       - pybind11 bindings
test/slughorn_render.py       - software shader emulator + image output
test/slughorn_export.py      - uses slughorn_render.py to output raster/SVG images
test/slughorn_serial.py       - pure-Python .slug/.slugb reader/writer (DEFERRED)
```

Raw URLs:
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/ext/slughorn-python.cpp
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_render.py
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_export.py
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_serial.py

## pybind11 bindings - current surface (slughorn-python.cpp)

Module: `slughorn`

Types exposed at module level (flat - see scoping note in file header):

```
slughorn.Key              fromCodepoint(uint32_t) / from_string(str)
                          __hash__ / __eq__ - usable as dict key
slughorn.Key.Type          Codepoint / Name enum
slughorn.KeyIterator      prefix (str) + counter (uint32_t), next() -> Key
                          __iter__ / __next__ - usable as Python iterator
slughorn.Color            r, g, b, a  (default opaque black)
slughorn.Matrix           xx, yx, xy, yy, dx, dy  (default identity)
                          identity(), is_identity(), apply(x,y), __mul__
slughorn.Layer            key, color, transform, scale, effect_id
slughorn.CompositeShape   layers, advance,  __len__
slughorn.Curve            x1,y1, x2,y2, x3,y3  (quadratic Bezier)
slughorn.ShapeInfo        curves, auto_metrics, bearing_x/y, width, height,
                          advance, num_bands_x, num_bands_y
slughorn.Shape            (read-only) all band/metric fields +
                          em_origin, em_size, em_to_uv(ex, ey)
slughorn.TextureData      width, height, format ("RGBA32F"/"RGBA16UI"),
                          bytes (zero-copy memoryview - keep Atlas alive)
slughorn.Atlas            add_shape, add_composite_shape, build, is_built,
                          get_shape, get_composite_shape, has_key,
                          get_shapes, get_composite_shapes,
                          curve_texture, band_texture
slughorn.CurveDecomposer  move_to, line_to, quad_to, cubic_to,
                          get_curves, clear, __len__
slughorn.emoji            submodule - Unicode 15.1 CLDR lookup table
```

Serial functions (only present when built with SLUGHORN_SERIAL=ON):

```
slughorn.read(path)       -> shared_ptr<Atlas> (is_built immediately)
slughorn.write(atlas, path)  extension determines format (.slug / .slugb)
```

FreeType submodule (only present when built with SLUGHORN_FREETYPE=ON):
  slughorn.freetype.load_ascii_font(path, atlas)             -> bool
  slughorn.freetype.load_emoji_font(path, codepoints, atlas) -> dict[int, CompositeShape]

Canvas submodule (always present):
  slughorn.canvas.Canvas(atlas, key_iterator)
    begin_path, move_to, line_to, quad_to, bezier_to, close_path
    rect, rounded_rect, circle, ellipse, arc, arc_to
    fill(color, scale) -> Key
    define_shape(key, scale) -> bool
    begin_composite, set_advance, finalize() / finalize(key)
    layer_count, has_pending_path, decomposer()

## slughorn_render.py - architecture

Three levels, each building on the one below:

**Level 1 - Pure math** (no slughorn import):

* `calc_root_code`, `solve_horiz_poly`, `solve_vert_poly`, `calc_coverage`
* `render_sample(curves, render_coord, pixels_per_em)` - ground truth, all curves, no bands
* `render_sample_banded(curves, hbands_idx, vbands_idx, ...)` - mirrors GPU shader exactly

**Level 2 - Atlas bridge (temporary)**:

* `AtlasView(atlas, key)` - decodes a built Atlas into Python-native structures
  that `render_sample_banded` can consume. Constructed once per (atlas, key) pair.

  Attributes:

  * `shape`
  * `curves` (flat tuples)
  * `curve_list` (Curve objects)
  * `hbands_idx`
  * `vbands_idx`

  AtlasView is a temporary bridge and will be replaced by a C++-backed
  `DecodedShape` exposed via pybind11.

**Level 3 - Grid samplers + image output**:

* `sample_grid(curves, size, margin)` - reference path, pure Curve list
* `sample_grid_from_atlas(atlas, key, size, margin, banded)` - uses AtlasView
* `save_image(grid, filename, flip_y)`
* `save_curves_debug(curves, shape, filename, scale)` - geometry diagram PNG
* `print_grid(grid)` - ASCII art to stdout

## AtlasView - temporary shader simulation bridge

Decodes raw texture bytes once into Python-native lists so per-pixel
render_sample_banded calls are feasible. NOT a data model.

Hidden assumption in _decode_band_texture: loc_to_index computes curve
index algebraically, assuming dense packing at texel positions 0,2,4...
Will break if packTextures() changes. Fix: build explicit (cx,cy)->index
map during decode instead.

Migration path: Atlas.decode(key) -> DecodedShape (C++/pybind11)
DecodedShape provides curves, hbands/vbands, and render_sample_banded()
natively. AtlasView is removed after this lands.

### Migration path (current direction)

Decoding and banded rendering move into C++ via pybind11:

```
Atlas.decode(key) -> DecodedShape
```

Where `DecodedShape` provides:

* curves (vector<Curve>)
* hbands / vbands (curve index lists)
* render_sample_banded()

After this:

* AtlasView becomes a thin wrapper or is removed entirely
* Python tools operate on DecodedShape instead

Refer to `ai/context-todo-pybind.md` for full implementation details.

## slughorn_serial.py - status: DEFERRED

Pure-Python reader/writer for .slug/.slugb.

Deferred because:

* duplicates C++ types
* adds complexity before pybind path is proven
* currently out of sync with bindings

**Do not use until pybind path is fully validated.**

## Forward-looking: the debugging / inspector goal

Python is the **experimentation and visualization layer**, not the production renderer.

Goals:

* visualize band coverage
* inspect per-band curve membership
* heatmap iteration cost
* experiment with band dimensions and offsets
* eventually auto-optimize band layouts

Architecture:

```
slughorn_render.py      <- reference math (truth)
DecodedShape (C++)      <- fast execution + decode
slughorn_export.py     <- harness (exports rasterized PNGs and outling SVGs)
slughorny   <- [future] interactive debugger/visualizer/optimizer/etc; will use PySide6
```

## TODOs

- Implement Atlas.decode(key) -> DecodedShape in C++/pybind11
- Replace AtlasView with DecodedShape
- numpy-vectorize render_sample / render_sample_banded (intermediate speedup)
- Bind Cairo / NanoSVG / Skia backends
- Revive slughorn_serial.py once pybind path is stable
- KeyIterator: support true Key instances + string-mode _N suffix increment

## Performance notes

render_sample and render_sample_banded are pure Python per-pixel per-curve.
Banded path is already ~5x+ faster - always prefer it.
Two planned speedup approaches in order of effort:
1. numpy vectorization across curves per pixel (no C++ required)
2. DecodedShape migration (the real fix, eliminates AtlasView entirely)
