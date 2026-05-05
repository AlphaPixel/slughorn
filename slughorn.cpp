#include "slughorn.hpp"

#ifdef SLUGHORN_SERIAL_IMPLEMENTATION
#include "slughorn-serial.hpp"
#endif

#ifdef SLUGHORN_NANOSVG_IMPLEMENTATION
#include "slughorn-nanosvg.hpp"
#endif

#ifdef SLUGHORN_FREETYPE_IMPLEMENTATION
#include "slughorn-freetype.hpp"
#endif

#include <algorithm>
#include <cstring>

// ================================================================================================
// File-local helpers
// ================================================================================================
namespace {

// Sweep-line density analysis for one axis. Returns numBands-1 split positions in em-space,
// placed at curve-density valleys (positions where fewest curves' bounding boxes cross).
//
// Uses the same bounding-box interval test as curveIntersectsBandY/X so the density profile
// exactly predicts which curves land in each band after buildShapeBands runs.
std::vector<slug_t> computeAxisSplits(
	const slughorn::Atlas::Curves& curves,
	bool useY,
	slug_t axisMin, slug_t axisMax, slug_t axisRange,
	uint32_t numBands
) {
	if(numBands <= 1) return {};

	const uint32_t numSplits = numBands - 1;
	const slug_t minSpacing = axisRange / slug_t(numBands * 4);

	// Build event list: +1 where a curve's bbox starts on this axis, -1 where it ends.
	struct Event { slug_t pos; int delta; };
	std::vector<Event> events;
	events.reserve(curves.size() * 2);

	for(const auto& c : curves) {
		const slug_t lo = useY
			? std::min({c.y1, c.y2, c.y3})
			: std::min({c.x1, c.x2, c.x3});
		const slug_t hi = useY
			? std::max({c.y1, c.y2, c.y3})
			: std::max({c.x1, c.x2, c.x3});

		if(hi > lo) {
			events.push_back({std::max(lo, axisMin), +1});
			events.push_back({std::min(hi, axisMax), -1});
		}
	}

	// Sort by position; at ties, process decrements before increments so that
	// a curve ending exactly where another begins produces a zero-density gap.
	std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) {
		return a.pos < b.pos || (a.pos == b.pos && a.delta < b.delta);
	});

	// Sweep: record density of each region between consecutive event positions.
	struct Region { slug_t lo, hi; int density; };
	std::vector<Region> regions;
	int active = 0;
	slug_t prevPos = axisMin;

	for(size_t i = 0; i < events.size(); ) {
		const slug_t evPos = events[i].pos;

		if(evPos > prevPos + 1e-7f) {
			regions.push_back({prevPos, evPos, active});
			prevPos = evPos;
		}

		while(i < events.size() && events[i].pos <= evPos + 1e-7f) {
			active = std::max(0, active + events[i].delta);
			i++;
		}
	}

	if(axisMax > prevPos + 1e-7f)
		regions.push_back({prevPos, axisMax, active});

	if(regions.empty()) {
		std::vector<slug_t> splits(numSplits);
		for(uint32_t i = 0; i < numSplits; i++)
			splits[i] = axisMin + axisRange * float(i + 1) / float(numBands);
		return splits;
	}

	// Sort regions by density ascending: lowest density = best valley candidates.
	std::vector<Region> sorted = regions;
	std::sort(sorted.begin(), sorted.end(), [](const Region& a, const Region& b) {
		return a.density < b.density;
	});

	// Greedily pick valley midpoints, enforcing minimum spacing between splits.
	std::vector<slug_t> splits;
	splits.reserve(numSplits);

	for(const auto& region : sorted) {
		if(splits.size() >= numSplits) break;

		const slug_t mid = (region.lo + region.hi) * 0.5f;
		bool tooClose = false;

		for(slug_t s : splits) {
			if(std::abs(mid - s) < minSpacing) { tooClose = true; break; }
		}

		if(!tooClose) splits.push_back(mid);
	}

	// Fallback: fill any remaining slots with uniform positions.
	for(uint32_t i = 1; i <= numSplits && splits.size() < numSplits; i++) {
		const slug_t pos = axisMin + axisRange * float(i) / float(numBands);
		bool tooClose = false;

		for(slug_t s : splits) {
			if(std::abs(pos - s) < minSpacing) { tooClose = true; break; }
		}

		if(!tooClose) splits.push_back(pos);
	}

	std::sort(splits.begin(), splits.end());

	for(auto& s : splits) s = (s - axisMin) / axisRange;

	return splits;
}

