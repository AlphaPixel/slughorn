#pragma once

// ================================================================================================
// render.hpp - CPU-side Slug coverage emulator
//
// Mirrors the GPU fragment shader analytically, enabling:
//
// - Software rendering / visual validation without a GPU
// - SDF/MSDF tile baking (render::field(), see Atlas::SDF)
// - Post-build curve access for strokeText / glyphOutline
//
// Usage:
//
// slughorn::render::Sampler s = slughorn::render::decode(atlas, key);
// slughorn::render::Grid g = s.renderGrid(128);
// // g.data is row-major float32: g.data[row * g.width + col]
//
// decode() reads the packed curve/band textures produced by Atlas::build() and
// Atlas::packTextures(). It can be called any time after packTextures() returns.
//
// No GPU, no Python, no external dependencies beyond slughorn.hpp.
// ================================================================================================

#include "slughorn.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <vector>


#ifdef SLUGHORN_HAS_SDF
#include <msdfgen.h>
#endif

namespace slughorn {
namespace render {

// ================================================================================================
// Sample - coverage result for a single em-space point
// ================================================================================================

struct Sample {
	slug_t fill = 0_cv;
	slug_t xcov = 0_cv;
	slug_t ycov = 0_cv;
	slug_t xwgt = 0_cv;
	slug_t ywgt = 0_cv;

	uint32_t iters = 0;
};

// ================================================================================================
// Grid - 2-D coverage result from renderGrid()
//
// data is row-major: data[row * width + col], values in [0, 1].
// ================================================================================================

struct Grid {
	uint32_t width = 0;
	uint32_t height = 0;

	std::vector<slug_t> data = {};

	slug_t at(uint32_t row, uint32_t col) const { return data[row * width + col]; }
};

// ================================================================================================
// Sampler - holds a decoded shape and provides coverage evaluation
// ================================================================================================

struct Sampler {
	Atlas::Shape shape;
	Atlas::Curves curves;

	std::vector<uint32_t> hbandOffsets;
	std::vector<uint32_t> hbandIndices;
	std::vector<uint32_t> vbandOffsets;
	std::vector<uint32_t> vbandIndices;
	std::array<uint8_t, Atlas::INDIRECTION_SIZE> indirY{};
	std::array<uint8_t, Atlas::INDIRECTION_SIZE> indirX{};

	// -------------------------------------------------------------------------
	// Em-space helpers
	// -------------------------------------------------------------------------

	std::pair<slug_t, slug_t> emOrigin() const {
		const slug_t ox = shape.bandScaleX != 0_cv ? -shape.bandOffsetX / shape.bandScaleX : 0_cv;
		const slug_t oy = shape.bandScaleY != 0_cv ? -shape.bandOffsetY / shape.bandScaleY : 0_cv;

		return {ox, oy};
	}

	std::pair<slug_t, slug_t> emSize() const {
		const slug_t sx = shape.bandScaleX != 0_cv
			? cv(Atlas::INDIRECTION_SIZE) / shape.bandScaleX
			: 0_cv
		;

		const slug_t sy = shape.bandScaleY != 0_cv
			? cv(Atlas::INDIRECTION_SIZE) / shape.bandScaleY
			: 0_cv
		;

		return {sx, sy};
	}

	std::pair<uint32_t, uint32_t> computeRenderSize(uint32_t sizeHint) const {
		const slug_t w = shape.width;
		const slug_t h = shape.height;

		if(w <= 0_cv || h <= 0_cv)
			throw std::runtime_error("Invalid shape dimensions for renderGrid()")
		;

		const slug_t scale = cv(sizeHint) / std::max(w, h);
		const auto outW = static_cast<uint32_t>(std::max(1_cv, cv(std::round(w * scale))));
		const auto outH = static_cast<uint32_t>(std::max(1_cv, cv(std::round(h * scale))));

		return {outW, outH};
	}

	// -------------------------------------------------------------------------
	// Coverage evaluation
	// -------------------------------------------------------------------------

	Sample renderSample(slug_t rx, slug_t ry, slug_t ppeX, slug_t ppeY) const {
		Sample out;

		for(const auto& c : curves) {
			out.iters++;

			const slug_t x1 = c.x1 - rx, y1 = c.y1 - ry;
			const slug_t x2 = c.x2 - rx, y2 = c.y2 - ry;
			const slug_t x3 = c.x3 - rx, y3 = c.y3 - ry;

			uint32_t code = _calcRootCode(y1, y2, y3);

			if(code) {
				auto [r1, r2] = _solveHorizPoly(x1, y1, x2, y2, x3, y3);

				r1 *= ppeX; r2 *= ppeX;

				if(code & 0x01u) {
					out.xcov += _clamp(r1 + 0.5_cv, 0_cv, 1_cv);
					out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r1) * 2_cv, 0_cv, 1_cv));
				}

