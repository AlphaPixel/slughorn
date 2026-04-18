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

// TODO: Something like `slugi_t` for `uint32_t`, since it SEEMS to be the only non-float type
// we're using (beyond `size_t`).

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
// x' = xx * x + xy * y + dx
// y' = yx * x + yy * y + dy
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
// Quad
//
// Used both as a LITERAL "quad" for use in creating compatible 2D geometry and as a "bounding box"
// holder type.
// ================================================================================================
struct Quad {
	slug_t x0, y0; // bottom-left
	slug_t x1, y1; // top-right
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
// Key::fromCodepoint(cp) - a Unicode codepoint (or any uint32_t ID). Implicitly constructible from
// uint32_t for backwards compatibility with existing call sites.
//
// Key::fromString(name) - a named shape / composite ("logo", "axolotl", ...)
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

struct KeyIterator {
	KeyIterator(uint32_t _counter=0): counter(_counter) {}
	KeyIterator(const char* _prefix): prefix(_prefix) {}
	KeyIterator(std::string _prefix): prefix(std::move(_prefix)) {}

	Key next() {
		if(prefix.empty()) return Key::fromCodepoint(counter++);

		return Key::fromString(prefix + "_" + std::to_string(counter++));
	}

	std::string prefix;

	uint32_t counter = 0;
};

// ================================================================================================
// Layer
//
// Represents the "state" of a single `Shape` instance, and includes all of the information the
// caller will need to contextually process the `Shape` in whatever setting slughorn is being used.
// ================================================================================================
struct Layer {
	Key key = Key(0u);

	Color color{};

	// Per-layer offset in scaled units. dx/dy record where this layer's curves
	// originated on the source canvas (set by decomposePath / loadShape). The
	// renderer multiplies dx/dy by scale to recover object-space position.
	// xx/yx/xy/yy are reserved for future rotation/shear support and should be
	// left as identity for now.
	Matrix transform = Matrix::identity();

	// TODO: Document WHEN and HOW this "should" be used!
	slug_t scale = 1.0_cv;

	// Shader effect to apply when rendering this layer. 0 = standard Slug coverage fill (default,
	// no overhead). Non-zero values select an effect branch in the fragment shader; the set of
	// valid IDs and their semantics are defined there. For example, something like:
	//
	// 0 - standard fill (color * slug coverage); this should ALWAYS be the 0/default
	// 1 - texture fill (texture(slug_effectTexture, v_emCoord) * slug coverage)
	// 2 - GLSL procedural
	// 3 - Etc, etc.
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
// Keys are Atlas::Key values - either a codepoint (uint32_t, implicitly convertible) or a named
// string. Both shapes and composite shapes share the same key namespace; do not register the same
// key under both addShape() and addCompositeShape().
// ================================================================================================
class Atlas {
public:
	Atlas();
	virtual ~Atlas();

	// A single quadratic Bezier segment: p1 -> p2 (control) -> p3.
	struct Curve {
		slug_t x1, y1; // start point
		slug_t x2, y2; // off-curve control point
		slug_t x3, y3; // end point
	};

	using Curves = std::vector<Curve>;

	// Everything the renderer needs to draw one shape. Populated by build() and returned by
	// getShape().
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

		// Compute the world-space bounding quad for this shape.
		//
		// transform.dx/dy is the canvas-space origin of the shape (as returned
		// by decomposePath / loadShape). scale converts from source units to
		// world units. expand adds a uniform outset in world units (useful for
		// a 1-pixel AA fringe: pass 1.0f / scale).
		//
		// The returned quad is relative to (0,0) - scene placement is the
		// caller's responsibility (e.g. osg::MatrixTransform).
		Quad computeQuad(const Matrix& transform, slug_t scale = 1.0_cv, slug_t expand = 0.0_cv) const {
			const slug_t ox = transform.dx * scale;
			const slug_t oy = transform.dy * scale;
			return {
				ox + (bearingX - expand) * scale,
				oy + (bearingY - height - expand) * scale,
				ox + (bearingX + width + expand) * scale,
				oy + (bearingY + expand) * scale
			};
		}
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
	// RGBA32F - four 32-bit floats per texel (curve texture)
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
	// Packing statistics - populated by build(), valid after isBuilt() == true.
	//
	// Tracks texture memory usage and alignment waste introduced by
	// alignCursorForSpan() - the mechanism that ensures band curve lists never
	// straddle a row boundary (required for correct Slug shader behaviour).
	//
	// curveTexelsUsed + curveTexelsPadding <= curveTexelsTotal
	// (total may exceed their sum due to unused space at the end of the last row)
	// --------------------------------------------------------------------------------------------
	struct PackingStats {
		// Curve texture
		uint32_t curveTexelsUsed = 0; // texels written with actual curve data
		uint32_t curveTexelsPadding = 0; // texels wasted to row-alignment bumps
		uint32_t curveTexelsTotal = 0; // width * height (allocated)

