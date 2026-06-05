//vimrun! ./slughorn-test-bands

// Experiment: cost-based greedy band bisection vs. uniform placement.
//
// Key question: does computing splits from raw curves (what SplitStrategy does,
// pre-build) give the same result as computing them from a decoded Sampler
// (post-build)? If yes, SplitStrategy was the right abstraction. If no, we
// need post-build analysis and SplitStrategy is the wrong hook point.
//
// Nothing in slughorn changes here. If computeCostSplits proves out, it
// graduates to Atlas::computeCostSplits and we wire up SplitStrategy then.

#include "slughorn/canvas.hpp"
#include "slughorn/render.hpp"
#include "slughorn/serial.hpp"

#include <algorithm>
#include <iostream>
#include <limits>
#include <set>

using namespace slughorn::literals;
using slughorn::Atlas;
using slughorn::Key;
using slughorn::slug_t;
using Sampler = slughorn::render::Sampler;

// ============================================================================
// Band cost reporting
// ============================================================================

static void printBandCosts(const Sampler& s, const char* label) {
	constexpr uint32_t N = Atlas::INDIRECTION_SIZE;

	std::cout << label << ":\n";

	auto report = [&](
		const char* axis,
		const std::vector<uint32_t>& offsets,
		const std::array<uint8_t, N>& indir
	) {
		const size_t numBands = offsets.size() > 0 ? offsets.size() - 1 : 0;

		std::vector<uint32_t> slotCounts(numBands, 0);

		for(uint32_t q = 0; q < N; q++) if(indir[q] < numBands) slotCounts[indir[q]]++;

		slug_t totalCost = 0_cv;
		uint32_t maxCurves = 0;

		std::cout << "  " << axis << " (" << numBands << "): ";

		for(size_t i = 0; i < numBands; i++) {
			const uint32_t curves = offsets[i + 1] - offsets[i];
			const slug_t width = slug_t(slotCounts[i]) / slug_t(N);

			totalCost += width * slug_t(curves);
			maxCurves = std::max(maxCurves, curves);

			std::cout << curves << "(w=" << width << ") ";
		}

		std::cout << "| worst=" << maxCurves << " cost=" << totalCost << "\n";
	};

	report("H", s.hbandOffsets, s.indirY);
	report("V", s.vbandOffsets, s.indirX);
}

// ============================================================================
// iters heatmap -- actual per-fragment shader cost via renderSampleBanded
// ============================================================================

static void printItersHeatmap(const Sampler& s, uint32_t size = 24) {
	const auto [ox, oy] = s.emOrigin();
	const auto [sx, sy] = s.emSize();

	const slug_t ppeX = slug_t(size) / sx;
	const slug_t ppeY = slug_t(size) / sy;

	uint32_t maxIters = 0;

	// Two passes: first find max, then render relative to it.
	for(uint32_t j = 0; j < size; j++) for(uint32_t i = 0; i < size; i++) {
		const slug_t ex = ox + (slug_t(i) + 0.5_cv) / slug_t(size) * sx;
		const slug_t ey = oy + (slug_t(j) + 0.5_cv) / slug_t(size) * sy;
		maxIters = std::max(maxIters, s.renderSampleBanded(ex, ey, ppeX, ppeY).iters);
	}

	for(uint32_t j = 0; j < size; j++) {
		for(uint32_t i = 0; i < size; i++) {
			const slug_t ex = ox + (slug_t(i) + 0.5_cv) / slug_t(size) * sx;
			const slug_t ey = oy + (slug_t(j) + 0.5_cv) / slug_t(size) * sy;
			const uint32_t iters = s.renderSampleBanded(ex, ey, ppeX, ppeY).iters;

			std::cout << (iters == 0 ? '.' : iters < maxIters / 2 ? '+' : '#');
		}

		std::cout << '\n';
	}

	std::cout << "  (. = 0 iters, + = low, # = high, max=" << maxIters << ")\n";
}

