# TODO: Dynamic Stroke / Line Rendering via Filled Outlines

## Goal

Prototype a dynamic line demo in slughorn/osgSlug by converting line strokes into ordinary filled vector shapes, allowing reuse of the existing:

- curves
- bands
- atlas
- shader

pipeline.

This is **not** a separate line renderer.

It is:

stroke geometry generation -> existing fill renderer

---

## Phase 1: Minimal Proof of Concept

### Scope

Keep it intentionally tiny:

- Single quadratic Bézier centerline
- Constant stroke width
- Flat end caps
- No joins
- CPU-side geometry generation
- Existing rendering path unchanged

### API Sketch

```cpp
StrokeBuilder s;

s.width(12.0f);
s.quadTo(p0, p1, p2);

auto shape = s.build();
```

Returns a normal `Shape` / `CompositeShape` compatible object.

---

## Geometry Strategy

Given centerline quadratic:

`P0, P1, P2`

Generate a stroked closed outline:

1. Sample / derive tangent along curve
2. Compute perpendicular normal
3. Offset one side by `+halfWidth`
4. Offset opposite side by `-halfWidth`
5. Reverse second edge
6. Close polygon
7. Convert result into quadratic path data

Feed result into the existing Slug pipeline.

---

## Temporary Shortcut (Fastest Demo)

Before true normal-offset math:

- Duplicate curve upward by `+halfWidth`
- Duplicate curve downward by `-halfWidth`
- Close ends

Good enough for immediate validation.

---

## Demo Target

``cpp
control.y = sin(time) * amplitude;
``

Render a thick glowing curve on screen.

If this works, concept proven instantly.

---

## Why This Matters

Unlocks:

- Dynamic UI connectors
- Graph/editor wires
- Realtime charts
- Trails
- Signature / pen input
- HUD vector widgets
- Roads / paths on terrain or globe surfaces

---

## Phase 2

After proof of concept:

- `lineTo()` polyline support
- Bevel joins
- Round joins
- Round caps
- Per-vertex width
- Per-vertex color
- Dashed patterns
- Texture/effect IDs

---

## Phase 3 (Advanced)

GPU-expanded centerlines:

Send control data only; shader expands into stroke region procedurally.

(Not needed now.)

---

## Architectural Principle

Prefer:

`stroke -> filled vector shape -> normal Slug renderer`

Over:

`special separate line renderer`

Because it preserves one unified rendering model.

---

## Success Condition

A moving thick curved line rendered cleanly with the current atlas/shader system and no bespoke raster path.
