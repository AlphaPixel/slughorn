//vimrun! ./slughorn-test-render
//
// Smoke-test for slughorn/render.hpp.
//
// No arguments: builds a circle and triangle in-memory, decodes via render::decode(),
// renders small coverage grids, and validates coverage values.
//
// With a .slug/.slugb argument: loads the atlas from disk, then iterates every
// shape proving that Shape::curves is populated post-load (the whole point of
// step 2.5). Renders the first shape with geometry as an ASCII grid.

#include "slughorn/canvas.hpp"
#include "slughorn/render.hpp"

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn/serial.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <utility>

using namespace slughorn::literals;

using slughorn::Atlas;
using slughorn::Key;
using slughorn::slug_t;

static void printGrid(const slughorn::render::Grid& g) {
	for(uint32_t j = 0; j < g.height; j++) {
		for(uint32_t i = 0; i < g.width; i++) {
			const slug_t v = g.at(j, i);
			std::cout << (v >= 0.75_cv ? '#' : v >= 0.4_cv ? '+' : v >= 0.1_cv ? '.' : ' ');
		}

		std::cout << '\n';
	}
}

// Mirrors slug_CalcCurveLoc() (Atlas.shaders.cpp / slughorn-example-glfw.cpp) bit-for-bit:
//
//   loc.y += loc.x >> texWidthLog2;
//   loc.x &= (1 << texWidthLog2) - 1;
//
// No real curve produced by today's packTextures() can ever exercise this formula's wrap
// branch -- curves are still unconditionally 2-texel-aligned, so texel0.x is always even and
// texel0.x+1 never crosses a row. Endpoint-shared packing (planned) removes that alignment, so
// this checks the formula in isolation against the row-boundary case it will need to handle,
// rather than waiting for a real Shape that can't produce it yet.
static std::pair<uint32_t, uint32_t> slug_CalcCurveLoc(
	uint32_t x,
	uint32_t y,
	uint32_t offset,
	uint32_t texWidthLog2
) {
	const uint32_t lx = x + offset;

	return {lx & ((1u << texWidthLog2) - 1), y + (lx >> texWidthLog2)};
}

static int runCurveLocWrapTest() {
	const uint32_t texWidthLog2 = 3;
	const uint32_t texWidth = 1u << texWidthLog2;

	// render.hpp's addressing scheme (decode()'s decodeBandList): the raw linear texel address
	// (cy * width + cx) is the dedup key, and a curve's second texel is just "+1" in that flat
	// space -- row boundaries are handled for free by linear indexing.
	auto flatAddr = [&](uint32_t x, uint32_t y) { return y * texWidth + x; };

	// Case 1: texel0 at the last column of a row. Its texel1 (offset +1) must land at column 0
	// of the NEXT row -- the case a shared-endpoint chain can produce once packTextures() stops
	// row-aligning every curve.
	{
		const auto [wx, wy] = slug_CalcCurveLoc(7, 2, 1, texWidthLog2);

		assert(wx == 0 && wy == 3 && "row-wrap case: texel0 at last column must wrap to next row");
		assert(flatAddr(7, 2) + 1 == flatAddr(wx, wy) && "GPU wrap formula disagrees with render.hpp's flat addressing");
	}

	// Case 2: mid-row, no wrap -- the ONLY case that occurs today, so this must match the old
	// flat "curveLoc.x + 1, same row" behavior exactly (the no-op requirement).
	{
		const auto [wx, wy] = slug_CalcCurveLoc(3, 2, 1, texWidthLog2);

		assert(wx == 4 && wy == 2 && "no-wrap case must be unchanged from the old curveLoc.x+1 path");
		assert(flatAddr(3, 2) + 1 == flatAddr(wx, wy) && "GPU wrap formula disagrees with render.hpp's flat addressing");
	}

	std::cout << "Curve-fetch row-wrap formula checks passed.\n";

	return 0;
}

static int runInMemory() {
	Atlas atlas;
	slughorn::canvas::Canvas canvas(atlas);

	// Circle
	canvas.circle(0.5_cv, 0.5_cv, 0.4_cv);
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("circle"));
	canvas.finalize(Key("circle_comp"));

	// Triangle
	canvas.beginPath();
	canvas.moveTo(0.5_cv, 0.1_cv);
	canvas.lineTo(0.9_cv, 0.9_cv);
	canvas.lineTo(0.1_cv, 0.9_cv);
	canvas.closePath();
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("triangle"));
	canvas.finalize(Key("triangle_comp"));

	atlas.build();

	std::cout << atlas.getPackingStats() << '\n';

	// --- Circle ---
	{
		const auto shape = atlas.getShape(Key("circle"));

		assert(shape && "circle shape missing");
		assert(!shape->curves.empty() && "circle Shape::curves should be populated after build()");

		std::cout << "circle: " << shape->curves.size() << " curves in Shape\n";

		auto s = slughorn::render::decode(atlas, Key("circle"));
		auto g = s.renderGrid(24);

		std::cout << "circle (" << g.width << 'x' << g.height << "):\n";
		printGrid(g);

		const slug_t center = g.at(g.height / 2, g.width / 2);
		const slug_t corner = g.at(0, 0);

		std::cout << " center=" << center << " corner=" << corner << '\n';

		assert(center >= 0.5_cv && "circle center should have high coverage");
		assert(corner <= 0.1_cv && "circle corner should have low coverage");
	}

	// --- Triangle ---
	{
		const auto shape = atlas.getShape(Key("triangle"));

		assert(shape && "triangle shape missing");
		assert(!shape->curves.empty() && "triangle Shape::curves should be populated after build()");

		std::cout << "\ntriangle: " << shape->curves.size() << " curves in Shape\n";

		auto s = slughorn::render::decode(atlas, Key("triangle"));
		auto g = s.renderGrid(24);

		std::cout << "triangle (" << g.width << 'x' << g.height << "):\n";
		printGrid(g);

		const slug_t tip = g.at(0, g.width / 2);
		const slug_t center = g.at(g.height * 2 / 3, g.width / 2);

		std::cout << " tip=" << tip << " center=" << center << '\n';

		assert(center >= 0.5_cv && "triangle center should have high coverage");
	}

