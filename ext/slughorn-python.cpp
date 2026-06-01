//vimrun! pytest -vs ../test/slughorn-test-python.py
//
// ================================================================================================
// slughorn-python.cpp - pybind11 bindings for slughorn
//
// Covers the core slughorn.hpp API:
//
// slughorn.Color
// slughorn.Matrix
// slughorn.Key (both Codepoint and Name flavors, full __hash__/__eq__)
// slughorn.Layer (key, color, transform, effectId)
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

#include "slughorn/canvas.hpp"
#include "slughorn/render.hpp"

#define SLUGHORN_EMOJI_IMPLEMENTATION
#include "slughorn/emoji.hpp"

#ifdef SLUGHORN_HAS_SERIAL
#include "slughorn/serial.hpp"
#endif

#ifdef SLUGHORN_HAS_FREETYPE
#include "slughorn/freetype.hpp"
#endif

#ifdef SLUGHORN_HAS_NANOSVG
#include "slughorn/nanosvg.hpp"
#endif

#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sstream>

using namespace slughorn::literals;
using slughorn::slug_t;

namespace py = pybind11;

PYBIND11_MAKE_OPAQUE(std::vector<slughorn::Layer>);

// ================================================================================================
// detail - internal helpers and Python trampolines (not exposed to Python directly)
// ================================================================================================

namespace detail {

// Zero-copy memoryview over a vector<uint8_t>.
// The vector must outlive the view - caller's responsibility.
py::memoryview bytesView(const std::vector<uint8_t>& v) {
	return py::memoryview::from_memory(
		const_cast<uint8_t*>(v.data()),
		static_cast<py::ssize_t>(v.size())
	);
}

template<typename T>
py::memoryview vectorView1D(const std::vector<T>& v) {
	const auto fmt = py::format_descriptor<T>::format();
	const std::vector<py::ssize_t> shape = {static_cast<py::ssize_t>(v.size())};
	const std::vector<py::ssize_t> strides = {static_cast<py::ssize_t>(sizeof(T))};

	return py::memoryview::from_buffer(
		static_cast<const void*>(v.data()),
		sizeof(T),
		fmt.c_str(),
		shape,
		strides
	);
}

template<typename T, size_t N>
py::memoryview arrayView1D(const std::array<T, N>& v) {
	const auto fmt = py::format_descriptor<T>::format();
	const std::vector<py::ssize_t> shape = {static_cast<py::ssize_t>(N)};
	const std::vector<py::ssize_t> strides = {static_cast<py::ssize_t>(sizeof(T))};

	return py::memoryview::from_buffer(
		static_cast<const void*>(v.data()),
		sizeof(T),
		fmt.c_str(),
		shape,
		strides
	);
}

py::memoryview curveView2D(const std::vector<slughorn::Atlas::Curve>& curves) {
	const auto fmt = py::format_descriptor<slughorn::slug_t>::format();
	const std::vector<py::ssize_t> shape = {static_cast<py::ssize_t>(curves.size()), 6};
	const std::vector<py::ssize_t> strides = {
		static_cast<py::ssize_t>(sizeof(slughorn::Atlas::Curve)),
		static_cast<py::ssize_t>(sizeof(slughorn::slug_t))
	};

	return py::memoryview::from_buffer(
		static_cast<const void*>(curves.data()),
		sizeof(slughorn::slug_t),
		fmt.c_str(),
		shape,
		strides
	);
}

// Use the C++ operator<< to build a repr string for any type that has one.
template<typename T>
std::string streamRepr(const T& v, const std::string& prefix="") {
	std::ostringstream ss;

	if(!prefix.empty()) ss << prefix << ".";

	ss << v;

	return ss.str();
}

// Sample, Sampler, and decodeShape() now live in slughorn/render.hpp.

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

// Non-owning Python-facing view over a real slughorn::CurveDecomposer.
// Used for Path.decomposer() / Canvas.decomposer(), where the underlying
// C++ object already exists and should be mutated in place.
struct CurveDecomposerRef {
	slughorn::CurveDecomposer* decomposer = nullptr;

	slug_t getTolerance() const { return decomposer ? decomposer->tolerance : 0_cv; }

	void setTolerance(slug_t tolerance) {
		if(decomposer) decomposer->tolerance = tolerance;
	}
};

#ifdef SLUGHORN_HAS_FREETYPE
inline slughorn::freetype::LoadConfig makeLoadConfig(
	std::optional<slughorn::Atlas::SplitStrategy> strategy,
	bool uniform,
	std::optional<slughorn::freetype::LogCallback> log
) {
	slughorn::freetype::LoadConfig config;

	if(strategy) config.strategy = *strategy;

	config.uniform = uniform;

	if(log) config.log = *log;

	return config;
}
#endif

} // namespace detail

