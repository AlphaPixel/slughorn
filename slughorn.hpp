#pragma once

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

using slug_t = float;

constexpr slug_t operator"" _cv(long double v) {
	return static_cast<slug_t>(v);
}

constexpr slug_t operator"" _cv(unsigned long long v) {
	return static_cast<slug_t>(v);
}

constexpr slug_t cv(auto x) {
	return static_cast<slug_t>(x);
}

namespace slughorn {

// ================================================================================================
// Color
//
// Simple RGBA color in linear floating-point space (0.0 – 1.0). Used by Layer / CompositeShape and
// by the FT2 / Cairo / Skia headers. Convert to your graphics backend's native type at the
// boundary.
// ================================================================================================
struct Color {
	slug_t r = 0_cv;
	slug_t g = 0_cv;
	slug_t b = 0_cv;
	slug_t a = 1_cv;
};

// ================================================================================================
// Matrix
//
// Column-major 2-D affine transform (the same 6-float layout used by FreeType's FT_Matrix +
// FT_Vector, Cairo's cairo_matrix_t, and the font-unit matrices threaded through the COLRv1 paint
// graph).
//
// The transform maps a point (x, y) as:
//
//   x' = xx*x + xy*y + dx
//   y' = yx*x + yy*y + dy
//
// xx/yx/xy/yy are dimensionless ratios (already divided by 65536 for FreeType callers). dx/dy are
// in the same coordinate space as the points being transformed (em-units for glyph work).
// ================================================================================================
struct Matrix {
	slug_t xx = 1_cv, yx = 0_cv; // first column
	slug_t xy = 0_cv, yy = 1_cv; // second column
	slug_t dx = 0_cv, dy = 0_cv; // translation

	static Matrix identity() { return {}; }

	bool isIdentity() const {
		constexpr slug_t eps = 1e-6_cv;

		return
			std::abs(xx - 1_cv) < eps &&
			std::abs(yy - 1_cv) < eps &&
			std::abs(yx) < eps &&
			std::abs(xy) < eps &&
			std::abs(dx) < eps &&
			std::abs(dy) < eps
		;
	}

	// Apply this matrix to a point.
	//
	// TODO: Investigate some operator overload instead!
	void apply(slug_t x, slug_t y, slug_t& xo, slug_t& yo) const {
		xo = xx * x + xy * y + dx;
		yo = yx * x + yy * y + dy;
	}

	// Concatenate: returns (this * rhs), i.e. rhs is applied first.
	Matrix operator*(const Matrix& rhs) const {
		Matrix m;

		m.xx = xx * rhs.xx + xy * rhs.yx;
		m.xy = xx * rhs.xy + xy * rhs.yy;
		m.yx = yx * rhs.xx + yy * rhs.yx;
		m.yy = yx * rhs.xy + yy * rhs.yy;
		m.dx = xx * rhs.dx + xy * rhs.dy + dx;
		m.dy = yx * rhs.dx + yy * rhs.dy + dy;

		return m;
	}
};

// ================================================================================================
// Layer
//
// Represents the "state" of a single `Shape` instance, and includes all of the information the
// caller will need to contextually process the `Shape` in whatever setting SlugHorn is being used.
// ================================================================================================
struct Layer {
	uint32_t key = 0;

	Color color{};

	Matrix transform = Matrix::identity();
};

// ================================================================================================
// CompositeShape
//
// A simple container that combines/organizes multiple `Layer` instances into an entity that should
// be conceptually treated as a SINGLE `Shape`.
// ================================================================================================
struct CompositeShape {
	std::vector<Layer> layers;