#ifdef SLUGHORN_HAS_MSDF
	// --- Circle SDF (msdfgen path) ---
	{
		auto grid = slughorn::render::renderSDF(atlas, Key("circle"), 64);

		assert(grid.width > 0 && grid.height > 0 && "renderSDF returned empty grid");

		const slug_t center = grid.at(grid.height / 2, grid.width / 2);
		const slug_t corner = grid.at(0, 0);

		std::cout << "\ncircle SDF (" << grid.width << 'x' << grid.height << "):\n";
		std::cout << " center=" << center << " corner=" << corner << '\n';

		assert(center > 0.5_cv && "SDF circle center should be interior (> 0.5)");
		assert(corner < 0.5_cv && "SDF circle corner should be exterior (< 0.5)");

		std::cout << "Circle SDF checks passed.\n";
	}

	// --- Circle MSDF (msdfgen path) ---
	{
		auto grid = slughorn::render::renderMSDF(atlas, Key("circle"), 64);

		assert(grid.width > 0 && grid.height > 0 && "renderMSDF returned empty grid");

		// Reconstruct signed distance via median(r,g,b) — same as the shader.
		auto median = [](float a, float b, float c) {
			return std::max(std::min(a, b), std::min(std::max(a, b), c));
		};

		const float centerSd = median(
			grid.at(grid.height / 2, grid.width / 2, 0),
			grid.at(grid.height / 2, grid.width / 2, 1),
			grid.at(grid.height / 2, grid.width / 2, 2)
		);
		const float cornerSd = median(
			grid.at(0, 0, 0),
			grid.at(0, 0, 1),
			grid.at(0, 0, 2)
		);

		std::cout << "\ncircle MSDF (" << grid.width << 'x' << grid.height << "):\n";
		std::cout << " center median=" << centerSd << " corner median=" << cornerSd << '\n';

		assert(centerSd > 0.5f && "MSDF circle center median should be interior (> 0.5)");
		assert(cornerSd < 0.5f && "MSDF circle corner median should be exterior (< 0.5)");

		std::cout << "Circle MSDF checks passed.\n";
	}
#endif

	std::cout << "\nAll in-memory render checks passed.\n";

	return 0;
}

static int runFromFile(const std::string& path) {
	std::cout << "Loading: " << path << "\n\n";

	Atlas atlas = slughorn::serial::read(path);

	const auto& shapes = atlas.getShapes();

	std::cout << shapes.size() << " shape(s) in atlas:\n\n";

	size_t withCurves = 0;
	size_t whitespace = 0;
	const Atlas::Shape* firstGeom = nullptr;
	Key firstGeomKey;

	for(const auto& [key, shape] : shapes) {
		const size_t n = shape.curves.size();

		std::cout
			<< " " << key
			<< " curves=" << n
			<< " advance=" << shape.advance
			<< '\n'
		;

		if(n > 0) {
			withCurves++;

			if(!firstGeom) { firstGeom = &shape; firstGeomKey = key; }
		}

		else whitespace++;
	}

	std::cout
		<< "\n " << withCurves << " with geometry, "
		<< whitespace << " whitespace/metric-only\n"
	;

	assert(withCurves > 0 && "no shapes with curves found - Shape::curves not populated on load?");

	// Render the first geometry shape as ASCII proof
	if(firstGeom) {
		std::cout << "\nRendering \"" << firstGeomKey << "\" (first geometry shape):\n";

		auto s = slughorn::render::decode(*firstGeom, atlas.getCurveTextureData(), atlas.getBandTextureData());
		auto g = s.renderGrid(32);

		std::cout << '(' << g.width << 'x' << g.height << "):\n";
		printGrid(g);
	}

	std::cout << "\nAll .slug load checks passed.\n";

	return 0;
}

int main(int argc, char** argv) {
	if(argc > 1) return runFromFile(argv[1]);

	if(const int rc = runCurveLocWrapTest()) return rc;

	return runInMemory();
}