bool curveIntersectsBandY(const slughorn::Atlas::Curve& c, slug_t lo, slug_t hi) {
	const slug_t minY = std::min({c.y1, c.y2, c.y3});
	const slug_t maxY = std::max({c.y1, c.y2, c.y3});

	return maxY >= lo && minY <= hi;
}

bool curveIntersectsBandX(const slughorn::Atlas::Curve& c, slug_t lo, slug_t hi) {
	const slug_t minX = std::min({c.x1, c.x2, c.x3});
	const slug_t maxX = std::max({c.x1, c.x2, c.x3});

	return maxX >= lo && minX <= hi;
}

// Advance @p cursor so that a span of @p span texels fits without crossing a row boundary in a
// texture of @p width texels.
uint32_t alignCursorForSpan(uint32_t cursor, uint32_t width, uint32_t span) {
	if(!span) return cursor;

	const uint32_t x = cursor % width;

	if(x + span > width) cursor += width - x;

	return cursor;
}

}

namespace slughorn {

Atlas::Atlas() = default;
Atlas::~Atlas() = default;

// ================================================================================================
// Atlas::computeAdaptiveSplits / computeUniformSplits
// ================================================================================================

std::pair<std::vector<slug_t>, std::vector<slug_t>> Atlas::computeAdaptiveSplits(
	const Curves& curves,
	int numBandsX,
	int numBandsY
) {
	if(curves.empty()) return {};

	slug_t minX = 1e9_cv, minY = 1e9_cv;
	slug_t maxX = -1e9_cv, maxY = -1e9_cv;

	for(const auto& c : curves) {
		minX = std::min({minX, c.x1, c.x2, c.x3});
		minY = std::min({minY, c.y1, c.y2, c.y3});
		maxX = std::max({maxX, c.x1, c.x2, c.x3});
		maxY = std::max({maxY, c.y1, c.y2, c.y3});
	}

	const slug_t rangeX = std::max(maxX - minX, 1e-6_cv);
	const slug_t rangeY = std::max(maxY - minY, 1e-6_cv);

	const uint32_t nY = numBandsY > 1 ? static_cast<uint32_t>(numBandsY) : 0u;
	const uint32_t nX = numBandsX > 1 ? static_cast<uint32_t>(numBandsX) : 0u;

	return {
		computeAxisSplits(curves, /*useY=*/false, minX, maxX, rangeX, nX),
		computeAxisSplits(curves, /*useY=*/true,  minY, maxY, rangeY, nY),
	};
}

std::pair<std::vector<slug_t>, std::vector<slug_t>> Atlas::computeUniformSplits(
	const Curves& /*curves*/,
	int numBandsX,
	int numBandsY
) {
	auto make = [](int numBands) -> std::vector<slug_t> {
		if(numBands <= 1) return {};

		const uint32_t n = static_cast<uint32_t>(numBands);
		std::vector<slug_t> splits(n - 1);

		for(uint32_t i = 0; i < n - 1; i++) splits[i] = slug_t(i + 1) / slug_t(n);

		return splits;
	};

	return { make(numBandsX), make(numBandsY) };
}

// ================================================================================================
// Atlas::addShape
// ================================================================================================

void Atlas::addShape(Key key, const ShapeInfo& desc) {
	// No logging available in this layer; callers should check isBuilt() before calling addShape()
	// if they need to diagnose this condition.
	if(_built) return;

	ShapeBuild build;

	build.curves = desc.curves;

	if(!desc.autoMetrics) {
		build.metrics.bearingX = desc.bearingX;
		build.metrics.bearingY = desc.bearingY;
		build.metrics.width = desc.width;
		build.metrics.height = desc.height;
		build.metrics.advance = desc.advance;
	}

	// Clamp negative values to 0 (auto); the signed sentinel is an API convenience; internally we
	// always work with uint32_t.
	const uint32_t numBandsX = desc.numBandsX > 0 ? static_cast<uint32_t>(desc.numBandsX) : 0u;
	const uint32_t numBandsY = desc.numBandsY > 0 ? static_cast<uint32_t>(desc.numBandsY) : 0u;

	build.splitsY = desc.splitsY;
	build.splitsX = desc.splitsX;

	buildShapeBands(key, build, numBandsX, numBandsY, /*overrideMetrics=*/!desc.autoMetrics);
}

// ================================================================================================
// Atlas::addCompositeShape
// ================================================================================================

void Atlas::addCompositeShape(Key key, CompositeShape composite) {
	_compositeShapes[key] = std::move(composite);
}

// ================================================================================================
// Atlas::build
// ================================================================================================

void Atlas::build() {
	if(_built) return;

	packTextures();

	_built = true;
}

// ================================================================================================
// Atlas::getShape
// ================================================================================================

const Atlas::Shape* Atlas::getShape(Key key) const {
	const auto it = _shapes.find(key);

	return it != _shapes.end() ? &it->second : nullptr;
}

// ================================================================================================
// Atlas::getCompositeShape
// ================================================================================================

const CompositeShape* Atlas::getCompositeShape(Key key) const {
	const auto it = _compositeShapes.find(key);

	return it != _compositeShapes.end() ? &it->second : nullptr;
}

// ================================================================================================
// Atlas::buildShapeBands
// ================================================================================================

void Atlas::buildShapeBands(
	Key key,
	ShapeBuild& build,
	uint32_t numBandsX,
	uint32_t numBandsY,
	bool overrideMetrics
) {
	const auto& splitsY = build.splitsY;
	const auto& splitsX = build.splitsX;
	if(build.curves.empty()) {
		_build[key] = build;

		return;
	}

	const size_t numCurves = build.curves.size();

	// --------------------------------------------------------------------------------------------
	// Band counts: splits override numBands; otherwise auto-pick or use caller's value.
	// --------------------------------------------------------------------------------------------
	if(!splitsY.empty()) numBandsY = static_cast<uint32_t>(splitsY.size() + 1);
	else if(numBandsY == 0) numBandsY = static_cast<uint32_t>(
		std::min(size_t(16), std::max(size_t(1), numCurves / 2))
	);

	if(!splitsX.empty()) numBandsX = static_cast<uint32_t>(splitsX.size() + 1);
	else if(numBandsX == 0) numBandsX = static_cast<uint32_t>(
		std::min(size_t(16), std::max(size_t(1), numCurves / 2))
	);

	// --------------------------------------------------------------------------------------------
	// Bounding box
	// --------------------------------------------------------------------------------------------
	slug_t minX = 1e9_cv, minY = 1e9_cv;
	slug_t maxX = -1e9_cv, maxY = -1e9_cv;

	for(const auto& c : build.curves) {
		minX = std::min({minX, c.x1, c.x2, c.x3});
		minY = std::min({minY, c.y1, c.y2, c.y3});
		maxX = std::max({maxX, c.x1, c.x2, c.x3});
		maxY = std::max({maxY, c.y1, c.y2, c.y3});
	}

	slug_t rangeX = maxX - minX;
	slug_t rangeY = maxY - minY;

	if(rangeX < 1e-6_cv) rangeX = 1e-6_cv;
	if(rangeY < 1e-6_cv) rangeY = 1e-6_cv;

	// --------------------------------------------------------------------------------------------
	// Derive metrics from bounding box when not overridden
	// --------------------------------------------------------------------------------------------
	if(!overrideMetrics) {
		build.metrics.width = rangeX;
		build.metrics.height = rangeY;
		build.metrics.bearingX = minX;
		build.metrics.bearingY = maxY; // top of shape, Y-up convention
		build.metrics.advance = rangeX;
	}

	// --------------------------------------------------------------------------------------------
	// Band transform
	// --------------------------------------------------------------------------------------------
	build.metrics.bandScaleX = cv(numBandsX) / rangeX;
	build.metrics.bandScaleY = cv(numBandsY) / rangeY;
	build.metrics.bandOffsetX = -minX * build.metrics.bandScaleX;
	build.metrics.bandOffsetY = -minY * build.metrics.bandScaleY;
	build.metrics.bandMaxX = numBandsX - 1;
	build.metrics.bandMaxY = numBandsY - 1;

	// --------------------------------------------------------------------------------------------
	// Horizontal bands (sliced along Y) - numBandsY slices
	// --------------------------------------------------------------------------------------------
	{
		std::vector<slug_t> hboundaries(numBandsY + 1);
		hboundaries[0] = minY;
		hboundaries[numBandsY] = maxY;

		if(!splitsY.empty()) {
			for(uint32_t i = 0; i < static_cast<uint32_t>(splitsY.size()); i++)
				hboundaries[i + 1] = minY + splitsY[i] * rangeY;
		} else {
			const slug_t bandHeightY = rangeY / cv(numBandsY);
			for(uint32_t i = 1; i < numBandsY; i++)
				hboundaries[i] = minY + cv(i) * bandHeightY;
		}

		build.hbands.resize(numBandsY);

		for(uint32_t b = 0; b < numBandsY; b++) {
			const slug_t lo = hboundaries[b];
			const slug_t hi = hboundaries[b + 1];
			auto& band = build.hbands[b];

			for(size_t ci = 0; ci < numCurves; ci++) {
				if(curveIntersectsBandY(build.curves[ci], lo, hi)) band.curveIndices.push_back(ci);
			}

			std::sort(band.curveIndices.begin(), band.curveIndices.end(), [&](size_t l, size_t r) {
				return std::max({
					build.curves[l].x1,
					build.curves[l].x2,
					build.curves[l].x3
				}) > std::max({
					build.curves[r].x1,
					build.curves[r].x2,
					build.curves[r].x3
				});
			});

			band.curveCount = static_cast<uint16_t>(band.curveIndices.size());
			if(!splitsY.empty()) band.splitFraction = (hi - minY) / rangeY;
		}
	}

	// --------------------------------------------------------------------------------------------
	// Vertical bands (sliced along X) - numBandsX slices
	// --------------------------------------------------------------------------------------------
	{
		std::vector<slug_t> vboundaries(numBandsX + 1);
		vboundaries[0] = minX;
		vboundaries[numBandsX] = maxX;

		if(!splitsX.empty()) {
			for(uint32_t i = 0; i < static_cast<uint32_t>(splitsX.size()); i++)
				vboundaries[i + 1] = minX + splitsX[i] * rangeX;
		} else {
			const slug_t bandWidthX = rangeX / cv(numBandsX);
			for(uint32_t i = 1; i < numBandsX; i++)
				vboundaries[i] = minX + cv(i) * bandWidthX;
		}

		build.vbands.resize(numBandsX);

		for(uint32_t b = 0; b < numBandsX; b++) {
			const slug_t lo = vboundaries[b];
			const slug_t hi = vboundaries[b + 1];
			auto& band = build.vbands[b];

			for(size_t ci = 0; ci < numCurves; ci++) {
				if(curveIntersectsBandX(build.curves[ci], lo, hi)) band.curveIndices.push_back(ci);
			}

			std::sort(band.curveIndices.begin(), band.curveIndices.end(), [&](size_t l, size_t r) {
				return std::max({
					build.curves[l].y1,
					build.curves[l].y2,
					build.curves[l].y3
				}) > std::max({
					build.curves[r].y1,
					build.curves[r].y2,
					build.curves[r].y3
				});
			});

			band.curveCount = static_cast<uint16_t>(band.curveIndices.size());
			if(!splitsX.empty()) band.splitFraction = (hi - minX) / rangeX;
		}
	}

	_build[key] = build;
}

// ================================================================================================
// Atlas::packTextures
//
// Two-pass layout (measure, then write) identical to the original, but writing
// into TextureData::bytes instead of osg::Image pixel buffers.
// ================================================================================================

void Atlas::packTextures() {
	// --------------------------------------------------------------------------------------------
	// Pass 1: measure curve texture height
	// --------------------------------------------------------------------------------------------
	uint32_t totalCurveTexels = 0;

	for(const auto& kv : _build) {
		for(size_t i = 0; i < kv.second.curves.size(); i++) {
			totalCurveTexels = alignCursorForSpan(totalCurveTexels, TEX_WIDTH, 2);
			totalCurveTexels += 2;
		}
	}

	const uint32_t curveTexHeight = std::max(1u, (totalCurveTexels + TEX_WIDTH - 1) / TEX_WIDTH);

	_curveData.width = TEX_WIDTH;
	_curveData.height = curveTexHeight;
	_curveData.format = TextureData::Format::RGBA32F;

	// 4 floats per texel
	_curveData.bytes.assign(size_t(TEX_WIDTH) * curveTexHeight * 4 * sizeof(float), 0);

	// --------------------------------------------------------------------------------------------
	// Pass 1: measure band texture height
	// --------------------------------------------------------------------------------------------
	uint32_t totalBandTexels = 0;

	for(const auto& kv : _build) {
		const auto& g = kv.second;
		const auto numHeaders = static_cast<uint32_t>(g.hbands.size() + g.vbands.size());

		totalBandTexels = alignCursorForSpan(totalBandTexels, TEX_WIDTH, numHeaders);

		uint32_t cursor = totalBandTexels + numHeaders;

		auto measureList = [&](const std::vector<BandEntry>& bands) {
			for(const auto& band : bands) {
				const auto count = static_cast<uint32_t>(band.curveIndices.size());

				cursor = alignCursorForSpan(cursor, TEX_WIDTH, count);
				cursor += count;
			}
		};

		measureList(g.hbands);
		measureList(g.vbands);

		totalBandTexels = cursor;
	}

	const uint32_t bandTexHeight = std::max(1u, (totalBandTexels + TEX_WIDTH - 1) / TEX_WIDTH);

	_bandData.width = TEX_WIDTH;
	_bandData.height = bandTexHeight;
	_bandData.format = TextureData::Format::RGBA16UI;

	// 4 uint16_t per texel
	_bandData.bytes.assign(size_t(TEX_WIDTH) * bandTexHeight * 4 * sizeof(uint16_t), 0);

	// --------------------------------------------------------------------------------------------
	// Write helpers - write directly into TextureData::bytes
	// --------------------------------------------------------------------------------------------
	auto writeCurveTexel = [&](uint32_t idx, slug_t r, slug_t g, slug_t b, slug_t a) {
		const uint32_t x = idx % TEX_WIDTH;
		const uint32_t y = idx / TEX_WIDTH;

		if(y >= curveTexHeight) return;

		float* p = reinterpret_cast<float*>(
			_curveData.bytes.data() + (size_t(y) * TEX_WIDTH + x) * 4 * sizeof(float)
		);

		p[0] = r; p[1] = g; p[2] = b; p[3] = a;
	};

	auto writeBandTexel = [&](uint32_t idx, uint16_t r, uint16_t g, uint16_t b, uint16_t a) {
		const uint32_t x = idx % TEX_WIDTH;
		const uint32_t y = idx / TEX_WIDTH;

		if(y >= bandTexHeight) return;

		uint16_t* p = reinterpret_cast<uint16_t*>(
			_bandData.bytes.data() + (size_t(y) * TEX_WIDTH + x) * 4 * sizeof(uint16_t)
		);

		p[0] = r; p[1] = g; p[2] = b; p[3] = a;
	};

	// --------------------------------------------------------------------------------------------
	// Pass 2: real packing
	//
	// Alignment wrappers record padding waste into _packingStats automatically.
	// --------------------------------------------------------------------------------------------
	_packingStats = PackingStats{};
	_packingStats.curveTexelsTotal = TEX_WIDTH * curveTexHeight;
	_packingStats.bandTexelsTotal = TEX_WIDTH * bandTexHeight;

	auto alignCurve = [&](uint32_t cursor, uint32_t span) -> uint32_t {
		const uint32_t aligned = alignCursorForSpan(cursor, TEX_WIDTH, span);

		_packingStats.curveTexelsPadding += aligned - cursor;

		return aligned;
	};

	auto alignBand = [&](uint32_t cursor, uint32_t span) -> uint32_t {
		const uint32_t aligned = alignCursorForSpan(cursor, TEX_WIDTH, span);

		_packingStats.bandTexelsPadding += aligned - cursor;

		return aligned;
	};

	uint32_t curveTexelOffset = 0;
	uint32_t bandTexelOffset = 0;

	for(auto& kv : _build) {
		const Key& key = kv.first;
		auto& g = kv.second;

		// Curves
		std::vector<uint32_t> curveLocs(g.curves.size());

		for(size_t ci = 0; ci < g.curves.size(); ci++) {
			curveTexelOffset = alignCurve(curveTexelOffset, 2);
			curveLocs[ci] = curveTexelOffset;

			const auto& c = g.curves[ci];

			writeCurveTexel(curveTexelOffset, c.x1, c.y1, c.x2, c.y2);
			writeCurveTexel(curveTexelOffset + 1, c.x3, c.y3, 0_cv, 0_cv);

			curveTexelOffset += 2;
			_packingStats.curveTexelsUsed += 2;
		}

		// Band headers + lists
		Shape sd = g.metrics;

		const auto numHBands = static_cast<uint32_t>(g.hbands.size());
		const auto numVBands = static_cast<uint32_t>(g.vbands.size());
		const uint32_t numHeaders = numHBands + numVBands;

		// Skip this shape; header block would exceed a full texture row.
		if(numHeaders > TEX_WIDTH) continue;

		bandTexelOffset = alignBand(bandTexelOffset, numHeaders);

		const uint32_t shapeStart = bandTexelOffset;

		sd.bandTexX = shapeStart % TEX_WIDTH;
		sd.bandTexY = shapeStart / TEX_WIDTH;

		struct HeaderTemp { uint16_t count = 0, offset = 0; };

		std::vector<HeaderTemp> headers(numHeaders);

		uint32_t cursor = shapeStart + numHeaders;

		auto packBandList = [&](const std::vector<BandEntry>& bands, uint32_t headerBase) {
			for(uint32_t b = 0; b < static_cast<uint32_t>(bands.size()); b++) {
				const auto& band = bands[b];
				auto count = static_cast<uint32_t>(band.curveIndices.size());

				// truncate oversized lists
				if(count > TEX_WIDTH) count = 0;

				cursor = alignBand(cursor, count);

				const uint32_t hi = headerBase + b;

				headers[hi].count = static_cast<uint16_t>(count);
				headers[hi].offset = static_cast<uint16_t>(cursor - shapeStart);

				for(uint32_t i = 0; i < count; i++) {
					const uint32_t curveLoc = curveLocs[band.curveIndices[i]];

					writeBandTexel(
						cursor,
						static_cast<uint16_t>(curveLoc % TEX_WIDTH),
						static_cast<uint16_t>(curveLoc / TEX_WIDTH),
						0, 0
					);

					cursor++;
				}

				_packingStats.bandTexelsUsed += count;
			}
		};

		packBandList(g.hbands, 0);
		packBandList(g.vbands, numHBands);

		for(uint32_t i = 0; i < numHBands; i++) {
			const uint16_t splitB = static_cast<uint16_t>(g.hbands[i].splitFraction * 65535.0f + 0.5f);

			writeBandTexel(shapeStart + i, headers[i].count, headers[i].offset, splitB, 0);
		}

		for(uint32_t i = 0; i < numVBands; i++) {
			const uint16_t splitB = static_cast<uint16_t>(g.vbands[i].splitFraction * 65535.0f + 0.5f);

			writeBandTexel(shapeStart + numHBands + i, headers[numHBands + i].count, headers[numHBands + i].offset, splitB, 0);
		}

		_packingStats.bandTexelsUsed += numHeaders;

		bandTexelOffset = cursor;
		_shapes[key] = sd;
	}

	_build.clear();
}

}
