## TODO(slug): Replace stem darkening with true dilation-based AA

### Current State

The renderer currently uses power-law **stem darkening**:

    fill = pow(fill, k(ppem, brightness))

This is a perceptual hack that thickens strokes at low ppem, but does not
respect actual curve geometry.

---

### Problem

Stem darkening operates on **final coverage**, not on the underlying geometry.
This leads to:

- Non-physical stroke thickening
- Inconsistent behavior across shapes and orientations
- Loss of geometric correctness (especially noticeable with diagonals)

Given that slughorn already computes **exact analytic coverage**, this is
a fallback, not the ideal solution.

---

### Available Data (from current pipeline)

The shader already has everything needed for a correct implementation:

- Analytic curve evaluation (exact coverage via Slug)
- em-space coordinates (`v_emCoord`)
- Local pixel footprint via `fwidth()`
- Implicit edge definition (coverage approx 0.5)

---

### Goal

Implement **true dilation-based anti-aliasing** by shifting the effective edge
position in em-space, instead of distorting coverage post-hoc.

---

### Direction

Instead of:

    fill = pow(fill, k)

Move toward:

1. Approximate signed distance near the edge using coverage + `fwidth`
2. Apply a small em-space bias proportional to `1 / ppem`
3. Adjust coverage based on shifted edge

Possible intermediate approach:

    bias = f(ppem)
    fill' = clamp(fill + bias * (1.0 - fill), 0.0, 1.0)

More correct approach:

    edge_shift = k / ppem
    adjust effective coverage transition threshold accordingly

---

### Constraints

- Must operate **after `slug_Render()`**, before discard
- Must NOT modify Slug core (banding, curve evaluation, packing)
- Effect should smoothly fade out for ppem > ~32-48
- Must preserve visual correctness at large sizes (no change vs raw Slug)

---

### Test Cases

- Small ppem (8-16):
  - Vertical stems (`l`, `i`)
  - Diagonals (`v`, `w`)
  - Punctuation
- Large ppem (>48):
  - Must match original Slug output exactly
- Mixed transforms:
  - Scale + perspective correctness

---

### References

- Lengyel, *Slug: Rendering Vector Fonts Using GPU Acceleration* (dynamic dilation section)
- HarfBuzz `hb-gpu-demo` (uses stem darkening as fallback)

---

### Rationale

slughorn already computes **exact analytic coverage**. Dilation is the correct
geometric solution to pixel under-sampling. Stem darkening is a perceptual
fallback, not the end goal.