				if(code & 0x100u) {
					out.xcov -= _clamp(r2 + 0.5_cv, 0_cv, 1_cv);
					out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r2) * 2_cv, 0_cv, 1_cv));
				}
			}

			code = _calcRootCode(x1, x2, x3);

			if(code) {
				auto [r1, r2] = _solveVertPoly(x1, y1, x2, y2, x3, y3);

				r1 *= ppeY; r2 *= ppeY;

				if(code & 0x01u) {
					out.ycov -= _clamp(r1 + 0.5_cv, 0_cv, 1_cv);
					out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r1) * 2_cv, 0_cv, 1_cv));
				}

				if(code & 0x100u) {
					out.ycov += _clamp(r2 + 0.5_cv, 0_cv, 1_cv);
					out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r2) * 2_cv, 0_cv, 1_cv));
				}
			}
		}

		out.fill = _calcCoverage(out.xcov, out.ycov, out.xwgt, out.ywgt);

		return out;
	}

	Sample renderSampleBanded(slug_t rx, slug_t ry, slug_t ppeX, slug_t ppeY) const {
		Sample out;

		if(hbandOffsets.size() < 2 || vbandOffsets.size() < 2) return out;

		const uint32_t bandX = _lookupBandIndir(rx * shape.bandScaleX + shape.bandOffsetX, indirX);
		const uint32_t bandY = _lookupBandIndir(ry * shape.bandScaleY + shape.bandOffsetY, indirY);

		auto process = [&](uint32_t ci, bool horizontal) -> bool {
			out.iters++;

			const auto& c = curves[ci];

			const slug_t x1 = c.x1 - rx, y1 = c.y1 - ry;
			const slug_t x2 = c.x2 - rx, y2 = c.y2 - ry;
			const slug_t x3 = c.x3 - rx, y3 = c.y3 - ry;

			if(horizontal) {
				if(std::max({x1, x2, x3}) * ppeX < -0.5_cv) return false;

				const uint32_t code = _calcRootCode(y1, y2, y3);

				if(!code) return true;

				auto [r1, r2] = _solveHorizPoly(x1, y1, x2, y2, x3, y3);

				r1 *= ppeX; r2 *= ppeX;

				if(code & 0x01u) {
					out.xcov += _clamp(r1 + 0.5_cv, 0_cv, 1_cv);
					out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r1) * 2_cv, 0_cv, 1_cv));
				}

				if(code & 0x100u) {
					out.xcov -= _clamp(r2 + 0.5_cv, 0_cv, 1_cv);
					out.xwgt = std::max(out.xwgt, _clamp(1_cv - std::abs(r2) * 2_cv, 0_cv, 1_cv));
				}
			}

			else {
				if(std::max({y1, y2, y3}) * ppeY < -0.5_cv) return false;

				const uint32_t code = _calcRootCode(x1, x2, x3);

				if(!code) return true;

				auto [r1, r2] = _solveVertPoly(x1, y1, x2, y2, x3, y3);

				r1 *= ppeY; r2 *= ppeY;

				if(code & 0x01u) {
					out.ycov -= _clamp(r1 + 0.5_cv, 0_cv, 1_cv);
					out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r1) * 2_cv, 0_cv, 1_cv));
				}

				if(code & 0x100u) {
					out.ycov += _clamp(r2 + 0.5_cv, 0_cv, 1_cv);
					out.ywgt = std::max(out.ywgt, _clamp(1_cv - std::abs(r2) * 2_cv, 0_cv, 1_cv));
				}
			}

			return true;
		};

		if(bandY + 1 < hbandOffsets.size()) {
			for(uint32_t i = hbandOffsets[bandY]; i < hbandOffsets[bandY + 1]; i++) {
				if(!process(hbandIndices[i], true)) break;
			}
		}

		if(bandX + 1 < vbandOffsets.size()) {
			for(uint32_t i = vbandOffsets[bandX]; i < vbandOffsets[bandX + 1]; i++) {
				if(!process(vbandIndices[i], false)) break;
			}
		}

		out.fill = _calcCoverage(out.xcov, out.ycov, out.xwgt, out.ywgt);

		return out;
	}

	Grid renderGrid(
		uint32_t sizeHint=128,
		slug_t margin=0_cv,
		bool banded=true,
		bool parallel=false
	) const {
		const auto [width, height] = computeRenderSize(sizeHint);

		auto [ox, oy] = emOrigin();
		auto [sx, sy] = emSize();

		ox -= margin * sx;
		oy -= margin * sy;
		sx *= (1_cv + 2_cv * margin);
		sy *= (1_cv + 2_cv * margin);

		Grid grid{width, height, std::vector<slug_t>(width * height, 0_cv)};

		const slug_t ppeX = cv(width);
		const slug_t ppeY = cv(height);

		auto renderRow = [&](uint32_t j) {
			for(uint32_t i = 0; i < width; i++) {
				const slug_t u = (cv(i) + 0.5_cv) / cv(width);
				const slug_t v = (cv(j) + 0.5_cv) / cv(height);
				const slug_t ex = ox + u * sx;
				const slug_t ey = oy + v * sy;

				const auto result = banded
					? renderSampleBanded(ex, ey, ppeX, ppeY)
					: renderSample(ex, ey, ppeX, ppeY)
				;

				grid.data[j * width + i] = result.fill;
			}
		};

#ifdef SLUGHORN_HAS_PARALLEL
		if(parallel) {
			#pragma omp parallel for schedule(static)
			for(uint32_t j = 0; j < height; j++) renderRow(j);
			return grid;
		}
#else
		static_cast<void>(parallel);
#endif

		for(uint32_t j = 0; j < height; j++) renderRow(j);

		return grid;
	}

