# slughorn - Python Context

## Files

```
ext/slughorn-python.cpp       - pybind11 bindings
test/slughorn_render.py       - software shader emulator + image output
test/slughorn_example.py      - end-to-end harness (pure pybind11, no serial.py)
test/slughorn_serial.py       - pure-Python .slug/.slugb reader/writer (DEFERRED)
```

Raw URLs:
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/ext/slughorn-python.cpp
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_render.py
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_example.py
https://raw.githubusercontent.com/AlphaPixel/slughorn/refs/heads/main/test/slughorn_serial.py

## pybind11 bindings - current surface (slughorn-python.cpp)

Module: `slughorn`

Types exposed at module level (flat - see scoping note in file header):

```
slughorn.Key              fromCodepoint(uint32_t) / from_string(str)
                          __hash__ / __eq__ - usable as dict key
slughorn.KeyType          Codepoint / Name enum
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

Backend submodules (ft2, skia, cairo) are stubbed but not yet bound.

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

## AtlasView - why it exists (and why it will go away)

`render_sample_banded` is a Python emulator of the GPU fragment shader. The
shader performs hardware texture fetches; Python cannot. `AtlasView.__init__`
decodes the raw texture bytes once into Python-native lists:

* `self.curves` - flat list of `(x1,y1,x2,y2,x3,y3)` tuples (curve texture unpacked)
* `self.hbands_idx` / `self.vbands_idx` - lists-of-lists of int indices (band texture decoded)

Without this one-time decode, `render_sample_banded` would re-parse both textures
via numpy at every pixel call - completely infeasible.

**AtlasView is NOT a data model - it is a shader simulation bridge.**

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

## slughorn_render.py - role after migration

The reference math in `slughorn_render.py` remains unchanged.

* It continues to serve as:

  * correctness oracle
  * debugging tool
  * algorithm reference

* The performance-critical path (banded rendering) migrates to C++

This ensures:

* shader parity is always verifiable
* Python remains simple and inspectable

## AtlasView._decode_band_texture - hidden assumption

`loc_to_index` computes curve index algebraically:

```
def loc_to_index(cx: int, cy: int) -> int:
    return (cy * BAND_TEX_WIDTH + cx) // 2
```

This assumes curves are densely packed at texel positions 0,2,4,6... in scan order.

This is true for current `packTextures()` but will break if packing changes.

**Future fix:**

* Build explicit `(cx, cy) -> index` map during decode instead of relying on math.

## slughorn_serial.py - status: DEFERRED

Pure-Python reader/writer for .slug/.slugb.

Deferred because:

* duplicates C++ types
* adds complexity before pybind path is proven
* currently out of sync with bindings

**Do not use until pybind path is fully validated.**

## Known bugs fixed this session (Day 7.5)

1. `Layer.scale` missing from bindings - now fixed
2. `num_bands` typo - replaced with `num_bands_x / num_bands_y`

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
slughorn_example.py     <- harness
slughorn_inspector.py   <- [future] interactive debugger
```

## CompositeShape rendering - design questions (UNRESOLVED)

(No changes - still valid as written)

## TODOs - Python layer

* [x] Run `slughorn_example.py --selftest`
* [ ] Run `slughorn_example.py <real.slug>` end-to-end
* [ ] Resolve CompositeShape rendering design
* [ ] Implement CompositeShape rendering

### pybind-driven migration

* [ ] Implement `Atlas.decode(key) -> DecodedShape` in pybind11
* [ ] Move `render_sample_banded` into C++
* [ ] Replace AtlasView usage with DecodedShape
* [ ] Keep Python renderer as validation/reference

### future work

* [ ] Fix `loc_to_index` assumption
* [ ] Revive `slughorn_serial.py`
* [ ] Bind FT2 / Cairo / NanoSVG
* [ ] nanobind port
* [ ] `slughorn_inspector.py`
