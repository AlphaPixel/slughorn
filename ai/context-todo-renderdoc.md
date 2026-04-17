# osgDebug + RenderDoc Integration - Status, Design, and TODO

## What exists (as of Day 8 1/2 )

`osgDebug.hpp` - OpenGL debug group labeling + per-drawable CPU/GPU timing.
Wraps `GL_KHR_debug` and `GL_TIMESTAMP` queries. Header-only, no build deps
beyond OSG itself.

### Files
    osgDebug.hpp              - debug groups, timing callbacks, viewer subclasses
    osgx.hpp                  - utilities: aring_buffer, call(), Path, ascii colors
    renderdoc.sh              - launch script: sets OSG env vars, invokes renderdoccmd

### Key types
    osgDebug::Scoped          - RAII push/pop debug group with optional CPU timing
    osgDebug::DrawCallback    - per-drawable CPU + GPU timestamp timing, rolling average
    osgDebug::DrawVisitor     - walks scene graph, installs DrawCallback on all Drawables
    osgDebug::GraphicsOperation - initializes GL_KHR_debug function pointers at startup
    osgDebug::Viewer          - osgViewer::Viewer subclass with per-traversal CPU timing
    osgDebug::FrameByFrameViewer - single-step viewer ('n' key advances one frame)

### DrawCallback timing (implemented)
    CPU timing via osg::Timer (wall clock, per draw call submission)
    GPU timing via GL_TIMESTAMP / glQueryCounter (actual GPU execution time)
    One-frame-behind async harvest (non-blocking, no GPU stall)
    Free-list recycling of query objects (matches OSG internal pattern)
    Separate CpuBuffer / GpuBuffer rolling averages (60 samples each)
    Per-drawable path label via getUserValue("path", ...)
    Graceful fallback to CPU-only if GL_TIMESTAMP unavailable

### RenderDoc integration (implemented)
    renderdoccmd capture launches app with HUD overlay injected
    F12 / PrtScrn captures a single frame to .rdc file
    qrenderdoc opens .rdc for inspection (Event Browser, Texture Viewer,
      Pipeline State, Mesh Viewer, Pixel History)
    GL_KHR_debug push/pop groups visible in RenderDoc Event Browser as
      labeled, collapsible regions

---

## Design invariants (DO NOT VIOLATE)

### GL_TIMESTAMP only - never GL_TIME_ELAPSED for per-shape timing
OSG's own Renderer.cpp uses GL_TIMESTAMP (ARB path) for frame-level timing.
GL_TIME_ELAPSED queries cannot overlap or nest. GL_TIMESTAMP bookmark pairs
can be interleaved freely. Always use glQueryCounter(id, GL_TIMESTAMP) for
osgDebug timing - never glBeginQuery / glEndQuery.

### One-frame-behind harvest
GPU query results are never available the same frame they are issued.
DrawCallback always harvests the *previous* frame's result at the top of
drawImplementation(). Never call GL_QUERY_RESULT without checking
GL_QUERY_RESULT_AVAILABLE first - this would stall the CPU on the GPU.

### Free-list recycling
Never call glGenQueries every frame. DrawCallback._freeList recycles query
object pairs after harvest. glGenQueries is called only when the free list
is empty (first frame, or after a spike in drawable count).

### path label via getUserValue
DrawCallback reads the "path" user value from the Drawable for log output.
Set this on every osgSlug ShapeDrawable at construction:
    drawable->setUserValue("path", "body/outline");
Without it, timing output is unlabeled (functional but unidentifiable).

---

## TODO - Near term

### Wire "path" labels into osgSlug ShapeDrawable
Every ShapeDrawable needs a meaningful "path" user value set at construction
time so DrawCallback output is identifiable. Suggested convention:

    compositeName/layerIndex        e.g. "axolotl/0", "axolotl/1"
    compositeName/keyString         e.g. "axolotl/body_outline"

The key string form is preferred - it maps directly to slughorn::Key and
makes RenderDoc event labels human-readable.

### renderdoc_app.h - in-process capture API
Wire RenderDoc's in-process API into osgDebug for programmatic frame capture.
`renderdoc_app.h` is available from the RenderDoc source tree (header only).
`librenderdoc.so` is provided by the binary install.

