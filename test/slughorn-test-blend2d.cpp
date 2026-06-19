//vimrun! ./slughorn-test-blend2d blend2d.slug

#define SLUGHORN_BLEND2D_IMPLEMENTATION
#include "slughorn/blend2d.hpp"

#ifdef SLUGHORN_HAS_SERIAL
#include "slughorn/serial.hpp"
#include <fstream>
#endif

#include <iostream>

using namespace slughorn::literals;
using slughorn::slug_t;

// Triangle built with BL_PATH_CMD_ON (native lines).
void test_Shape(slughorn::Atlas& atlas) {
	BLPath path;

	path.move_to(0.0, 0.0);
	path.line_to(100.0, 0.0);
	path.line_to(0.0, 100.0);
	path.close();

	auto [info, transform] = slughorn::blend2d::decomposePath(path, 1_cv / 100_cv);

	std::cout << "=== test_Shape (triangle) ===" << std::endl;
	std::cout << "Transform: " << transform << std::endl;
	std::cout << "Curves: " << info.curves.size() << std::endl;

	for(size_t i = 0; i < info.curves.size(); i++) std::cout
		<< " [" << i << "] " << info.curves[i] << std::endl
	;

	atlas.addShape(1u, info);
}

// Rounded rect -- exercises BL_PATH_CMD_QUAD via arc_to expansion.
void test_RoundedRect(slughorn::Atlas& atlas) {
	BLPath path;

	path.add_round_rect(BLRoundRect{ 10.0, 10.0, 80.0, 80.0, 15.0, 15.0 });

	auto [info, transform] = slughorn::blend2d::decomposePath(path, 1_cv / 100_cv);

	std::cout << std::endl << "=== test_RoundedRect ===" << std::endl;
	std::cout << "Transform: " << transform << std::endl;
	std::cout << "Curves: " << info.curves.size() << std::endl;

	atlas.addShape(2u, info);
}

// Stroke-to-fill -- the capability unique to this backend vs Cairo.
void test_StrokedShape(slughorn::Atlas& atlas) {
	BLPath path;

	// A simple L-shape stroke.
	path.move_to(20.0, 80.0);
	path.line_to(20.0, 20.0);
	path.line_to(80.0, 20.0);

	BLStrokeOptions stroke;

	stroke.width = 8.0;
	stroke.caps[BL_STROKE_CAP_POSITION_START] = BL_STROKE_CAP_ROUND;
	stroke.caps[BL_STROKE_CAP_POSITION_END] = BL_STROKE_CAP_ROUND;
	stroke.join = BL_STROKE_JOIN_ROUND;

	const auto transform = slughorn::blend2d::loadStrokedShape(
		path, atlas, 3u, stroke, 1_cv / 100_cv
	);

	std::cout << std::endl << "=== test_StrokedShape (L-shape, round cap/join) ===" << std::endl;
	std::cout << "Transform: " << transform << std::endl;
}

int main(int argc, char** argv) {
	slughorn::Atlas atlas;

	test_Shape(atlas);
	test_RoundedRect(atlas);
	test_StrokedShape(atlas);

	atlas.build();

	std::cout << std::endl << "=== Atlas ===" << std::endl;

	for(auto key : {1u, 2u, 3u}) {
		const auto shape = atlas.getShape(key);

		if(shape) std::cout << "[" << key << "] " << *shape << std::endl;
		else std::cout << "[" << key << "] ERROR: not found" << std::endl;
	}

#ifdef SLUGHORN_HAS_SERIAL
	if(argc > 1) {
		std::ofstream f(argv[1]);

		slughorn::serial::writeJSON(atlas, f);

		std::cout << std::endl << "Wrote: " << argv[1] << std::endl;
	}

	else slughorn::serial::writeJSON(atlas, std::cout);
#endif

	return 0;
}
