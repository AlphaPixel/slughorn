#pragma once

#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <ostream>

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
// Simple RGBA color in linear floating-point space (0.0 - 1.0). Used by Layer / CompositeShape and
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
// Atlas (forward declaration - Key is needed by Layer)
// ================================================================================================
class Atlas;

// ================================================================================================
// Atlas::Key
//
// Discriminated union identifying a shape or composite shape in the Atlas. Two flavors:
//
//   Key::fromCodepoint(cp) - a Unicode codepoint (or any uint32_t ID). Implicitly constructible
//                            from uint32_t for backwards compatibility with existing call sites.
//   Key::fromString(name) - a named shape / composite ("logo", "axolotl", ...)
//
// The hash is computed once at construction and stored; KeyHash just returns it. operator== uses
// the hash as a fast pre-check, then falls back to value comparison. The two namespaces are kept
// disjoint by mixing the type tag into the hash seed, so a codepoint and a string that happen to
// produce the same raw hash will never collide.
// ================================================================================================
struct Key {
	enum class Type { Codepoint, Name };

	// With this in the public section:
	Key(): _type(Type::Codepoint), _codepoint(0), _hash(_hashCp(0)) {}

	// Implicit from uint32_t - preserves all existing call sites unchanged.
	Key(uint32_t cp): _type(Type::Codepoint), _codepoint(cp), _hash(_hashCp(cp)) {}

	static Key fromCodepoint(uint32_t cp) { return Key(cp); }

	static Key fromString(std::string name) {
		Key k;

		k._type = Type::Name;
		k._name = std::move(name);
		k._hash = _hashStr(k._name);

		return k;
	}

	// Accessors

	Type type() const { return _type; }

	// Only valid when type() == Codepoint.
	uint32_t codepoint() const { return _codepoint; }

	// Only valid when type() == Name.
	const std::string& name() const { return _name; }

	size_t hash() const { return _hash; }

	// Equality

	bool operator==(const Key& o) const {
		if(_hash != o._hash) return false;
		if(_type != o._type) return false;
		if(_type == Type::Codepoint) return _codepoint == o._codepoint;
		return _name == o._name;
	}

	bool operator!=(const Key& o) const { return !(*this == o); }

private:
	static size_t _hashCp(uint32_t cp) {
		// Mix a type tag (0) into the seed so the codepoint and name namespaces
		// cannot collide even if their raw hashes match.
		size_t h = std::hash<uint32_t>{}(cp);

		h ^= std::hash<size_t>{}(0) + 0x9e3779b9 + (h << 6) + (h >> 2);

		return h;
	}

	static size_t _hashStr(const std::string& s) {
		size_t h = std::hash<std::string>{}(s);

		h ^= std::hash<size_t>{}(1) + 0x9e3779b9 + (h << 6) + (h >> 2);

		return h;
	}

	Type _type = Type::Codepoint;

	uint32_t _codepoint = 0;
	std::string _name;

	size_t _hash = 0;
};

struct KeyHash {
	size_t operator()(const Key& k) const { return k.hash(); }
};

// ================================================================================================
// Layer
//
// Represents the "state" of a single `Shape` instance, and includes all of the information the
// caller will need to contextually process the `Shape` in whatever setting SlugHorn is being used.
// ================================================================================================
struct Layer {
	Key key = Key(0u);

	Color color{};

	Matrix transform = Matrix::identity();

	// Shader effect to apply when rendering this layer. 0 = standard Slug coverage fill (default,
	// no overhead). Non-zero values select an effect branch in the fragment shader; the set of
	// valid IDs and their semantics are defined there. For example, something like:
	//
	//   0 - standard fill (color * slug coverage); this should ALWAYS be the 0/default
	//   1 - texture fill (texture(slug_effectTexture, v_emCoord) * slug coverage)
	//   2 - GLSL procedural
	//   3 - Etc, etc.
	//
	// TODO: It is likely this will be called `fillMode` or similar in future versions!
	uint32_t effectId = 0;
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
// Atlas is completely backend-agnostic; it has no dependencies on OSG, VSG, raw OpenGL, or any
// other graphics library. After build() the caller retrieves TextureData descriptors and hands them
// to whatever graphics layer it is using (see osgSlug::Atlas for the OSG adapter).
//
// Keys are Atlas::Key values — either a codepoint (uint32_t, implicitly convertible) or a named
// string. Both shapes and composite shapes share the same key namespace; do not register the same
// key under both addShape() and addCompositeShape().
// ================================================================================================
class Atlas {
public:
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

