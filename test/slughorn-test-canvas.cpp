//vimrun! ./slughorn-test-canvas

#include "slughorn-canvas.hpp"

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn-serial.hpp"

#include <iostream>

int main(int argc, char** argv) {
	slughorn::Atlas atlas;

	uint32_t keyBase = 0xE0000;

	slughorn::canvas::Canvas canvas(atlas, keyBase);

	// fast, visible only at large sizes
	canvas.decomposer().tolerance = slughorn::TOLERANCE_DRAFT;

	// good default for screen work
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_BALANCED;

	// high-DPI / print / export
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_FINE;

	// The DEFAULT; lowest quality, always results in the simplest cubic -> 2x quadratics
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_EXACT;

	// Shape 1: a filled red triangle
	canvas.beginPath();
	canvas.moveTo(0.0_cv, 0.0_cv);
	canvas.lineTo(1.0_cv, 0.0_cv);
	canvas.lineTo(0.5_cv, 1.0_cv);
	canvas.closePath();
	canvas.fill({1_cv, 0_cv, 0_cv, 1_cv});

	// Shape 2: a blue semicircle via arc()
	canvas.beginPath();
	canvas.arc(0.5_cv, 0.5_cv, 0.4_cv, 0.0_cv, cv(M_PI));
	canvas.closePath();
	canvas.fill({0_cv, 0_cv, 1_cv, 1_cv});

	// Shape 3: a green stadium shape via arcTo() rounded corners
	canvas.beginPath();
	canvas.moveTo(0.2_cv, 0.3_cv);
	canvas.arcTo(0.8_cv, 0.3_cv, 0.8_cv, 0.7_cv, 0.1_cv);
	canvas.arcTo(0.8_cv, 0.7_cv, 0.2_cv, 0.7_cv, 0.1_cv);
	canvas.arcTo(0.2_cv, 0.7_cv, 0.2_cv, 0.3_cv, 0.1_cv);
	canvas.arcTo(0.2_cv, 0.3_cv, 0.8_cv, 0.3_cv, 0.1_cv);
	canvas.closePath();
	canvas.fill({0_cv, 0.6_cv, 0_cv, 1_cv});

	// Commit all three layers as one named composite shape
	canvas.finalize(slughorn::Key::fromString("my_scene"));

	atlas.build();

	std::cerr << "PackingStats: " << atlas.getPackingStats() << std::endl;

	slughorn::serial::writeJSON(atlas, std::cout);

	return 0;
}
