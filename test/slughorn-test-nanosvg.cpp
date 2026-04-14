//vimrun! ./slughorn-test-nanosvg

// Verifies that slughorn-nanosvg.hpp correctly decomposes SVG path data into
// slughorn curves, and that the round-trip through Atlas produces correct
// Shape metrics.
//
// Tests mirror slughorn-test-cairo.cpp exactly so both backends can be
// compared against the same ground truth.
//
// Usage:
//   ./slughorn-test-nanosvg                    -- run unit tests
//   ./slughorn-test-nanosvg <file.svg> [...]   -- dump one or more SVG files
//                                                 as .slug JSON to stdout;
//                                                 skipped-shape warnings on stderr

#ifndef SLUGHORN_HAS_NANOSVG
#  error "This test requires SLUGHORN_NANOSVG=ON"
#endif

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn-nanosvg.hpp"
#include "slughorn-serial.hpp"

#include <cmath>
#include <iostream>
#include <string>

// =============================================================================
// Minimal assertion helpers
// =============================================================================

static int s_pass = 0;
static int s_fail = 0;

static void check(const char* label, bool cond) {
    if(cond) {
        std::cout << "  PASS: " << label << std::endl;
        s_pass++;
    } else {
        std::cout << "  FAIL: " << label << std::endl;
        s_fail++;
    }
}

static void checkNear(const char* label, slug_t actual, slug_t expected, slug_t eps = 1e-3_cv) {
    const bool ok = std::abs(actual - expected) <= eps;
    if(ok) {
        std::cout << "  PASS: " << label << " (" << actual << ")" << std::endl;
    } else {
        std::cout << "  FAIL: " << label
                  << " expected=" << expected
                  << " actual=" << actual
                  << " delta=" << std::abs(actual - expected)
                  << std::endl;
        s_fail++;
        return;
    }
    s_pass++;
}

// =============================================================================
// SVG fixtures
// =============================================================================

// Single right triangle, SVG Y-down space, 100x100 canvas.
// M 0,0 L 100,0 L 0,100 Z
// After local normalization (scale=1/100):
//   curves in [0,1] space
//   transform.dx=0, transform.dy=0 (shape is at canvas origin)
//   Shape: w=1 h=1 bearingX=0 bearingY=1
static const std::string SVG_TRIANGLE = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <path fill="red" d="M 0,0 L 100,0 L 0,100 Z"/>
</svg>
)";

// Three triangles arranged diagonally, 300x300 canvas.
// Mirrors test_CompositeShape() in slughorn-test-cairo.cpp.
// Each triangle is offset by (i*100, i*100) in SVG space.
// After normalization (scale=1/300):
//   Each shape has identical curves in [0,1/3] local space
//   transforms: dx=0/dy=0, dx=1/3/dy=1/3, dx=2/3/dy=2/3
static const std::string SVG_THREE_TRIANGLES = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">
  <path fill="red"   d="M 0,0   L 100,0   L 0,100   Z"/>
  <path fill="green" d="M 100,100 L 200,100 L 100,200 Z"/>
  <path fill="blue"  d="M 200,200 L 300,200 L 200,300 Z"/>
</svg>
)";

// =============================================================================
// test_Shape
//
// Single triangle. Verifies:
//   - correct curve count
//   - curves are in [0,1] local space
//   - transform is identity offset (dx=0, dy=0) since shape is at origin
//   - atlas Shape has w=1, h=1, bearingX=0, bearingY=1
//   - computeQuad gives (0,0)->(1,1)
// =============================================================================

void test_Shape() {
    std::cout << "\n=== test_Shape ===" << std::endl;

    std::string buf = SVG_TRIANGLE;
    NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);

    check("image parsed", image != nullptr);
    check("has shapes",   image && image->shapes != nullptr);

    if(!image || !image->shapes) { nsvgDelete(image); return; }

    const NSVGshape* shape = image->shapes;
    const slug_t scale = 1.0_cv / cv(image->width); // = 1/100

    // --- decomposePath ---
    auto [curves, transform] = slughorn::nanosvg::decomposePath(shape, scale);

    std::cout << "  transform: " << transform << std::endl;
    std::cout << "  curves:    " << curves.size() << std::endl;
    for(size_t i = 0; i < curves.size(); i++)
        std::cout << "    [" << i << "] " << curves[i] << std::endl;

    check("curve count >= 3", curves.size() >= 3);

    // All curve points should be in [0,1]
    bool inRange = true;
    for(const auto& c : curves) {
        if(c.x1 < -1e-3_cv || c.x1 > 1.001_cv) inRange = false;
        if(c.x2 < -1e-3_cv || c.x2 > 1.001_cv) inRange = false;
        if(c.x3 < -1e-3_cv || c.x3 > 1.001_cv) inRange = false;
        if(c.y1 < -1e-3_cv || c.y1 > 1.001_cv) inRange = false;
        if(c.y2 < -1e-3_cv || c.y2 > 1.001_cv) inRange = false;
        if(c.y3 < -1e-3_cv || c.y3 > 1.001_cv) inRange = false;
    }
    check("all curves in [0,1]", inRange);

    // Transform: shape is at canvas origin so dx=0, dy=0
    checkNear("transform.dx == 0", transform.dx, 0.0_cv);
    checkNear("transform.dy == 0", transform.dy, 0.0_cv);

    // --- atlas round-trip ---
    slughorn::Atlas atlas;
    slughorn::Atlas::ShapeInfo info;
    info.autoMetrics = true;
    info.curves = curves;
    atlas.addShape(1u, info);
    atlas.build();

    const slughorn::Atlas::Shape* s = atlas.getShape(1u);
    check("shape in atlas", s != nullptr);

    if(s) {
        std::cout << "  " << *s << std::endl;

        checkNear("width  == 1", s->width,    1.0_cv);
        checkNear("height == 1", s->height,   1.0_cv);
        checkNear("bearingX == 0", s->bearingX, 0.0_cv);
        checkNear("bearingY == 1", s->bearingY, 1.0_cv);

        auto q = s->computeQuad(transform);
        std::cout << "  " << q << std::endl;

        checkNear("quad.x0 == 0", q.x0, 0.0_cv);
        checkNear("quad.y0 == 0", q.y0, 0.0_cv);
        checkNear("quad.x1 == 1", q.x1, 1.0_cv);
        checkNear("quad.y1 == 1", q.y1, 1.0_cv);
    }

    nsvgDelete(image);
}

