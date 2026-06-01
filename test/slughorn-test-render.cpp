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

	// --- Circle ---
	{
		const auto* shape = atlas.getShape(Key("circle"));

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
		const auto* shape = atlas.getShape(Key("triangle"));

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

	return runInMemory();
}
