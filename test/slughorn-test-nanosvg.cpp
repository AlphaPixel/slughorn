//vimrun! ./slughorn-test-nanosvg

// Verifies that slughorn-nanosvg.hpp correctly decomposes SVG path data into
// slughorn curves, and that the round-trip through Atlas produces correct
// Shape metrics.
//
// Tests mirror slughorn-test-cairo.cpp exactly so both backends can be
// compared against the same ground truth.
//
// Usage:
//
// ./slughorn-test-nanosvg - run unit tests
// ./slughorn-test-nanosvg <file.svg> [...] - dump one or more SVG files as JSON

#ifndef SLUGHORN_HAS_NANOSVG
#  error "This test requires SLUGHORN_NANOSVG=ON"
#endif

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn/nanosvg.hpp"
#include "slughorn/serial.hpp"

#include <cmath>
#include <iostream>
#include <string>

using namespace slughorn::literals;
using slughorn::slug_t;

// =============================================================================
// Minimal assertion helpers
// =============================================================================

static int s_pass = 0;
static int s_fail = 0;

static void check(const char* label, bool cond) {
	if(cond) {
		std::cout << "  PASS: " << label << std::endl;
		s_pass++;
	}

	else {
		std::cout << "  FAIL: " << label << std::endl;
		s_fail++;
	}
}

static void checkNear(const char* label, slug_t actual, slug_t expected, slug_t eps = 1e-3_cv) {
	const bool ok = std::abs(actual - expected) <= eps;
	if(ok) {
		std::cout << "  PASS: " << label << " (" << actual << ")" << std::endl;
	}

	else {
		std::cout << "  FAIL: " << label
			<< " expected=" << expected
			<< " actual=" << actual
			<< " delta=" << std::abs(actual - expected)
			<< std::endl
		;

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
//
// curves in [0,1] space
// transform.dx=0, transform.dy=0 (shape is at canvas origin)
// Shape: w=1 h=1 bearingX=0 bearingY=1
static const std::string SVG_TRIANGLE = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<path fill="red" d="M 0,0 L 100,0 L 0,100 Z"/>
</svg>
)";

// Square with a horizontal linear gradient (red -> blue), 100x100 canvas.
// gradient-units="objectBoundingBox" is SVG default; x1=0,y1=0,x2=1,y2=0
// maps to left -> right in pixel space.
static const std::string SVG_LINEAR_GRADIENT = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<defs>
		<linearGradient id="lg" x1="0" y1="0" x2="1" y2="0">
			<stop offset="0" stop-color="#ff0000"/>
			<stop offset="1" stop-color="#0000ff"/>
		</linearGradient>
	</defs>
	<rect x="0" y="0" width="100" height="100" fill="url(#lg)"/>
</svg>
)SVG";

// Square with a radial gradient (white center -> black edge), 100x100 canvas.
static const std::string SVG_RADIAL_GRADIENT = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
	<defs>
		<radialGradient id="rg" cx="0.5" cy="0.5" r="0.5">
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="100" height="100" fill="url(#rg)"/>
</svg>
)SVG";

// 400x200 SVG with a radial gradient using gradientUnits="objectBoundingBox" on a non-square rect.
// NanoSVG collapses the radius to sl = sqrt(pow(sw, 2) + pow(sh, 2)) / sqrt(2) = 316 for sw=400,
// sh=200. The objectBoundingBox correction restores the correct anisotropic B: B = diag(sl/sw,
// sl/sh) * B_iso. With cx="50%" cy="50%" r="50%": rx_em = 0.5*400/400 = 0.5, ry_em = 0.5*200/400 =
// 0.25,
// -> B[0,0] = 2, B[1,1] = 4, center = (0.5, 0.25).
static const std::string SVG_RADIAL_OBB_NON_SQUARE = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 200">
	<defs>
		<radialGradient id="rg3" cx="50%" cy="50%" r="50%" gradientUnits="objectBoundingBox">
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="400" height="200" fill="url(#rg3)"/>
</svg>
)SVG";