		// Metrics in em-space (normalized to the font's em square, or to the
		// bounding box of the curves when autoMetrics = true)
		slug_t bearingX = 0, bearingY = 0;
		slug_t width = 0, height = 0;
		slug_t advance = 0;
	};

	// Descriptor passed to addShape().
	//
	// Curves must be in em-normalized coordinates (same convention as FreeType's FT_LOAD_NO_SCALE
	// path, divided by units_per_EM).
	//
	// Set autoMetrics = true (the default) to derive width/height/bearing/advance automatically
	// from the curve bounding box. Set it to false and fill in the metric fields when you need
	// precise control (e.g. when forwarding FreeType's own metrics for a font glyph).
	//
	// numBandsX / numBandsY control the band grid dimensions independently.
	// 0 means "pick automatically" for that axis. Negative values are invalid.
	// Set both to the same value to get a square grid (equivalent to the old numBands scalar).
	struct ShapeInfo {
		Curves curves;

		bool autoMetrics = true;

		slug_t bearingX = 0, bearingY = 0;
		slug_t width = 0, height = 0;
		slug_t advance = 0;

		// Signed: 0 means "pick automatically"; negative values are invalid.
		int numBandsX = 0;
		int numBandsY = 0;
	};

	// --------------------------------------------------------------------------------------------
	// Raw texture descriptor returned after build() is called.
	//
	// The `bytes` member holds the complete pixel data in row-major order, ready to be uploaded to
	// a GPU texture (width/height are in texels); `format` tells the graphics backend how to
	// interpret the bytes:
	//
	// RGBA32F  - four 32-bit floats per texel (curve texture)
	// RGBA16UI - four 16-bit unsigned ints per texel (band texture)
	// --------------------------------------------------------------------------------------------
	struct TextureData {
		enum class Format { RGBA32F, RGBA16UI };

		std::vector<uint8_t> bytes;

		uint32_t width = 0;
		uint32_t height = 0;

		Format format = Format::RGBA32F;
	};

	// --------------------------------------------------------------------------------------------
	// Population (call before build())
	// --------------------------------------------------------------------------------------------

	// Register a geometry shape under @p key.
	//
	// Must be called before build(). Calling addShape() with an already-registered key silently
	// replaces the previous definition.
	//
	// Shapes with empty curve lists are stored as metric-only entries (useful for whitespace
	// characters that need an advance but no visible geometry).
	void addShape(Key key, const ShapeInfo& desc);

	// Register a composite shape (ordered layer stack) under @p key.
	//
	// CompositeShapes are stored separately from geometry shapes and are not processed by build().
	// They can be registered before or after build() — the atlas does not need to be rebuilt when
	// new composites are added. Calling addCompositeShape() with an already-registered key silently
	// replaces the previous definition.
	void addCompositeShape(Key key, CompositeShape composite);

	// --------------------------------------------------------------------------------------------
	// Build (call once, then the atlas is frozen for geometry)
	// --------------------------------------------------------------------------------------------

	// Pack all registered shapes into the raw pixel buffers.
	//
	// After build() returns, addShape() must not be called again. Calling build() a second time is
	// a no-op (guarded internally). addCompositeShape() may still be called after build().
	void build();

	bool isBuilt() const { return _built; }

	// --------------------------------------------------------------------------------------------
	// Accessors (geometry valid after build(); composites valid any time after registration)
	// --------------------------------------------------------------------------------------------