		// Band texture
		uint32_t bandTexelsUsed = 0;
		uint32_t bandTexelsPadding = 0;
		uint32_t bandTexelsTotal = 0;

		// Fraction of allocated texels actually containing live data [0, 1].
		float curveUtilization() const {
			return curveTexelsTotal
				? float(curveTexelsUsed) / float(curveTexelsTotal)
				: 0.f
			;
		}

		float bandUtilization() const {
			return bandTexelsTotal
				? float(bandTexelsUsed) / float(bandTexelsTotal)
				: 0.f
			;
		}

		// Fraction of live texels that are padding (not curve data) [0, 1]. High values suggest
		// band count or shape ordering could be improved.
		float curvePaddingRatio() const {
			const uint32_t live = curveTexelsUsed + curveTexelsPadding;

			return live ? float(curveTexelsPadding) / float(live) : 0.f;
		}

		float bandPaddingRatio() const {
			const uint32_t live = bandTexelsUsed + bandTexelsPadding;

			return live ? float(bandTexelsPadding) / float(live) : 0.f;
		}
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
	// They can be registered before or after build() - the atlas does not need to be rebuilt when
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
	const TextureData& getBandTextureData() const { return _bandData; }

	// Valid after build(). Reports texture utilization and alignment padding waste.
	// See PackingStats for field documentation.
	const PackingStats& getPackingStats() const { return _packingStats; }

	// Bulk accessors - primarily for serialization (slughorn-serial.hpp) and
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
	// Serial reconstruction (used by slughorn-serial.hpp - not for general use)
	//
	// Injects pre-built texture data and shape/composite maps directly, bypassing build().
	// The Atlas is marked as built on return; calling build() afterwards is a no-op.
	// --------------------------------------------------------------------------------------------
	struct SerialData {
		TextureData curveData;
		TextureData bandData;
		std::unordered_map<Key, Shape, KeyHash> shapes;
		std::unordered_map<Key, CompositeShape, KeyHash> composites;
	};

	void loadFromSerial(SerialData&& sd) {
		_curveData = std::move(sd.curveData);
		_bandData = std::move(sd.bandData);
		_shapes = std::move(sd.shapes);
		_compositeShapes = std::move(sd.composites);
		_built = true;
	}

private:
	// --------------------------------------------------------------------------------------------
	// Internal build structures (discarded after build())
	// --------------------------------------------------------------------------------------------
	struct BandEntry {
		// mirrors the uint16_t written to the band texture
		uint16_t curveCount = 0;

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
	void buildShapeBands(
		Key key,
		ShapeBuild& build,
		uint32_t numBandsX,
		uint32_t numBandsY,
		bool overrideMetrics
	);

	void packTextures();

	// --------------------------------------------------------------------------------------------
	// Data
	// --------------------------------------------------------------------------------------------
	std::unordered_map<Key, ShapeBuild, KeyHash> _build; // discarded after build()
	std::unordered_map<Key, Shape, KeyHash> _shapes; // live after build()
	std::unordered_map<Key, CompositeShape, KeyHash> _compositeShapes; // live always

	TextureData _curveData;
	TextureData _bandData;

	PackingStats _packingStats; // populated by packTextures()

	bool _built = false;

