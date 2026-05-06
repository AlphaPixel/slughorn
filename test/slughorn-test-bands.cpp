//vimrun! ./slughorn-test-bands

#include "slughorn/serial.hpp"

#include <iostream>

using namespace slughorn::literals;
using slughorn::slug_t;

#if 0
  Original uniform path (sentinel fires, direct formula in shader):
  slughorn::Atlas::ShapeInfo info;
  info.curves = myCurves;
  info.numBandsX = 8;
  info.numBandsY = 8;
  // splitsY/X left empty → splitFraction stays 0.f → B==0 → direct formula
  atlas.addShape(key, info);

  Adaptive path (scan with valley positions):
  slughorn::Atlas::ShapeInfo info;
  info.curves = myCurves;
  auto [splitsX, splitsY] = slughorn::Atlas::computeAdaptiveSplits(myCurves, 8, 8); // numBandsX, numBandsY
  info.splitsY = splitsY;
  info.splitsX = splitsX;
  // splitsY/X non-empty → splitFraction written → B!=0 → scan path
  atlas.addShape(key, info);
#endif

int main(int argc, char** argv) {
	slughorn::Atlas atlas;

	auto curves = slughorn::Atlas::Curves{
		// Bottom-left blob: rough circle around (0.25, 0.2)
		// 4 quadratic arcs, each covering one quadrant of the circle
		{0.10_cv, 0.20_cv, 0.10_cv, 0.05_cv, 0.25_cv, 0.05_cv},
		{0.25_cv, 0.05_cv, 0.40_cv, 0.05_cv, 0.40_cv, 0.20_cv},
		{0.40_cv, 0.20_cv, 0.40_cv, 0.35_cv, 0.25_cv, 0.35_cv},
		{0.25_cv, 0.35_cv, 0.10_cv, 0.35_cv, 0.10_cv, 0.20_cv},

		// Top-right blob: rough circle around (0.75, 0.8)
		{0.60_cv, 0.80_cv, 0.60_cv, 0.65_cv, 0.75_cv, 0.65_cv},
		{0.75_cv, 0.65_cv, 0.90_cv, 0.65_cv, 0.90_cv, 0.80_cv},
		{0.90_cv, 0.80_cv, 0.90_cv, 0.95_cv, 0.75_cv, 0.95_cv},
		{0.75_cv, 0.95_cv, 0.60_cv, 0.95_cv, 0.60_cv, 0.80_cv},
	};

	const int x = 8, y = 4;

	// dbm = Dumbbell MANUAL
	slughorn::Atlas::ShapeInfo dbm;

	dbm.curves = curves;
	dbm.splitsX = {0.05_cv, 0.1_cv, 0.15_cv, 0.2_cv, 0.25_cv, 0.3_cv, 0.5_cv};
	dbm.splitsY = {0.25_cv, 0.5_cv, 0.75_cv};
	dbm.numBandsX = x;
	dbm.numBandsY = y;

	atlas.addShape("dbm", dbm);

	// dba = Dumbbell ADAPTIVE
	slughorn::Atlas::ShapeInfo dba;

	std::tie(dba.splitsX, dba.splitsY) = slughorn::Atlas::computeAdaptiveSplits(curves, x, y); // x=numBandsX, y=numBandsY

	auto showSplits = [](auto& splits, size_t size, const char* name) {
		std::cout << "splits" << name << ": ";

		for(size_t i = 0; i < size; i++) std::cout << splits[i] << " ";

		std::cout << std::endl;
	};

	showSplits(dba.splitsX, x - 1, "X");
	showSplits(dba.splitsY, y - 1, "Y");

	dba.curves = curves;
	dba.numBandsX = x;
	dba.numBandsY = y;

	atlas.addShape("dba", dba);

	// dbo = Dumbbell ORIGINAL
	slughorn::Atlas::ShapeInfo dbo;

	dbo.curves = curves;
	dbo.numBandsX = x;
	dbo.numBandsY = y;

	atlas.addShape("dbo", dbo);

	// Finish up, and dump the .slug file!
	atlas.build();

	slughorn::serial::write(atlas, "slughorn-test-bands.slug");

	return 0;
}