// 400x200 SVG with an explicitly elliptical radial gradient via gradientTransform.
// gradientTransform="matrix(200,0,0,100,200,100)" maps the unit circle to an ellipse
// with x-radius=200 and y-radius=100 centered at (200,100) in SVG pixel space.
// This produces an anisotropic B matrix (B[0,0] != B[1,1]).
static const std::string SVG_RADIAL_NON_SQUARE = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 200">
	<defs>
		<radialGradient id="rg2" cx="0" cy="0" r="1"
			gradientUnits="userSpaceOnUse"
			gradientTransform="matrix(200,0,0,100,200,100)"
		>
			<stop offset="0" stop-color="#ffffff"/>
			<stop offset="1" stop-color="#000000"/>
		</radialGradient>
	</defs>
	<rect x="0" y="0" width="400" height="200" fill="url(#rg2)"/>
</svg>
)SVG";

// Three triangles arranged diagonally, 300x300 canvas. Mirrors test_CompositeShape() in
// slughorn-test-cairo.cpp. Each triangle is offset by (i*100, i*100) in SVG space.
//
// After normalization (scale=1/300):
//
// Each shape has identical curves in [0,1/3] local space
// transforms: dx=0/dy=0, dx=1/3/dy=1/3, dx=2/3/dy=2/3
static const std::string SVG_THREE_TRIANGLES = R"(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 300">
	<path fill="red" d="M 0,0 L 100,0 L 0,100 Z"/>
	<path fill="green" d="M 100,100 L 200,100 L 100,200 Z"/>
	<path fill="blue" d="M 200,200 L 300,200 L 200,300 Z"/>
</svg>
)";

// =============================================================================
// test_Shape
//
// Single triangle that verifies:
//
// - correct curve count
// - curves are in [0,1] local space
// - transform is identity offset (dx=0, dy=0) since shape is at origin
// - atlas Shape has w=1, h=1, bearingX=0, bearingY=1
// - computeQuad gives (0,0)->(1,1)
// =============================================================================

void test_Shape() {
	std::cout << "\n=== test_Shape ===" << std::endl;

	std::string buf = SVG_TRIANGLE;

	NSVGimage* image = nsvgParse(buf.data(), "px", 96.0f);

	check("image parsed", image != nullptr);
	check("has shapes", image && image->shapes != nullptr);

	if(!image || !image->shapes) { nsvgDelete(image); return; }

	const NSVGshape* shape = image->shapes;
	const slug_t scale = 1_cv / cv(image->width); // = 1/100

	// --- decomposePath ---
	auto [info, transform] = slughorn::nanosvg::decomposePath(shape, scale);

	std::cout << " transform: " << transform << std::endl;
	std::cout << " curves: " << info.curves.size() << std::endl;

	for(size_t i = 0; i < info.curves.size(); i++) std::cout
		<< "    [" << i << "] " << info.curves[i] << std::endl
	;

	check("curve count >= 3", info.curves.size() >= 3);

	// All curve points should be in [0,1]
	bool inRange = true;

	for(const auto& c : info.curves) {
		if(c.x1 < -1e-3_cv || c.x1 > 1.001_cv) inRange = false;
		if(c.x2 < -1e-3_cv || c.x2 > 1.001_cv) inRange = false;
		if(c.x3 < -1e-3_cv || c.x3 > 1.001_cv) inRange = false;
		if(c.y1 < -1e-3_cv || c.y1 > 1.001_cv) inRange = false;
		if(c.y2 < -1e-3_cv || c.y2 > 1.001_cv) inRange = false;
		if(c.y3 < -1e-3_cv || c.y3 > 1.001_cv) inRange = false;
	}

	check("all curves in [0,1]", inRange);

	// Transform: shape is at canvas origin so dx=0, dy=0
	checkNear("transform.dx == 0", transform.dx, 0_cv);
	checkNear("transform.dy == 0", transform.dy, 0_cv);

	slughorn::Atlas atlas;

	atlas.addShape(1u, info);
	atlas.build();

	const slughorn::Atlas::Shape* s = atlas.getShape(1u);

	check("shape in atlas", s != nullptr);

	if(s) {
		std::cout << "  " << *s << std::endl;

		checkNear("width == 1", s->width, 1_cv);
		checkNear("height == 1", s->height, 1_cv);
		checkNear("bearingX == 0", s->bearingX, 0_cv);
		checkNear("bearingY == 1", s->bearingY, 1_cv);

		auto q = s->computeQuad(transform);
		std::cout << "  " << q << std::endl;

		checkNear("quad.x0 == 0", q.x0, 0_cv);
		checkNear("quad.y0 == 0", q.y0, 0_cv);
		checkNear("quad.x1 == 1", q.x1, 1_cv);
		checkNear("quad.y1 == 1", q.y1, 1_cv);
	}

	nsvgDelete(image);
}

