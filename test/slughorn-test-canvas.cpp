//vimrun! ./slughorn-test-canvas

#include "slughorn/canvas.hpp"

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn/serial.hpp"

#include <iostream>

using namespace slughorn::literals;
using slughorn::slug_t;

int main(int argc, char** argv) {
	slughorn::Atlas atlas;

	// slughorn::canvas::Canvas canvas(atlas, 0x0);
	slughorn::canvas::Canvas canvas(atlas, "MyShape");

	// fast, visible only at large sizes
	canvas.decomposer().tolerance = slughorn::TOLERANCE_DRAFT;

	// good default for screen work
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_BALANCED;

	// high-DPI / print / export
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_FINE;

	// The DEFAULT; lowest quality, always results in the simplest cubic -> 2x quadratics
	// canvas.decomposer().tolerance = slughorn::TOLERANCE_EXACT;

	slug_t scale = 1_cv / 4096_cv;

	// Shape 1: a filled red triangle
	canvas.beginPath();
	canvas.moveTo(0.0_cv * scale, 0.0_cv * scale);
	canvas.lineTo(1.0_cv * scale, 0.0_cv * scale);
	canvas.lineTo(0.5_cv * scale, 1.0_cv * scale);
	canvas.closePath();
	canvas.fill({1_cv, 0_cv, 0_cv, 1_cv}, scale);

	// Shape 2: a blue semicircle via arc()
	canvas.beginPath();
	canvas.arc(0.5_cv * scale, 0.5_cv * scale, 0.4_cv * scale, 0.0_cv, cv(M_PI));
	canvas.closePath();
	canvas.fill({0_cv, 0_cv, 1_cv, 1_cv}, scale);

	// Shape 3: a green stadium shape via arcTo() rounded corners
	canvas.beginPath();
	canvas.moveTo(0.2_cv * scale, 0.3_cv * scale);
	canvas.arcTo(0.8_cv * scale, 0.3_cv * scale, 0.8_cv * scale, 0.7_cv * scale, 0.1_cv);
	canvas.arcTo(0.8_cv * scale, 0.7_cv * scale, 0.2_cv * scale, 0.7_cv * scale, 0.1_cv);
	canvas.arcTo(0.2_cv * scale, 0.7_cv * scale, 0.2_cv * scale, 0.3_cv * scale, 0.1_cv);
	canvas.arcTo(0.2_cv * scale, 0.3_cv * scale, 0.8_cv * scale, 0.3_cv * scale, 0.1_cv);
	canvas.closePath();
	canvas.fill({0_cv, 0.6_cv, 0_cv, 1_cv}, scale);

	// Commit all three layers as one named composite shape
	canvas.finalize(slughorn::Key::fromString("my_scene"));

	// Stroke: S-curve (two connected quadratics, opposite curvature).
	canvas.beginPath();
	canvas.moveTo(0.2_cv, 0.85_cv);
	canvas.quadTo(0.1_cv, 0.5_cv, 0.5_cv, 0.5_cv);
	canvas.quadTo(0.9_cv, 0.5_cv, 0.8_cv, 0.15_cv);
	canvas.stroke(0.12_cv);

	// TODO: Phase 2 will see how far we get with this...
	/* canvas.moveTo(0_cv, 0_cv);
	canvas.lineTo(5_cv, 5_cv);
	canvas.lineTo(10_cv, 0_cv);
	canvas.lineTo(15_cv, 0_cv);
	canvas.lineTo(20_cv, 10_cv);
	canvas.lineTo(25_cv, 10_cv);
	canvas.lineTo(30_cv, 15_cv);
	canvas.lineTo(35_cv, 0_cv);
	canvas.lineTo(40_cv, 5_cv);
	canvas.lineTo(45_cv, 0_cv);
	canvas.lineTo(50_cv, 0_cv);
	canvas.stroke(2_cv); */

	canvas.defineShape(slughorn::Key::fromString("stroke_test"), 1.0_cv);

	// TODO: This is the corresponding Phase 2 test call.
	// canvas.defineShape(slughorn::Key::fromString("stroke_test"), 1_cv / 50_cv);

	atlas.build();

	std::cerr << "PackingStats: " << atlas.getPackingStats() << std::endl;

	slughorn::serial::writeJSON(atlas, std::cout);

	return 0;
}