private:
	static constexpr slug_t EPS = 1_cv / 65536_cv;

	static uint32_t _floatBitsToUint32(slug_t x) { return std::bit_cast<uint32_t>(x); }

	static slug_t _clamp(slug_t x, slug_t lo, slug_t hi) {
		return x < lo ? lo : (x > hi ? hi : x);
	}

	static uint32_t _calcRootCode(slug_t y1, slug_t y2, slug_t y3) {
		const uint32_t i1 = _floatBitsToUint32(y1) >> 31;
		const uint32_t i2 = _floatBitsToUint32(y2) >> 30;
		const uint32_t i3 = _floatBitsToUint32(y3) >> 29;

		uint32_t shift = (i2 & 0x2u) | (i1 & ~0x2u);

		shift = (i3 & 0x4u) | (shift & ~0x4u);

		return (0x2E74u >> shift) & 0x0101u;
	}

	static std::pair<slug_t, slug_t> _solveHorizPoly(
		slug_t x1, slug_t y1,
		slug_t x2, slug_t y2,
		slug_t x3, slug_t y3
	) {
		const slug_t ax = x1 - 2_cv * x2 + x3, ay = y1 - 2_cv * y2 + y3;
		const slug_t bx = x1 - x2, by = y1 - y2;

		if(std::abs(ay) < EPS) {
			const slug_t t = std::abs(by) >= EPS ? y1 * (0.5_cv / by) : 0_cv;
			const slug_t x = (ax * t - 2_cv * bx) * t + x1;

			return {x, x};
		}

		const slug_t d = std::sqrt(std::max(by * by - ay * y1, 0_cv));
		const slug_t t1 = (by - d) / ay, t2 = (by + d) / ay;

		return {(ax * t1 - 2_cv * bx) * t1 + x1, (ax * t2 - 2_cv * bx) * t2 + x1};
	}

	static std::pair<slug_t, slug_t> _solveVertPoly(
		slug_t x1, slug_t y1,
		slug_t x2, slug_t y2,
		slug_t x3, slug_t y3
	) {
		const slug_t ax = x1 - 2_cv * x2 + x3, ay = y1 - 2_cv * y2 + y3;
		const slug_t bx = x1 - x2, by = y1 - y2;

		if(std::abs(ax) < EPS) {
			const slug_t t = std::abs(bx) >= EPS ? x1 * (0.5_cv / bx) : 0_cv;
			const slug_t y = (ay * t - 2_cv * by) * t + y1;

			return {y, y};
		}

		const slug_t d = std::sqrt(std::max(bx * bx - ax * x1, 0_cv));
		const slug_t t1 = (bx - d) / ax, t2 = (bx + d) / ax;

		return {(ay * t1 - 2_cv * by) * t1 + y1, (ay * t2 - 2_cv * by) * t2 + y1};
	}

	static slug_t _calcCoverage(slug_t xcov, slug_t ycov, slug_t xwgt, slug_t ywgt) {
		const slug_t weighted = std::abs(xcov * xwgt + ycov * ywgt) / std::max(xwgt + ywgt, EPS);
		const slug_t conservative = std::min(std::abs(xcov), std::abs(ycov));

		return _clamp(std::max(weighted, conservative), 0_cv, 1_cv);
	}

	static uint32_t _lookupBandIndir(
		slug_t coordScaled,
		const std::array<uint8_t, Atlas::INDIRECTION_SIZE>& indir
	) {
		const auto q = static_cast<uint32_t>(_clamp(coordScaled, 0_cv, cv(Atlas::INDIRECTION_SIZE - 1)));

		return indir[q];
	}
};

