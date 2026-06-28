//vimrun! ./slughorn-test-scanline
//
// Smoke-test: verify toMonotonicCurves() correctness and Atlas scanline texture population.

#include "slughorn/canvas.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

using namespace slughorn;
using namespace slughorn::literals;

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------

static bool isMonotonicAxis(const Atlas::Curve& c, bool useY) {
	const float c0 = useY ? c.y1 : c.x1;
	const float c1 = useY ? c.y2 : c.x2;
	const float c2 = useY ? c.y3 : c.x3;
	const float denom = c0 - 2.0f * c1 + c2;

	if(std::abs(denom) < 1e-6f) return true;

	const float t = (c0 - c1) / denom;

	return t <= 0.0f || t >= 1.0f;
}

static bool allMonotonic(const Atlas::Curves& curves) {
	for(const auto& c : curves) {
		if(!isMonotonicAxis(c, false) || !isMonotonicAxis(c, true)) return false;
	}

	return true;
}

static bool continuous(const Atlas::Curves& curves) {
	for(size_t i = 1; i < curves.size(); i++) {
		if(std::abs(curves[i].x1 - curves[i-1].x3) > 1e-5f) return false;
		if(std::abs(curves[i].y1 - curves[i-1].y3) > 1e-5f) return false;
	}

	return true;
}

static void checkDecompose(
	const char* label,
	const Atlas::Curve& c,
	size_t expectedMin,
	size_t expectedMax
) {
	Atlas::Curves out;

	toMonotonicCurves(c, out);

	const bool sizeOk = out.size() >= expectedMin && out.size() <= expectedMax;
	const bool monoOk = allMonotonic(out);
	const bool contOk = out.empty() || continuous(out);

	std::cout
		<< (sizeOk && monoOk && contOk ? "PASS" : "FAIL")
		<< " " << label
		<< " n=" << out.size()
		<< (monoOk ? "" : " [NOT MONOTONIC]")
		<< (contOk ? "" : " [NOT CONTINUOUS]")
		<< std::endl
	;

	assert(sizeOk && monoOk && contOk);
}

// ------------------------------------------------------------------------------------------------
// Main
// ------------------------------------------------------------------------------------------------

int main() {
	std::cout << "=== toMonotonicCurves ===" << std::endl;

	// Already monotonic diagonal.
	checkDecompose("diagonal", {0_cv, 0_cv, 0.5_cv, 0.5_cv, 1_cv, 1_cv}, 1, 1);

	// Horizontal linear: y-range = 0, dropped.
	{
		Atlas::Curves out;

		toMonotonicCurves({0_cv, 0.5_cv, 0.5_cv, 0.5_cv, 1_cv, 0.5_cv}, out);

		std::cout << (out.empty() ? "PASS" : "FAIL") << " horizontal drop n=" << out.size() << std::endl;

		assert(out.empty());
	}

	// Arch in y: y goes up then down -> 2 y-monotonic halves.
	checkDecompose("arch (y non-monotonic)", {0_cv, 0_cv, 0.5_cv, 1_cv, 1_cv, 0_cv}, 2, 2);

	// Arch in x: x goes right then back -> 2 x-monotonic halves.
	checkDecompose("x-arch (x non-monotonic)", {0_cv, 0_cv, 1_cv, 0.5_cv, 0_cv, 1_cv}, 2, 2);

	// Non-monotonic in both -> 3-4 sub-curves.
	checkDecompose("both non-monotonic", {0_cv, 0_cv, 1_cv, 1_cv, 0.5_cv, 0_cv}, 3, 4);

	// U-bowl: y-extremum only -> 2 curves.
	checkDecompose("U-bowl", {0_cv, 1_cv, 0.5_cv, 0_cv, 1_cv, 1_cv}, 2, 2);

	// Typical glyph arc: already monotonic.
	checkDecompose("glyph arc", {0.1_cv, 0_cv, 0.2_cv, 0.5_cv, 0.3_cv, 1_cv}, 1, 1);

	std::cout << std::endl << "=== Atlas scanline texture ===" << std::endl;

	Atlas atlas;

	// Triangle: 3 curves (1 horizontal -> dropped, 2 diagonals stay).
	{
		Atlas::Curves tri;

		CurveDecomposer cd(tri);

		cd.moveTo(0.0_cv, 0.0_cv);
		cd.lineTo(1.0_cv, 0.0_cv); // horizontal -> dropped
		cd.lineTo(0.5_cv, 1.0_cv);
		cd.close();

		Atlas::ShapeInfo info;

		info.curves = tri;

		atlas.addShape("tri", info);
	}

	// Circle approximation: 4 quadratic arcs.
	{
		Atlas::Curves circ;

		CurveDecomposer cd(circ);

		cd.moveTo(1.0_cv, 0.5_cv);
		cd.quadTo(1.0_cv, 1.0_cv, 0.5_cv, 1.0_cv);
		cd.quadTo(0.0_cv, 1.0_cv, 0.0_cv, 0.5_cv);
		cd.quadTo(0.0_cv, 0.0_cv, 0.5_cv, 0.0_cv);
		cd.quadTo(1.0_cv, 0.0_cv, 1.0_cv, 0.5_cv);

		Atlas::ShapeInfo info;

		info.curves = circ;

		atlas.addShape("circ", info);
	}

	atlas.enableScanlineData();
	atlas.build();

	const auto& td = atlas.getScanlineCurveTextureData();
	const auto& stats = atlas.getPackingStats();
	const auto tri = atlas.getShape("tri");
	const auto circ = atlas.getShape("circ");

	std::cout
		<< "Scanline texture: " << td.width << "x" << td.height
		<< " texels=" << stats.scanlineTexelsTotal << std::endl
	;

	assert(tri.has_value() && circ.has_value());
	assert(stats.scanlineTexelsTotal > 0);
	assert(td.bytes.size() == size_t{td.width} * td.height * 4 * sizeof(float));

	std::cout
		<< "tri start=" << tri->scanlineCurveStart
		<< " count=" << tri->scanlineCurveCount << std::endl
	;

	std::cout
		<< "circ start=" << circ->scanlineCurveStart
		<< " count=" << circ->scanlineCurveCount << std::endl
	;

	// The triangle had 1 horizontal (dropped) and 2 slanted lines -> at least 2 scanline curves.
	assert(tri->scanlineCurveCount >= 1);

	// Circle arcs: non-monotonic arcs get split -> should have more than 4.
	assert(circ->scanlineCurveCount >= 4);

	// Ranges must not overlap.
	assert(
		tri->scanlineCurveStart + tri->scanlineCurveCount * 2 <= circ->scanlineCurveStart ||
		circ->scanlineCurveStart + circ->scanlineCurveCount * 2 <= tri->scanlineCurveStart
	);

	// First texel of circ must be non-zero.
	const float* p = reinterpret_cast<const float*>(td.bytes.data());
	const uint32_t tx = circ->scanlineCurveStart % td.width;
	const uint32_t ty = circ->scanlineCurveStart / td.width;
	const float* c0 = p + (ty * td.width + tx) * 4;

	assert(c0[0] != 0.f || c0[1] != 0.f || c0[2] != 0.f || c0[3] != 0.f);

	std::cout
		<< "circ first texel: ("
		<< c0[0] << ", " << c0[1] << ", " << c0[2] << ", " << c0[3] << ")" << std::endl
	;

	std::cout << std::endl << "All tests passed." << std::endl;
}