// =============================================================================
// test_Gradients
//
// Verifies that linear and radial gradient fills are wired correctly:
//
// - layer.gradientId is non-zero
// - atlas.getGradients() has one entry
// - stop count and type are correct
// - gradient type discriminator matches request
// =============================================================================

void test_Gradients() {
	auto testGradient = [](const char* label, const std::string& svg, auto&& checks) {
		std::cout << "\n=== test_Gradients (" << label << ") ===" << std::endl;

		slughorn::Atlas atlas;

		slughorn::KeyIterator keys;

		auto composite = slughorn::nanosvg::loadString(svg, atlas, keys);

		check("1 layer loaded", composite.layers.size() == 1);
		check(
			"layer has gradientId",
			composite.layers.size() >= 1 &&
			composite.layers[0].gradientId != 0
		);

		atlas.build();

		const auto& grads = atlas.getGradients();

		check("one gradient registered", grads.size() == 1);

		if(!grads.empty()) checks(grads[0]);
	};

	auto printBMatrix = [](const auto& g) {
		std::cout
			<< "  B = [" << g.transform.xx << ", " << g.transform.xy
			<< "; " << g.transform.yx << ", " << g.transform.yy << "]"
			<< std::endl
		;

		std::cout
			<< "  center = (" << g.transform.dx << ", " << g.transform.dy << ")"
			<< std::endl
		;
	};

	testGradient("linear", SVG_LINEAR_GRADIENT, [](const auto& g) {
		check("type == Linear", g.type == slughorn::GradientInfo::Type::Linear);
		check("2 stops", g.stops.size() == 2);

		if(g.stops.size() >= 2) {
			// stop 0: red (#ff0000), offset 0
			checkNear("stop[0].t == 0", g.stops[0].t, 0_cv);
			checkNear("stop[0].r == 1", g.stops[0].color.r, 1_cv);
			checkNear("stop[0].g == 0", g.stops[0].color.g, 0_cv);
			checkNear("stop[0].b == 0", g.stops[0].color.b, 0_cv);

			// stop 1: blue (#0000ff), offset 1
			checkNear("stop[1].t == 1", g.stops[1].t, 1_cv);
			checkNear("stop[1].r == 0", g.stops[1].color.r, 0_cv);
			checkNear("stop[1].g == 0", g.stops[1].color.g, 0_cv);
			checkNear("stop[1].b == 1", g.stops[1].color.b, 1_cv);
		}
	});

	testGradient("radial square", SVG_RADIAL_GRADIENT, [](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		check("2 stops", g.stops.size() == 2);

		// For a square bbox, B should be approximately scalar * I (b01 and b10 near 0).
		checkNear("b01 ~= 0", g.transform.xy, 0_cv, 1e-3_cv);
		checkNear("b10 ~= 0", g.transform.yx, 0_cv, 1e-3_cv);

		// b00 and b11 should be equal (isotropic) and positive.
		check("b00 > 0", g.transform.xx > 0_cv);
		checkNear("b00 == b11", g.transform.xx, g.transform.yy, 1e-3_cv);
	});

	testGradient("radial non-square", SVG_RADIAL_NON_SQUARE, [&printBMatrix](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		check("2 stops", g.stops.size() == 2);

		// gradientTransform="matrix(200,0,0,100,200,100)" -> x-radius=200, y-radius=100 in SVG.
		// In em-space (scale=1/400): B should be [[2,0],[0,4]] and center at (0.5, 0.25).
		check("b11 > 0", g.transform.yy > 0_cv);
		check("b00 != b11 (anisotropic)", std::abs(g.transform.xx - g.transform.yy) > 1e-3_cv);
		checkNear("b00 ~= 2", g.transform.xx, 2_cv, 0.01_cv);
		checkNear("b11 ~= 4", g.transform.yy, 4_cv, 0.01_cv);
		checkNear("b01 ~= 0", g.transform.xy, 0_cv, 1e-3_cv);
		checkNear("b10 ~= 0", g.transform.yx, 0_cv, 1e-3_cv);
		checkNear("center.x ~= 0.5", g.transform.dx, 0.5_cv, 0.01_cv);
		checkNear("center.y ~= 0.25", g.transform.dy, 0.25_cv, 0.01_cv);

		printBMatrix(g);
	});

	testGradient("radial OBB non-square", SVG_RADIAL_OBB_NON_SQUARE, [&printBMatrix](const auto& g) {
		check("type == AffineRadial", g.type == slughorn::GradientInfo::Type::AffineRadial);
		check("2 stops", g.stops.size() == 2);

		// objectBoundingBox on 400x200: sl/sw = 0.79, sl/sh = 1.58 -> B = [[2,0],[0,4]],
		// center=(0.5,0.25)
		check("b11 > 0", g.transform.yy > 0_cv);
		check("b00 != b11 (anisotropic)", std::abs(g.transform.xx - g.transform.yy) > 1e-3_cv);
		checkNear("b00 ~= 2", g.transform.xx, 2_cv, 0.05_cv);
		checkNear("b11 ~= 4", g.transform.yy, 4_cv, 0.05_cv);
		checkNear("b01 ~= 0", g.transform.xy, 0_cv, 1e-3_cv);
		checkNear("b10 ~= 0", g.transform.yx, 0_cv, 1e-3_cv);
		checkNear("center.x ~= 0.5", g.transform.dx, 0.5_cv, 0.01_cv);
		checkNear("center.y ~= 0.25", g.transform.dy, 0.25_cv, 0.01_cv);

		printBMatrix(g);
	});
}