// ================================================================================================
// decode() - reconstruct a Sampler from packed atlas textures
//
// Reads the curve and band textures produced by Atlas::build() + packTextures().
// Throws std::out_of_range if the key is not found, std::runtime_error on texture
// format or bounds violations.
// ================================================================================================

inline Sampler decode(
	const Atlas::Shape& shape,
	const Atlas::TextureData& curveTex,
	const Atlas::TextureData& bandTex
);

inline Sampler decode(const Atlas& atlas, Key key) {
	const auto info = atlas.getShape(key);

	if(!info) throw std::out_of_range("Key not found in atlas (or atlas not built yet)");

	return decode(*info, atlas.getCurveTextureData(), atlas.getBandTextureData());
}

inline Sampler decode(
	const Atlas::Shape& shape,
	const Atlas::TextureData& curveTex,
	const Atlas::TextureData& bandTex
) {

	const bool curveIsHalf = curveTex.format == Atlas::TextureData::Format::RGBA16F;

	if(!curveIsHalf && curveTex.format != Atlas::TextureData::Format::RGBA32F)
		throw std::runtime_error("Unexpected curve texture format")
	;

	const bool bandIsLegacy = bandTex.format == Atlas::TextureData::Format::RGBA16UI;

	if(!bandIsLegacy && bandTex.format != Atlas::TextureData::Format::RG16UI)
		throw std::runtime_error("Unexpected band texture format")
	;

	// RG16UI (current) is 2 uint16_t/texel; RGBA16UI (legacy read-compat) is 4 - callers only
	// ever read indices 0/1 either way (B/A were never consumed even in the legacy format).
	const uint32_t bandStride = bandIsLegacy ? 4 : 2;

	Sampler out;

	out.shape = shape;

	if(shape.bandScaleX == 0_cv || shape.bandScaleY == 0_cv) {
		out.hbandOffsets = {0, 0};
		out.vbandOffsets = {0, 0};

		return out;
	}

	const auto* curveBytes = curveTex.bytes.data();
	const auto* bandData = reinterpret_cast<const uint16_t*>(bandTex.bytes.data());

	// Curve texel values are either raw float32 or half-float, per curveIsHalf above --
	// always returned as float32 here so the rest of decode() stays format-agnostic.
	auto readCurveTexel = [&](uint32_t texelIndex) -> std::array<float, 4> {
		if(texelIndex >= curveTex.width * curveTex.height)
			throw std::runtime_error("Curve texture read out of bounds")
		;

		std::array<float, 4> v;

		if(curveIsHalf) {
			const auto* p = reinterpret_cast<const uint16_t*>(curveBytes) + size_t{texelIndex} * 4;

			for(size_t i = 0; i < 4; i++) v[i] = detail::halfToFloat(p[i]);
		} else {
			const auto* p = reinterpret_cast<const float*>(curveBytes) + size_t{texelIndex} * 4;

			for(size_t i = 0; i < 4; i++) v[i] = p[i];
		}

		return v;
	};

	const uint32_t shapeStart = shape.bandTexY * bandTex.width + shape.bandTexX;
	const uint32_t numHBands = shape.bandMaxY + 1;
	const uint32_t numVBands = shape.bandMaxX + 1;
	const uint32_t numBandHdrs = numHBands + numVBands;
	const uint32_t indirSize = numBandHdrs > 0 ? 2 * Atlas::INDIRECTION_SIZE : 0;

	auto readBandTexel = [&](uint32_t texelIndex) -> const uint16_t* {
		if(texelIndex >= bandTex.width * bandTex.height)
			throw std::runtime_error("Band texture read out of bounds")
		;

		return bandData + size_t{texelIndex} * bandStride;
	};

	for(uint32_t q = 0; q < Atlas::INDIRECTION_SIZE; q++) {
		out.indirY[q] = static_cast<uint8_t>(readBandTexel(shapeStart + q)[0]);
		out.indirX[q] = static_cast<uint8_t>(readBandTexel(shapeStart + Atlas::INDIRECTION_SIZE + q)[0]);
	}

	struct Header { uint32_t count = 0, offset = 0; };

	std::vector<Header> headers(numBandHdrs);

	for(uint32_t i = 0; i < numBandHdrs; i++) {
		const auto* texel = readBandTexel(shapeStart + indirSize + i);

		headers[i].count = texel[0];
		headers[i].offset = texel[1];
	}

	std::vector<uint32_t> globalIndices;

	globalIndices.reserve(64);

	auto decodeBandList = [&](
		uint32_t headerIndex,
		uint32_t numBands,
		std::vector<uint32_t>& offsets,
		std::vector<uint32_t>& indices
	) {
		offsets.clear();
		indices.clear();
		offsets.push_back(0);

		for(uint32_t i = 0; i < numBands; i++) {
			const auto& h = headers[headerIndex + i];

			for(uint32_t j = 0; j < h.count; j++) {
				const auto* texel = readBandTexel(shapeStart + h.offset + j);
				const uint32_t cx = texel[0];
				const uint32_t cy = texel[1];

				// Raw texel address of the curve's first texel, used directly as the dedup/remap
				// key - NOT divided down into a "curve index" via an assumed 2-texels-per-curve
				// stride. That assumption breaks once endpoint-shared packing lets a curve's
				// first texel land at an odd offset (a shared texel isn't always curve-aligned).
				const uint32_t idx = cy * curveTex.width + cx;

				indices.push_back(idx);
				globalIndices.push_back(idx);
			}

			offsets.push_back(static_cast<uint32_t>(indices.size()));
		}
	};

	decodeBandList(0, numHBands, out.hbandOffsets, out.hbandIndices);
	decodeBandList(numHBands, numVBands, out.vbandOffsets, out.vbandIndices);

	std::sort(globalIndices.begin(), globalIndices.end());

	globalIndices.erase(
		std::unique(globalIndices.begin(), globalIndices.end()),
		globalIndices.end()
	);

	std::unordered_map<uint32_t, uint32_t> remap;

	remap.reserve(globalIndices.size());

	out.curves.reserve(globalIndices.size());

	for(uint32_t globalIndex : globalIndices) {
		const uint32_t texel0 = globalIndex;
		const uint32_t texel1 = texel0 + 1;

		const auto t0 = readCurveTexel(texel0);
		const auto t1 = readCurveTexel(texel1);

		remap[globalIndex] = static_cast<uint32_t>(out.curves.size());

		out.curves.push_back({t0[0], t0[1], t0[2], t0[3], t1[0], t1[1]});
	}

	for(auto& idx : out.hbandIndices) idx = remap.at(idx);
	for(auto& idx : out.vbandIndices) idx = remap.at(idx);

	return out;
}