// ============================================================================
// Cost-based greedy bisect
//
// Counts curves whose bbox overlaps [lo, hi) on the given axis.
// Works on any Atlas::Curves -- raw ShapeInfo curves OR sampler.curves.
// ============================================================================

// Curves whose bbox is FULLY CONTAINED in [lo, hi] -- the "localizable" curves.
//
// Spanning curves (e.g. the straight sides of a stroke that run the full width)
// overlap every band and can never be isolated by splitting. Using containedCount
// for split placement means we ignore that irreducible background noise and focus
// splits where they actually help: around clusters of localizable geometry.
static uint32_t containedCount(
	const Atlas::Curves& curves, bool useY, slug_t lo, slug_t hi
) {
	uint32_t n = 0;

	for(const auto& c : curves) {
		const slug_t a = useY ? std::min({c.y1, c.y2, c.y3}) : std::min({c.x1, c.x2, c.x3});
		const slug_t b = useY ? std::max({c.y1, c.y2, c.y3}) : std::max({c.x1, c.x2, c.x3});

		if(a >= lo && b <= hi) n++;
	}

	return n;
}

// shapeMin/shapeRange must match what the atlas will use for this shape's band transform
// (i.e. the shape's declared extent, which equals the curve bbox when autoMetrics=true).
// Splits are returned as fractions of [shapeMin, shapeMin+shapeRange].
static std::vector<slug_t> computeCostAxis(
	const Atlas::Curves& curves, bool useY, int numBands,
	slug_t shapeMin, slug_t shapeRange
) {
	if(numBands <= 1 || curves.empty()) return {};

	constexpr uint32_t N = Atlas::INDIRECTION_SIZE;

	struct Band { slug_t lo, hi; };

	// Curve bbox in em-space -- used to bound the search region.
	slug_t minV = std::numeric_limits<slug_t>::max();
	slug_t maxV = std::numeric_limits<slug_t>::lowest();

	for(const auto& c : curves) {
		const slug_t a = useY ? std::min({c.y1, c.y2, c.y3}) : std::min({c.x1, c.x2, c.x3});
		const slug_t b = useY ? std::max({c.y1, c.y2, c.y3}) : std::max({c.x1, c.x2, c.x3});
		minV = std::min(minV, a);
		maxV = std::max(maxV, b);
	}

	std::vector<Band> bands{{minV, maxV}};
	std::vector<slug_t> splits;

	while(static_cast<int>(bands.size()) < numBands) {
		// Find the most expensive band using CONTAINED curves only.
		// Spanning curves (bbox wider than the band) are irreducible -- splitting
		// never removes them -- so they must not drive the greedy selection.
		size_t worst = 0;
		slug_t worstCost = -1_cv;

		for(size_t i = 0; i < bands.size(); i++) {
			const slug_t cost = (bands[i].hi - bands[i].lo)
				* slug_t(containedCount(curves, useY, bands[i].lo, bands[i].hi));

			if(cost > worstCost) { worstCost = cost; worst = i; }
		}

		const slug_t lo = bands[worst].lo, hi = bands[worst].hi;

		// Candidates: curve-bbox edges inside this band, plus the midpoint.
		// These are the natural split points where the cost function is meaningful --
		// splitting at a curve boundary never creates new "spanning" assignments.
		std::vector<slug_t> rawCandidates;

		for(const auto& c : curves) {
			const slug_t a = useY ? std::min({c.y1, c.y2, c.y3}) : std::min({c.x1, c.x2, c.x3});
			const slug_t b = useY ? std::max({c.y1, c.y2, c.y3}) : std::max({c.x1, c.x2, c.x3});

			if(a > lo && a < hi) rawCandidates.push_back(a);
			if(b > lo && b < hi) rawCandidates.push_back(b);
		}

		rawCandidates.push_back((lo + hi) * 0.5_cv);

		// Snap each raw candidate to the nearest grid position strictly inside (lo, hi).
		// Multiple raw candidates that round to the same slot collapse to one entry.
		// This guarantees no two splits ever share a slot (-> no w=0 bands).
		std::set<slug_t> snappedSet;

		for(slug_t s : rawCandidates) {
			const slug_t frac    = (s - shapeMin) / shapeRange;
			const slug_t snapped = std::round(frac * slug_t(N)) / slug_t(N);
			const slug_t emPos   = shapeMin + snapped * shapeRange;

			if(emPos > lo && emPos < hi) snappedSet.insert(emPos);
		}

		// Fallback when the band is narrower than one slot: use the lowest grid point
		// strictly above lo.
		if(snappedSet.empty()) {
			const slug_t fracLo = (lo - shapeMin) / shapeRange;
			const slug_t snap   = std::ceil(fracLo * slug_t(N)) / slug_t(N);
			const slug_t emPos  = shapeMin + snap * shapeRange;

			if(emPos > lo && emPos < hi) snappedSet.insert(emPos);
		}

		// If the band has no representable grid position at all, stop splitting.
		if(snappedSet.empty()) break;

		const std::vector<slug_t> candidates(snappedSet.begin(), snappedSet.end());

		// Pick the grid-aligned candidate that minimizes total sub-band contained cost.
		slug_t bestSplit = candidates.front();
		slug_t bestCost  = std::numeric_limits<slug_t>::max();

		for(slug_t s : candidates) {
			const slug_t cost =
				(s - lo) * slug_t(containedCount(curves, useY, lo, s)) +
				(hi - s) * slug_t(containedCount(curves, useY, s, hi));

			if(cost < bestCost) { bestCost = cost; bestSplit = s; }
		}

		const auto it = bands.begin() + static_cast<ptrdiff_t>(worst);
		bands.erase(it);
		bands.insert(bands.begin() + static_cast<ptrdiff_t>(worst), Band{bestSplit, hi});
		bands.insert(bands.begin() + static_cast<ptrdiff_t>(worst), Band{lo, bestSplit});

		splits.push_back((bestSplit - shapeMin) / shapeRange);
	}

	std::sort(splits.begin(), splits.end());
	return splits;
}

