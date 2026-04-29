// ================================================================================================
// slughorn-python.cpp - pybind11 bindings for slughorn
//
// Covers the core slughorn.hpp API:
//
// slughorn.Color
// slughorn.Matrix
// slughorn.Key (both Codepoint and Name flavors, full __hash__/__eq__)
// slughorn.Layer (key, color, transform, effect_id)
// slughorn.CompositeShape (layers, advance)
// slughorn.Curve (flat - not Atlas.Curve, intentional; see note below)
// slughorn.ShapeInfo (flat)
// slughorn.Shape (flat, readonly)
// slughorn.TextureData (flat, zero-copy memoryview)
// slughorn.Atlas (add_shape, add_composite_shape, build, get_shape, get_composite_shape, has_key,
// is_built property, curve_texture, band_texture)
// slughorn.CurveDecomposer (owns its Curves internally - safe for Python GC)
//
// slughorn.emoji (submodule)
//
// SCOPING NOTE
// ------------
// Curve / ShapeInfo / Shape / TextureData are nested inside Atlas in C++ (Atlas::Curve etc.)
// because they belong to Atlas conceptually. In Python they are exposed at module level
// (slughorn.Curve etc.) because:
//
// 1. Python has no "using" / typedef - writing Atlas.Curve everywhere is awkward for the user.
// 2. slughorn is small; the Atlas parentage is an implementation detail, not a semantic boundary
// that Python users need to see.
// 3. test.py / slughorn_todo.py already use the flat names and read well.
//
// OWNERSHIP
// ---------
// Atlas is heap-allocated and managed by shared_ptr so Python GC and C++ ref-counting cooperate
// safely.
//
// ShapeInfo / Shape / TextureData / Curve are copied across the boundary (they are value types).
//
// TextureData.bytes is a zero-copy memoryview that borrows from the Atlas's internal buffer - keep
// the Atlas alive for the duration of any view over its data.
//
// CurveDecomposer owns its Curves vector internally (unlike the C++ version which holds a
// reference). Call .get_curves() to retrieve them. This avoids the dangling-reference hazard that
// would exist if Python's GC collected the Curves list before the decomposer.
// ================================================================================================

#include "slughorn.hpp"
#include "slughorn-canvas.hpp"

#define SLUGHORN_EMOJI_IMPLEMENTATION
#include "slughorn-emoji.hpp"

#ifdef SLUGHORN_HAS_SERIAL
#include "slughorn-serial.hpp"
#endif

#ifdef SLUGHORN_HAS_FREETYPE
#include "slughorn-freetype.hpp"
#endif

#include <pybind11/pybind11.h>
#include <pybind11/stl.h> // std::vector, std::optional
#include <pybind11/functional.h>

#include <sstream>

namespace py = pybind11;

// ================================================================================================
// Internal helpers
// ================================================================================================

// Zero-copy memoryview over a vector<uint8_t>.
// The vector must outlive the view - caller's responsibility.
static py::memoryview bytesView(const std::vector<uint8_t>& v) {
	return py::memoryview::from_memory(
		const_cast<uint8_t*>(v.data()),
		static_cast<py::ssize_t>(v.size())
	);
}

// Use the C++ operator<< to build a repr string for any type that has one.
template<typename T>
static std::string streamRepr(const T& v) {
	std::ostringstream ss;

	ss << v;

	return ss.str();
}

// ================================================================================================
// Python-friendly CurveDecomposer
//
// Owns its Curves vector so Python's GC cannot collect it out from under us. The C++
// CurveDecomposer holds a Curves& - that's fine in C++ but unsafe to expose directly to Python.
// ================================================================================================
struct PyCurveDecomposer {
	slughorn::Atlas::Curves curves;
	slughorn::CurveDecomposer decomposer;

	PyCurveDecomposer(): decomposer(curves) {}

	void moveTo(slug_t x, slug_t y) { decomposer.moveTo(x, y); }
	void lineTo(slug_t x3, slug_t y3) { decomposer.lineTo(x3, y3); }
	void quadTo(slug_t cx, slug_t cy, slug_t x3, slug_t y3) { decomposer.quadTo(cx, cy, x3, y3); }
	void cubicTo(
		slug_t c1x, slug_t c1y,
		slug_t c2x, slug_t c2y,
		slug_t x3, slug_t y3
	) { decomposer.cubicTo(c1x, c1y, c2x, c2y, x3, y3); }

	const slughorn::Atlas::Curves& getCurves() const { return curves; }
	void close() { decomposer.close(); }
	slug_t getTolerance() const { return decomposer.tolerance; }
	void setTolerance(slug_t tolerance) { decomposer.tolerance = tolerance; }
	void clear() { curves.clear(); }
};

// ================================================================================================
// Module
// ================================================================================================