// ================================================================================================
// SDF baking - only available when built with -DSLUGHORN_SDF=ON (msdfgen)
// ================================================================================================

#ifdef SLUGHORN_HAS_SDF

// One baked distance-field tile, straight out of msdfgen. See Atlas::SDF (slughorn.hpp) for the
// full tile contract: uniform scale, em-space frame, row 0 = BOTTOM, values clamped to [0, 1]
// with edge = 0.5. `width == 0` means the shape has no geometry (there is nothing to bake).
//
// Reconstruct in a shader: channels == 1 -> sd = data.r; channels == 3 -> sd = median(r, g, b).
struct Field {
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t channels = 0; // 1 = Type::SDF, 3 = Type::MSDF

	slug_t range = 0_cv; // em-space half-range this field was baked with
	slug_t texelsPerEm = 0_cv;
	slug_t emOriginX = 0_cv, emOriginY = 0_cv; // em-space point at the bottom-left corner

	std::vector<float> data = {}; // row-major, `channels` floats per texel

	float at(uint32_t row, uint32_t col, uint32_t channel=0) const {
		return data[(size_t{row} * width + col) * channels + channel];
	}
};

// Build an msdfgen::Shape from the atlas contours for key.
// Coordinates are in em-space (same space as Atlas::Curves).
// Returns an empty Shape if the key has no geometry.
//
// Slughorn's curve winding appears CW to msdfgen (Y-up, CCW=filled convention).
// Each contour is reversed so that filled regions become CCW, then orientContours()
// assigns the correct fill/hole role for compound shapes.
inline msdfgen::Shape toMsdfgenShape(const Atlas& atlas, Key key) {
	msdfgen::Shape shape;

	for(const auto& contour : atlas.getShapeContours(key)) {
		auto& c = shape.addContour();

		// Each Atlas::Curve is a quadratic Bezier: p1=start, p2=off-curve, p3=end.
		for(const auto& curve : contour) c.addEdge(msdfgen::EdgeHolder(
			{curve.x1, curve.y1},
			{curve.x2, curve.y2},
			{curve.x3, curve.y3}
		));

		c.reverse();
	}

	if(!shape.contours.empty()) shape.orientContours();

	return shape;
}