	static constexpr uint32_t TEX_WIDTH = 512;
};

// ================================================================================================
// CurveDecomposer
//
// Stateful helper that accepts path commands (moveTo / lineTo / quadTo / cubicTo) and appends the
// equivalent quadratic Bezier segments to a Curves vector.
//
// Cubic segments are handled adaptively via De Casteljau subdivision: each cubic is recursively
// split at its midpoint until the segment is flat enough (both control points within `tolerance`
// of the p0->p3 chord), at which point it is emitted as two quadratics - one for each half of the
// flattened cubic. This produces far fewer curves than blind subdivision for gentle cubics while
// faithfully tracking tight arcs and S-curves.
//
// The two-quadratic leaf matches NanoSVG's own output convention (e.g. a triangle in NanoSVG
// produces 6 quadratic curves, not 3), which keeps curve-texture alignment predictable.
//
// `tolerance` is in the same coordinate space as the curves (em-units by default). The default
// value (1e-4) is suitable for em-normalized [0,1] geometry. Scale it proportionally if your
// authoring space uses different units - e.g. for a 1000-unit em square use 0.1f.
//
// All backends (Cairo, NanoSVG, Skia, Canvas) route cubics through cubicTo and therefore benefit
// from this improvement automatically. Callers that want coarser/faster decomposition can raise
// `tolerance`; callers that want higher fidelity (e.g. very small text) can lower it.
// ================================================================================================
static constexpr slug_t TOLERANCE_DRAFT = 1e-2f; // fast, visible only at large sizes
static constexpr slug_t TOLERANCE_BALANCED = 1e-3f; // good default for screen work
static constexpr slug_t TOLERANCE_FINE = 1e-4f; // high-DPI / print / export

// This is the DEFAULT value, and always produces the predictable (fast, lowest-quality)
// "two-leafs" from a cubiz Bezier.
static constexpr slug_t TOLERANCE_EXACT = std::numeric_limits<slug_t>::max();

struct CurveDecomposer {
	Atlas::Curves& curves;

	// Flatness threshold in curve-space units. A cubic is considered flat enough to emit when both
	// interior control points are within this distance of the p0->p3 chord. Default suits
	// em-normalized [0,1] geometry.
	slug_t tolerance = TOLERANCE_EXACT;

	slug_t _x = 0_cv;
	slug_t _y = 0_cv;
	slug_t _sx = 0_cv;
	slug_t _sy = 0_cv;

	CurveDecomposer(Atlas::Curves& c) : curves(c) {}