// =============================================================================
// test_CompositeShape
//
// Three triangles diagonal. Verifies:
//   - 3 layers loaded
//   - each shape has identical curves (same geometry, different offset)
//   - transforms carry correct canvas offsets: (0,0), (1/3,1/3), (2/3,2/3)
//     (in normalized units, since scale = 1/300 = 1/image->width)
//   - quads tile correctly: (0,0)->(1/3,1/3), etc.
// =============================================================================

void test_CompositeShape() {
    std::cout << "\n=== test_CompositeShape ===" << std::endl;

    slughorn::Atlas atlas;
    uint32_t baseKey = 0;

    slughorn::CompositeShape composite =
        slughorn::nanosvg::loadString(SVG_THREE_TRIANGLES, atlas, baseKey);

    atlas.build();

    check("3 layers loaded", composite.layers.size() == 3);

    // Expected offsets in normalized space (scale = 1/300)
    const slug_t third = 1.0_cv / 3.0_cv;
    const slug_t offsets[3] = { 0.0_cv, third, 2.0_cv * third };

    for(size_t i = 0; i < composite.layers.size(); i++) {
        const auto& layer = composite.layers[i];
        const slughorn::Atlas::Shape* s = atlas.getShape(layer.key);

        std::cout << "\n  Layer " << i << ": " << layer << std::endl;

        check("shape in atlas", s != nullptr);
        if(!s) continue;

        std::cout << "  " << *s << std::endl;

        // All three shapes should have the same normalized size
        checkNear(("width  ~= 1/3 [" + std::to_string(i) + "]").c_str(),
                  s->width, third);
        checkNear(("height ~= 1/3 [" + std::to_string(i) + "]").c_str(),
                  s->height, third);

        // Canvas offsets
        checkNear(("transform.dx [" + std::to_string(i) + "]").c_str(),
                  layer.transform.dx, offsets[i]);
        checkNear(("transform.dy [" + std::to_string(i) + "]").c_str(),
                  layer.transform.dy, offsets[i]);

        // Quad tiling
        auto q = s->computeQuad(layer.transform);
        std::cout << "  " << q << std::endl;

        checkNear(("quad.x0 [" + std::to_string(i) + "]").c_str(), q.x0, offsets[i]);
        checkNear(("quad.y0 [" + std::to_string(i) + "]").c_str(), q.y0, offsets[i]);
        checkNear(("quad.x1 [" + std::to_string(i) + "]").c_str(), q.x1, offsets[i] + third);
        checkNear(("quad.y1 [" + std::to_string(i) + "]").c_str(), q.y1, offsets[i] + third);
    }
}

// =============================================================================
// dumpSVGFile
//
// Diagnostic mode: load an SVG file, build the atlas, and emit the full .slug
// JSON to stdout via slughorn::serial::writeJSON. This captures all shape
// metrics, composite layers, transforms, colors, band transform, and texture
// layout in one shot. Warnings about skipped shapes go to stderr as usual.
// =============================================================================

void dumpSVGFile(const std::string& path) {
    std::cerr << "=== SVG dump: " << path << " ===" << std::endl;

    slughorn::Atlas atlas;
    uint32_t baseKey = 0;

    auto composite = slughorn::nanosvg::loadFile(path, atlas, baseKey);

    atlas.addCompositeShape(slughorn::Key::fromString("composite"), composite);

    atlas.build();

    std::cerr << "PackingStats: " << atlas.getPackingStats() << std::endl;

    slughorn::serial::writeJSON(atlas, std::cout);

    std::cout << std::endl;
}

// =============================================================================
// main
// =============================================================================

int main(int argc, char** argv) {
    if(argc >= 2) {
        // Diagnostic mode: dump every file passed on the command line.
        for(int i = 1; i < argc; i++) {
            dumpSVGFile(argv[i]);
        }
        return 0;
    }

    // Unit test mode.
    test_Shape();
    test_CompositeShape();

    std::cout << "\n=== Results: "
              << s_pass << " passed, "
              << s_fail << " failed ==="
              << std::endl;

    return s_fail > 0 ? 1 : 0;
}