	const Shape* getShape(Key key) const;
	const CompositeShape* getCompositeShape(Key key) const;

	const TextureData& getCurveTextureData() const { return _curveData; }
	const TextureData& getBandTextureData()  const { return _bandData; }

	// Bulk accessors — primarily for serialization (slughorn-serial.hpp) and
	// diagnostics. Prefer getShape() / getCompositeShape() for normal use.
	const std::unordered_map<Key, Shape, KeyHash>& getShapes() const {
		return _shapes;
	}

	const std::unordered_map<Key, CompositeShape, KeyHash>& getCompositeShapes() const {
		return _compositeShapes;
	}

	bool hasKey(Key key) const {
		return _build.count(key) || _shapes.count(key) || _compositeShapes.count(key);
	}

	// --------------------------------------------------------------------------------------------
	// Serial reconstruction (used by slughorn-serial.hpp — not for general use)
	//
	// Injects pre-built texture data and shape/composite maps directly, bypassing build().
	// The Atlas is marked as built on return; calling build() afterwards is a no-op.
	// --------------------------------------------------------------------------------------------
	struct SerialData {
		TextureData                                      curveData;
		TextureData                                      bandData;
		std::unordered_map<Key, Shape, KeyHash>          shapes;
		std::unordered_map<Key, CompositeShape, KeyHash> composites;
	};

	void loadFromSerial(SerialData&& sd) {
		_curveData        = std::move(sd.curveData);
		_bandData         = std::move(sd.bandData);
		_shapes           = std::move(sd.shapes);
		_compositeShapes  = std::move(sd.composites);
		_built            = true;
	}

private:
	// --------------------------------------------------------------------------------------------
	// Internal build structures (discarded after build())
	// --------------------------------------------------------------------------------------------
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

	// --------------------------------------------------------------------------------------------
	// Internal pipeline
	// --------------------------------------------------------------------------------------------
	void buildShapeBands(Key key, ShapeBuild& build, uint32_t numBandsX, uint32_t numBandsY, bool overrideMetrics);

	void packTextures();

	// --------------------------------------------------------------------------------------------
	// Data
	// --------------------------------------------------------------------------------------------
	std::unordered_map<Key, ShapeBuild, KeyHash>     _build;           // discarded after build()
	std::unordered_map<Key, Shape, KeyHash>           _shapes;          // live after build()
	std::unordered_map<Key, CompositeShape, KeyHash>  _compositeShapes; // live always

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
// midpoint into two quadratics, a lightweight approximation sufficient for the Slug band-building.
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

// ================================================================================================
// Debugging Helpers
// ================================================================================================

inline std::ostream& operator<<(std::ostream& os, const Key& k) {
	if(k.type() == Key::Type::Codepoint)
		return os << "Key(0x" << std::hex << k.codepoint() << std::dec << ")";
	return os << "Key(\"" << k.name() << "\")";
}

inline std::ostream& operator<<(std::ostream& os, const Color& c) {
	return os << "Color(r=" << c.r << " g=" << c.g << " b=" << c.b << " a=" << c.a << ")";
}

inline std::ostream& operator<<(std::ostream& os, const Matrix& m) {
	return os
		<< "Matrix(xx=" << m.xx << " yx=" << m.yx
		<< " xy=" << m.xy << " yy=" << m.yy
		<< " dx=" << m.dx << " dy=" << m.dy << ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Layer& l) {
	return os << "Layer(" << l.key << " color=" << l.color << " transform=" << l.transform << " effectId=" << l.effectId << ")";
}

inline std::ostream& operator<<(std::ostream& os, const Atlas::Shape& s) {
	return os
		<< "Shape(w=" << s.width << " h=" << s.height
		<< " bx=" << s.bearingX << " by=" << s.bearingY
		<< " bandScale=" << s.bandScaleX << "/" << s.bandScaleY
		<< " bandOffset=" << s.bandOffsetX << "/" << s.bandOffsetY << ")";
}

}