// Bake one shape into a Field.
//
// msdfgen's Projection formula is pixel = scale * (shape + translate) with `translate` in em-space,
// so translating by -bounds.(l, b) puts the bounds' bottom-left corner at pixel (0, 0) - which is
// exactly the emOrigin recorded on the result. ONE scale is used for both axes (the tile keeps the
// shape's aspect ratio), so a tile is (ceil(bw * scale), ceil(bh * scale)) texels, the longer axis
// being config.tileSize.
//
// Bounds are expanded by `range` on every side so the tile's edge is deeply exterior (SDF << 0.5);
// otherwise curves touching the tight bbox leave edge texels at ~0.5, which bilinear filtering (and
// the frontend's AA margin) turns into ghost fringes. Distances of +/- `range` em map onto [0, 1]
// with NO clamp in msdfgen's DistanceMapping, so values overshoot in the padded margin; they are
// clamped here so the stored data is well-defined for any frontend.
//
// No Y flip, deliberately: msdfgen's native row 0 = bottom already agrees with GL/osg::Image
// (row 0 -> V = 0) and with the shape's own Y-up em-space. Flip only where a human-facing image
// needs it (e.g. `bin/slughorn sdf`).
inline Field field(
	const Atlas& atlas,
	Key key,
	const Atlas::SDF::Config& config,
	slug_t range
) {
	const bool msdf = config.type == Atlas::SDF::Type::MSDF;

	Field out;

	out.channels = msdf ? 3u : 1u;
	out.range = range;

	msdfgen::Shape shape = toMsdfgenShape(atlas, key);

	if(shape.contours.empty()) return out;

	if(msdf) {
		if(config.coloring == Atlas::SDF::Coloring::ByDistance) {
			msdfgen::edgeColoringByDistance(shape, 3.0);
		}

		else msdfgen::edgeColoringSimple(shape, 3.0);
	}

	const auto bounds = shape.getBounds(range);
	const double bw = bounds.r - bounds.l;
	const double bh = bounds.t - bounds.b;
	const double scale = config.tileSize / std::max(bw, bh);

	// The tiny epsilon keeps floating-point noise on the longer axis from rounding up to
	// tileSize + 1; the clamp is belt and braces for the same reason.
	const auto texels = [&](double extent) {
		return static_cast<uint32_t>(
			std::clamp(std::ceil(extent * scale - 1e-4), 1.0, static_cast<double>(config.tileSize))
		);
	};

	const uint32_t w = texels(bw);
	const uint32_t h = texels(bh);

	const msdfgen::SDFTransformation transform(
		msdfgen::Projection({scale, scale}, {-bounds.l, -bounds.b}),
		msdfgen::DistanceMapping(msdfgen::Range(2.0 * range)) // [-range, range] em -> [0, 1]
	);

	std::vector<float> buf(size_t{w} * h * out.channels);

	if(msdf) {
		msdfgen::BitmapSection<float, 3> bmp(buf.data(), static_cast<int>(w), static_cast<int>(h));

		msdfgen::generateMSDF(bmp, shape, transform);
	}

	else {
		msdfgen::BitmapSection<float, 1> bmp(buf.data(), static_cast<int>(w), static_cast<int>(h));

		msdfgen::generateSDF(bmp, shape, transform);
	}

	for(float& v : buf) v = std::clamp(v, 0.0f, 1.0f);

	out.width = w;
	out.height = h;
	out.texelsPerEm = static_cast<slug_t>(scale);
	out.emOriginX = static_cast<slug_t>(bounds.l);
	out.emOriginY = static_cast<slug_t>(bounds.b);
	out.data = std::move(buf);

	return out;
}

#endif

}
}