	// Usage is obvious in text situations; in "pure shape" modes, can be used to help arrange
	// groups of `Shape` instances horizontally.
	slug_t advance = 0_cv;
};

// ================================================================================================
// Atlas
//
// Owns the two raw pixel buffers required by the Slug rendering algorithm (Lengyel 2017):
//
// - Curve texture (RGBA32F): packed quadratic Bezier control points
// - Band texture (RGBA16UI): band headers + curve index lists
//
// Atlas is completely backend-agnostic — it has no dependencies on OSG, VSG, raw OpenGL, or any
// other graphics library. After build() the caller retrieves TextureData descriptors and hands them
// to whatever graphics layer it is using (see osgSlug::Atlas for the OSG adapter).
//
// Key type is uint32_t, which comfortably covers:
// - Unicode codepoints (fed by a font loader)
// - User-defined shape IDs (icon sets, procedural geometry, …)
//
// If you need to mix fonts and shapes in the same atlas, reserve a range of IDs for each source
// (e.g. 0x00000–0xFFFF for codepoints, 0x10000+ for custom shapes).
// ================================================================================================
class Atlas {
public:
	// TODO: Convert to this instead!
	/* struct Key {
		enum class Type { Codepoint, Name };

		Type type;
		uint32_t codepoint;
		std::string name;

		static Key fromCodepoint(uint32_t cp) {
			return {Type::Codepoint, cp, {}};
		}

		static Key fromString(std::string s) {
			return {Type::Name, 0, std::move(s)};
		}

		bool operator==(const Key& other) const {
			if(type != other.type) return false;
			if(type == Type::Codepoint) return codepoint == other.codepoint;

			return name == other.name;
		}
	};

	struct KeyHash {
		size_t operator()(const Key& k) const {
			if(k.type == Key::Type::Codepoint) return std::hash<uint32_t>{}(k.codepoint);

			return std::hash<std::string>{}(k.name);
		}
	}; */

	Atlas();
	virtual ~Atlas();

	// -------------------------------------------------------------------------
	// Input types
	// -------------------------------------------------------------------------

	// A single quadratic Bezier segment: p1 -> p2 (control) -> p3.
	struct Curve {
		slug_t x1, y1; // start point
		slug_t x2, y2; // off-curve control point
		slug_t x3, y3; // end point
	};

	using Curves = std::vector<Curve>;

	// Everything the renderer needs to draw one shape.
	// Populated by build() and returned by getShape().
	struct Shape {
		// Location of this shape's band header block in the band texture (texel coords)
		uint32_t bandTexX = 0, bandTexY = 0;

		// Band index clamping limits (numBands - 1)
		uint32_t bandMaxX = 0, bandMaxY = 0;

		// Band-space transform: bandCoord = emPos * bandScale + bandOffset
		slug_t bandScaleX = 0, bandScaleY = 0;
		slug_t bandOffsetX = 0, bandOffsetY = 0;

		// Metrics in em-space (normalised to the font's em square, or to the
		// bounding box of the curves when autoMetrics = true)
		slug_t bearingX = 0, bearingY = 0;
		slug_t width = 0, height = 0;
		slug_t advance = 0;
	};

	// Descriptor passed to addShape().
	//
	// Curves must be in em-normalised coordinates (same convention as FreeType's FT_LOAD_NO_SCALE
	// path, divided by units_per_EM).
	//
	// Set autoMetrics = true (the default) to derive width/height/bearing/ advance automatically
	// from the curve bounding box. Set it to false and fill in the metric fields when you need
	// precise control (e.g. when forwarding FreeType's own metrics for a font glyph).
	//
	// numBands = 0 lets the atlas pick a sensible default.
	struct ShapeInfo {
		Curves curves;

		bool autoMetrics = true;

		slug_t bearingX = 0, bearingY = 0;
		slug_t width = 0, height = 0;
		slug_t advance = 0;

		// Signed: 0 means "pick automatically"; negative values are invalid.
		int numBands = 0;
	};

	// -------------------------------------------------------------------------
	// Raw texture descriptor — returned after build()
	//
	// bytes holds the complete pixel data in row-major order, ready to be
	// uploaded to a GPU texture (width/height are in texels). Format tells
	// the graphics backend how to interpret the bytes:
	//
	// RGBA32F — four 32-bit floats per texel (curve texture)
	// RGBA16UI — four 16-bit unsigned ints per texel (band texture)
	// -------------------------------------------------------------------------
	struct TextureData {
		enum class Format { RGBA32F, RGBA16UI };

		std::vector<uint8_t> bytes;

		uint32_t width = 0;
		uint32_t height = 0;

		Format format = Format::RGBA32F;
	};

	// -------------------------------------------------------------------------
	// Population (call before build())
	// -------------------------------------------------------------------------

	// Register a shape under @p key.
	//
	// Must be called before build(). Calling addShape() with an already-
	// registered key silently replaces the previous definition.
	//
	// Shapes with empty curve lists are stored as metric-only entries (useful
	// for whitespace characters that need an advance but no visible geometry).
	void addShape(uint32_t key, const ShapeInfo& desc);

