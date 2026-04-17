## TODO(pybind): Move AtlasView + banded renderer into C++ (pybind11)

### Context

The current Python renderer (`slughorn_render.py`) includes an `AtlasView`
class that:

- Decodes `TextureData` (curve + band textures) into Python-native structures
- Builds:
  - flat curve list `(x1,y1,x2,y2,x3,y3)`
  - `hbands_idx` / `vbands_idx` (curve index lists per band)
- Provides `render_sample_banded()` as a Python implementation mirroring the GPU shader

This works correctly (banded vs reference diff = 0), but:

- Duplicates packing/decoding logic already defined in C++
- Performs heavy per-pixel work in Python (very slow for real shapes)
- Mixes low-level texture interpretation with high-level usage

---

### Goal

Introduce a **first-class decoded shape view in pybind11**, replacing `AtlasView`,
while preserving:

- Existing `.slug` / `.slugb` workflows
- Direct access to raw `TextureData` for serialization and debugging

---

### Design

#### Add: `Atlas.decode(key) -> DecodedShape`

Returns a C++-constructed object containing:

- `curves` - `vector<Curve>` (same data as Python tuples today)
- `hbands` - `vector<vector<uint32_t>>`
- `vbands` - `vector<vector<uint32_t>>`
- band transform parameters:
  - `band_scale_x`, `band_scale_y`
  - `band_offset_x`, `band_offset_y`
  - `band_max_x`, `band_max_y`

This mirrors exactly what `AtlasView` constructs in Python.

---

#### Add: Rendering methods on `DecodedShape`

```cpp
float render_sample(float x, float y, float ppe_x, float ppe_y);
float render_sample_banded(float x, float y, float ppe_x, float ppe_y);