// Public entry point. Takes a Sampler so it works post-build on any shape.
// Sampler carries the shape's band transform (scale/offset) which encodes the
// exact shapeMin/shapeRange the atlas used -- pass those so grid positions align.
static std::pair<std::vector<slug_t>, std::vector<slug_t>> computeCostSplits(
	const Sampler& s, int numBandsX, int numBandsY
) {
	const auto [ox, oy] = s.emOrigin();
	const auto [sx, sy] = s.emSize();

	return {
		computeCostAxis(s.curves, false, numBandsX, ox, sx),
		computeCostAxis(s.curves, true,  numBandsY, oy, sy),
	};
}

// ============================================================================
// Rounded rect experiment
//
// Uses Canvas so we can set a SplitStrategy at commit time (pre-build).
// Captures both the raw curves the strategy sees AND the splits it computed,
// then compares against the post-build path (computeCostSplits on sampler.curves).
//
// Hand-tuned reference from osgslug-tmp.splits.cpp test 2:
//   SPLIT_01-04 + SPLIT_28-31 per axis -> ~70us vs 110-120us uniform (36-40% faster)
// ============================================================================

static void runRoundedRect() {
	using slughorn::canvas::Canvas;
	using slughorn::Color;

	const Color white    = {1_cv, 1_cv, 1_cv, 1_cv};
	const int numBandsX  = 9;
	const int numBandsY  = 9;

	auto drawShape = [](Canvas& c) {
		c.beginPath();
		c.roundedRect(0.1_cv, 0.1_cv, 0.9_cv, 0.9_cv, 0.05_cv);
	};

	// ---- Pass 1: uniform -----------------------------------------------

	Atlas atlas1;
	{
		Canvas canvas(atlas1);

		canvas.setSplitStrategy([&](const Atlas::Curves& c) {
			return Atlas::computeUniformSplits(c, numBandsX, numBandsY);
		});

		drawShape(canvas);
		canvas.stroke(0.05_cv, white, 1_cv, Key("rr"));
		atlas1.build();
	}

	const auto s1 = slughorn::render::decode(atlas1, Key("rr"));

	std::cout << "\n=== Rounded Rect -- uniform " << numBandsX << "x" << numBandsY << " ===\n";
	printBandCosts(s1, "rr");
	std::cout << "\n";
	printItersHeatmap(s1);

	// ---- Pass 2: SplitStrategy (pre-build) -----------------------------
	//
	// The lambda is called at canvas.stroke() time with the raw tessellated
	// curves before the atlas is built. We capture both the curves and the
	// computed splits for comparison.

	Atlas::Curves capturedCurves;
	std::vector<slug_t> capturedSplitsX, capturedSplitsY;

	Atlas atlas2;
	{
		Canvas canvas(atlas2);

		canvas.setSplitStrategy([&](const Atlas::Curves& c) {
			capturedCurves = c;

			// Shape extent = curve bbox (autoMetrics=true is Canvas default).
			// Compute it here so grid positions align with what the atlas will use.
			slug_t minX = 1e9_cv, maxX = -1e9_cv, minY = 1e9_cv, maxY = -1e9_cv;

			for(const auto& cv : c) {
				minX = std::min({minX, cv.x1, cv.x2, cv.x3});
				maxX = std::max({maxX, cv.x1, cv.x2, cv.x3});
				minY = std::min({minY, cv.y1, cv.y2, cv.y3});
				maxY = std::max({maxY, cv.y1, cv.y2, cv.y3});
			}

			capturedSplitsX = computeCostAxis(c, false, numBandsX, minX, maxX - minX);
			capturedSplitsY = computeCostAxis(c, true,  numBandsY, minY, maxY - minY);

			return std::make_pair(capturedSplitsX, capturedSplitsY);
		});

		drawShape(canvas);
		canvas.stroke(0.05_cv, white, 1_cv, Key("rr"));
		atlas2.build();
	}

	const auto s2 = slughorn::render::decode(atlas2, Key("rr"));

	std::cout << "\n=== Rounded Rect -- SplitStrategy (pre-build) " << numBandsX << "x" << numBandsY << " ===\n";

	std::cout << "Computed X: "; for(slug_t s : capturedSplitsX) std::cout << s << " "; std::cout << "\n";
	std::cout << "Computed Y: "; for(slug_t s : capturedSplitsY) std::cout << s << " "; std::cout << "\n";
	std::cout << "Hand-tuned: SPLIT_01-04 + SPLIT_28-31 = "
	          << "0.03125 0.0625 0.09375 0.125 ... 0.875 0.90625 0.9375 0.96875\n";

	printBandCosts(s2, "rr");
	std::cout << "\n";
	printItersHeatmap(s2);

	// ---- Pre==post check -----------------------------------------------
	//
	// Do the raw Canvas curves (capturedCurves, pre-build) and the decoded
	// sampler.curves (post-build from uniform atlas) produce identical splits?
	// If yes, SplitStrategy is the right hook point.

	const auto [pox, poy] = s1.emOrigin();
	const auto [psx, psy] = s1.emSize();

	const auto postX = computeCostAxis(s1.curves, false, numBandsX, pox, psx);
	const auto postY = computeCostAxis(s1.curves, true,  numBandsY, poy, psy);

	std::cout << "\npre==post?"
	          << " X=" << (capturedSplitsX == postX ? "YES" : "NO")
	          << " Y=" << (capturedSplitsY == postY ? "YES" : "NO") << "\n";

	std::cout << "capturedCurves.size()=" << capturedCurves.size()
	          << " sampler.curves.size()=" << s1.curves.size() << "\n";
}

