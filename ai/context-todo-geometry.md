# osgSlug Geometry Abstraction - Status, Design, and TODO

## What exists (as of Day 9)

A progression of proof-of-concept drawables that prove the slug pipeline
is geometry-agnostic. All live in osgSlug (a separate project) as `Drawable.cpp`
for now (acknowledged hack).

### Files
    Drawable.hpp / Drawable.cpp - ShapeDrawable + all current subclasses

### Class hierarchy (current)
    ShapeDrawable - base: flat quads, one per layer
    +-- SphereDrawable - UV sphere via equirectangular subdivision
    +-- BoxDrawable - unit cube, 6 faces, one layer per face
    +-- SubdividedDrawable - generalized: (u,v)->vec3 position function
        +-- HalfCylinderDrawable - inside of a partial cylinder ("curved monitor")
        +-- SphereDrawable (v2) - sphere reimplemented as SubdividedDrawable

### SubdividedDrawable design
    PositionFn = std::function<osg::Vec3(float u, float v)>

The subdivider owns: em-coord mapping, index stitching, vertex attribute
binding. The position function owns: all geometric decisions.

The slug pipeline sees identical vertex attribute layout regardless of
which drawable produced the geometry:

    slot 0: a_position (vec3)
    slot 1: a_color (vec4)
    slot 2: a_emCoord (vec2) <- u,v mapped linearly into em bounding box
    slot 3: a_bandXform (vec4) <- broadcast constant per shape
    slot 4: a_glyphData (vec4) <- broadcast constant per shape
    slot 5: a_effectId (float) <- broadcast constant per shape

### Key insight (proven by PoC)
The slug fragment shader is fully geometry-agnostic. It receives an
interpolated em-coord and evaluates Bezier coverage per-fragment; the
host geometry (quad, sphere, cube, cylinder) is irrelevant to the math.

### Known hacks / acknowledged debt
- All three compile() implementations share Drawable.cpp (pre-refactor)
- SLUG_EXPAND = 0.01f hardcoded everywhere; not validated for curved surfaces
- Single-layer only for SubdividedDrawable / SphereDrawable / HalfCylinder
- BoxDrawable uses a flat lambda-less face approach; not yet ported to
  SubdividedDrawable infrastructure
- _layers accessed as _layers[0] throughout; no multi-layer support

---

## Design invariants (DO NOT VIOLATE)

### em-coord mapping is linear in UV space
u ? [0,1] maps linearly to [emX0, emX1]; v ? [0,1] maps to [emY0, emY1].
This is correct for any geometry where the slug shape should appear
"stretched" across the surface. Non-linear mappings (e.g. equal-area
projections) are possible but require deliberate design; do not introduce
them accidentally.

### band/shape data are broadcast constants per shape
bandScaleX/Y, bandOffsetX/Y, bandTexX/Y, bandMaxX/Y are identical for all
vertices belonging to the same shape. They must never vary per-vertex within
a single shape's geometry. Violating this corrupts band texture lookups.

### SLUG_EXPAND is a fixed AA fringe, not a function of geometry
SLUG_EXPAND = 0.01f is correct for flat quads. Its geometric meaning on
curved surfaces is currently undefined (see TODO below). Do not derive it
from scale, curvature, or any other geometric property without deliberate
design.

### Position function owns geometry; compile() owns slug plumbing
The PositionFn is called exactly once per vertex and returns a world-space
position. It must not touch any slug state. All em-coord math, band data,
and attribute binding belong exclusively in compile().

---

## TODO - Near term

### Move UV->em mapping into slughorn::Atlas::Shape
Currently every compile() independently computes:

    emX = (bearingX - expand) + u * (width + 2*expand)
    emY = (bearingY - height - expand) + v * (height + 2*expand)

This arithmetic belongs in slughorn; the shape owns its own bounding box.
Proposed addition to slughorn.hpp:

```cpp
// In Atlas::Shape:
std::pair<slug_t, slug_t> uvToEm(slug_t u, slug_t v, slug_t expand = 0) const {
    const slug_t x0 = bearingX - expand;
    const slug_t y0 = bearingY - height - expand;
    const slug_t x1 = bearingX + width  + expand;
    const slug_t y1 = bearingY          + expand;
    return { x0 + u * (x1 - x0), y0 + v * (y1 - y0) };
}
```

Then SubdividedDrawable::compile() becomes:
```cpp
auto [emX, emY] = shape->uvToEm(u, v, SLUG_EXPAND);
emCoords->push_back({emX, emY});
```

Justification: computeQuad() already lives on Shape for the same reason;
the shape owns the arithmetic that derives geometry from its own fields.
uvToEm() is the natural companion for subdivided/3D use cases.