	void moveTo(slug_t x, slug_t y) {
		_x = _sx = x;
		_y = _sy = y;
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

	// Adaptive cubic -> quadratic decomposition via De Casteljau subdivision.
	//
	// Recursively splits the cubic until flat enough, then emits two quadratics
	// at the leaf. MAX_DEPTH caps recursion for degenerate inputs. Backends that
	// previously called cubicTo() get improved fidelity automatically; no call
	// site changes required.
	void cubicTo(
		slug_t c1x, slug_t c1y,
		slug_t c2x, slug_t c2y,
		slug_t x3, slug_t y3
	) {
		_cubicAdaptive(_x, _y, c1x, c1y, c2x, c2y, x3, y3, 0);

		_x = x3;
		_y = y3;
	}

	// Close the current subpath by drawing a line back to the subpath start.
	// No-op if the current position is already at the start (degenerate close).
	void close() {
		constexpr slug_t eps = 1e-6_cv;

		if(std::abs(_x - _sx) > eps || std::abs(_y - _sy) > eps) lineTo(_sx, _sy);
	}

private:
	// Maximum recursion depth. Prevents infinite loops on degenerate/malformed
	// input. At depth 8 the maximum chord error is already <0.4% of the original
	// cubic's bounding box, which is well below any practical rendering threshold.
	static constexpr int MAX_DEPTH = 8;

	// Squared distance from point (px,py) to the infinite line through (ax,ay)->(bx,by).
	// Using squared distance avoids a sqrt in the hot path; we compare against tolerance2.
	static slug_t _pointToLineDistSq(
		slug_t px, slug_t py,
		slug_t ax, slug_t ay,
		slug_t bx, slug_t by
	) {
		const slug_t dx = bx - ax;
		const slug_t dy = by - ay;
		const slug_t lenSq = dx*dx + dy*dy;

		if(lenSq < 1e-12_cv) {
			// Degenerate chord - just return distance to the start point.
			const slug_t ex = px - ax;
			const slug_t ey = py - ay;

			return ex*ex + ey*ey;
		}

		// |cross(b-a, a-p)| / |b-a| - perpendicular distance
		const slug_t cross = dx*(ay - py) - dy*(ax - px);

		return (cross * cross) / lenSq;
	}

	// Returns true when both interior control points of the cubic lie within
	// `tolerance` of the p0->p3 chord - i.e. the cubic is visually flat.
	bool _flatEnough(
		slug_t p0x, slug_t p0y,
		slug_t p1x, slug_t p1y,
		slug_t p2x, slug_t p2y,
		slug_t p3x, slug_t p3y
	) const {
		const slug_t tolSq = tolerance * tolerance;

		return
			_pointToLineDistSq(p1x, p1y, p0x, p0y, p3x, p3y) <= tolSq &&
			_pointToLineDistSq(p2x, p2y, p0x, p0y, p3x, p3y) <= tolSq
		;
	}

	// Emit the flat-enough (or max-depth) cubic as two quadratics via midpoint split.
	// This is the leaf operation - matches the original cubicTo behavior and NanoSVG convention.
	void _emitTwoQuads(
		slug_t p0x, slug_t p0y,
		slug_t p1x, slug_t p1y,
		slug_t p2x, slug_t p2y,
		slug_t p3x, slug_t p3y
	) {
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
	}

	// Recursive De Casteljau subdivision. Splits at the midpoint and recurses into each half until
	// flat or MAX_DEPTH is reached.
	void _cubicAdaptive(
		slug_t p0x, slug_t p0y,
		slug_t p1x, slug_t p1y,
		slug_t p2x, slug_t p2y,
		slug_t p3x, slug_t p3y,
		size_t depth
	) {
		if(depth >= MAX_DEPTH || _flatEnough(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y)) {
			_emitTwoQuads(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y);

			return;
		}

		// De Casteljau split at t=0.5
		const slug_t m01x = (p0x + p1x) * 0.5_cv;
		const slug_t m01y = (p0y + p1y) * 0.5_cv;

		const slug_t m12x = (p1x + p2x) * 0.5_cv;
		const slug_t m12y = (p1y + p2y) * 0.5_cv;

		const slug_t m23x = (p2x + p3x) * 0.5_cv;
		const slug_t m23y = (p2y + p3y) * 0.5_cv;

		const slug_t m012x = (m01x + m12x) * 0.5_cv;
		const slug_t m012y = (m01y + m12y) * 0.5_cv;

		const slug_t m123x = (m12x + m23x) * 0.5_cv;
		const slug_t m123y = (m12y + m23y) * 0.5_cv;

		const slug_t m0123x = (m012x + m123x) * 0.5_cv;
		const slug_t m0123y = (m012y + m123y) * 0.5_cv;

		_cubicAdaptive(p0x, p0y, m01x, m01y, m012x, m012y, m0123x, m0123y, depth + 1);
		_cubicAdaptive(m0123x, m0123y, m123x, m123y, m23x, m23y, p3x, p3y, depth + 1);
	}
};

// ================================================================================================
// Debugging Helpers
// ================================================================================================

inline std::ostream& operator<<(std::ostream& os, const Key& k) {
	if(k.type() == Key::Type::Codepoint) return os << "Key(0x"
		<< std::hex << k.codepoint() << std::dec << ")"
	;

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

inline std::ostream& operator<<(std::ostream& os, const Quad& q) {
	return os
		<< "Quad("
		<< "(" << q.x0 << "," << q.y0 << ")"
		<< " -> (" << q.x1 << "," << q.y1 << ")"
		<< ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Layer& l) {
	return os
		<< "Layer(" << l.key
		<< " color=" << l.color
		<< " transform=" << l.transform
		<< " scale=" << l.scale
		<< " effectId=" << l.effectId << ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Atlas::Shape& s) {
	return os
		<< "Shape(w=" << s.width << " h=" << s.height
		<< " bearing=" << s.bearingX << "/" << s.bearingY
		<< " bandScale=" << s.bandScaleX << "/" << s.bandScaleY
		<< " bandOffset=" << s.bandOffsetX << "/" << s.bandOffsetY << ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Atlas::Curve& c) {
	return os
		<< "Curve("
		<< "(" << c.x1 << "," << c.y1 << ")"
		<< " -> (" << c.x2 << "," << c.y2 << ")"
		<< " -> (" << c.x3 << "," << c.y3 << ")"
		<< ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Atlas::PackingStats& p) {
	return os
		<< "PackingStats("
		<< "curve: " << p.curveTexelsUsed << " used"
		<< " + " << p.curveTexelsPadding << " padding"
		<< " / " << p.curveTexelsTotal << " total"
		<< " (" << int(p.curveUtilization() * 100.f) << "% util,"
		<< " " << int(p.curvePaddingRatio() * 100.f) << "% pad)"
		<< " | band: " << p.bandTexelsUsed << " used"
		<< " + " << p.bandTexelsPadding << " padding"
		<< " / " << p.bandTexelsTotal << " total"
		<< " (" << int(p.bandUtilization() * 100.f) << "% util,"
		<< " " << int(p.bandPaddingRatio() * 100.f) << "% pad)"
		<< ")"
	;
}

}