// ============================================================================
// Main
// ============================================================================

int main() {
	// Dumbbell: two blobs at opposite corners, large empty center.
	// Perfect stress-test: the ideal splits isolate each blob and leave
	// the center as a zero-curve band the shader exits immediately.
	const Atlas::Curves curves = {
		// Bottom-left blob: circle around (0.25, 0.2), r?0.15
		{0.10_cv, 0.20_cv, 0.10_cv, 0.05_cv, 0.25_cv, 0.05_cv},
		{0.25_cv, 0.05_cv, 0.40_cv, 0.05_cv, 0.40_cv, 0.20_cv},
		{0.40_cv, 0.20_cv, 0.40_cv, 0.35_cv, 0.25_cv, 0.35_cv},
		{0.25_cv, 0.35_cv, 0.10_cv, 0.35_cv, 0.10_cv, 0.20_cv},

		// Top-right blob: circle around (0.75, 0.8), r?0.15
		{0.60_cv, 0.80_cv, 0.60_cv, 0.65_cv, 0.75_cv, 0.65_cv},
		{0.75_cv, 0.65_cv, 0.90_cv, 0.65_cv, 0.90_cv, 0.80_cv},
		{0.90_cv, 0.80_cv, 0.90_cv, 0.95_cv, 0.75_cv, 0.95_cv},
		{0.75_cv, 0.95_cv, 0.60_cv, 0.95_cv, 0.60_cv, 0.80_cv},
	};

	const int numBandsX = 4, numBandsY = 4;

	// ---- Pass 1: uniform splits ----------------------------------------

	Atlas atlas1;
	{
		Atlas::ShapeInfo info;

		info.curves    = curves;
		info.numBandsX = numBandsX;
		info.numBandsY = numBandsY;

		atlas1.addShape("dbell", info);
	}

	atlas1.build();
	slughorn::serial::write(atlas1, "slughorn-test-bands.slug");

	const auto s1 = slughorn::render::decode(atlas1, Key("dbell"));

	std::cout << "=== Pass 1: uniform " << numBandsX << "x" << numBandsY << " ===\n";
	printBandCosts(s1, "dbell");
	std::cout << "\n";
	printItersHeatmap(s1);

	// ---- Derive cost-based splits from the decoded Sampler ---------------
	//
	// This is the post-build path: sampler.curves are decoded from the atlas
	// textures and require no access to the original ShapeInfo.
	//
	// To test whether SplitStrategy (pre-build) gives the same answer, also
	// run computeCostAxis directly on the raw curves and compare the results.

	const auto [splitsX, splitsY] = computeCostSplits(s1, numBandsX, numBandsY);

	// Pre-build answer: same shape extent as the atlas so pre==post remains meaningful.
	const auto [ox, oy] = s1.emOrigin();
	const auto [sx, sy] = s1.emSize();

	const auto preX = computeCostAxis(curves, false, numBandsX, ox, sx);
	const auto preY = computeCostAxis(curves, true,  numBandsY, oy, sy);

	std::cout << "\nCost-based splits (post-build from sampler.curves):\n";
	std::cout << "  X: "; for(slug_t s : splitsX) std::cout << s << " "; std::cout << "\n";
	std::cout << "  Y: "; for(slug_t s : splitsY) std::cout << s << " "; std::cout << "\n";

	std::cout << "\nCost-based splits (pre-build from raw curves):\n";
	std::cout << "  X: "; for(slug_t s : preX) std::cout << s << " "; std::cout << "\n";
	std::cout << "  Y: "; for(slug_t s : preY) std::cout << s << " "; std::cout << "\n";

	std::cout << "\npre==post? X=" << (preX == splitsX ? "YES" : "NO")
	          << " Y=" << (preY == splitsY ? "YES" : "NO") << "\n";

	// ---- Pass 2: cost-based splits --------------------------------------

	Atlas atlas2;
	{
		Atlas::ShapeInfo info;

		info.curves  = curves;
		info.splitsX = splitsX;
		info.splitsY = splitsY;

		atlas2.addShape("dbell", info);
	}

	atlas2.build();

	const auto s2 = slughorn::render::decode(atlas2, Key("dbell"));

	std::cout << "\n=== Pass 2: cost-based " << numBandsX << "x" << numBandsY << " ===\n";
	printBandCosts(s2, "dbell");
	std::cout << "\n";
	printItersHeatmap(s2);

	runRoundedRect();

	return 0;
}