// Bring detail helpers into file scope so existing call sites in PYBIND11_MODULE need no change.
// makeLoadConfig is intentionally left out; call sites should say detail::makeLoadConfig explicitly.
using detail::bytesView;
using detail::vectorView1D;
using detail::arrayView1D;
using detail::curveView2D;
using detail::streamRepr;
using slughorn::render::Sample;
using slughorn::render::Sampler;
using detail::PyCurveDecomposer;
using detail::CurveDecomposerRef;

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

	// TODO: Why are these necessary!?
	py::implicitly_convertible<std::string, slughorn::Key>();
	py::implicitly_convertible<uint32_t, slughorn::Key>();

	// =========================================================================
	// slughorn.KeyIterator
	// =========================================================================
	py::class_<slughorn::KeyIterator>(m, "KeyIterator")
		.def(py::init<>(), "Numeric auto-key iterator starting at 0.")
		.def(py::init<uint32_t>(), py::arg("counter"),
			"Numeric auto-key iterator starting at counter."
		)
		.def(py::init([](std::string prefix, bool force) {
			return slughorn::KeyIterator(prefix, force);
		}), py::arg("prefix"), py::arg("force") = false,
			"String key iterator: produces prefix_0, prefix_1, ...\n"
			"If force=True, the iterator name is always used even when the source\n"
			"element (e.g. an SVG path) provides its own id attribute."
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
		.def_readwrite("force", &slughorn::KeyIterator::force,
			"When True, iterator keys override any source-provided element id."
		)
		.def("__repr__", [](const slughorn::KeyIterator& ki) { return streamRepr(ki); })
	;

	// ============================================================================================
	// slughorn.Color
	// ============================================================================================
	py::class_<slughorn::Color>(m, "Color")
		.def(py::init<>(), "Default: (0, 0, 0, 1) - opaque black.")
		.def(py::init([](slug_t r, slug_t g, slug_t b, slug_t a) {
			return slughorn::Color{r, g, b, a};
		}), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1_cv,
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

	m.attr("VERSION_MAJOR") = py::int_(SLUGHORN_VERSION_MAJOR);
	m.attr("VERSION_MINOR") = py::int_(SLUGHORN_VERSION_MINOR);
	m.attr("VERSION_PATCH") = py::int_(SLUGHORN_VERSION_PATCH);
	m.attr("version") = slughorn::versionString();

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
		.def_static("translate", &slughorn::Matrix::translate, py::arg("tx"), py::arg("ty"),
			"Return a pure-translation matrix."
		)
		.def_static("scale", &slughorn::Matrix::scale, py::arg("sx"), py::arg("sy"),
			"Return a pure-scale matrix."
		)
		.def_static("rotate", &slughorn::Matrix::rotate, py::arg("angle"),
			"Return a pure-rotation matrix (angle in radians, CCW positive)."
		)
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
	// slughorn.GradientStop / slughorn.GradientInfo
	// ============================================================================================
	py::class_<slughorn::GradientStop>(m, "GradientStop")
		.def(py::init<>(), "Default: t=0, color=(0, 0, 0, 1).")
		.def(py::init([](slug_t t, slughorn::Color color) {
			return slughorn::GradientStop{t, color};
		}), py::arg("t"), py::arg("color"),
			"Construct from position t in [0,1] and RGBA color."
		)
		.def_readwrite("t", &slughorn::GradientStop::t,
			"Position along the gradient axis [0, 1]."
		)
		.def_readwrite("color", &slughorn::GradientStop::color)
		.def("__repr__", [](const slughorn::GradientStop& s) { return streamRepr(s); })
	;

	auto gradinfo_ = py::class_<slughorn::GradientInfo>(m, "GradientInfo")
		.def(py::init<>(), "Default: linear gradient, no stops.")
		.def_readwrite("type", &slughorn::GradientInfo::type,
			"GradientInfo.Type.Linear, .Radial, or .Sweep."
		)
		.def_readwrite("stops", &slughorn::GradientInfo::stops,
			"List of GradientStop objects defining the color ramp."
		)
		.def_readwrite("transform", &slughorn::GradientInfo::transform,
			"Affine matrix mapping em-space to gradient-space. "
			"Build with slughorn.build_linear_gradient_matrix() for linear gradients."
		)
		.def_readwrite("inner_radius", &slughorn::GradientInfo::innerRadius,
			"Radial only: inner radius as a fraction of outer [0, 1]."
		)
		.def_readwrite("start_angle", &slughorn::GradientInfo::startAngle,
			"Sweep only: start angle in turns [0, 1]."
		)
		.def_readwrite("end_angle", &slughorn::GradientInfo::endAngle,
			"Sweep only: end angle in turns [0, 1]. Default = 1 (full circle)."
		)
	;

	py::enum_<slughorn::GradientInfo::Type>(gradinfo_, "Type")
		.value("Linear", slughorn::GradientInfo::Type::Linear)
		.value("Radial", slughorn::GradientInfo::Type::Radial)
		.value("Sweep", slughorn::GradientInfo::Type::Sweep)
		.export_values()
	;

	// Free function: convert two em-space endpoints to a GradientInfo::transform matrix.
	m.def("build_linear_gradient_matrix",
		&slughorn::buildLinearGradientMatrix,
		py::arg("x0"), py::arg("y0"), py::arg("x1"), py::arg("y1"),
		"Build the affine matrix for a linear gradient from em-space points (x0,y0)->(x1,y1).\n"
		"Store the result in GradientInfo.transform.\n"
		"Returns Matrix.identity() for degenerate (zero-length) inputs."
	);

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
	// slughorn.Transform
	// ============================================================================================
	py::class_<slughorn::Transform>(m, "Transform")
		.def(py::init([](slug_t x, slug_t y, slug_t z) {
			return slughorn::Transform{x, y, z};
		}), py::arg("x") = 0_cv, py::arg("y") = 0_cv, py::arg("z") = 0_cv)
		.def_readwrite("x", &slughorn::Transform::x)
		.def_readwrite("y", &slughorn::Transform::y)
		.def_readwrite("z", &slughorn::Transform::z)
		.def("__repr__", [](const slughorn::Transform& t) { return streamRepr(t); })
	;

	// ============================================================================================
	// slughorn.Layer
	//
	// key, color, transform, effectId - all four fields now present.
	// ============================================================================================
	py::class_<slughorn::Layer>(m, "Layer")
		.def(py::init<>())

		.def(
			py::init([](
				py::object key,
				slughorn::Color color,
				slughorn::Transform transform,
				slug_t scale,
				uint32_t effectId,
				uint32_t gradientId
			) {
				slughorn::Layer layer;

				if (py::isinstance<py::str>(key)) {
					layer.key = slughorn::Key(py::cast<std::string>(key));
				}
				else {
					layer.key = py::cast<slughorn::Key>(key);
				}

				layer.color = color;
				layer.transform = transform;
				layer.scale = scale;
				layer.effectId = effectId;
				layer.gradientId = gradientId;

				return layer;
			}),
			py::arg("key"),
			py::arg("color") = slughorn::Color{},
			py::arg("transform") = slughorn::Transform{},
			py::arg("scale") = 1_cv,
			py::arg("effectId") = 0,
			py::arg("gradientId") = 0
		)

		.def_readwrite("key", &slughorn::Layer::key,
			"Key identifying the shape in the Atlas.")
		.def_readwrite("color", &slughorn::Layer::color,
			"RGBA fill color for this layer.")
		.def_readwrite("transform", &slughorn::Layer::transform,
			"World-space placement. x/y position the layer; z offsets depth.")
		.def_readwrite("scale", &slughorn::Layer::scale,
			"World-scale multiplier.\n"
			"  Text / FreeType2: set to the font size in world units (e.g. 0.1 for\n"
			"    a glyph that should be 0.1 world-units tall). computeQuad() and\n"
			"    compile() both read this value.\n"
			"  SVG / Cairo / NanoSVG: leave at the default of 1.0 - curves are\n"
			"    already em-normalised by the backend.")
		.def_readwrite("effectId", &slughorn::Layer::effectId,
			"Fragment-shader fill mode selector. "
			"0 = standard Slug fill (default). "
			"See osgSlug-frag.glsl slug_ApplyEffect() for the full table.")
		.def_readwrite("gradientId", &slughorn::Layer::gradientId,
			"Gradient fill ID. 0 = flat color (layer.color used). "
			"Non-zero = 1-based index into the atlas gradient list "
			"(registered via Atlas.add_gradient()). "
			"When non-zero, layer.color.rgb is ignored; layer.color.a is a global opacity multiplier.")
		.def("__repr__", [](const slughorn::Layer& l) { return streamRepr(l); })
	;

	// ============================================================================================
	// slughorn.CompositeShape
	// ============================================================================================
	py::bind_vector<std::vector<slughorn::Layer>>(m, "Layers");

	py::class_<slughorn::CompositeShape>(m, "CompositeShape")
		.def(py::init<>())
		.def_readwrite(
			"layers",
			&slughorn::CompositeShape::layers,
			py::return_value_policy::reference_internal,
			"Ordered list of Layer objects drawn bottom-to-top."
		)
		.def_readwrite("advance", &slughorn::CompositeShape::advance,
			"Horizontal advance in em-space (used for text cursor / layout)."
		)
		.def("__len__", [](const slughorn::CompositeShape& g) { return g.layers.size(); })
		.def("__repr__", [](const slughorn::CompositeShape& g) { return streamRepr(g); })
	;

	// ============================================================================================
	// slughorn.FontMetrics
	// ============================================================================================

	py::class_<slughorn::FontMetrics>(m, "FontMetrics",
		"Dimensionless em-space ratios for a typeface.\n\n"
		"All ratio fields are fractions of the em-square in [0, 1]. Multiply by\n"
		"fontSize to get world-space distances. Produced by\n"
		"slughorn.freetype.load_font_metrics(); consumed by Canvas.text()."
	)
		.def(py::init<>())
		.def_readwrite("units_per_em", &slughorn::FontMetrics::unitsPerEM,
			"Raw em units (e.g. 1000 or 2048); not a ratio."
		)
		.def_readwrite("cap_height_ratio", &slughorn::FontMetrics::capHeightRatio,
			"OS/2 sCapHeight / unitsPerEM (~0.72 for Latin)."
		)
		.def_readwrite("x_height_ratio", &slughorn::FontMetrics::xHeightRatio,
			"OS/2 sxHeight / unitsPerEM (~0.53)."
		)
		.def_readwrite("ascender_ratio", &slughorn::FontMetrics::ascenderRatio,
			"ascender / unitsPerEM (~0.80)."
		)
		.def_readwrite("descender_ratio", &slughorn::FontMetrics::descenderRatio,
			"|descender| / unitsPerEM (~0.20)."
		)
		.def_readwrite("line_gap_ratio", &slughorn::FontMetrics::lineGapRatio,
			"Recommended line gap / unitsPerEM (0 if none).\n"
			"lineHeight = fontSize * (1 + line_gap_ratio)"
		)
		.def("__repr__", [](const slughorn::FontMetrics& fm) { return streamRepr(fm); })
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
		.def("__repr__", [](const slughorn::Atlas::Curve& c) { return streamRepr(c); })
	;

	// ============================================================================================
	// slughorn.ShapeInfo (Atlas::ShapeInfo in C++, flat in Python)
	// ============================================================================================
	auto shapeinfo_ = py::class_<slughorn::Atlas::ShapeInfo>(m, "ShapeInfo")
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
		.def_readwrite("splits_y", &slughorn::Atlas::ShapeInfo::splitsY,
			"Optional list of interior Y split positions as normalized [0, 1] fractions of the "
			"shape's Y range (sorted ascending). When non-empty, overrides num_bands_y. "
			"Use Atlas.compute_adaptive_splits() / Atlas.compute_uniform_splits(), or set manually."
		)
		.def_readwrite("splits_x", &slughorn::Atlas::ShapeInfo::splitsX,
			"Optional list of interior X split positions as normalized [0, 1] fractions of the "
			"shape's X range (sorted ascending). When non-empty, overrides num_bands_x. "
			"Use Atlas.compute_adaptive_splits() / Atlas.compute_uniform_splits(), or set manually."
		)
		.def_readwrite("origin", &slughorn::Atlas::ShapeInfo::origin,
			"Where the transform origin (Layer.transform.x/y) is placed relative to the geometry.\n"
			"Origin() = Default, Origin(Type) = type-only (e.g. Centered), Origin(x, y) = Pivot, Origin(Type, x, y) = explicit type + coords."
		)
		.def("__repr__", [](const slughorn::Atlas::ShapeInfo& info) { return streamRepr(info); })
	;

	auto origin_ = py::class_<slughorn::Atlas::ShapeInfo::Origin>(shapeinfo_, "Origin")
		.def(py::init<>(),
			"Default origin: Layer.transform.x/y = bbox corner (existing behavior)."
		)
		.def(py::init<slughorn::Atlas::ShapeInfo::Origin::Type>(),
			py::arg("type"),
			"Type-only origin: pass Origin.Type.Centered (or any future named variant)."
		)
		.def(py::init<slughorn::slug_t, slughorn::slug_t>(),
			py::arg("x"), py::arg("y"),
			"Pivot origin: authoring-space pivot (bbox-min subtracted). "
			"Layer.transform.x/y will equal (x, y) scaled to local em-space."
		)
		.def(py::init<slughorn::Atlas::ShapeInfo::Origin::Type, slughorn::slug_t, slughorn::slug_t>(),
			py::arg("type"), py::arg("x"), py::arg("y"),
			"Explicit type + coords. Use Origin.Type.Custom to store (x, y) * scale verbatim "
			"with no em-space adjustment (e.g. raw ejection direction vectors)."
		)
		.def_readwrite("type", &slughorn::Atlas::ShapeInfo::Origin::type)
		.def_readwrite("x", &slughorn::Atlas::ShapeInfo::Origin::x)
		.def_readwrite("y", &slughorn::Atlas::ShapeInfo::Origin::y)
		.def("__eq__", &slughorn::Atlas::ShapeInfo::Origin::operator==)
		.def("__ne__", &slughorn::Atlas::ShapeInfo::Origin::operator!=)
		.def("__repr__", [](const slughorn::Atlas::ShapeInfo::Origin& origin) {
			return streamRepr(origin, "ShapeInfo");
		})
	;

	py::enum_<slughorn::Atlas::ShapeInfo::Origin::Type>(origin_, "Type")
		.value("Default", slughorn::Atlas::ShapeInfo::Origin::Type::Default)
		.value("Centered", slughorn::Atlas::ShapeInfo::Origin::Type::Centered)
		.value("Pivot", slughorn::Atlas::ShapeInfo::Origin::Type::Pivot)
		.value("Custom", slughorn::Atlas::ShapeInfo::Origin::Type::Custom)
		.export_values()
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
		.def_readonly("origin_x", &slughorn::Atlas::Shape::originX,
			"Em-space X offset of the transform origin. "
			"0 = bottom-left corner (Origin.Default), width/2 = center (Origin.Centered)."
		)
		.def_readonly("origin_y", &slughorn::Atlas::Shape::originY,
			"Em-space Y offset of the transform origin. "
			"0 = bottom-left corner (Origin.Default), height/2 = center (Origin.Centered)."
		)
		.def_readonly("origin", &slughorn::Atlas::Shape::origin,
			"The ShapeInfo::Origin spec supplied at build time. "
			"Preserved post-build for diagnostics and computeQuad branching."
		)

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
			// emSize = INDIRECTION_SIZE / bandScale (mirrors slug_EmToUV's emSize)
			float sx = (s.bandScaleX != 0.f) ? float(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleX : 0.f;
			float sy = (s.bandScaleY != 0.f) ? float(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleY : 0.f;
			return py::make_tuple(sx, sy);
		}, "Em-space (width, height) of the shape's bounding box. "
			"Mirrors slug_EmToUV's emSize computation."
		)
		.def("em_to_uv", [](const slughorn::Atlas::Shape& s, slug_t ex, slug_t ey) {
			// Direct Python port of slug_EmToUV()
			float ox = (s.bandScaleX != 0.f) ? -s.bandOffsetX / s.bandScaleX : 0.f;
			float oy = (s.bandScaleY != 0.f) ? -s.bandOffsetY / s.bandScaleY : 0.f;
			float sx = (s.bandScaleX != 0.f) ? float(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleX : 1.f;
			float sy = (s.bandScaleY != 0.f) ? float(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleY : 1.f;

			return py::make_tuple((ex - ox) / sx, (ey - oy) / sy);
		}, py::arg("em_x"), py::arg("em_y"),
			"Convert an em-space coordinate to a normalized [0, 1] UV. "
			"Python port of the GLSL slug_EmToUV() helper. "
			"(0,0) = bottom-left of bounding box, (1,1) = top-right.")
		.def("compute_quad", &slughorn::Atlas::Shape::computeQuad,
			py::arg("transform"),
			py::arg("scale") = 1_cv,
			py::arg("expand") = 0_cv,
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
		.def_property_readonly("format", [](const slughorn::Atlas::TextureData& td) -> const char* {
			switch(td.format) {
				case slughorn::Atlas::TextureData::Format::RGBA32F: return "RGBA32F";
				case slughorn::Atlas::TextureData::Format::RGBA16UI: return "RGBA16UI";
				case slughorn::Atlas::TextureData::Format::RGBA8: return "RGBA8";
			}
			return "unknown";
		}, "String: 'RGBA32F' (curve texture), 'RGBA16UI' (band texture), or 'RGBA8' (gradient texture).")
		.def_property_readonly("bytes", [](const slughorn::Atlas::TextureData& td) {
			return bytesView(td.bytes);
		}, "Zero-copy memoryview of the raw pixel data (row-major). "
			"Keep the Atlas alive for the duration of any view."
		)
		.def("__repr__", [](const slughorn::Atlas::TextureData& td) { return streamRepr(td); })
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
		.def_readonly("gradient_count", &slughorn::Atlas::PackingStats::gradientCount,
			"Number of registered gradients (0 when none).")
		.def_readonly("gradient_texels_total", &slughorn::Atlas::PackingStats::gradientTexelsTotal,
			"Total gradient texture texels (GRADIENT_STRIP_WIDTH * gradient_count).")
		.def("curve_utilization", &slughorn::Atlas::PackingStats::curveUtilization)
		.def("band_utilization", &slughorn::Atlas::PackingStats::bandUtilization)
		.def("curve_padding_ratio", &slughorn::Atlas::PackingStats::curvePaddingRatio)
		.def("band_padding_ratio", &slughorn::Atlas::PackingStats::bandPaddingRatio)
		.def("__repr__", [](const slughorn::Atlas::PackingStats& p) { return streamRepr(p); })
	;

	// ============================================================================================
	// slughorn.Atlas
	// ============================================================================================
	// py::class_<slughorn::Atlas, std::shared_ptr<slughorn::Atlas>>(m, "Atlas")
	py::class_<slughorn::Atlas>(m, "Atlas")
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

		.def("normalize_shape_metrics",
			&slughorn::Atlas::normalizeShapeMetrics,
			py::arg("keys"),
			"Force all shapes in keys to share the same em-space bounding box.\n\n"
			"Must be called after add_shape() but before build(). Keys not present\n"
			"in the atlas or shapes with no curves are silently skipped.\n\n"
			"When all shapes share the same advance (tabular/monospaced), the cell\n"
			"width equals that advance. Otherwise the cell is the union bbox.\n\n"
			"Only layout fields (bearing, width, height, advance) are updated.\n"
			"Per-shape band transforms are left intact — required for\n"
			"setLayerShapeIndex cycling across shapes from different backends."
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

		.def("get_shape_curves",
			[](const slughorn::Atlas& a, slughorn::Key key) -> py::object {
				const auto* curves = a.getShapeCurves(key);

				if(!curves) return py::none();

				py::list result;

				for(const auto& c : *curves)
					result.append(py::make_tuple(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3));

				return result;
			},
			py::arg("key"),
			"Return flat list of (x1,y1,x2,y2,x3,y3) curve tuples, or None if not found.\n"
			"Works at any build lifecycle stage. Use when contour topology does not matter\n"
			"(e.g. feeding a pixel rasterizer or the SDF pipeline)."
		)

		.def("get_shape_contours",
			[](const slughorn::Atlas& a, slughorn::Key key) {
				py::list result;

				for(const auto& contour : a.getShapeContours(key)) {
					py::list cl;

					for(const auto& c : contour)
						cl.append(py::make_tuple(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3));

					result.append(cl);
				}

				return result;
			},
			py::arg("key"),
			"Return list of contours; each contour is a list of (x1,y1,x2,y2,x3,y3) tuples.\n"
			"Contour breaks are detected where p3 of curve[i] != p1 of curve[i+1].\n"
			"Returns an empty list if the key is not found. Use when building paths for\n"
			"stroking or filling — each contour must be handled independently."
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

		.def_property_readonly("gradient_texture",
			[](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
				return a.getGradientTextureData();
			},
			py::return_value_policy::reference_internal,
			"TextureData for the RGBA8 gradient color-strip texture (valid after build()). "
			"Empty (width=height=0) when no gradients are registered."
		)

		.def("add_gradient",
			&slughorn::Atlas::addGradient,
			py::arg("info"),
			"Register a gradient. Returns a 1-based ID (0 = error / atlas already built).\n"
			"Store the ID in Layer.gradientId to activate the gradient for that layer.\n"
			"Must be called before build(). Gradients are rasterized during build()."
		)

		.def("get_gradients",
			[](const slughorn::Atlas& a) {
				return a.getGradients();
			},
			"Return a copy of the registered GradientInfo list (valid after build())."
		)

		.def("decode",
			[](const slughorn::Atlas& a, slughorn::Key key) {
				return slughorn::render::decode(a, key);
			},
			py::arg("key"),
			"Decode a built shape into a Python-facing software-render view.\n"
			"Returns a slughorn.render.Sampler."
		)

		.def_static("compute_adaptive_splits",
			[](const slughorn::Atlas::Curves& curves, int num_bands_x, int num_bands_y)
				-> py::tuple
			{
				auto [sx, sy] = slughorn::Atlas::computeAdaptiveSplits(
					curves, num_bands_x, num_bands_y
				);

				return py::make_tuple(sx, sy);
			},
			py::arg("curves"), py::arg("num_bands_x"), py::arg("num_bands_y"),
			"Sweep-line valley placement: places band boundaries where fewest curves cross,\n"
			"minimizing per-fragment shader iterations.\n\n"
			"Returns (splits_x, splits_y): normalized [0, 1] fraction lists to assign to\n"
			"ShapeInfo.splits_x / splits_y.\n\n"
			"Example::\n\n"
			"    splits_x, splits_y = slughorn.Atlas.compute_adaptive_splits(curves, num_bands_x=8, num_bands_y=8)\n"
			"    info.splits_x = splits_x\n"
			"    info.splits_y = splits_y"
		)

		.def_static("compute_uniform_splits",
			[](const slughorn::Atlas::Curves& curves, int num_bands_x, int num_bands_y)
				-> py::tuple
			{
				auto [sx, sy] = slughorn::Atlas::computeUniformSplits(
					curves, num_bands_x, num_bands_y
				);

				return py::make_tuple(sx, sy);
			},
			py::arg("curves"), py::arg("num_bands_x"), py::arg("num_bands_y"),
			"Uniform placement: evenly-spaced fractions (i+1)/num_bands.\n"
			"Equivalent to the implicit uniform fallback, but returned as an explicit vector\n"
			"for inspection or manual adjustment.\n\n"
			"Returns (splits_x, splits_y): normalized [0, 1] fraction lists.\n\n"
			"Example::\n\n"
			"    splits_x, splits_y = slughorn.Atlas.compute_uniform_splits(curves, num_bands_x=8, num_bands_y=8)\n"
			"    info.splits_x = splits_x\n"
			"    info.splits_y = splits_y"
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

	py::class_<CurveDecomposerRef>(m, "_CurveDecomposerRef",
		"Non-owning view over an internal CurveDecomposer.\n\n"
		"Returned by canvas.Path.decomposer() and canvas.Canvas.decomposer() to expose\n"
		"the underlying tolerance control without copying the decomposer state.")
		.def_property("tolerance", &CurveDecomposerRef::getTolerance, &CurveDecomposerRef::setTolerance,
			"Flatness threshold for cubic decomposition in curve-space units."
		)
	;

	// =========================================================================
	// slughorn.render submodule
	// =========================================================================
	{
		py::module_ render = m.def_submodule("render",
			"Software decode and rendering helpers built on top of a compiled slughorn.Atlas.\n\n"
			"Provides a decoded per-shape view plus native reference and banded sample paths."
		);

		py::class_<Sample>(render, "Sample")
			.def_readonly("fill", &Sample::fill)
			.def_readonly("xcov", &Sample::xcov)
			.def_readonly("ycov", &Sample::ycov)
			.def_readonly("xwgt", &Sample::xwgt)
			.def_readonly("ywgt", &Sample::ywgt)
			.def_readonly("iters", &Sample::iters)
			.def("__repr__", [](const Sample& r) {
				std::ostringstream ss;

				ss
					<< "Sample(fill=" << r.fill
					<< ", xcov=" << r.xcov
					<< ", ycov=" << r.ycov
					<< ", xwgt=" << r.xwgt
					<< ", ywgt=" << r.ywgt
					<< ", iters=" << r.iters << ")"
				;

				return ss.str();
			})
		;

		py::class_<Sampler>(render, "Sampler")
			.def_property_readonly("shape", [](const Sampler& d) { return d.shape; })
			.def_property_readonly("curves", [](const Sampler& d) { return d.curves; })
			.def_property_readonly("curve_buffer", [](const Sampler& d) {
				return curveView2D(d.curves);
			}, "2-D float32 memoryview of decoded curves with shape (num_curves, 6).")
			.def_property_readonly("hband_offsets", [](const Sampler& d) {
				return vectorView1D(d.hbandOffsets);
			}, "CSR offsets for horizontal bands.")
			.def_property_readonly("hband_indices", [](const Sampler& d) {
				return vectorView1D(d.hbandIndices);
			}, "CSR payload for horizontal bands.")
			.def_property_readonly("vband_offsets", [](const Sampler& d) {
				return vectorView1D(d.vbandOffsets);
			}, "CSR offsets for vertical bands.")
			.def_property_readonly("vband_indices", [](const Sampler& d) {
				return vectorView1D(d.vbandIndices);
			}, "CSR payload for vertical bands.")
			.def_property_readonly("indir_y", [](const Sampler& d) {
				return arrayView1D(d.indirY);
			}, "Band indirection table for Y, length INDIRECTION_SIZE.")
			.def_property_readonly("indir_x", [](const Sampler& d) {
				return arrayView1D(d.indirX);
			}, "Band indirection table for X, length INDIRECTION_SIZE.")
			.def("get_hband", [](const Sampler& d, uint32_t i) {
				if(i + 1 >= d.hbandOffsets.size()) throw py::index_error("horizontal band out of range");

				py::list out;

				for(uint32_t j = d.hbandOffsets[i]; j < d.hbandOffsets[i + 1]; j++)
					out.append(d.hbandIndices[j]);

				return out;
			}, py::arg("index"))
			.def("get_vband", [](const Sampler& d, uint32_t i) {
				if(i + 1 >= d.vbandOffsets.size()) throw py::index_error("vertical band out of range");

				py::list out;

				for(uint32_t j = d.vbandOffsets[i]; j < d.vbandOffsets[i + 1]; j++)
					out.append(d.vbandIndices[j]);

				return out;
			}, py::arg("index"))
			.def("render_sample",
				[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
					return d.renderSample(x, y, ppeX, ppeY);
				},
				py::arg("x"), py::arg("y"), py::arg("ppe_x"), py::arg("ppe_y"),
				"Reference software sample using all decoded curves."
			)
			.def("render_sample_banded",
				[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
					return d.renderSampleBanded(x, y, ppeX, ppeY);
				},
				py::arg("x"), py::arg("y"), py::arg("ppe_x"), py::arg("ppe_y"),
				"Band-accelerated software sample mirroring the GPU shader path."
			)
			.def("render_grid",
				[](const Sampler& d, uint32_t size, slug_t margin, bool banded) {
					const auto grid = d.renderGrid(size, margin, banded);
					py::array_t<slug_t> arr({
						static_cast<py::ssize_t>(grid.height),
						static_cast<py::ssize_t>(grid.width)
					});
					auto buf = arr.mutable_unchecked<2>();
					for(uint32_t j = 0; j < grid.height; j++)
						for(uint32_t i = 0; i < grid.width; i++)
							buf(j, i) = grid.at(j, i);
					return arr;
				},
				py::arg("size") = 128,
				py::arg("margin") = 0_cv,
				py::arg("banded") = true,
				"Render a full grayscale coverage grid as a float32 NumPy-compatible array."
			)
			.def("__repr__", [](const Sampler& d) {
				return "Sampler(curves=" + std::to_string(d.curves.size()) +
					", hbands=" + std::to_string(
						d.hbandOffsets.empty() ? 0 : d.hbandOffsets.size() - 1
					) +
					", vbands=" + std::to_string(
						d.vbandOffsets.empty() ? 0 : d.vbandOffsets.size() - 1
					) + ")";
			})
		;

		render.def("decode",
			[](const slughorn::Atlas& atlas, slughorn::Key key) {
				return slughorn::render::decode(atlas, key);
			},
			py::arg("atlas"), py::arg("key"),
			"Decode a built atlas shape into a slughorn.render.Sampler."
		);
	}

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

		py::enum_<slughorn::canvas::TextAnchorY>(canvas, "TextAnchorY",
			"Vertical anchor for Canvas.text()."
		)
			.value("BASELINE", slughorn::canvas::TextAnchorY::Baseline,
				"y is the text baseline (default)."
			)
			.value("CAP_CENTER", slughorn::canvas::TextAnchorY::CapCenter,
				"y is the vertical center of the cap-height band."
			)
			.value("CAP_TOP", slughorn::canvas::TextAnchorY::CapTop,
				"y is the top of capital letters."
			)
			.value("X_CENTER", slughorn::canvas::TextAnchorY::XCenter,
				"y is the vertical center of the x-height band."
			)
		;

		py::enum_<slughorn::canvas::TextAlignX>(canvas, "TextAlignX",
			"Horizontal alignment for Canvas.text()."
		)
			.value("LEFT", slughorn::canvas::TextAlignX::Left,
				"x is the left edge of the first glyph (default, single-pass)."
			)
			.value("CENTER", slughorn::canvas::TextAlignX::Center,
				"x is the horizontal center of the run (two-pass)."
			)
			.value("RIGHT", slughorn::canvas::TextAlignX::Right,
				"x is the right edge of the last glyph (two-pass)."
			)
		;

		py::class_<slughorn::canvas::Canvas::GradientHandle>(canvas, "GradientHandle",
			"Lightweight gradient descriptor returned by Canvas.create_linear_gradient().\n"
			"Pass it to Canvas.fill_gradient() to commit the current path with a gradient fill.\n"
			"The endpoints are in the same authoring space as the path coordinates.")
			.def_readwrite("x0", &slughorn::canvas::Canvas::GradientHandle::x0)
			.def_readwrite("y0", &slughorn::canvas::Canvas::GradientHandle::y0)
			.def_readwrite("x1", &slughorn::canvas::Canvas::GradientHandle::x1)
			.def_readwrite("y1", &slughorn::canvas::Canvas::GradientHandle::y1)
			.def_readwrite("stops", &slughorn::canvas::Canvas::GradientHandle::stops,
				"List of GradientStop objects."
			)
			.def("__repr__", [](const slughorn::canvas::Canvas::GradientHandle& h) {
				return streamRepr(h, "Canvas");
			})
		;

		// Path class ----------------------------------------------------------

		{
			using Path = slughorn::canvas::Path;
			using PathSample = slughorn::canvas::Path::Sample;

			auto path = py::class_<Path>(canvas, "Path",
				"Standalone geometry primitive (analogous to HTML Canvas Path2D).\n\n"
				"Build geometry with move_to/line_to/bezier_to/etc, then pass the Path\n"
				"to canvas.fill(path, color) or canvas.stroke(path, width, color).\n"
				"Paths are copyable and reusable: fill/stroke do not consume them.\n\n"
				"A Path can also be used standalone for arc-length sampling:\n"
				"    p = slughorn.canvas.Path()\n"
				"    p.move_to(0, 0); p.line_to(1, 0)\n"
				"    s = p.sample(0.5) # Path.Sample at midpoint"
			);

			py::class_<PathSample>(path, "Sample",
				"Position and tangent direction at a normalized arc-length parameter t.\n"
				"Returned by Path.sample(t). Fields are read-only."
			)
				.def_readonly("x", &PathSample::x, "X coordinate.")
				.def_readonly("y", &PathSample::y, "Y coordinate.")
				.def_readonly("angle", &PathSample::angle, "Tangent angle in radians.")
				.def("__repr__", [](const PathSample& s) { return streamRepr(s, "Path"); })
			;

			path
				.def(py::init<>(), "Create an empty path with identity transform.")

				// Path management
				.def("clear", &Path::clear,
					"Reset all geometry state. The CTM (transform) is NOT cleared,\n"
					"matching HTML Canvas beginPath() semantics.")
				.def("add_path",
					py::overload_cast<const Path&>(&Path::addPath),
					py::arg("other"),
					"Append all curves from other into this path's accumulator. Does not affect other."
				)
				.def("add_path",
					py::overload_cast<const Path&, const slughorn::Matrix&>(&Path::addPath),
					py::arg("other"), py::arg("transform"),
					"Append curves from other with each control point transformed by transform.\n"
					"Matches HTML Canvas Path2D.addPath(path, DOMMatrix) semantics."
				)

				// Transform stack
				.def("save", &Path::save, "Push the current transform onto the stack.")
				.def("restore", &Path::restore, "Pop the transform stack.")
				.def("reset_transform", &Path::resetTransform, "Set CTM to identity.")
				.def("set_transform", &Path::setTransform, py::arg("m"),
					"Replace CTM with matrix m.")
				.def("transform", py::overload_cast<const slughorn::Matrix&>(&Path::transform),
					py::arg("m"), "Post-multiply CTM by m.")
				.def("translate", &Path::translate, py::arg("tx"), py::arg("ty"))
				.def("rotate", &Path::rotate, py::arg("angle"), "Angle in radians.")
				.def("scale", &Path::scale, py::arg("sx"), py::arg("sy"))

				// Path commands
				.def("move_to", &Path::moveTo, py::arg("x"), py::arg("y"))
				.def("line_to", &Path::lineTo, py::arg("x"), py::arg("y"))
				.def("quad_to", &Path::quadTo,
					py::arg("cx"), py::arg("cy"), py::arg("x"), py::arg("y"))
				.def("bezier_to", &Path::bezierTo,
					py::arg("c1x"), py::arg("c1y"), py::arg("c2x"), py::arg("c2y"),
					py::arg("x"), py::arg("y"))
				.def("close_path", &Path::closePath)

				// Shape helpers
				.def("rect", &Path::rect,
					py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"))
				.def("rounded_rect", &Path::roundedRect,
					py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"), py::arg("r"))
				.def("circle", &Path::circle, py::arg("cx"), py::arg("cy"), py::arg("r"))
				.def("ellipse", &Path::ellipse,
					py::arg("cx"), py::arg("cy"), py::arg("rx"), py::arg("ry"))
				.def("arc", &Path::arc,
					py::arg("cx"), py::arg("cy"), py::arg("r"),
					py::arg("start_angle"), py::arg("end_angle"), py::arg("ccw") = false,
					"Circular arc. Does NOT call clear(); appends to the current path.\n"
					"Angles in radians from +X axis, Y-up convention.")
				.def("arc_to", &Path::arcTo,
					py::arg("x1"), py::arg("y1"), py::arg("x2"), py::arg("y2"), py::arg("r"),
					"Tangential arc. Matches HTML Canvas arcTo().")

				// Stroke expansion
				.def("stroke_path", &Path::strokePath,
					py::arg("width"), py::arg("cw") = false,
					"Expand from centerline to constant-width stroke outline in place.\n"
					"cw=True reverses the winding (CW, for punch-out effects with nonzero rule).\n"
					"Returns False if the path was empty.")

				// Decomposer
				.def("decomposer",
					[](Path& p) {
						return CurveDecomposerRef{&p.decomposer()};
					},
					"Access the internal CurveDecomposer to tune tolerance.")

				// Accessors
				.def_property_readonly("has_pending_path", &Path::hasPendingPath,
					"True if the path has any accumulated curves.")
				.def("arc_length", &Path::arcLength,
					"Total arc length of the path. Triggers LUT rebuild if geometry changed.")
				.def("sample", &Path::sample, py::arg("t"),
					"Sample position and tangent at normalized arc-length t in [0,1].\n"
					"Returns a Path.Sample(x, y, angle).")
				.def("__repr__", [](const Path& p) { return streamRepr(p); })
			;
		}

		// Canvas class --------------------------------------------------------

		py::class_<slughorn::canvas::Canvas>(canvas, "Canvas")
			.def(py::init<slughorn::Atlas&, slughorn::KeyIterator>(),
				py::arg("atlas"), py::arg("key_iterator")=slughorn::KeyIterator(),
				"Construct a Canvas writing into atlas, using key_iterator for auto-generated keys."
			)

			// CurveDecomposer / path snapshot ---------------------------------

			.def("decomposer",
				[](slughorn::canvas::Canvas& c) {
					return CurveDecomposerRef{&c.decomposer()};
				},
				"Access the internal CurveDecomposer to tune tolerance etc."
			)
			.def("path", &slughorn::canvas::Canvas::path,
				"Return a copy of the internal path. Non-destructive: the canvas path is intact."
			)

			// Transform stack -------------------------------------------------

			.def("save", &slughorn::canvas::Canvas::save, "Push the current transform.")
			.def("restore", &slughorn::canvas::Canvas::restore, "Pop the transform stack.")
			.def("reset_transform", &slughorn::canvas::Canvas::resetTransform)
			.def("set_transform", &slughorn::canvas::Canvas::setTransform, py::arg("m"))
			.def("transform",
				py::overload_cast<const slughorn::Matrix&>(&slughorn::canvas::Canvas::transform),
				py::arg("m"))
			.def("translate", &slughorn::canvas::Canvas::translate, py::arg("tx"), py::arg("ty"))
			.def("rotate", &slughorn::canvas::Canvas::rotate, py::arg("angle"))
			.def("scale", &slughorn::canvas::Canvas::scale, py::arg("sx"), py::arg("sy"))

			// Path commands ---------------------------------------------------

			.def("begin_path", &slughorn::canvas::Canvas::beginPath,
				"Discard any accumulated path state and start fresh."
			)
			.def("add_path",
				py::overload_cast<const slughorn::canvas::Path&>(&slughorn::canvas::Canvas::addPath),
				py::arg("other"),
				"Append all curves from an explicit Path into the canvas's internal path."
			)
			.def("add_path",
				py::overload_cast<const slughorn::canvas::Path&, const slughorn::Matrix&>(&slughorn::canvas::Canvas::addPath),
				py::arg("other"), py::arg("transform"),
				"Append curves from an explicit Path with each control point transformed by transform.\n"
				"Matches HTML Canvas Path2D.addPath(path, DOMMatrix) semantics."
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

			// Commit / implicit path ------------------------------------------

			// fill() - auto-key
			.def("fill",
				[](
					slughorn::canvas::Canvas& c,
					slughorn::Color color,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fill(color, scale, origin);
				},
				py::arg("color"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Commit the current path as a new Layer with the given color.\n"
				"Returns the Layer (use .key to access the auto-generated Key), or an empty Layer if the path was empty."
			)
			// fill() - named-key
			.def("fill",
				[](
					slughorn::canvas::Canvas& c,
					slughorn::Color color,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fill(color, scale, key, origin);
				},
				py::arg("color"), py::arg("scale"), py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Commit the current path as a new Layer, registering the Shape under key.\n"
				"Returns the Layer (use .key to access the registered Key), or an empty Layer if the path was empty."
			)
			.def("define_shape",
				[](
					slughorn::canvas::Canvas& c,
					slughorn::Key key,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.defineShape(key, scale, origin);
				},
				py::arg("key"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Register the current path as a named Shape (geometry only, no Layer).\n"
				"Returns False if the path was empty."
			)
			// stroke_path() / in-place expand, then commit separately
			.def("stroke_path",
				[](slughorn::canvas::Canvas& c, slug_t width, bool cw) {
					return c.strokePath(width, cw);
				},
				py::arg("width"), py::arg("cw") = false,
				"Expand the current path from a centerline into a stroke outline in place.\n"
				"cw=True reverses the winding (CW, for punch-out effects).\n"
				"Call fill() or stroke() afterwards to commit."
			)
			// stroke() - auto-key
			.def("stroke",
				[](
					slughorn::canvas::Canvas& c,
					slug_t width,
					slughorn::Color color,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.stroke(width, color, scale, origin);
				},
				py::arg("width"), py::arg("color"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Expand the current path as a stroke outline and commit as a colored Layer."
			)
			// stroke() - named-key
			.def("stroke",
				[](
					slughorn::canvas::Canvas& c,
					slug_t width,
					slughorn::Color color,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.stroke(width, color, scale, key, origin);
				},
				py::arg("width"), py::arg("color"), py::arg("scale"), py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Expand the current path as a stroke outline, registering under key."
			)

			// Commit / explicit Path ------------------------------------------

			// fill(path, color, ...) - auto-key
			.def("fill",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					slughorn::Color color,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fill(p, color, scale, origin);
				},
				py::arg("path"), py::arg("color"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Fill a standalone Path. path is not consumed or modified."
			)
			// fill(path, color, scale, key) - named-key
			.def("fill",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					slughorn::Color color,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fill(p, color, scale, key, origin);
				},
				py::arg("path"), py::arg("color"), py::arg("scale"), py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Fill a standalone Path, registering under key. path is not consumed."
			)
			.def("define_shape",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					slughorn::Key key,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.defineShape(p, key, scale, origin);
				},
				py::arg("path"), py::arg("key"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Register a standalone Path as a named Shape (geometry only, no Layer)."
			)
			// stroke(path, ...) - auto-key
			.def("stroke",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					slug_t width,
					slughorn::Color color,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.stroke(p, width, color, scale, origin);
				},
				py::arg("path"), py::arg("width"), py::arg("color"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Stroke a standalone Path. path is not consumed or modified."
			)
			// stroke(path, ...) - named-key
			.def("stroke",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					slug_t width,
					slughorn::Color color,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.stroke(p, width, color, scale, key, origin);
				},
				py::arg("path"), py::arg("width"), py::arg("color"),
				py::arg("scale"), py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Stroke a standalone Path, registering under key."
			)
			.def("fill_gradient",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Path& p,
					const slughorn::canvas::Canvas::GradientHandle& handle,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fillGradient(p, handle, scale, origin);
				},
				py::arg("path"), py::arg("handle"), py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Gradient-fill a standalone Path. path is not consumed."
			)

			// Gradient fills / implicit path ----------------------------------

			.def("create_linear_gradient",
				[](
					slughorn::canvas::Canvas& c,
					slug_t x0, slug_t y0,
					slug_t x1, slug_t y1,
					std::vector<slughorn::GradientStop> stops
				) {
					return c.createLinearGradient(x0, y0, x1, y1, std::move(stops));
				},
				py::arg("x0"), py::arg("y0"),
				py::arg("x1"), py::arg("y1"),
				py::arg("stops"),
				"Create a GradientHandle for a linear gradient from (x0,y0) to (x1,y1)."
			)
			.def("create_radial_gradient",
				[](
					slughorn::canvas::Canvas& c,
					slug_t cx, slug_t cy,
					slug_t r0, slug_t r1,
					std::vector<slughorn::GradientStop> stops
				) {
					return c.createRadialGradient(cx, cy, r0, r1, std::move(stops));
				},
				py::arg("cx"), py::arg("cy"),
				py::arg("r0"), py::arg("r1"),
				py::arg("stops"),
				"Create a GradientHandle for a radial gradient centered at (cx,cy).\n"
				"r0 is the inner radius, r1 is the outer radius."
			)
			.def("create_sweep_gradient",
				[](
					slughorn::canvas::Canvas& c,
					slug_t cx, slug_t cy,
					slug_t start_angle, slug_t end_angle,
					std::vector<slughorn::GradientStop> stops
				) {
					return c.createSweepGradient(cx, cy, start_angle, end_angle, std::move(stops));
				},
				py::arg("cx"), py::arg("cy"),
				py::arg("start_angle"), py::arg("end_angle"),
				py::arg("stops"),
				"Create a GradientHandle for a sweep (conic) gradient centered at (cx,cy).\n"
				"Angles in radians."
			)

			// fill_gradient() - auto-key
			.def("fill_gradient",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Canvas::GradientHandle& handle,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fillGradient(handle, scale, origin);
				},
				py::arg("handle"),
				py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Commit the current path as a gradient-filled Layer."
			)
			// fill_gradient() - named-key
			.def("fill_gradient",
				[](
					slughorn::canvas::Canvas& c,
					const slughorn::canvas::Canvas::GradientHandle& handle,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.fillGradient(handle, scale, key, origin);
				},
				py::arg("handle"),
				py::arg("scale"),
				py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Commit the current path as a gradient-filled Layer, registering under key."
			)
			// stroke_gradient() - auto-key
			.def("stroke_gradient",
				[](
					slughorn::canvas::Canvas& c,
					slug_t width,
					const slughorn::canvas::Canvas::GradientHandle& handle,
					slug_t scale,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.strokeGradient(width, handle, scale, origin);
				},
				py::arg("width"), py::arg("handle"),
				py::arg("scale") = 1_cv,
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Expand the current path as a stroke outline and commit with a gradient fill."
			)
			// stroke_gradient() - named-key
			.def("stroke_gradient",
				[](
					slughorn::canvas::Canvas& c,
					slug_t width,
					const slughorn::canvas::Canvas::GradientHandle& handle,
					slug_t scale,
					slughorn::Key key,
					slughorn::Atlas::ShapeInfo::Origin origin
				) {
					return c.strokeGradient(width, handle, scale, key, origin);
				},
				py::arg("width"), py::arg("handle"),
				py::arg("scale"), py::arg("key"),
				py::arg("origin") = slughorn::Atlas::ShapeInfo::Origin{},
				"Expand the current path as a stroke outline with a gradient, registering under key."
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
			.def("text",
				&slughorn::canvas::Canvas::text,
				py::arg("s"),
				py::arg("font_size"),
				py::arg("x"),
				py::arg("y"),
				py::arg("color"),
				py::arg("metrics"),
				py::arg("anchor_y") = slughorn::canvas::TextAnchorY::Baseline,
				py::arg("align_x") = slughorn::canvas::TextAlignX::Left,
				"Place glyphs from s into the current composite.\n\n"
				"The atlas must already contain the requested codepoints (loaded via a\n"
				"freetype function before atlas.build()). Handles em-space conversion,\n"
				"vertical anchoring, and optional horizontal alignment internally.\n\n"
				"anchor_y controls what y refers to (baseline, cap center, cap top, x-center).\n"
				"align_x LEFT is single-pass; CENTER and RIGHT do a measure pass first."
			)

			.def("stroke_text",
				&slughorn::canvas::Canvas::strokeText,
				py::arg("s"),
				py::arg("font_size"),
				py::arg("stroke_width"),
				py::arg("x"),
				py::arg("y"),
				py::arg("color"),
				py::arg("metrics"),
				py::arg("anchor_y") = slughorn::canvas::TextAnchorY::Baseline,
				py::arg("align_x") = slughorn::canvas::TextAlignX::Left,
				"Stroke glyph outlines from s into the current composite.\n\n"
				"Unlike text(), this tessellates each glyph's contours as stroked paths.\n"
				"Must be called before atlas.build() — stroke shapes are registered via\n"
				"addShape() which is disabled post-build.\n\n"
				"For fill+stroke in one CompositeShape, call stroke_text() first (outline\n"
				"underneath), then text() (fill on top), then finalize().\n\n"
				"anchor_y and align_x work identically to text()."
			)

			// Accessors -------------------------------------------------------

			.def_property_readonly("layer_count", &slughorn::canvas::Canvas::layerCount,
				"Number of Layers accumulated in the current composite."
			)
			.def_property_readonly("has_pending_path", &slughorn::canvas::Canvas::hasPendingPath,
				"True if the pending path has any curves."
			)
			.def("__repr__", [](const slughorn::canvas::Canvas& c) { return streamRepr(c); })
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
			// return std::make_shared<slughorn::Atlas>(slughorn::serial::read(path));
			return slughorn::serial::read(path);
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

	freetype.def("load_ascii_font",
		[](
			const std::string& fontPath,
			slughorn::Atlas& atlas,
			std::optional<slughorn::Atlas::SplitStrategy> strategy,
			bool uniform,
			std::optional<slughorn::freetype::LogCallback> log
		) {
			return slughorn::freetype::loadAsciiFont(
				fontPath,
				atlas,
				detail::makeLoadConfig(strategy, uniform, log)
			);
		},
		py::arg("font_path"),
		py::arg("atlas"),
		py::arg("strategy") = py::none(),
		py::arg("uniform") = false,
		py::arg("log") = py::none(),
		"Load printable ASCII (codepoints 32-126) from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"strategy: optional callable(curves) -> (splits_x, splits_y), e.g.:\n"
		"    lambda c: slughorn.Atlas.compute_adaptive_splits(c, 8, 8)\n"
		"uniform: if True, all glyphs share the same em-space bounding box\n"
		"    (required for setLayerShapeIndex glyph-swap cycling).\n"
		"log: optional callable(level: int, msg: str) for load-time diagnostics.\n"
		"Returns True on success, False if the font cannot be opened."
	);

	freetype.def("load_font_glyphs",
		[](
			const std::string& fontPath,
			const std::vector<uint32_t>& codepoints,
			slughorn::Atlas& atlas,
			std::optional<slughorn::Atlas::SplitStrategy> strategy,
			bool uniform,
			std::optional<slughorn::freetype::LogCallback> log
		) {
			return slughorn::freetype::loadFontGlyphs(
				fontPath,
				codepoints,
				atlas,
				detail::makeLoadConfig(strategy, uniform, log)
			);
		},
		py::arg("font_path"),
		py::arg("codepoints"),
		py::arg("atlas"),
		py::arg("strategy") = py::none(),
		py::arg("uniform") = false,
		py::arg("log") = py::none(),
		"Load an explicit list of Unicode codepoints from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"strategy: optional callable(curves) -> (splits_x, splits_y).\n"
		"uniform: if True, all glyphs share the same em-space bounding box\n"
		"    (required for setLayerShapeIndex glyph-swap cycling).\n"
		"log: optional callable(level: int, msg: str) for load-time diagnostics.\n"
		"Returns the number of glyphs successfully added."
	);

	freetype.def("load_all_font_glyphs",
		[](
			const std::string& fontPath,
			slughorn::Atlas& atlas,
			std::optional<slughorn::Atlas::SplitStrategy> strategy,
			bool uniform,
			std::optional<slughorn::freetype::LogCallback> log
		) {
			return slughorn::freetype::loadAllFontGlyphs(
				fontPath,
				atlas,
				detail::makeLoadConfig(strategy, uniform, log)
			);
		},
		py::arg("font_path"),
		py::arg("atlas"),
		py::arg("strategy") = py::none(),
		py::arg("uniform") = false,
		py::arg("log") = py::none(),
		"Load every mapped codepoint from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"strategy: optional callable(curves) -> (splits_x, splits_y).\n"
		"uniform: if True, all glyphs share the same em-space bounding box\n"
		"    (required for setLayerShapeIndex glyph-swap cycling).\n"
		"log: optional callable(level: int, msg: str) for load-time diagnostics.\n"
		"Returns the number of glyphs successfully added."
	);

	freetype.def("load_emoji_font", [](
		const std::string& fontPath,
		const std::vector<uint32_t>& codepoints,
		slughorn::Atlas& atlas,
		std::optional<slughorn::Atlas::SplitStrategy> strategy,
		bool uniform,
		std::optional<slughorn::freetype::LogCallback> log
	) -> py::dict {
		std::map<uint32_t, slughorn::CompositeShape> colorGlyphs;

		slughorn::freetype::loadEmojiFont(
			fontPath,
			codepoints,
			atlas,
			colorGlyphs,
			detail::makeLoadConfig(strategy, uniform, log)
		);

		py::dict result;

		for(auto& [cp, cs] : colorGlyphs) result[py::cast(cp)] = std::move(cs);

		return result;
	},
		py::arg("font_path"),
		py::arg("codepoints"),
		py::arg("atlas"),
		py::arg("strategy") = py::none(),
		py::arg("uniform") = false,
		py::arg("log") = py::none(),
		"Load COLR emoji from font_path for the given codepoints into atlas.\n"
		"codepoints is a list of uint32_t Unicode codepoints.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"strategy: optional callable(curves) -> (splits_x, splits_y).\n"
		"log: optional callable(level: int, msg: str) for load-time diagnostics.\n"
		"Returns a dict mapping codepoint (int) -> CompositeShape "
		"for each successfully loaded glyph."
	);

	freetype.def("load_font_metrics",
		[](const std::string& fontPath) -> std::optional<slughorn::FontMetrics> {
			return slughorn::freetype::loadFontMetrics(fontPath);
		},
		py::arg("font_path"),
		"Read em-space metrics from font_path and return a FontMetrics object.\n"
		"Returns None if the font cannot be opened.\n"
		"Safe to call before or independently of any load_* call."
	);
#endif

#ifdef SLUGHORN_HAS_NANOSVG
	{
		py::module_ nanosvg = m.def_submodule("nanosvg",
			"NanoSVG backend - parse SVG files or strings into Atlas shapes.\n\n"
			"Produces a CompositeShape with one Layer per filled SVG shape, "
			"back-to-front order preserved.\n\n"
			"Both functions accept an optional KeyIterator that is advanced in-place "
			"as shapes are registered. Pass the same KeyIterator across multiple calls "
			"to pack several SVGs into one atlas without key collisions."
		);

		nanosvg.def("load_file",
			[](const std::string& path, slughorn::Atlas& atlas, slughorn::KeyIterator& keys, float dpi) {
				return slughorn::nanosvg::loadFile(path, atlas, keys, dpi);
			},
			py::arg("path"),
			py::arg("atlas"),
			py::arg("keys") = slughorn::KeyIterator(),
			py::arg("dpi") = 96.0f,
			"Parse an SVG file and pack every filled shape into atlas.\n"
			"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
			"to pack multiple SVGs into the same atlas without key collisions."
		);

		nanosvg.def("load_string",
			[](const std::string& svg, slughorn::Atlas& atlas, slughorn::KeyIterator& keys, float dpi) {
				return slughorn::nanosvg::loadString(svg, atlas, keys, dpi);
			},
			py::arg("svg"),
			py::arg("atlas"),
			py::arg("keys") = slughorn::KeyIterator(),
			py::arg("dpi") = 96.0f,
			"Parse an SVG string and pack every filled shape into atlas.\n"
			"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
			"to pack multiple SVGs into the same atlas without key collisions."
		);
	}
#endif

	// py::module_ skia = m.def_submodule("skia",
	// "Skia path backend - decompose SkPath objects, stroke-to-fill expansion.");
	// TODO: bind slughorn::skia::decomposePath, strokeToFill, loadShape, etc.

	// py::module_ cairo = m.def_submodule("cairo",
	// "Cairo path backend - decompose cairo_t paths.");
	// TODO: bind slughorn::cairo::decomposePath, loadShape.
}