// =============================================================================
// test_CompositeShape
//
// Three triangles diagonal. Verifies:
//
// - 3 layers loaded
// - each shape has identical curves (same geometry, different offset)
// - transforms carry correct canvas offsets: (0,0), (1/3,1/3), (2/3,2/3)
//   (in normalized units, since scale = 1/300 = 1/image->width)
// - quads tile correctly: (0,0)->(1/3,1/3), etc.
// =============================================================================

void test_CompositeShape() {
	std::cout << "\n=== test_CompositeShape ===" << std::endl;

	slughorn::Atlas atlas;

	slughorn::KeyIterator keys;

	slughorn::CompositeShape composite = slughorn::nanosvg::loadString(
		SVG_THREE_TRIANGLES,
		atlas,
		keys
	);

	atlas.build();

	check("3 layers loaded", composite.layers.size() == 3);

	// Expected offsets in normalized space (scale = 1/300)
	const slug_t third = 1_cv / 3_cv;
	const slug_t offsets[3] = { 0_cv, third, 2_cv * third };

	for(size_t i = 0; i < composite.layers.size(); i++) {
		const auto& layer = composite.layers[i];
		const slughorn::Atlas::Shape* s = atlas.getShape(layer.key);

		std::cout << "\n  Layer " << i << ": " << layer << std::endl;

		check("shape in atlas", s != nullptr);

		if(!s) continue;

		std::cout << "  " << *s << std::endl;

		// All three shapes should have the same normalized size
		checkNear(
			("width ~= 1/3 [" + std::to_string(i) + "]").c_str(),
			s->width, third
		);

		checkNear(
			("height ~= 1/3 [" + std::to_string(i) + "]").c_str(),
			s->height, third
		);

		// Canvas offsets
		checkNear(
			("transform.dx [" + std::to_string(i) + "]").c_str(),
			layer.transform.dx, offsets[i]
		);

		checkNear(
			("transform.dy [" + std::to_string(i) + "]").c_str(),
			layer.transform.dy, offsets[i]
		);

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

	slughorn::KeyIterator keys;
	auto composite = slughorn::nanosvg::loadFile(path, atlas, keys);

	atlas.addCompositeShape(slughorn::Key("composite"), composite);

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
		for(int i = 1; i < argc; i++) dumpSVGFile(argv[i]);

		return 0;
	}

	// Unit test mode.
	test_Shape();
	test_Gradients();
	test_CompositeShape();

	std::cout
		<< "\n=== Results: "
		<< s_pass << " passed, "
		<< s_fail << " failed ==="
		<< std::endl
	;

	return s_fail > 0 ? 1 : 0;
}