PYBIND11_MODULE(slughorn, m) {
	m.doc() = "slughorn - GPU-native vector shape renderer (Slug algorithm, Lengyel 2017)";

	// ============================================================================================
	// slughorn.Key
	//
	// Discriminated union: Codepoint (uint32_t) or Name (string).
	// Both namespaces are hash-disjoint in C++; __hash__ and __eq__ reflect
	// that so Key objects can be used as Python dict keys correctly.
	// ============================================================================================
	auto key_ = py::class_<slughorn::Key>(m, "Key");

	py::enum_<slughorn::Key::Type>(key_, "Type")
		.value("Codepoint", slughorn::Key::Type::Codepoint)
		.value("Name", slughorn::Key::Type::Name)
		.export_values()
	;

	key_
		// Constructors
		.def(py::init<>(), "Default key: codepoint 0.")
		.def(py::init<uint32_t>(), py::arg("codepoint"),
			"Construct a Codepoint key from a uint32_t (e.g. ord('A'))."
		)
		.def(py::init<const std::string&>(), py::arg("name"),
			"Construct a named key from a string (e.g. Key('logo'))."
		)

		// Static factories
		.def_static("from_codepoint", &slughorn::Key::fromCodepoint, py::arg("codepoint"),
			"Construct a Codepoint key. Equivalent to Key(codepoint)."
		)
		.def_static("from_string", &slughorn::Key::fromString, py::arg("name"),
			"Construct a named key (e.g. Key.from_string('logo'))."
		)

		// Accessors
		.def_property_readonly("type", &slughorn::Key::type,
			"KeyType.Codepoint or KeyType.Name."
		)
		.def_property_readonly("codepoint", &slughorn::Key::codepoint,
			"The uint32_t codepoint. Only valid when type == KeyType.Codepoint."
		)
		.def_property_readonly("name", &slughorn::Key::name,
			"The string name. Only valid when type == KeyType.Name."
		)
		.def_property_readonly("hash", &slughorn::Key::hash,
			"Precomputed hash (same value used by C++ KeyHash)."
		)

		// Python protocol
		.def("__eq__", &slughorn::Key::operator==)
		.def("__ne__", &slughorn::Key::operator!=)
		.def("__hash__", &slughorn::Key::hash, "Enable use as a Python dict key or set member.")
		.def("__repr__", [](const slughorn::Key& k) { return streamRepr(k); })
	;

	// =========================================================================
	// slughorn.KeyIterator
	// =========================================================================
	py::class_<slughorn::KeyIterator>(m, "KeyIterator")
		.def(py::init<>(), "Numeric auto-key iterator starting at 0.")
		.def(py::init<uint32_t>(), py::arg("counter"),
			"Numeric auto-key iterator starting at counter."
		)
		.def(py::init([](std::string prefix) {
			return slughorn::KeyIterator(prefix);
		}), py::arg("prefix"),
			"String key iterator: produces prefix_0, prefix_1, ..."
		)
		.def("next", &slughorn::KeyIterator::next, "Return the next Key and advance the counter.")
		.def("__iter__", [](slughorn::KeyIterator& ki) -> slughorn::KeyIterator& {
			return ki;
		}, py::return_value_policy::reference)
		.def("__next__", &slughorn::KeyIterator::next)
		.def_readwrite("counter", &slughorn::KeyIterator::counter,
			"Current counter value (read/write)."
		)
		.def_readwrite("prefix", &slughorn::KeyIterator::prefix,
			"Prefix string, or empty string for numeric mode."
		)
		.def("__repr__", [](const slughorn::KeyIterator& ki) {
			if(ki.prefix.empty()) return "KeyIterator(counter=" + std::to_string(ki.counter) + ")";

			return "KeyIterator(prefix='" + ki.prefix + "', counter=" + std::to_string(ki.counter) + ")";
		})
	;

	// ============================================================================================
	// slughorn.Color
	// ============================================================================================
	py::class_<slughorn::Color>(m, "Color")
		.def(py::init<>(), "Default: (0, 0, 0, 1) - opaque black.")
		.def(py::init([](slug_t r, slug_t g, slug_t b, slug_t a) {
			return slughorn::Color{r, g, b, a};
		}), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1.0f,
			"Construct from r, g, b [, a]. All values in [0, 1]."
		)
		.def_readwrite("r", &slughorn::Color::r)
		.def_readwrite("g", &slughorn::Color::g)
		.def_readwrite("b", &slughorn::Color::b)
		.def_readwrite("a", &slughorn::Color::a)
		.def_property_readonly("values", [](const slughorn::Color& c) {
			return py::make_tuple(c.r, c.g, c.b, c.a);
		}, "Return (r, g, b, a) as a Python tuple.")
		.def("__repr__", [](const slughorn::Color& c) { return streamRepr(c); })
	;

	m.attr("TOLERANCE_DRAFT") = py::float_(slughorn::TOLERANCE_DRAFT);
	m.attr("TOLERANCE_BALANCED") = py::float_(slughorn::TOLERANCE_BALANCED);
	m.attr("TOLERANCE_FINE") = py::float_(slughorn::TOLERANCE_FINE);
	m.attr("TOLERANCE_EXACT") = py::float_(slughorn::TOLERANCE_EXACT);

	// ============================================================================================
	// slughorn.Matrix
	//
	// Column-major 2-D affine:
	//
	// x' = xx * x + xy * y + dx
	// y' = yx * x + yy * y + dy
	// ============================================================================================
	py::class_<slughorn::Matrix>(m, "Matrix")
		.def(py::init<>(), "Default: identity.")
		.def_static("identity", &slughorn::Matrix::identity, "Return the identity matrix.")
		.def_readwrite("xx", &slughorn::Matrix::xx)
		.def_readwrite("yx", &slughorn::Matrix::yx)
		.def_readwrite("xy", &slughorn::Matrix::xy)
		.def_readwrite("yy", &slughorn::Matrix::yy)
		.def_readwrite("dx", &slughorn::Matrix::dx)
		.def_readwrite("dy", &slughorn::Matrix::dy)
		.def("is_identity", &slughorn::Matrix::isIdentity,
			"Return True if this matrix is (approximately) the identity."
		)
		.def("apply", [](const slughorn::Matrix& mat, slug_t x, slug_t y) {
			slug_t ox, oy;

			mat.apply(x, y, ox, oy);

			return py::make_tuple(ox, oy);
		}, py::arg("x"), py::arg("y"),
			"Apply the matrix to point (x, y), returning (x', y').")
		.def("__mul__", &slughorn::Matrix::operator*, py::arg("rhs"),
			"Concatenate: (self * rhs) - rhs is applied first."
		)
		.def("__repr__", [](const slughorn::Matrix& mat) { return streamRepr(mat); })
	;

	// ============================================================================================
	// slughorn.Quad
	// ============================================================================================
	py::class_<slughorn::Quad>(m, "Quad")
		.def(py::init([](slug_t x0, slug_t y0, slug_t x1, slug_t y1) {
			return slughorn::Quad{x0, y0, x1, y1};
		}), py::arg("x0"), py::arg("y0"), py::arg("x1"), py::arg("y1"))
		.def_readwrite("x0", &slughorn::Quad::x0)
		.def_readwrite("y0", &slughorn::Quad::y0)
		.def_readwrite("x1", &slughorn::Quad::x1)
		.def_readwrite("y1", &slughorn::Quad::y1)
		.def_property_readonly("values", [](const slughorn::Quad& q) {
			return py::make_tuple(q.x0, q.y0, q.x1, q.y1);
		}, "Return (x0, y0, x1, y1) as a Python tuple.")
		.def("__repr__", [](const slughorn::Quad& q) { return streamRepr(q); })
	;

	// ============================================================================================
	// slughorn.Layer
	//
	// key, color, transform, effect_id - all four fields now present.
	// ============================================================================================
	py::class_<slughorn::Layer>(m, "Layer")
		.def(py::init<>())
		.def_readwrite("key", &slughorn::Layer::key,
			"Key identifying the shape in the Atlas.")
		.def_readwrite("color", &slughorn::Layer::color,
			"RGBA fill color for this layer.")
		.def_readwrite("transform", &slughorn::Layer::transform,
			"Local-coords affine transform. dx/dy carry the canvas offset; "
			"xx/yx/xy/yy carry any additional rotation/scale (COLRv1 paint nodes).")
		.def_readwrite("scale", &slughorn::Layer::scale,
			"World-scale multiplier.\n"
			"  Text / FreeType2: set to the font size in world units (e.g. 0.1 for\n"
			"    a glyph that should be 0.1 world-units tall). computeQuad() and\n"
			"    compile() both read this value.\n"
			"  SVG / Cairo / NanoSVG: leave at the default of 1.0 - curves are\n"
			"    already em-normalised by the backend.")
		.def_readwrite("effect_id", &slughorn::Layer::effectId,
			"Fragment-shader fill mode selector. "
			"0 = standard Slug fill (default). "
			"See osgSlug-frag.glsl slug_ApplyEffect() for the full table.")
		.def("__repr__", [](const slughorn::Layer& l) { return streamRepr(l); })
	;

	// ============================================================================================
	// slughorn.CompositeShape
	// ============================================================================================
	py::class_<slughorn::CompositeShape>(m, "CompositeShape")
		.def(py::init<>())
		.def_readwrite("layers", &slughorn::CompositeShape::layers,
			"Ordered list of Layer objects drawn bottom-to-top."
		)
		.def_readwrite("advance", &slughorn::CompositeShape::advance,
			"Horizontal advance in em-space (used for text cursor / layout)."
		)
		.def("__len__", [](const slughorn::CompositeShape& g) { return g.layers.size(); })
		.def("__repr__", [](const slughorn::CompositeShape& g) {
			return
				"CompositeShape(" + std::to_string(g.layers.size()) +
				" layers, advance=" + std::to_string(g.advance) + ")"
			;
		})
	;

	// ============================================================================================
	// slughorn.Curve (Atlas::Curve in C++, flat in Python - see file header)
	// ============================================================================================
	py::class_<slughorn::Atlas::Curve>(m, "Curve")
		.def(py::init<>())
		.def(py::init([](
			slug_t x1, slug_t y1,
			slug_t x2, slug_t y2,
			slug_t x3, slug_t y3
		) { return slughorn::Atlas::Curve{x1, y1, x2, y2, x3, y3}; }),
			py::arg("x1"), py::arg("y1"),
			py::arg("x2"), py::arg("y2"),
			py::arg("x3"), py::arg("y3"),
			"Quadratic Bezier: p1=(x1,y1) start, p2=(x2,y2) control, p3=(x3,y3) end."
		)
		.def_readwrite("x1", &slughorn::Atlas::Curve::x1)
		.def_readwrite("y1", &slughorn::Atlas::Curve::y1)
		.def_readwrite("x2", &slughorn::Atlas::Curve::x2)
		.def_readwrite("y2", &slughorn::Atlas::Curve::y2)
		.def_readwrite("x3", &slughorn::Atlas::Curve::x3)
		.def_readwrite("y3", &slughorn::Atlas::Curve::y3)
		.def("to_tuple", [](const slughorn::Atlas::Curve& c) {
			return py::make_tuple(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3);
		}, "Return (x1,y1, x2,y2, x3,y3) as a flat Python tuple.")
		.def("__repr__", [](const slughorn::Atlas::Curve& c) {
			return
				"Curve(start=(" + std::to_string(c.x1) + ", " + std::to_string(c.y1) +
				") ctrl=(" + std::to_string(c.x2) + ", " + std::to_string(c.y2) +
				") end=(" + std::to_string(c.x3) + ", " + std::to_string(c.y3) + "))"
			;
		})
	;

	// ============================================================================================
	// slughorn.ShapeInfo (Atlas::ShapeInfo in C++, flat in Python)
	// ============================================================================================
	py::class_<slughorn::Atlas::ShapeInfo>(m, "ShapeInfo")
		.def(py::init<>())
		.def_readwrite("curves", &slughorn::Atlas::ShapeInfo::curves,
			"List of Curve objects in em-normalized coordinates."
		)
		.def_readwrite("auto_metrics", &slughorn::Atlas::ShapeInfo::autoMetrics,
			"If True (default), derive width/height/bearing/advance from the "
			"curve bounding box automatically."
		)
		.def_readwrite("bearing_x", &slughorn::Atlas::ShapeInfo::bearingX)
		.def_readwrite("bearing_y", &slughorn::Atlas::ShapeInfo::bearingY)
		.def_readwrite("width", &slughorn::Atlas::ShapeInfo::width)
		.def_readwrite("height", &slughorn::Atlas::ShapeInfo::height)
		.def_readwrite("advance", &slughorn::Atlas::ShapeInfo::advance)
		.def_readwrite("num_bands_x", &slughorn::Atlas::ShapeInfo::numBandsX,
			"Number of X bands (0 = auto-pick a sensible default)."
		)
		.def_readwrite("num_bands_y", &slughorn::Atlas::ShapeInfo::numBandsY,
			"Number of Y bands (0 = auto-pick a sensible default)."
		)
	;

	// ============================================================================================
	// slughorn.Shape (Atlas::Shape in C++, flat in Python - read-only)
	// ============================================================================================
	py::class_<slughorn::Atlas::Shape>(m, "Shape")
		.def_readonly("band_tex_x", &slughorn::Atlas::Shape::bandTexX,
			"X texel coordinate of this shape's band header block."
		)
		.def_readonly("band_tex_y", &slughorn::Atlas::Shape::bandTexY,
			"Y texel coordinate of this shape's band header block."
		)
		.def_readonly("band_max_x", &slughorn::Atlas::Shape::bandMaxX,
			"numBands - 1 in X (band index clamp limit)."
		)
		.def_readonly("band_max_y", &slughorn::Atlas::Shape::bandMaxY,
			"numBands - 1 in Y (band index clamp limit)."
		)
		.def_readonly("band_scale_x", &slughorn::Atlas::Shape::bandScaleX)
		.def_readonly("band_scale_y", &slughorn::Atlas::Shape::bandScaleY)
		.def_readonly("band_offset_x", &slughorn::Atlas::Shape::bandOffsetX)
		.def_readonly("band_offset_y", &slughorn::Atlas::Shape::bandOffsetY)
		.def_readonly("bearing_x", &slughorn::Atlas::Shape::bearingX)
		.def_readonly("bearing_y", &slughorn::Atlas::Shape::bearingY)
		.def_readonly("width", &slughorn::Atlas::Shape::width)
		.def_readonly("height", &slughorn::Atlas::Shape::height)
		.def_readonly("advance", &slughorn::Atlas::Shape::advance)

		// Convenience: recover em-space origin and size (mirrors slug_EmToUV logic)
		.def_property_readonly("em_origin", [](const slughorn::Atlas::Shape& s) {
			// emOrigin = -bandOffset / bandScale
			float ox = (s.bandScaleX != 0.f) ? -s.bandOffsetX / s.bandScaleX : 0.f;
			float oy = (s.bandScaleY != 0.f) ? -s.bandOffsetY / s.bandScaleY : 0.f;

			return py::make_tuple(ox, oy);
		}, "Em-space (x, y) of the shape's bottom-left corner. "
			"Mirrors slug_EmToUV's emOrigin computation."
		)
		.def_property_readonly("em_size", [](const slughorn::Atlas::Shape& s) {
			// emSize = (bandMax + 1) / bandScale
			float sx = (s.bandScaleX != 0.f) ? float(s.bandMaxX + 1) / s.bandScaleX : 0.f;
			float sy = (s.bandScaleY != 0.f) ? float(s.bandMaxY + 1) / s.bandScaleY : 0.f;
			return py::make_tuple(sx, sy);
		}, "Em-space (width, height) of the shape's bounding box. "
			"Mirrors slug_EmToUV's emSize computation."
		)
		.def("em_to_uv", [](const slughorn::Atlas::Shape& s, slug_t ex, slug_t ey) {
			// Direct Python port of slug_EmToUV()
			float ox = (s.bandScaleX != 0.f) ? -s.bandOffsetX / s.bandScaleX : 0.f;
			float oy = (s.bandScaleY != 0.f) ? -s.bandOffsetY / s.bandScaleY : 0.f;
			float sx = (s.bandScaleX != 0.f) ? float(s.bandMaxX + 1) / s.bandScaleX : 1.f;
			float sy = (s.bandScaleY != 0.f) ? float(s.bandMaxY + 1) / s.bandScaleY : 1.f;

			return py::make_tuple((ex - ox) / sx, (ey - oy) / sy);
		}, py::arg("em_x"), py::arg("em_y"),
			"Convert an em-space coordinate to a normalized [0,1] UV. "
			"Python port of the GLSL slug_EmToUV() helper. "
			"(0,0) = bottom-left of bounding box, (1,1) = top-right.")
		.def("compute_quad", &slughorn::Atlas::Shape::computeQuad,
			py::arg("transform"),
			py::arg("scale") = 1.0f,
			py::arg("expand") = 0.0f,
			"Compute the world-space bounding quad for this shape."
		)
		.def("__repr__", [](const slughorn::Atlas::Shape& s) { return streamRepr(s); })
	;

	// ============================================================================================
	// slughorn.TextureData (Atlas::TextureData in C++, flat in Python)
	// ============================================================================================
	py::class_<slughorn::Atlas::TextureData>(m, "TextureData")
		.def_readonly("width", &slughorn::Atlas::TextureData::width)
		.def_readonly("height", &slughorn::Atlas::TextureData::height)
		.def_property_readonly("format", [](const slughorn::Atlas::TextureData& td) {
			return td.format == slughorn::Atlas::TextureData::Format::RGBA32F
				? "RGBA32F" : "RGBA16UI"
			;
		}, "String: 'RGBA32F' (curve texture) or 'RGBA16UI' (band texture).")
		.def_property_readonly("bytes", [](const slughorn::Atlas::TextureData& td) {
			return bytesView(td.bytes);
		}, "Zero-copy memoryview of the raw pixel data (row-major). "
			"Keep the Atlas alive for the duration of any view."
		)
		.def("__repr__", [](const slughorn::Atlas::TextureData& td) {
			const char* fmt = td.format == slughorn::Atlas::TextureData::Format::RGBA32F
				? "RGBA32F" : "RGBA16UI";

			return "TextureData(" + std::to_string(td.width) + "x" +
				std::to_string(td.height) + " " + fmt + " " +
				std::to_string(td.bytes.size()) + " bytes)"
			;
		})
	;

	// ============================================================================================
	// slughorn.PackingStats (Atlas::PackingStats in C++, flat in Python)
	// ============================================================================================
	py::class_<slughorn::Atlas::PackingStats>(m, "PackingStats")
		.def_readonly("curve_texels_used", &slughorn::Atlas::PackingStats::curveTexelsUsed)
		.def_readonly("curve_texels_padding", &slughorn::Atlas::PackingStats::curveTexelsPadding)
		.def_readonly("curve_texels_total", &slughorn::Atlas::PackingStats::curveTexelsTotal)
		.def_readonly("band_texels_used", &slughorn::Atlas::PackingStats::bandTexelsUsed)
		.def_readonly("band_texels_padding", &slughorn::Atlas::PackingStats::bandTexelsPadding)
		.def_readonly("band_texels_total", &slughorn::Atlas::PackingStats::bandTexelsTotal)
		.def("curve_utilization", &slughorn::Atlas::PackingStats::curveUtilization)
		.def("band_utilization", &slughorn::Atlas::PackingStats::bandUtilization)
		.def("curve_padding_ratio", &slughorn::Atlas::PackingStats::curvePaddingRatio)
		.def("band_padding_ratio", &slughorn::Atlas::PackingStats::bandPaddingRatio)
		.def("__repr__", [](const slughorn::Atlas::PackingStats& p) { return streamRepr(p); })
	;

	// ============================================================================================
	// slughorn.Atlas
	// ============================================================================================
	py::class_<slughorn::Atlas, std::shared_ptr<slughorn::Atlas>>(m, "Atlas")
		.def(py::init<>())

		.def("add_shape", &slughorn::Atlas::addShape,
			py::arg("key"), py::arg("info"),
			"Register a shape under key. Must be called before build()."
		)

		.def("add_composite_shape", &slughorn::Atlas::addCompositeShape,
			py::arg("key"), py::arg("composite"),
			"Register a CompositeShape under key. "
			"May be called before or after build()."
		)

		.def("build", &slughorn::Atlas::build,
			"Pack all registered shapes into the texture buffers. "
			"Idempotent - subsequent calls are no-ops."
		)

		.def_property_readonly("is_built", &slughorn::Atlas::isBuilt,
			"True after build() has been called."
		)

		.def("get_shape",
			[](const slughorn::Atlas& a, slughorn::Key key)
				-> std::optional<slughorn::Atlas::Shape>
			{
				const auto* s = a.getShape(key);

				if(!s) return std::nullopt;

				return *s;
			},
			py::arg("key"),
			"Return the Shape for key (valid after build()), or None if not found. "
			"Accepts both Key objects and raw uint32_t codepoints."
		)

		.def("get_composite_shape",
			[](const slughorn::Atlas& a, slughorn::Key key)
				-> std::optional<slughorn::CompositeShape>
			{
				const auto* c = a.getCompositeShape(key);

				if(!c) return std::nullopt;

				return *c;
			},
			py::arg("key"),
			"Return the CompositeShape for key, or None if not found."
		)

		.def("has_key",
			&slughorn::Atlas::hasKey,
			py::arg("key"),
			"Return True if key is registered (shape, composite, or pending build)."
		)

		// Bulk accessors - primarily for slughorn_serial.py
		.def("get_shapes",
			[](const slughorn::Atlas& a) {
				// Return a Python dict {Key: Shape} - copies values (Shape is small)
				py::dict d;
				for(const auto& [k, v] : a.getShapes()) d[py::cast(k)] = v;
				return d;
			},
			"Return a dict of all {Key: Shape} entries (valid after build()). "
			"Primarily used by slughorn_serial for serialization.")

		.def("get_composite_shapes",
			[](const slughorn::Atlas& a) {
				py::dict d;
				for(const auto& [k, v] : a.getCompositeShapes()) d[py::cast(k)] = v;
				return d;
			},
			"Return a dict of all {Key: CompositeShape} entries. "
			"Primarily used by slughorn_serial for serialization.")

		.def_property_readonly("packing_stats",
			[](const slughorn::Atlas& a) -> const slughorn::Atlas::PackingStats& {
				return a.getPackingStats();
			},
			py::return_value_policy::reference_internal,
			"Packing statistics for the built atlas."
		)

		.def_property_readonly("curve_texture",
			[](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
				return a.getCurveTextureData();
			},
			py::return_value_policy::reference_internal,
			"TextureData for the RGBA32F curve texture (valid after build())."
		)

		.def_property_readonly("band_texture",
			[](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
				return a.getBandTextureData();
			},
			py::return_value_policy::reference_internal,
			"TextureData for the RGBA16UI band texture (valid after build())."
		)
	;

	// ============================================================================================
	// slughorn.CurveDecomposer
	//
	// Wraps PyCurveDecomposer (owns its Curves internally) rather than the raw
	// C++ CurveDecomposer (which holds a Curves& - unsafe for Python GC).
	// ============================================================================================
	py::class_<PyCurveDecomposer>(m, "CurveDecomposer",
		"Stateful path sink: accepts move_to / line_to / quad_to / cubic_to "
		"and accumulates quadratic Bezier segments internally.\n\n"
		"Call get_curves() to retrieve the resulting Curves list, then pass "
		"it to ShapeInfo.curves.")
		.def(py::init<>())
		.def_property("tolerance", &PyCurveDecomposer::getTolerance, &PyCurveDecomposer::setTolerance,
			"Flatness threshold for cubic decomposition in curve-space units."
		)
		.def("move_to", &PyCurveDecomposer::moveTo, py::arg("x"), py::arg("y"))
		.def("line_to", &PyCurveDecomposer::lineTo, py::arg("x3"), py::arg("y3"))
		.def("quad_to", &PyCurveDecomposer::quadTo,
			py::arg("cx"), py::arg("cy"), py::arg("x3"), py::arg("y3")
		)
		.def("cubic_to", &PyCurveDecomposer::cubicTo,
			py::arg("c1x"), py::arg("c1y"),
			py::arg("c2x"), py::arg("c2y"),
			py::arg("x3"), py::arg("y3")
		)
		.def("get_curves", &PyCurveDecomposer::getCurves,
			py::return_value_policy::copy,
			"Return a copy of the accumulated Curves list."
		)
		.def("close", &PyCurveDecomposer::close,
			"Close the current subpath by drawing a line back to the start point."
		)
		.def("clear", &PyCurveDecomposer::clear,
			"Discard all accumulated curves (reuse the decomposer for a new path)."
		)
		.def("__len__", [](const PyCurveDecomposer& d) {
			return d.getCurves().size();
		}, "Number of curves accumulated so far.")
	;

	// =========================================================================
	// slughorn.canvas submodule
	// =========================================================================
	{
		py::module_ canvas = m.def_submodule("canvas",
			"HTML Canvas-style drawing context for slughorn.\n\n"
			"Build CompositeShapes from 2-D path commands (moveTo, lineTo, quadTo, "
			"bezierTo, closePath) plus arc primitives and convenience shape helpers "
			"(rect, roundedRect, circle, ellipse).\n\n"
			"Each fill() call commits the current path as a new Layer. "
			"Call finalize() to retrieve the completed CompositeShape."
		);

		py::class_<slughorn::canvas::Canvas>(canvas, "Canvas")
			.def(py::init<slughorn::Atlas&, slughorn::KeyIterator>(),
				py::arg("atlas"), py::arg("key_iterator")=slughorn::KeyIterator(),
				"Construct a Canvas writing into atlas, using key_iterator for auto-generated keys."
			)

			// CurveDecomposer access ------------------------------------------

			.def("decomposer", py::overload_cast<>(&slughorn::canvas::Canvas::decomposer),
				py::return_value_policy::reference_internal,
				"Access the internal CurveDecomposer to tune tolerance etc."
			)

			// Path commands ---------------------------------------------------

			.def("begin_path", &slughorn::canvas::Canvas::beginPath,
				"Discard any accumulated path state and start fresh."
			)
			.def("move_to", &slughorn::canvas::Canvas::moveTo, py::arg("x"), py::arg("y"))
			.def("line_to", &slughorn::canvas::Canvas::lineTo, py::arg("x"), py::arg("y"))
			.def("quad_to", &slughorn::canvas::Canvas::quadTo,
				py::arg("cx"), py::arg("cy"), py::arg("x"), py::arg("y")
			)
			.def("bezier_to", &slughorn::canvas::Canvas::bezierTo,
				py::arg("c1x"), py::arg("c1y"), py::arg("c2x"), py::arg("c2y"),
				py::arg("x"), py::arg("y")
			)
			.def("close_path", &slughorn::canvas::Canvas::closePath)

			// Convenience shape helpers ---------------------------------------

			.def("rect", &slughorn::canvas::Canvas::rect,
				py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"),
				"Axis-aligned rectangle."
			)
			.def("rounded_rect", &slughorn::canvas::Canvas::roundedRect,
				py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"), py::arg("r"),
				"Rounded rectangle with uniform corner radius r."
			)
			.def("circle", &slughorn::canvas::Canvas::circle,
				py::arg("cx"), py::arg("cy"), py::arg("r")
			)
			.def("ellipse", &slughorn::canvas::Canvas::ellipse,
				py::arg("cx"), py::arg("cy"), py::arg("rx"), py::arg("ry")
			)
			.def("arc", &slughorn::canvas::Canvas::arc,
				py::arg("cx"), py::arg("cy"), py::arg("r"),
				py::arg("start_angle"), py::arg("end_angle"), py::arg("ccw") = false,
				"Circular arc. Angles in radians from +X axis, Y-up convention."
			)
			.def("arc_to", &slughorn::canvas::Canvas::arcTo,
				py::arg("x1"), py::arg("y1"), py::arg("x2"), py::arg("y2"), py::arg("r"),
				"Tangential arc from current point. Matches HTML Canvas arcTo()."
			)

			// Commit ----------------------------------------------------------

			.def("fill",
				[](slughorn::canvas::Canvas& c, slughorn::Color color, slug_t scale) {
					return c.fill(color, scale);
				},
				py::arg("color"), py::arg("scale") = 1.0f,
				"Commit the current path as a new Layer with the given color.\n"
				"Returns the auto-generated Key, or Key(0) if the path was empty."
			)
			.def("define_shape",
				[](slughorn::canvas::Canvas& c, slughorn::Key key, slug_t scale) {
					return c.defineShape(key, scale);
				},
				py::arg("key"), py::arg("scale") = 1.0f,
				"Register the current path as a named Shape (geometry only, no Layer).\n"
				"Returns False if the path was empty."
			)

			// CompositeShape management ---------------------------------------

			.def("begin_composite", &slughorn::canvas::Canvas::beginComposite,
				"Discard all accumulated layers and start a fresh composite."
			)
			.def("set_advance", &slughorn::canvas::Canvas::setAdvance,
				py::arg("advance"),
				"Set the horizontal advance of the composite being built."
			)
			.def("finalize",
				py::overload_cast<>(&slughorn::canvas::Canvas::finalize),
				"Return the completed CompositeShape and reset internal state."
			)
			.def("finalize",
				py::overload_cast<slughorn::Key>(&slughorn::canvas::Canvas::finalize),
				py::arg("key"),
				"Register the completed CompositeShape in the Atlas under key and reset."
			)

			// Accessors -------------------------------------------------------

			.def_property_readonly("layer_count", &slughorn::canvas::Canvas::layerCount,
				"Number of Layers accumulated in the current composite."
			)
			.def_property_readonly("has_pending_path", &slughorn::canvas::Canvas::hasPendingPath,
				"True if the pending path has any curves."
			)
		;
	}

	// ============================================================================================
	// Serial I/O (only present when built with SLUGHORN_SERIAL=ON)
	// ============================================================================================
#ifdef SLUGHORN_HAS_SERIAL
	m.def("read",
		[](const std::string& path) {
			// serial::read() returns Atlas by value; move into a shared_ptr so
			// Python's ref-counting and C++'s shared_ptr cooperate correctly.
			return std::make_shared<slughorn::Atlas>(slughorn::serial::read(path));
		},
		py::arg("path"),
		"Load a .slug (JSON) or .slugb (binary) atlas file.\n"
		"Format is auto-detected from the file header ('{' -> JSON, 'S' -> binary).\n"
		"Returns a fully-built Atlas - is_built is True immediately.\n"
		"Raises RuntimeError if the file cannot be opened or the format is invalid.\n"
		"Only available when slughorn was compiled with SLUGHORN_SERIAL=ON."
	);

	m.def("write",
		[](const slughorn::Atlas& atlas, const std::string& path) {
			slughorn::serial::write(atlas, path);
		},
		py::arg("atlas"), py::arg("path"),
		"Write a built Atlas to disk.\n"
		"Extension determines format: .slug -> JSON + base64, .slugb -> binary.\n"
		"Raises RuntimeError if the atlas is not built or the file cannot be written.\n"
		"Only available when slughorn was compiled with SLUGHORN_SERIAL=ON."
	);
#endif

	// ============================================================================================
	// slughorn.emoji
	// ============================================================================================
	py::module_ emoji = m.def_submodule("emoji",
		"Unicode 15.1 RGI emoji lookup table (973 single-codepoint entries).\n"
		"Names are CLDR short names, lower-case, spaces replaced with underscores."
	);

	emoji.def("name_to_codepoint",
		[](std::string_view name) -> std::optional<uint32_t> {
			return slughorn::emoji::nameToCodepoint(name);
		}, py::arg("name"),
		"Return the codepoint for a normalised CLDR short name, or None.\n"
		"Example: slughorn.emoji.name_to_codepoint('dragon') -> 0x1F409"
	);

	emoji.def("codepoint_to_name",
		[](uint32_t cp) -> std::optional<std::string> {
			auto sv = slughorn::emoji::codepointToName(cp);
			if(!sv) return std::nullopt;
			return std::string(*sv);
		}, py::arg("codepoint"),
		"Return the CLDR short name for a codepoint, or None."
	);

	emoji.def("codepoint_at_index",
		&slughorn::emoji::codepointAtIndex,
		py::arg("index"),
		"Return the codepoint at position index in the sorted table.\n"
		"Pair with table_size() to iterate the full table."
	);

	emoji.def("strip_colons",
		[](std::string_view name) -> std::string {
			return std::string(slughorn::emoji::stripColons(name));
		}, py::arg("name"),
		"Strip leading/trailing colons: ':dragon:' -> 'dragon'."
	);

	emoji.def("slack_name_to_codepoint",
		[](std::string_view name) -> std::optional<uint32_t> {
			return slughorn::emoji::slackNameToCodepoint(name);
		}, py::arg("slack_name"),
		"Strip colons then look up. ':dragon:' -> 0x1F409"
	);

	emoji.def("random_codepoint",
		py::overload_cast<>(&slughorn::emoji::randomCodepoint),
		"Return a random codepoint from the table (thread-local RNG, "
		"seeded from random_device on first call)."
	);

	emoji.def("table_size",
		&slughorn::emoji::tableSize,
		"Return the number of entries in the lookup table (973 for Unicode 15.1)."
	);

	// ============================================================================================
	// Submodule stubs - uncomment and implement as you add each backend
	// ============================================================================================

#ifdef SLUGHORN_HAS_FREETYPE
	py::module_ freetype = m.def_submodule("freetype",
		"FreeType backend - decompose TrueType/OpenType outlines and COLR emoji "
		"into Atlas shapes.\n\n"
		"High-level functions manage their own FT_Library/FT_Face lifetime; "
		"no FreeType handles are exposed to Python."
	);

	freetype.def("load_ascii_font", &slughorn::freetype::loadAsciiFont,
		py::arg("font_path"),
		py::arg("atlas"),
		"Load printable ASCII (codepoints 32-126) from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"Returns True on success, False if the font cannot be opened."
	);

	freetype.def("load_emoji_font", [](
		const std::string& fontPath,
		const std::vector<uint32_t>& codepoints,
		slughorn::Atlas& atlas
	) -> py::dict {
		std::map<uint32_t, slughorn::CompositeShape> colorGlyphs;

		slughorn::freetype::loadEmojiFont(fontPath, codepoints, atlas, colorGlyphs);

		py::dict result;

		for(auto& [cp, cs] : colorGlyphs) result[py::cast(cp)] = std::move(cs);

		return result;
	},
		py::arg("font_path"),
		py::arg("codepoints"),
		py::arg("atlas"),
		"Load COLR emoji from font_path for the given codepoints into atlas.\n"
		"codepoints is a list of uint32_t Unicode codepoints.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"Returns a dict mapping codepoint (int) -> CompositeShape "
		"for each successfully loaded glyph."
	);
#endif

	// py::module_ skia = m.def_submodule("skia",
	// "Skia path backend - decompose SkPath objects, stroke-to-fill expansion.");
	// TODO: bind slughorn::skia::decomposePath, strokeToFill, loadShape, etc.

	// py::module_ cairo = m.def_submodule("cairo",
	// "Cairo path backend - decompose cairo_t paths.");
	// TODO: bind slughorn::cairo::decomposePath, loadShape.
}