	// TODO: Investigate this!
	// void addCompositeShape(const CompositeShape& composite, ...)

	// -------------------------------------------------------------------------
	// Build (call once, then the atlas is frozen)
	// -------------------------------------------------------------------------

	// Pack all registered shapes into the raw pixel buffers.
	//
	// After build() returns, addShape() must not be called again. Calling
	// build() a second time is a no-op (guarded internally).
	void build();

	bool isBuilt() const { return _built; }

	// -------------------------------------------------------------------------
	// Accessors (valid after build())
	// -------------------------------------------------------------------------

	const Shape* getShape(uint32_t key) const;
	const TextureData& getCurveTextureData() const { return _curveData; }
	const TextureData& getBandTextureData() const { return _bandData; }

	bool hasKey(uint32_t key) const {
		return _build.count(key) || _shapes.count(key);
	}

private:
	// -------------------------------------------------------------------------
	// Internal build structures (discarded after build())
	// -------------------------------------------------------------------------
	struct BandEntry {
		uint16_t curveCount = 0; // mirrors the uint16_t written to the band texture

		std::vector<size_t> curveIndices;
	};

	struct ShapeBuild {
		Shape metrics;
		Curves curves;
		std::vector<BandEntry> hbands;
		std::vector<BandEntry> vbands;
	};

	// -------------------------------------------------------------------------
	// Internal pipeline
	// -------------------------------------------------------------------------
	void buildShapeBands(uint32_t key, ShapeBuild& build, uint32_t numBands, bool overrideMetrics);

	void packTextures();

	// -------------------------------------------------------------------------
	// Data
	// -------------------------------------------------------------------------
	std::map<uint32_t, ShapeBuild> _build; // discarded after build()
	std::map<uint32_t, Shape> _shapes; // live after build()

	TextureData _curveData;
	TextureData _bandData;

	bool _built = false;

	static constexpr uint32_t TEX_WIDTH = 512;
};

// ================================================================================================
// CurveDecomposer
//
// Stateful helper that accepts path commands (moveTo / lineTo / quadTo / cubicTo) and appends the
// equivalent quadratic Bezier segments to a Curves vector. Cubic segments are split at their
// midpoint into two quadratics — a lightweight approximation sufficient for the Slug band-building
// pass.
// ================================================================================================
struct CurveDecomposer {
	Atlas::Curves& curves;

	slug_t _x = 0_cv;
	slug_t _y = 0_cv;

	CurveDecomposer(Atlas::Curves& c) : curves(c) {}

	void moveTo(slug_t x, slug_t y) {
		_x = x;
		_y = y;
	}

	void lineTo(slug_t x3, slug_t y3) {
		curves.push_back({
			_x, _y,
			(_x + x3) * 0.5_cv,
			(_y + y3) * 0.5_cv,
			x3, y3
		});

		_x = x3;
		_y = y3;
	}

	void quadTo(slug_t cx, slug_t cy, slug_t x3, slug_t y3) {
		curves.push_back({_x, _y, cx, cy, x3, y3});

		_x = x3;
		_y = y3;
	}

	void cubicTo(
		slug_t c1x, slug_t c1y,
		slug_t c2x, slug_t c2y,
		slug_t x3, slug_t y3
	) {
		const slug_t p0x = _x, p0y = _y;
		const slug_t p1x = c1x, p1y = c1y;
		const slug_t p2x = c2x, p2y = c2y;
		const slug_t p3x = x3, p3y = y3;

		const slug_t midx = (p0x + 3_cv * p1x + 3_cv * p2x + p3x) * 0.125_cv;
		const slug_t midy = (p0y + 3_cv * p1y + 3_cv * p2y + p3y) * 0.125_cv;

		curves.push_back({
			p0x, p0y,
			(p0x + 3_cv * p1x) * 0.25_cv,
			(p0y + 3_cv * p1y) * 0.25_cv,
			midx, midy
		});

		curves.push_back({
			midx, midy,
			(3_cv * p2x + p3x) * 0.25_cv,
			(3_cv * p2y + p3y) * 0.25_cv,
			p3x, p3y
		});

		_x = p3x;
		_y = p3y;
	}
};

}
