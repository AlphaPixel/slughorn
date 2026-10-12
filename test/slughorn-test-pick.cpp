//vimrun! ./slughorn-test-pick
//
// Smoke-test for render::Sampler's pick() method (slughorn/render.hpp) - the CPU-side, GL-free
// hit test against a shape's analytic fill (see Sampler::pick()'s own comment for the design).
// No SLUGHORN_SERIAL dependency: everything here is in-memory, built straight from canvas::Canvas.
//
// WIP: there is deliberately no multi-layer CompositeShape hit test here yet - dropped pending a
// future CompositeSampler; not an oversight.

#include "slughorn/canvas.hpp"
#include "slughorn/render.hpp"

#include <cassert>
#include <iostream>

using namespace slughorn::literals;

using slughorn::Atlas;
using slughorn::Key;
using slughorn::Transform;
using slughorn::slug_t;
using Sampler = slughorn::render::Sampler;

namespace {

Atlas buildCircleAtlas() {
	Atlas atlas;
	slughorn::canvas::Canvas canvas(atlas);

	// Circle at authored center (0.5, 0.5), radius 0.4 - default origin (0,0), so world == local
	// em-space with the identity Transform/scale=1 the identity-transform checks below use.
	canvas.circle(0.5_cv, 0.5_cv, 0.4_cv);
	canvas.fill({1_cv, 1_cv, 1_cv, 1_cv}, 1_cv, Key("circle"));
	canvas.finalize(Key("circle_comp"));

	atlas.build();

	return atlas;
}

}

// Sampler(shape) - the cheap ctor, no decode(). Covers the identity-transform and placed-
// transform (non-trivial scale/origin) hit-test math.
static int runUnbandedLevel() {
	const Atlas atlas = buildCircleAtlas();
	const auto shape = atlas.getShape(Key("circle"));

	assert(shape && "circle shape missing");

	const Sampler sampler(*shape);

	assert(sampler.hbandOffsets.empty() && "cheap ctor must not decode bands");

	const Transform identity{};

	assert(sampler.pick(identity, 1_cv, 0.5_cv, 0.5_cv) && "center must hit");
	assert(!sampler.pick(identity, 1_cv, 0_cv, 0_cv) && "corner must miss");
	assert(!sampler.pick(identity, 1_cv, 0.85_cv, 0.5_cv) && "just past the right edge must miss");
	assert(sampler.pick(identity, 1_cv, 0.75_cv, 0.5_cv) && "just inside the right edge must hit");

	std::cout << "Sampler(shape).pick() identity-transform checks passed.\n";

	// Same shape, moved/scaled - proves the inverse-transform math, not just the trivial identity
	// case. World center = scale * (local - origin + transform) = 2 * (0.5 - 0 + 10) = 21,
	// 2 * (0.5 - 0 + 5) = 11 (see Sampler::pick()'s own comment).
	const Transform placed{10_cv, 5_cv, 0_cv};
	const slug_t placedScale = 2_cv;

	assert(sampler.pick(placed, placedScale, 21_cv, 11_cv) && "placed center must hit");
	assert(!sampler.pick(placed, placedScale, 0.5_cv, 0.5_cv) && "pre-transform coords must miss once placed");
	assert(!sampler.pick(placed, placedScale, 21_cv, 0_cv) && "far away must miss");

	std::cout << "Sampler(shape).pick() placed-transform checks passed.\n";

	return 0;
}

// Sampler(atlas, key) - the full decode() ctor. Confirms the banded path agrees with the
// unbanded one on the same query points.
static int runBandedLevel() {
	const Atlas atlas = buildCircleAtlas();
	const Sampler sampler(atlas, Key("circle"));

	assert(sampler.hbandOffsets.size() >= 2 && "atlas+key ctor must decode bands");

	const Transform identity{};

	assert(sampler.pick(identity, 1_cv, 0.5_cv, 0.5_cv) && "banded center must hit");
	assert(!sampler.pick(identity, 1_cv, 0_cv, 0_cv) && "banded corner must miss");

	std::cout << "Sampler(atlas, key).pick() banded checks passed.\n";

	return 0;
}

int main() {
	int rc = 0;

	rc |= runUnbandedLevel();
	rc |= runBandedLevel();

	std::cout << "All slughorn-test-pick checks passed.\n";

	return rc;
}