### Port BoxDrawable to SubdividedDrawable
BoxDrawable currently has its own compile() with a push_face lambda.
It should be reimplemented as 6 SubdividedDrawable instances (one per face)
or as a single SubdividedDrawable with a face-dispatch position function.
The latter keeps it one draw call; the former is cleaner for per-face layers.

### Clear arrays on repeated compile()
All compile() implementations accumulate into arrays without clearing first.
Safe only because compile() is currently called once. Before compile() is
called more than once (e.g. dynamic shapes, animation), add:

```cpp
_vertices->clear(); _colors->clear(); _emCoords->clear();
_bandXform->clear(); _shapeData->clear(); _effectIds->clear();
_indices->clear(); removePrimitiveSet(0, getNumPrimitiveSets());
```

### Split Drawable.cpp by class
ShapeDrawable, SubdividedDrawable, SphereDrawable, HalfCylinderDrawable,
BoxDrawable each belong in their own .cpp. Current single-file layout is
an acknowledged PoC convenience.

---

## TODO - Medium term

### SLUG_EXPAND on curved surfaces
SLUG_EXPAND = 0.01f is the AA fringe in em-space. On a flat quad this
corresponds to a fixed sub-pixel border. On a curved surface (sphere,
cylinder) the relationship between em-space expand and screen-space pixels
varies with curvature, distance, and projection. Options:

A) Keep 0.01f and accept slightly incorrect AA fringe on curved surfaces
   (probably imperceptible at normal viewing distances).
B) Derive expand from fwidth() in the fragment shader (already available
   via emsPerPixel in slug_Render); remove it from the CPU side entirely.
C) Expose expand as a per-drawable parameter and let the caller tune it.

Option B is the most principled. Investigate whether removing the CPU-side
expand and relying solely on the shader's fwidth() AA path is sufficient.

### Multi-layer SubdividedDrawable
SubdividedDrawable currently uses _layers[0] only. For surfaces where
different UV regions should show different shapes (e.g. a globe with
country-shaped slughorn outlines), the position function should accept a
layer index and the subdivider should support per-region layer assignment.

One approach: tile the UV space into N regions, each mapped to a different
layer. The position function signature becomes:
    PositionFn = std::function<osg::Vec3(float u, float v, int layerIdx)>

### Normal generation for lighting
SubdividedDrawable generates no normals. For lit rendering, normals can be
computed analytically from the position function via finite differences:

```cpp
// Partial derivatives at (u, v):
osg::Vec3 du = (_positionFn(u + eps, v) - _positionFn(u - eps, v)) / (2*eps);
osg::Vec3 dv = (_positionFn(u, v + eps) - _positionFn(u, v - eps)) / (2*eps);
osg::Vec3 normal = (du ^ dv);  // cross product
normal.normalize();
```

This works for any smooth position function without requiring the subclass
to provide an explicit normal function.

### Generic "project slug onto arbitrary mesh" pathway
The logical endpoint of the SubdividedDrawable design: accept an existing
osg::Geometry (with UV coordinates on channel 0) and remap its UVs into
em-space using uvToEm(). This would allow slug shapes to be projected onto
any externally-authored mesh (loaded from .obj, .fbx, procedural terrain,
etc.) with no geometry-specific code in osgSlug.

Sketch:
```cpp
class MeshProjectedDrawable : public ShapeDrawable {
public:
    void setSourceGeometry(osg::Geometry* geom); // provides positions + UVs
    void compile(); // remaps UVs -> em-coords, copies positions, binds arrays
};
```

---

## TODO - Long term / design discussion

### computeQuad() vs uvToEm() - unified bounding primitive
computeQuad() returns the world-space quad for flat rendering.
uvToEm() (proposed) returns em-coords for subdivided rendering.
Both derive from the same Shape bounding box fields. Consider whether
a unified "ShapeBounds" helper on Shape covers both use cases cleanly,
or whether two focused methods are preferable (current direction).

### Depth/blend correctness for 3D drawables
Current PoC renders with incorrect depth sorting and blend state; back
faces show through front faces. Required state set changes:
- GL_DEPTH_TEST on, depth writes enabled
- GL_CULL_FACE on (back-face culling)
- Blend func: GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA
For transparent slug shapes on 3D surfaces, order-independent transparency
(OIT) may be needed for correct results.

### ShapeDrawable API cleanup
The current addLayer() / addCompositeShape() / compile() pattern was
designed for flat quads. The subclassing story (SubdividedDrawable,
BoxDrawable) has outgrown it. A future design pass should define:
- How layers map to geometry regions (1:1, N:1, 1:N)
- Whether compile() should be virtual (currently non-virtual override)
- Whether \_atlas / \_layers belong on a shared base distinct from
  the osg::Geometry machinery