Pattern (zero overhead when RenderDoc not attached):
```cpp
// osgDebug.hpp addition:
namespace detail {
    inline RENDERDOC_API_1_0_0* rdoc() {
        static RENDERDOC_API_1_0_0* api = nullptr;
        static bool init = false;

        if(!init) {
            init = true;
            void* mod = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);

            if(mod) {
                auto fn = (pRENDERDOC_GetAPI)dlsym(mod, "RENDERDOC_GetAPI");
                if(fn) fn(eRENDERDOC_API_Version_1_0_0, (void**)&api);
            }
        }

        return api;
    }
}
```

Expose via osgDebug:
```cpp
inline void beginCapture() {
    if(auto* r = detail::rdoc()) r->StartFrameCapture(nullptr, nullptr);
}

inline void endCapture() {
    if(auto* r = detail::rdoc()) r->EndFrameCapture(nullptr, nullptr);
}
```

### FrameByFrameViewer + in-process capture
Once renderdoc_app.h is wired in, bind capture to the 'c' key in
FrameByFrameViewer::EventHandler:

    'n' -> advance one frame
    'c' -> StartFrameCapture + renderingTraversals() + EndFrameCapture

This gives single-keypress "capture exactly this frame" with zero manual
interaction in qrenderdoc. Optional: call LaunchReplayUI() automatically
after EndFrameCapture to open qrenderdoc pointed at the new capture.

### shapeId rename (effectId -> shapeId)
The `effectId` vertex attribute (BIND_OVERALL, flat varying) should be
renamed `shapeId` to reflect its primary role as a shape identifier.
Required changes:
- osgSlug vertex attribute layout (location 5)
- osgSlug-vert.glsl: a_effectId -> a_shapeId
- osgSlug-frag.glsl: effectId -> shapeId
- ShapeDrawable::compile() attribute setup
- context-core.md shader vertex attribute layout table

`shapeId` as a `flat uint` varying enables future per-shape GPU queries
keyed by the same identifier used in DrawCallback path labels and
RenderDoc debug group names - one consistent identity across all layers
of the diagnostic stack.

---

## TODO - Medium term

### osgDebug::Scoped - GL_TIMESTAMP support
Scoped currently measures CPU time only (osg::Timer). Add optional GPU
timestamp pair so RAII-scoped blocks also capture GPU cost:

    osgDebug::Scoped s(1, "build_composite", Source::APPLICATION, true);

Implementation: issue glQueryCounter at construction and destruction,
harvest result on next frame via a thread-local or context-local pending list.
Requires access to osg::State - consider adding a constructor overload that
accepts osg::RenderInfo.

### Tracy integration
osgEarth already uses Tracy. osgDebug push/pop groups map naturally to
Tracy zones. DrawCallback could emit Tracy::Zone alongside GL_KHR_debug
groups - same label string, two profilers fed from one call site.

Tracy gives CPU call stacks and memory profiling that RenderDoc doesn't.
GL_KHR_debug / RenderDoc gives GPU pipeline inspection that Tracy doesn't.
They are complementary, not redundant.

Integration point: wrap Tracy macros behind an OSGDEBUG_TRACY compile
flag so Tracy remains an optional dependency.

### Per-shape GPU cost feedback loop
Once shapeId and per-drawable GPU timing are both in place:

1. DrawCallback records GPU cost per path label per frame
2. After N frames, compute per-shape average GPU cost
3. Export as a map: Key -> average GPU microseconds
4. Feed into the planned offline band-tuning tool to prioritize which
   shapes most need numBandsX/Y adjustment

This closes the loop: heatmap (visual) -> GPU query (numeric) ->
band tuning (corrective) -> remeasure.

### global function pointers -> per-context
detail::_pushGroup, detail::_popGroup, detail::_messageInsert are currently
global. Multi-context OSG applications (CompositeViewer with multiple
windows) require per-context function pointer storage. Wrap in a
std::unordered_map<unsigned int, FuncPtrs> keyed by contextID, populated
in initialize(gc). Single-context applications (current osgSlug usage)
are unaffected.
