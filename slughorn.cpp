#include "slughorn.hpp"

#include <algorithm>
#include <cstring>

// =============================================================================
// File-local helpers
// =============================================================================
namespace {

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

// Advance @p cursor so that a span of @p span texels fits without crossing a
// row boundary in a texture of @p width texels.
uint32_t alignCursorForSpan(uint32_t cursor, uint32_t width, uint32_t span) {
	if(span == 0) return cursor;

	const uint32_t x = cursor % width;

	if(x + span > width) cursor += width - x;

	return cursor;
}

}

namespace slughorn {

Atlas::Atlas() = default;
Atlas::~Atlas() = default;

// =============================================================================
// Atlas::addShape
// =============================================================================

void Atlas::addShape(Key key, const ShapeInfo& desc) {
	if(_built) {
		// No logging available in this layer — callers should check isBuilt()
		// before calling addShape() if they need to diagnose this condition.
		return;
	}

	ShapeBuild build;

	build.curves = desc.curves;

	if(!desc.autoMetrics) {
		build.metrics.bearingX = desc.bearingX;
		build.metrics.bearingY = desc.bearingY;
		build.metrics.width = desc.width;
		build.metrics.height = desc.height;
		build.metrics.advance = desc.advance;
	}

	// Clamp negative numBands to 0 (auto) — the signed sentinel is an API
	// convenience; internally we always work with uint32_t.
	const uint32_t numBands = desc.numBands > 0
		? static_cast<uint32_t>(desc.numBands)
		: 0u
	;

	buildShapeBands(key, build, numBands, /*overrideMetrics=*/!desc.autoMetrics);
}

// =============================================================================
// Atlas::addCompositeShape
// =============================================================================

void Atlas::addCompositeShape(Key key, CompositeShape composite) {
	_composites[key] = std::move(composite);
}

// =============================================================================
// Atlas::build
// =============================================================================

void Atlas::build() {
	if(_built) return;

	packTextures();

	_built = true;
}

// =============================================================================
// Atlas::getShape
// =============================================================================

const Atlas::Shape* Atlas::getShape(Key key) const {
	const auto it = _shapes.find(key);

	return it != _shapes.end() ? &it->second : nullptr;
}

// =============================================================================
// Atlas::getCompositeShape
// =============================================================================

const CompositeShape* Atlas::getCompositeShape(Key key) const {
	const auto it = _composites.find(key);

	return it != _composites.end() ? &it->second : nullptr;
}

// =============================================================================
// Atlas::buildShapeBands
// =============================================================================

void Atlas::buildShapeBands(
	Key key,
	ShapeBuild& build,
	uint32_t numBands,
	bool overrideMetrics
) {
	if(build.curves.empty()) {
		_build[key] = build;

		return;
	}

	const size_t numCurves = build.curves.size();

	if(numBands == 0) {
		numBands = static_cast<uint32_t>(
			std::min(size_t(16), std::max(size_t(1), numCurves / 2))
		);
	}

	// -------------------------------------------------------------------------
	// Bounding box
	// -------------------------------------------------------------------------
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

	// -------------------------------------------------------------------------
	// Derive metrics from bounding box when not overridden
	// -------------------------------------------------------------------------
	if(!overrideMetrics) {
		build.metrics.width = rangeX;
		build.metrics.height = rangeY;
		build.metrics.bearingX = minX;
		build.metrics.bearingY = maxY; // top of shape, Y-up convention
		build.metrics.advance = rangeX;
	}

	// -------------------------------------------------------------------------
	// Band transform
	// -------------------------------------------------------------------------
	const slug_t fBands = cv(numBands);

	build.metrics.bandScaleX = fBands / rangeX;
	build.metrics.bandScaleY = fBands / rangeY;
	build.metrics.bandOffsetX = -minX * build.metrics.bandScaleX;
	build.metrics.bandOffsetY = -minY * build.metrics.bandScaleY;
	build.metrics.bandMaxX = numBands - 1;
	build.metrics.bandMaxY = numBands - 1;

	// -------------------------------------------------------------------------
	// Horizontal bands (sliced along Y)
	// -------------------------------------------------------------------------
	build.hbands.resize(numBands);

	const slug_t bandHeightY = rangeY / fBands;

	for(uint32_t b = 0; b < numBands; b++) {
		const slug_t lo = minY + cv(b) * bandHeightY;
		const slug_t hi = lo + bandHeightY;
		auto& band = build.hbands[b];

		for(size_t ci = 0; ci < numCurves; ci++) {
			if(curveIntersectsBandY(build.curves[ci], lo, hi)) band.curveIndices.push_back(ci);
		}

		std::sort(band.curveIndices.begin(), band.curveIndices.end(), [&](size_t a, size_t b_) {
			const slug_t mA = std::max({build.curves[a].x1, build.curves[a].x2, build.curves[a].x3});
			const slug_t mB = std::max({build.curves[b_].x1, build.curves[b_].x2, build.curves[b_].x3});

			return mA > mB;
		});

		band.curveCount = static_cast<uint16_t>(band.curveIndices.size());
	}

	// -------------------------------------------------------------------------
	// Vertical bands (sliced along X)
	// -------------------------------------------------------------------------
	build.vbands.resize(numBands);

	const slug_t bandWidthX = rangeX / fBands;

	for(uint32_t b = 0; b < numBands; b++) {
		const slug_t lo = minX + cv(b) * bandWidthX;
		const slug_t hi = lo + bandWidthX;
		auto& band = build.vbands[b];

		for(size_t ci = 0; ci < numCurves; ci++) {
			if(curveIntersectsBandX(build.curves[ci], lo, hi)) band.curveIndices.push_back(ci);
		}

		std::sort(band.curveIndices.begin(), band.curveIndices.end(), [&](size_t a, size_t b_) {
			const slug_t mA = std::max({build.curves[a].y1, build.curves[a].y2, build.curves[a].y3});
			const slug_t mB = std::max({build.curves[b_].y1, build.curves[b_].y2, build.curves[b_].y3});

			return mA > mB;
		});

		band.curveCount = static_cast<uint16_t>(band.curveIndices.size());
	}

	_build[key] = build;
}

// =============================================================================
// Atlas::packTextures
//
// Two-pass layout (measure, then write) identical to the original, but writing
// into TextureData::bytes instead of osg::Image pixel buffers.
// =============================================================================

void Atlas::packTextures() {
	// -------------------------------------------------------------------------
	// Pass 1: measure curve texture height
	// -------------------------------------------------------------------------
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

	// -------------------------------------------------------------------------
	// Pass 1: measure band texture height
	// -------------------------------------------------------------------------
	uint32_t totalBandTexels = 0;

	for(const auto& kv : _build) {
		const auto& g = kv.second;
		const uint32_t numHeaders = static_cast<uint32_t>(g.hbands.size() + g.vbands.size());

		totalBandTexels = alignCursorForSpan(totalBandTexels, TEX_WIDTH, numHeaders);
		uint32_t cursor = totalBandTexels + numHeaders;

		auto measureList = [&](const std::vector<BandEntry>& bands) {
			for(const auto& band : bands) {
				const uint32_t count = static_cast<uint32_t>(band.curveIndices.size());

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

	// -------------------------------------------------------------------------
	// Write helpers — write directly into TextureData::bytes
	// -------------------------------------------------------------------------
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

	// -------------------------------------------------------------------------
	// Pass 2: real packing
	// -------------------------------------------------------------------------
	uint32_t curveTexelOffset = 0;
	uint32_t bandTexelOffset = 0;

	for(auto& kv : _build) {
		const Key& key = kv.first;
		auto& g = kv.second;

		// --- Curves -----------------------------------------------------------
		std::vector<uint32_t> curveLocs(g.curves.size());

		for(size_t ci = 0; ci < g.curves.size(); ci++) {
			curveTexelOffset = alignCursorForSpan(curveTexelOffset, TEX_WIDTH, 2);
			curveLocs[ci] = curveTexelOffset;

			const auto& c = g.curves[ci];

			writeCurveTexel(curveTexelOffset, c.x1, c.y1, c.x2, c.y2);
			writeCurveTexel(curveTexelOffset + 1, c.x3, c.y3, 0_cv, 0_cv);

			curveTexelOffset += 2;
		}

		// --- Band headers + lists --------------------------------------------
		Shape sd = g.metrics;

		const uint32_t numHBands = static_cast<uint32_t>(g.hbands.size());
		const uint32_t numVBands = static_cast<uint32_t>(g.vbands.size());
		const uint32_t numHeaders = numHBands + numVBands;

		if(numHeaders > TEX_WIDTH) {
			// Skip this shape — header block would exceed a full texture row.
			continue;
		}

		bandTexelOffset = alignCursorForSpan(bandTexelOffset, TEX_WIDTH, numHeaders);

		const uint32_t shapeStart = bandTexelOffset;

		sd.bandTexX = shapeStart % TEX_WIDTH;
		sd.bandTexY = shapeStart / TEX_WIDTH;

		struct HeaderTemp { uint16_t count = 0, offset = 0; };

		std::vector<HeaderTemp> headers(numHeaders);

		uint32_t cursor = shapeStart + numHeaders;

		auto packBandList = [&](const std::vector<BandEntry>& bands, uint32_t headerBase) {
			for(uint32_t b = 0; b < static_cast<uint32_t>(bands.size()); b++) {
				const auto& band = bands[b];
				uint32_t count = static_cast<uint32_t>(band.curveIndices.size());

				if(count > TEX_WIDTH) count = 0; // truncate oversized lists

				cursor = alignCursorForSpan(cursor, TEX_WIDTH, count);

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
			}
		};

		packBandList(g.hbands, 0);
		packBandList(g.vbands, numHBands);

		for(uint32_t i = 0; i < numHeaders; i++) {
			writeBandTexel(
				shapeStart + i,
				headers[i].count,
				headers[i].offset,
				0, 0
			);
		}

		bandTexelOffset = cursor;
		_shapes[key] = sd;
	}

	_build.clear();
}

}
