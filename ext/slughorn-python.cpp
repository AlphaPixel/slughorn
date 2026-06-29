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
// slughorn.Atlas (add_shape, add_composite_shape, build, get_shape, get_composite_shape,
// get_shape_contours, has_key, is_built property, curve_texture, band_texture)
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
// #include <pybind11/numpy.h>
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
using namespace py::literals;

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
	static const std::string fmt = py::format_descriptor<slughorn::slug_t>::format();
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

	size_t mark() const { return decomposer.mark(); }
	void reverseFrom(size_t pos) { decomposer.reverseFrom(pos); }
	void reverseCurves(size_t begin, size_t end) {
		slughorn::CurveDecomposer::reverseCurves(curves, begin, end);
	}
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

	size_t mark() const { return decomposer ? decomposer->mark() : 0; }

	void reverseFrom(size_t pos) {
		if(decomposer) decomposer->reverseFrom(pos);
	}

	void reverseCurves(size_t begin, size_t end) {
		if(decomposer) slughorn::CurveDecomposer::reverseCurves(decomposer->curves, begin, end);
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

inline slughorn::nanosvg::LoadConfig makeNanosvgLoadConfig(
	std::optional<slughorn::nanosvg::LogCallback> log,
	bool autoMetrics = true,
	slughorn::Atlas::ShapeInfo::Origin origin = {}
) {
	slughorn::nanosvg::LoadConfig config;

	if(log) config.log = *log;

	config.autoMetrics = autoMetrics;
	config.origin = origin;

	return config;
}

} // namespace detail

// Bring detail helpers into file scope so existing call sites in PYBIND11_MODULE need no change.
// makeLoadConfig is intentionally left out; call sites should say detail::makeLoadConfig explicitly.
using detail::bytesView;
using detail::vectorView1D;
using detail::arrayView1D;
using detail::curveView2D;
using detail::streamRepr;
using detail::PyCurveDecomposer;
using detail::CurveDecomposerRef;

using slughorn::render::Sample;
using slughorn::render::Sampler;
using slughorn::render::Grid;

#ifdef SLUGHORN_HAS_MSDF
using slughorn::render::MSDFGrid;
#endif

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
		.def(py::init<uint32_t>(), "codepoint"_a,
			"Construct a Codepoint key from a uint32_t (e.g. ord('A'))."
		)
		.def(py::init<const std::string&>(), "name"_a,
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
		.def(py::init<uint32_t>(), "counter"_a,
			"Numeric auto-key iterator starting at counter."
		)
		.def(py::init([](std::string prefix, bool force) {
			return slughorn::KeyIterator(prefix, force);
		}), "prefix"_a, "force"_a=false,
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
		}), "r"_a, "g"_a, "b"_a, "a"_a=1_cv,
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
		.def_static("translate", &slughorn::Matrix::translate, "tx"_a, "ty"_a,
			"Return a pure-translation matrix."
		)
		.def_static("scale", &slughorn::Matrix::scale, "sx"_a, "sy"_a,
			"Return a pure-scale matrix."
		)
		.def_static("rotate", &slughorn::Matrix::rotate, "angle"_a,
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
		}, "x"_a, "y"_a,
			"Apply the matrix to point (x, y), returning (x', y').")
		.def("__mul__", &slughorn::Matrix::operator*, "rhs"_a,
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
		}), "t"_a, "color"_a,
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
		"x0"_a, "y0"_a, "x1"_a, "y1"_a,
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
		}), "x0"_a, "y0"_a, "x1"_a, "y1"_a)
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
		}), "x"_a=0_cv, "y"_a=0_cv, "z"_a=0_cv)
		.def_readwrite("x", &slughorn::Transform::x)
		.def_readwrite("y", &slughorn::Transform::y)
		.def_readwrite("z", &slughorn::Transform::z)
		.def("__repr__", [](const slughorn::Transform& t) { return streamRepr(t); })
	;

	// ============================================================================================
	// slughorn.DrawMode / slughorn.BlendMode
	// ============================================================================================
	py::enum_<slughorn::DrawMode>(m, "DrawMode")
		.value("Visible", slughorn::DrawMode::Visible)
		.value("Hidden", slughorn::DrawMode::Hidden)
		.value("Geometry", slughorn::DrawMode::Geometry)
		.value("Mask", slughorn::DrawMode::Mask)
	;

	py::enum_<slughorn::BlendMode>(m, "BlendMode")
		.value("SrcOver", slughorn::BlendMode::SrcOver)
		.value("Src", slughorn::BlendMode::Src)
		.value("Dst", slughorn::BlendMode::Dst)
		.value("SrcIn", slughorn::BlendMode::SrcIn)
		.value("DstIn", slughorn::BlendMode::DstIn)
		.value("SrcOut", slughorn::BlendMode::SrcOut)
		.value("DstOut", slughorn::BlendMode::DstOut)
		.value("SrcAtop", slughorn::BlendMode::SrcAtop)
		.value("DstAtop", slughorn::BlendMode::DstAtop)
		.value("Xor", slughorn::BlendMode::Xor)
		.value("Clear", slughorn::BlendMode::Clear)
		.value("Multiply", slughorn::BlendMode::Multiply)
		.value("Screen", slughorn::BlendMode::Screen)
		.value("Overlay", slughorn::BlendMode::Overlay)
		.value("Darken", slughorn::BlendMode::Darken)
		.value("Lighten", slughorn::BlendMode::Lighten)
		.value("ColorDodge", slughorn::BlendMode::ColorDodge)
		.value("ColorBurn", slughorn::BlendMode::ColorBurn)
		.value("HardLight", slughorn::BlendMode::HardLight)
		.value("SoftLight", slughorn::BlendMode::SoftLight)
		.value("Difference", slughorn::BlendMode::Difference)
		.value("Exclusion", slughorn::BlendMode::Exclusion)
	;

	// ============================================================================================
	// slughorn.Mask
	// ============================================================================================
	auto mask_ = py::class_<slughorn::Mask>(m, "Mask")
		.def(py::init<>())
		.def_readwrite("key", &slughorn::Mask::key,
			"Key of a shape whose MSDF tile is used as coverage. Required when type == Mask.Type.MSDF.")
		.def_readwrite("type", &slughorn::Mask::type,
			"Mask.Type.MSDF = sample baked MSDF tile; Circle/Rect/Capsule/Arc = analytical SDF.")
		.def_property(
			"params",
			[](const slughorn::Mask& mk) {
				py::list out;
				for(size_t i = 0; i < 6; ++i) out.append(mk.params[i]);
				return out;
			},
			[](slughorn::Mask& mk, py::sequence seq) {
				const size_t n = std::min<size_t>(py::len(seq), 6);
				for(size_t i = 0; i < n; ++i) mk.params[i] = py::cast<slug_t>(seq[i]);
			},
			"Analytical SDF parameters (up to 6 floats). Interpretation depends on type:\n"
			"  Circle:  cx, cy, r\n"
			"  Rect:    x, y, w, h\n"
			"  Capsule: ax, ay, bx, by, r\n"
			"  Arc:     cx, cy, r, angle_start, angle_end"
		)
		.def_readwrite("invert", &slughorn::Mask::invert,
			"If True, inverts coverage so the outside of the mask shape becomes the inside.")
		.def("__repr__", [](const slughorn::Mask& mk) { return streamRepr(mk); })
	;

	py::enum_<slughorn::Mask::Type>(mask_, "Type")
		.value("MSDF",    slughorn::Mask::Type::MSDF)
		.value("Circle",  slughorn::Mask::Type::Circle)
		.value("Rect",    slughorn::Mask::Type::Rect)
		.value("Capsule", slughorn::Mask::Type::Capsule)
		.value("Arc",     slughorn::Mask::Type::Arc)
	;

	// ============================================================================================
	// slughorn.Layer
	//
	// key, color, transform, effectId, effectParam - all fields present.
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
				slug_t effectParam,
				uint32_t gradientId,
				slug_t expand,
				slughorn::DrawMode drawMode,
				slughorn::BlendMode blendMode
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
				layer.effectParam = effectParam;
				layer.gradientId = gradientId;
				layer.expand = expand;
				layer.drawMode = drawMode;
				layer.blendMode = blendMode;

				return layer;
			}),
			"key"_a,
			"color"_a=slughorn::Color{},
			"transform"_a=slughorn::Transform{},
			"scale"_a=1_cv,
			"effectId"_a=0,
			"effectParam"_a=0_cv,
			"gradientId"_a=0,
			"expand"_a=0.01_cv,
			"drawMode"_a=slughorn::DrawMode::Visible,
			"blendMode"_a=slughorn::BlendMode::SrcOver
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
		.def_readwrite("effectParam", &slughorn::Layer::effectParam,
			"Per-layer float hint passed to the frontend vertex shader. "
			"slughorn does not interpret this value; typical uses include rotation speed, "
			"scale factor, or any other per-layer scalar the vertex hook needs.")
		.def_readwrite("gradientId", &slughorn::Layer::gradientId,
			"Gradient fill ID. 0 = flat color (layer.color used). "
			"Non-zero = 1-based index into the atlas gradient list "
			"(registered via Atlas.add_gradient()). "
			"When non-zero, layer.color.rgb is ignored; layer.color.a is a global opacity multiplier.")
		.def_readwrite("expand", &slughorn::Layer::expand,
			"Extra em-space margin added to the quad on each side (default 0.01). "
			"Set to 0 for shapes authored with canvas.set_auto_metrics(False) so that "
			"em-coords stay exactly in [0,1] for GPU tiling.")
		.def_readwrite("drawMode", &slughorn::Layer::drawMode,
			"Controls whether/how this layer is rendered. "
			"Visible=normal draw; Hidden=temporarily suppressed; Geometry=path-source only (no quad).")
		.def_readwrite("blendMode", &slughorn::Layer::blendMode,
			"Per-layer compositing mode. SrcOver=normal alpha blend (default). "
			"Photoshop-style modes (Multiply, Screen, etc.) require GL_KHR_blend_equation_advanced.")
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
		.def_readwrite("mask", &slughorn::CompositeShape::mask,
			"Optional Mask applied to the composited output of all layers. "
			"Layers composite first; the mask gates the result as a whole."
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
			"x1"_a, "y1"_a, "x2"_a, "y2"_a, "x3"_a, "y3"_a,
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
		.def_property("curves",
			[](const slughorn::Atlas::ShapeInfo& info) { return info.curves; },
			[](slughorn::Atlas::ShapeInfo& info, py::object obj) {
				if(PyObject_CheckBuffer(obj.ptr())) {
					py::buffer_info bi = py::reinterpret_borrow<py::buffer>(obj).request();

					if(
						bi.format != py::format_descriptor<slug_t>::format() ||
						bi.ndim != 2 ||
						bi.shape[1] != 6
					) throw std::runtime_error(
						"ShapeInfo.curves: expected (N, 6) float32 buffer"
					);

					info.curves.clear();
					info.curves.reserve(static_cast<size_t>(bi.shape[0]));

					for(py::ssize_t i = 0; i < bi.shape[0]; i++) {
						const slug_t* row = reinterpret_cast<const slug_t*>(
							static_cast<const char*>(bi.ptr) + i * bi.strides[0]
						);

						info.curves.push_back({row[0], row[1], row[2], row[3], row[4], row[5]});
					}
				}

				else info.curves = obj.cast<slughorn::Atlas::Curves>();
			},
			"List of Curve objects in em-normalized coordinates (get), "
			"or a (N, 6) float32 buffer to assign from (set)."
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
			"type"_a,
			"Type-only origin: pass Origin.Type.Centered (or any future named variant)."
		)
		.def(py::init<slughorn::slug_t, slughorn::slug_t>(),
			"x"_a, "y"_a,
			"Pivot origin: authoring-space pivot (bbox-min subtracted). "
			"Layer.transform.x/y will equal (x, y) scaled to local em-space."
		)
		.def(py::init<slughorn::Atlas::ShapeInfo::Origin::Type, slughorn::slug_t, slughorn::slug_t>(),
			"type"_a, "x"_a, "y"_a,
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

#ifdef SLUGHORN_HAS_MSDF
		.def_readonly("msdf_layer", &slughorn::Atlas::Shape::msdfLayer,
			"Texture2DArray layer index for this shape's MSDF tile. "
			"-1 if register_msdf() has not been called for this key."
		)
		.def_readonly("msdf_range", &slughorn::Atlas::Shape::msdfRange,
			"Em-space SDF range used when the MSDF tile was generated. "
			"0.0 if register_msdf() has not been called for this key."
		)
#endif

		// Convenience: recover em-space origin and size (mirrors slug_EmToUV logic)
		.def_property_readonly("em_origin", [](const slughorn::Atlas::Shape& s) {
			// emOrigin = -bandOffset / bandScale
			slug_t ox = (s.bandScaleX != 0_cv) ? -s.bandOffsetX / s.bandScaleX : 0_cv;
			slug_t oy = (s.bandScaleY != 0_cv) ? -s.bandOffsetY / s.bandScaleY : 0_cv;

			return py::make_tuple(ox, oy);
		}, "Em-space (x, y) of the shape's bottom-left corner. "
			"Mirrors slug_EmToUV's emOrigin computation."
		)
		.def_property_readonly("em_size", [](const slughorn::Atlas::Shape& s) {
			// emSize = INDIRECTION_SIZE / bandScale (mirrors slug_EmToUV's emSize)
			slug_t sx = (s.bandScaleX != 0_cv) ? cv(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleX : 0_cv;
			slug_t sy = (s.bandScaleY != 0_cv) ? cv(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleY : 0_cv;
			return py::make_tuple(sx, sy);
		}, "Em-space (width, height) of the shape's bounding box. "
			"Mirrors slug_EmToUV's emSize computation."
		)
		.def("em_to_uv", [](const slughorn::Atlas::Shape& s, slug_t ex, slug_t ey) {
			// Direct Python port of slug_EmToUV()
			slug_t ox = (s.bandScaleX != 0_cv) ? -s.bandOffsetX / s.bandScaleX : 0_cv;
			slug_t oy = (s.bandScaleY != 0_cv) ? -s.bandOffsetY / s.bandScaleY : 0_cv;
			slug_t sx = (s.bandScaleX != 0_cv) ? cv(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleX : 1_cv;
			slug_t sy = (s.bandScaleY != 0_cv) ? cv(slughorn::Atlas::INDIRECTION_SIZE) / s.bandScaleY : 1_cv;

			return py::make_tuple((ex - ox) / sx, (ey - oy) / sy);
		}, "em_x"_a, "em_y"_a,
			"Convert an em-space coordinate to a normalized [0, 1] UV. "
			"Python port of the GLSL slug_EmToUV() helper. "
			"(0,0) = bottom-left of bounding box, (1,1) = top-right.")
		.def("compute_quad", &slughorn::Atlas::Shape::computeQuad,
			"transform"_a, "scale"_a=1_cv, "expand"_a=0_cv,
			"Compute the world-space bounding quad for this shape."
		)
		.def_property_readonly("curves",
			[](const slughorn::Atlas::Shape& s) { return curveView2D(s.curves); },
			"Em-space curves as a (N, 6) float32 memoryview (x1,y1,x2,y2,x3,y3 per row). "
			"Pass directly to Path(curves), memoryview(), or np.asarray(). "
			"Valid at any build lifecycle stage when accessed via get_shape()."
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
				case slughorn::Atlas::TextureData::Format::RGB32F: return "RGB32F";
			}
			return "unknown";
		}, "String: 'RGBA32F' (curve), 'RGBA16UI' (band), 'RGBA8' (gradient), 'RGB32F' (MSDF array).")
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
		.def_readonly("sdf_tile_count", &slughorn::Atlas::PackingStats::sdfTileCount,
			"Number of shapes with a packed SDF/MSDF atlas tile (0 unless set_sdf_options() was used).")
		.def_readonly("sdf_texels_used", &slughorn::Atlas::PackingStats::sdfTexelsUsed)
		.def_readonly("sdf_texels_padding", &slughorn::Atlas::PackingStats::sdfTexelsPadding)
		.def_readonly("sdf_texels_total", &slughorn::Atlas::PackingStats::sdfTexelsTotal)
		.def_readonly("msdf_layer_count", &slughorn::Atlas::PackingStats::msdfLayerCount,
			"Number of layers registered via register_msdf() (0 unless used).")
		.def_readonly("msdf_tile_size", &slughorn::Atlas::PackingStats::msdfTileSize)
		.def_readonly("msdf_texels_total", &slughorn::Atlas::PackingStats::msdfTexelsTotal)
		.def("curve_utilization", &slughorn::Atlas::PackingStats::curveUtilization)
		.def("band_utilization", &slughorn::Atlas::PackingStats::bandUtilization)
		.def("sdf_utilization", &slughorn::Atlas::PackingStats::sdfUtilization)
		.def("curve_padding_ratio", &slughorn::Atlas::PackingStats::curvePaddingRatio)
		.def("band_padding_ratio", &slughorn::Atlas::PackingStats::bandPaddingRatio)
		.def("sdf_padding_ratio", &slughorn::Atlas::PackingStats::sdfPaddingRatio)
		.def("curve_bytes", &slughorn::Atlas::PackingStats::curveBytes)
		.def("band_bytes", &slughorn::Atlas::PackingStats::bandBytes)
		.def("gradient_bytes", &slughorn::Atlas::PackingStats::gradientBytes)
		.def("sdf_bytes", &slughorn::Atlas::PackingStats::sdfBytes)
		.def("msdf_bytes", &slughorn::Atlas::PackingStats::msdfBytes)
		.def("total_bytes", &slughorn::Atlas::PackingStats::totalBytes,
			"Total GPU memory across every channel, in bytes "
			"(curve + band + gradient + SDF atlas + MSDF array).")
		.def("__repr__", [](const slughorn::Atlas::PackingStats& p) { return streamRepr(p); })
	;

	// ============================================================================================
	// slughorn.Atlas
	// ============================================================================================
	// py::class_<slughorn::Atlas, std::shared_ptr<slughorn::Atlas>>(m, "Atlas")
	auto atlas_ = py::class_<slughorn::Atlas>(m, "Atlas")
		.def(py::init<>())

		.def("add_shape", &slughorn::Atlas::addShape,
			"key"_a, "info"_a,
			"Register a shape under key. Must be called before build()."
		)

		.def("add_composite_shape", &slughorn::Atlas::addCompositeShape,
			"key"_a, "composite"_a,
			"Register a CompositeShape under key. "
			"May be called before or after build()."
		)

		.def("normalize_shape_metrics",
			&slughorn::Atlas::normalizeShapeMetrics,
			"keys"_a,
			"Force all shapes in keys to share the same em-space bounding box.\n\n"
			"Must be called after add_shape() but before build(). Keys not present\n"
			"in the atlas or shapes with no curves are silently skipped.\n\n"
			"When all shapes share the same advance (tabular/monospaced), the cell\n"
			"width equals that advance. Otherwise the cell is the union bbox.\n\n"
			"Only layout fields (bearing, width, height, advance) are updated.\n"
			"Per-shape band transforms are left intact - required for\n"
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
				return a.getShape(key);
			},
			"key"_a,
			"Return a Shape with all info (metrics, curves, origin) for key, or None if not found.\n"
			"Works at any build lifecycle stage - pre-build returns font metrics and em-space\n"
			"curves; post-build also includes GPU band fields. Use get_shape_contours() to\n"
			"retrieve curves split by closed contour."
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
			"key"_a,
			"Return list of contours; each contour is a list of (x1,y1,x2,y2,x3,y3) tuples.\n"
			"Contour breaks are detected where p3 of curve[i] != p1 of curve[i+1].\n"
			"Returns an empty list if the key is not found. Use when building paths for\n"
			"stroking or filling - each contour must be handled independently."
		)

		.def("get_composite_shape",
			[](const slughorn::Atlas& a, slughorn::Key key)
				-> std::optional<slughorn::CompositeShape>
			{
				const auto* c = a.getCompositeShape(key);

				if(!c) return std::nullopt;

				return *c;
			},
			"key"_a,
			"Return the CompositeShape for key, or None if not found."
		)

		.def("has_key",
			&slughorn::Atlas::hasKey,
			"key"_a,
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
			"info"_a,
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
			"key"_a,
			"Decode a built shape into a Python-facing software-render view.\n"
			"Returns a slughorn.render.Sampler."
		)

#if 0
		.def_static("compute_adaptive_splits",
			[](const slughorn::Atlas::Curves& curves, int num_bands_x, int num_bands_y)
				-> py::tuple
			{
				auto [sx, sy] = slughorn::Atlas::computeAdaptiveSplits(
					curves, num_bands_x, num_bands_y
				);

				return py::make_tuple(sx, sy);
			},
			"curves"_a, "num_bands_x"_a, "num_bands_y"_a,
			"Sweep-line valley placement: places band boundaries where fewest curves cross,\n"
			"minimizing per-fragment shader iterations.\n\n"
			"Returns (splits_x, splits_y): normalized [0, 1] fraction lists to assign to\n"
			"ShapeInfo.splits_x / splits_y.\n\n"
			"Example::\n\n"
			"    splits_x, splits_y = slughorn.Atlas.compute_adaptive_splits(curves, num_bands_x=8, num_bands_y=8)\n"
			"    info.splits_x = splits_x\n"
			"    info.splits_y = splits_y"
		)
#endif

		.def_static("compute_uniform_splits",
			[](const slughorn::Atlas::Curves& curves, int num_bands_x, int num_bands_y)
				-> py::tuple
			{
				auto [sx, sy] = slughorn::Atlas::computeUniformSplits(
					curves, num_bands_x, num_bands_y
				);

				return py::make_tuple(sx, sy);
			},
			"curves"_a, "num_bands_x"_a, "num_bands_y"_a,
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

	// The MSDFEdgeColoring enum must be registered on atlas_ BEFORE the MSDF methods below,
	// because pybind11 evaluates default argument values at .def() call time and needs
	// MSDFEdgeColoring to be a known Python type before it appears as a default.
#ifdef SLUGHORN_HAS_MSDF
	py::enum_<slughorn::Atlas::MSDFEdgeColoring>(atlas_, "MSDFEdgeColoring",
		"Edge-coloring algorithm used by msdfgen when generating MSDF tiles.\n\n"
		"ByDistance: assigns edge colors by measuring angles to all contours - eliminates\n"
		"corner spike artifacts at the cost of slightly more CPU work. Recommended default.\n"
		"Simple: uses a greedy angle-threshold approach - faster but prone to artifacts at\n"
		"convex corners with acute angles."
	)
		.value("Simple", slughorn::Atlas::MSDFEdgeColoring::Simple)
		.value("ByDistance", slughorn::Atlas::MSDFEdgeColoring::ByDistance)
		.export_values()
	;

	atlas_
		.def_property("msdf_tile_size",
			&slughorn::Atlas::getMSDFTileSize,
			&slughorn::Atlas::setMSDFTileSize,
			"Tile size for MSDF tiles (default 128). All layers in a sampler2DArray must be\n"
			"identical - hard GPU constraint. Read any time; write only before register_msdf().\n"
			"Setting after register_msdf() has been called raises RuntimeError."
		)

		.def("register_msdf",
			[](
				slughorn::Atlas& a,
				slughorn::Key key,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				return a.registerMSDF(key, range, coloring);
			},
			"key"_a, "range"_a=0.1, "coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Opt this shape in to MSDF tile generation.\n"
			"Must be called after build() and before the graphics adapter packs textures.\n"
			"range: em-space SDF spread; controls gradient depth and tile bbox margin.\n"
			"coloring: MSDFEdgeColoring.ByDistance (default, fewer artifacts) or .Simple (faster).\n"
			"Returns the layer index in the resulting Texture2DArray.\n"
			"Shape.msdf_layer and .msdf_range are updated in-place. Idempotent for repeated keys."
		)

		.def("register_msdf",
			[](
				slughorn::Atlas& a,
				const std::vector<slughorn::Key>& keys,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				a.registerMSDF(keys, range, coloring);
			},
			"keys"_a, "range"_a=0.1, "coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Batch overload: register MSDF tiles for a list of keys.\n"
			"Tiles are rendered in parallel (when built with SLUGHORN_RENDER_PARALLEL=ON),\n"
			"then committed in deterministic order. Idempotent for already-registered keys.\n"
			"Must be called after build() and before the graphics adapter packs textures."
		)

		.def("get_msdf_layer",
			[](const slughorn::Atlas& a, slughorn::Key key) { return a.getMSDFLayer(key); },
			"key"_a,
			"Return the Texture2DArray layer index for key, or -1 if not registered."
		)

		.def("get_msdf_texture_data",
			[](const slughorn::Atlas& a) -> py::object {
				const auto& td = a.getMSDFTextureData();

				if(td.empty()) return py::none();

				return py::memoryview::from_memory(
					const_cast<uint8_t*>(td.bytes.data()),
					static_cast<py::ssize_t>(td.bytes.size())
				);
			},
			"Return a zero-copy memoryview over the packed RGB32F MSDF tile data.\n"
			"Cast to float32 and reshape to (depth, tile_size, tile_size, 3).\n"
			"Returns None when no shapes are registered."
		)

		.def("render_sdf",
			[](const slughorn::Atlas& a, slughorn::Key key, uint32_t tileSize, slug_t range) {
				return slughorn::render::renderSDF(a, key, tileSize, range);
			},
			"key"_a, "tile_size"_a=128, "range"_a=0.1,
			"Generate a single-channel SDF tile via msdfgen.\n"
			"Returns a Grid; use memoryview(grid) for a (H, W) float32 view,\n"
			"or np.asarray(grid) for NumPy. Edge pixels are ~0.5."
		)

		.def("render_msdf",
			[](const slughorn::Atlas& a, slughorn::Key key, uint32_t tileSize, slug_t range) {
				return slughorn::render::renderMSDF(a, key, tileSize, range);
			},
			"key"_a, "tile_size"_a=128, "range"_a=0.1,
			"Generate a multi-channel SDF tile via msdfgen. Aspect-ratio preserving.\n"
			"Returns an MSDFGrid; use memoryview(grid) for a (H, W, 3) float32 view,\n"
			"or np.asarray(grid) for NumPy. Reconstruct in shader: median(r, g, b)."
		)

		.def("render_msdf_tile",
			[](
				const slughorn::Atlas& a,
				slughorn::Key key,
				uint32_t tileSize,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				return slughorn::render::renderMSDFTile(a, key, tileSize, range, coloring);
			},
			"key"_a,
			"tile_size"_a=128,
			"range"_a=0.1,
			"coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Generate a square tile_size x tile_size MSDF tile for GPU Texture2DArray use.\n"
			"Uses anisotropic projection: UV [0,1] fills the tile exactly in both axes.\n"
			"range: em-space SDF spread; also sets the tile bbox margin to prevent ghost fringes.\n"
			"coloring: MSDFEdgeColoring.ByDistance (default) or .Simple.\n"
			"Returns an MSDFGrid; use memoryview(grid) for a (H, W, 3) float32 view."
		)
	;
#endif // SLUGHORN_HAS_MSDF

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
		.def_property(
			"tolerance",
			&PyCurveDecomposer::getTolerance,
			&PyCurveDecomposer::setTolerance,
			"Flatness threshold for cubic decomposition in curve-space units."
		)
		.def("move_to", &PyCurveDecomposer::moveTo, "x"_a, "y"_a)
		.def("line_to", &PyCurveDecomposer::lineTo, "x3"_a, "y3"_a)
		.def("quad_to", &PyCurveDecomposer::quadTo,
			"cx"_a, "cy"_a, "x3"_a, "y3"_a
		)
		.def("cubic_to", &PyCurveDecomposer::cubicTo,
			"c1x"_a, "c1y"_a, "c2x"_a, "c2y"_a, "x3"_a, "y3"_a
		)
		.def("get_curves", &PyCurveDecomposer::getCurves,
			py::return_value_policy::copy,
			"Return a copy of the accumulated Curves list."
		)
		.def_property_readonly("curve_buffer",
			[](const PyCurveDecomposer& d) { return curveView2D(d.getCurves()); },
			"Zero-copy (N, 6) float32 memoryview of accumulated curves.\n"
			"View is invalidated if the decomposer is mutated after this call."
		)
		.def("close", &PyCurveDecomposer::close,
			"Close the current subpath by drawing a line back to the start point."
		)
		.def("clear", &PyCurveDecomposer::clear,
			"Discard all accumulated curves (reuse the decomposer for a new path)."
		)
		.def("mark", &PyCurveDecomposer::mark,
			"Return the current curve count as a position snapshot for reverse_from()."
		)
		.def("reverse_from", &PyCurveDecomposer::reverseFrom, "pos"_a,
			"Reverse the winding of all curves appended since mark(pos).\n"
			"Swaps each curve's endpoints and reverses the sequence order."
		)
		.def("reverse_curves", &PyCurveDecomposer::reverseCurves,
			"begin"_a, "end"_a,
			"Reverse the winding of curves[begin:end] in-place.\n"
			"Swaps each curve's endpoints and reverses the sequence order."
		)
		.def("__len__", [](const PyCurveDecomposer& d) {
			return d.getCurves().size();
		}, "Number of curves accumulated so far.")
	;

	py::class_<CurveDecomposerRef>(m, "_CurveDecomposerRef",
		"Non-owning view over an internal CurveDecomposer.\n\n"
		"Returned by canvas.Path.decomposer() and canvas.Canvas.decomposer() to expose\n"
		"the underlying tolerance control without copying the decomposer state.")
		.def_property(
			"tolerance",
			&CurveDecomposerRef::getTolerance,
			&CurveDecomposerRef::setTolerance,
			"Flatness threshold for cubic decomposition in curve-space units."
		)
		.def("mark", &CurveDecomposerRef::mark,
			"Return the current curve count as a position snapshot for reverse_from()."
		)
		.def("reverse_from", &CurveDecomposerRef::reverseFrom, "pos"_a,
			"Reverse the winding of all curves appended since mark(pos)."
		)
		.def("reverse_curves", &CurveDecomposerRef::reverseCurves,
			"begin"_a, "end"_a,
			"Reverse the winding of curves[begin:end] in-place."
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

		py::class_<Grid>(render, "Grid", py::buffer_protocol())
			.def_readonly("width", &Grid::width)
			.def_readonly("height", &Grid::height)
			.def_buffer([](Grid& g) -> py::buffer_info {
				return py::buffer_info(
					g.data.data(),
					sizeof(slug_t),
					py::format_descriptor<slug_t>::format(),
					2,
					{ static_cast<py::ssize_t>(g.height), static_cast<py::ssize_t>(g.width) },
					{
						static_cast<py::ssize_t>(g.width * sizeof(slug_t)),
						static_cast<py::ssize_t>(sizeof(slug_t))
					}
				);
			})
			.def("__repr__", [](const Grid& g) {
				return "Grid(" + std::to_string(g.width) + "x" + std::to_string(g.height) + ")";
			})
		;

#ifdef SLUGHORN_HAS_MSDF
		py::class_<MSDFGrid>(render, "MSDFGrid", py::buffer_protocol())
			.def_readonly("width", &MSDFGrid::width)
			.def_readonly("height", &MSDFGrid::height)
			.def_buffer([](MSDFGrid& g) -> py::buffer_info {
				return py::buffer_info(
					g.data.data(),
					sizeof(slug_t),
					py::format_descriptor<slug_t>::format(),
					3,
					{
						static_cast<py::ssize_t>(g.height),
						static_cast<py::ssize_t>(g.width),
						static_cast<py::ssize_t>(3)
					},
					{
						static_cast<py::ssize_t>(g.width * 3 * sizeof(slug_t)),
						static_cast<py::ssize_t>(3 * sizeof(slug_t)),
						static_cast<py::ssize_t>(sizeof(slug_t))
					}
				);
			})
			.def("__repr__", [](const MSDFGrid& g) {
				return "MSDFGrid(" + std::to_string(g.width) + "x" + std::to_string(g.height) + ")";
			})
		;
#endif

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
				if(i + 1 >= d.hbandOffsets.size()) {
					throw py::index_error("horizontal band out of range");
				}

				py::list out;

				for(uint32_t j = d.hbandOffsets[i]; j < d.hbandOffsets[i + 1]; j++)
					out.append(d.hbandIndices[j]);

				return out;
			}, "index"_a)
			.def("get_vband", [](const Sampler& d, uint32_t i) {
				if(i + 1 >= d.vbandOffsets.size()) {
					throw py::index_error("vertical band out of range");
				}

				py::list out;

				for(uint32_t j = d.vbandOffsets[i]; j < d.vbandOffsets[i + 1]; j++)
					out.append(d.vbandIndices[j]);

				return out;
			}, "index"_a)
			.def("render_sample",
				[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
					return d.renderSample(x, y, ppeX, ppeY);
				},
				"x"_a, "y"_a, "ppe_x"_a, "ppe_y"_a,
				"Reference software sample using all decoded curves."
			)
			.def("render_sample_banded",
				[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
					return d.renderSampleBanded(x, y, ppeX, ppeY);
				},
				"x"_a, "y"_a, "ppe_x"_a, "ppe_y"_a,
				"Band-accelerated software sample mirroring the GPU shader path."
			)
			.def("render_grid",
				[](const Sampler& d, uint32_t size, slug_t margin, bool banded, bool parallel) {
					py::gil_scoped_release release;
					return d.renderGrid(size, margin, banded, parallel);
				},
				"size"_a=128, "margin"_a=0_cv, "banded"_a=true, "parallel"_a=false,
				"Render a full grayscale coverage grid.\n"
				"Returns a Grid; use memoryview(grid) for a zero-copy (H, W) float32 view,\n"
				"or np.asarray(grid) for NumPy users.\n"
				"parallel=True uses OpenMP row parallelism (requires SLUGHORN_RENDER_PARALLEL=ON)."
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
			"atlas"_a, "key"_a,
			"Decode a built atlas shape into a slughorn.render.Sampler."
		);

#ifdef SLUGHORN_HAS_MSDF
		render.def("sdf",
			[](const slughorn::Atlas& atlas, slughorn::Key key, uint32_t tileSize, slug_t range) {
				return slughorn::render::renderSDF(atlas, key, tileSize, range);
			},
			"atlas"_a, "key"_a, "tile_size"_a=128, "range"_a=0.1,
			"Generate a single-channel SDF tile. Aspect-ratio preserving.\n"
			"Returns a Grid with values in [0, 1]; edge pixels are ~0.5."
		);

		render.def("msdf",
			[](const slughorn::Atlas& atlas, slughorn::Key key, uint32_t tileSize, slug_t range) {
				return slughorn::render::renderMSDF(atlas, key, tileSize, range);
			},
			"atlas"_a, "key"_a, "tile_size"_a=128, "range"_a=0.1,
			"Generate a multi-channel SDF tile. Aspect-ratio preserving.\n"
			"Returns an MSDFGrid; reconstruct signed distance with median(r, g, b)."
		);

		render.def("msdf_tile",
			[](
				const slughorn::Atlas& atlas,
				slughorn::Key key,
				uint32_t tileSize,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				return slughorn::render::renderMSDFTile(atlas, key, tileSize, range, coloring);
			},
			"atlas"_a,
			"key"_a,
			"tile_size"_a=128,
			"range"_a=0.1,
			"coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Generate a square tile_size x tile_size MSDF tile for GPU Texture2DArray use.\n"
			"Uses anisotropic projection: UV [0,1] fills the tile exactly in both axes.\n"
			"range: em-space SDF spread; also sets the tile bbox margin to prevent ghost fringes.\n"
			"coloring: Atlas.MSDFEdgeColoring.ByDistance (default) or .Simple.\n"
			"Returns an MSDFGrid; reconstruct signed distance with median(r, g, b)."
		);
#endif
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
				.def(py::init([](py::buffer b) {
					py::buffer_info info = b.request();

					if(
						info.format != py::format_descriptor<slug_t>::format() ||
						info.ndim != 2 ||
						info.shape[1] != 6
					) throw std::runtime_error(
						"Path(curves): expected (N, 6) float32 buffer"
					);

					slughorn::Atlas::Curves curves;

					curves.reserve(static_cast<size_t>(info.shape[0]));

					for(py::ssize_t i = 0; i < info.shape[0]; i++) {
						const slug_t* row = reinterpret_cast<const slug_t*>(
							static_cast<const char*>(info.ptr) + i * info.strides[0]
						);

						curves.push_back({row[0], row[1], row[2], row[3], row[4], row[5]});
					}

					return slughorn::canvas::Path(std::move(curves));
				}), "curves"_a,
					"Create a path from a (N, 6) float32 buffer (memoryview, numpy array, etc.).\n"
					"Accepts Shape.curves directly: Path(atlas.get_shape(key).curves)"
				)
				.def(py::init<slughorn::Atlas::Curves>(), "curves"_a,
					"Create a path pre-populated with a list of Curve objects."
				)

				// Path management
				.def("clear", &Path::clear,
					"Reset all geometry state. The CTM (transform) is NOT cleared,\n"
					"matching HTML Canvas beginPath() semantics.")
				.def("add_path",
					py::overload_cast<const Path&>(&Path::addPath),
					"other"_a,
					"Append all curves from other into this path's accumulator. Does not affect other."
				)
				.def("add_path",
					py::overload_cast<const Path&, const slughorn::Matrix&>(&Path::addPath),
					"other"_a, "transform"_a,
					"Append curves from other with each control point transformed by transform.\n"
					"Matches HTML Canvas Path2D.addPath(path, DOMMatrix) semantics."
				)

				// Transform stack
				.def("save", &Path::save, "Push the current transform onto the stack.")
				.def("restore", &Path::restore, "Pop the transform stack.")
				.def("reset_transform", &Path::resetTransform, "Set CTM to identity.")
				.def("set_transform", &Path::setTransform, "m"_a,
					"Replace CTM with matrix m.")
				.def("transform", py::overload_cast<const slughorn::Matrix&>(&Path::transform),
					"m"_a, "Post-multiply CTM by m.")
				.def("translate", &Path::translate, "tx"_a, "ty"_a)
				.def("rotate", &Path::rotate, "angle"_a, "Angle in radians.")
				.def("scale", &Path::scale, "sx"_a, "sy"_a)

				// Path commands
				.def("move_to", &Path::moveTo, "x"_a, "y"_a)
				.def("line_to", &Path::lineTo, "x"_a, "y"_a)
				.def("quad_to", &Path::quadTo,
					"cx"_a, "cy"_a, "x"_a, "y"_a)
				.def("bezier_to", &Path::bezierTo,
					"c1x"_a, "c1y"_a, "c2x"_a, "c2y"_a,
					"x"_a, "y"_a)
				.def("close_path", &Path::closePath)

				// Shape helpers
				.def("rect", &Path::rect,
					"x"_a, "y"_a, "w"_a, "h"_a)
				.def("rounded_rect", &Path::roundedRect,
					"x"_a, "y"_a, "w"_a, "h"_a, "r"_a)
				.def("circle", &Path::circle, "cx"_a, "cy"_a, "r"_a)
				.def("ellipse", &Path::ellipse,
					"cx"_a, "cy"_a, "rx"_a, "ry"_a)
				.def("arc", &Path::arc,
					"cx"_a, "cy"_a, "r"_a, "start_angle"_a, "end_angle"_a, "ccw"_a=false,
					"Circular arc. Does NOT call clear(); appends to the current path.\n"
					"Angles in radians from +X axis, Y-up convention.")
				.def("arc_to", &Path::arcTo,
					"x1"_a, "y1"_a, "x2"_a, "y2"_a, "r"_a,
					"Tangential arc. Matches HTML Canvas arcTo().")

				// Stroke expansion
				.def("stroke_path", &Path::strokePath,
					"width"_a, "cw"_a=false,
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
				.def("sample", &Path::sample, "t"_a,
					"Sample position and tangent at normalized arc-length t in [0,1].\n"
					"Returns a Path.Sample(x, y, angle).")
				.def("__repr__", [](const Path& p) { return streamRepr(p); })
			;
		}

		// Canvas class --------------------------------------------------------

		py::class_<slughorn::canvas::Canvas>(canvas, "Canvas")
			.def(py::init<slughorn::Atlas&, slughorn::KeyIterator>(),
				"atlas"_a, "key_iterator"_a=slughorn::KeyIterator(),
				"Construct a Canvas writing into atlas, using key_iterator for auto-generated keys."
			)

			// CurveDecomposer / path snapshot ---------------------------------

			.def("decomposer",
				[](slughorn::canvas::Canvas& c) {
					return CurveDecomposerRef{&c.decomposer()};
				},
				"Access the internal CurveDecomposer to tune tolerance etc."
			)
			.def_property("tolerance",
				&slughorn::canvas::Canvas::getTolerance,
				&slughorn::canvas::Canvas::setTolerance,
				"Curve decomposition tolerance (shorthand for decomposer().tolerance)."
			)
			.def("path", &slughorn::canvas::Canvas::path,
				"Return a copy of the internal path. Non-destructive: the canvas path is intact."
			)

			// Transform stack -------------------------------------------------

			.def("save", &slughorn::canvas::Canvas::save, "Push the current transform.")
			.def("restore", &slughorn::canvas::Canvas::restore, "Pop the transform stack.")
			.def("reset_transform", &slughorn::canvas::Canvas::resetTransform)
			.def("set_transform", &slughorn::canvas::Canvas::setTransform, "m"_a)
			.def("transform",
				py::overload_cast<const slughorn::Matrix&>(&slughorn::canvas::Canvas::transform),
				"m"_a)
			.def("translate", &slughorn::canvas::Canvas::translate, "tx"_a, "ty"_a)
			.def("rotate", &slughorn::canvas::Canvas::rotate, "angle"_a)
			.def("scale", &slughorn::canvas::Canvas::scale, "sx"_a, "sy"_a)

			// Path commands ---------------------------------------------------

			.def("begin_path", &slughorn::canvas::Canvas::beginPath,
				"Discard any accumulated path state and start fresh."
			)
			.def("add_path",
				py::overload_cast<const slughorn::canvas::Path&>(&slughorn::canvas::Canvas::addPath),
				"other"_a,
				"Append all curves from an explicit Path into the canvas's internal path."
			)
			.def("add_path",
				py::overload_cast<
					const slughorn::canvas::Path&,
					const slughorn::Matrix&
				>(&slughorn::canvas::Canvas::addPath),
				"other"_a, "transform"_a,
				"Append curves from an explicit Path with each control point transformed by transform.\n"
				"Matches HTML Canvas Path2D.addPath(path, DOMMatrix) semantics."
			)
			.def("move_to", &slughorn::canvas::Canvas::moveTo, "x"_a, "y"_a)
			.def("line_to", &slughorn::canvas::Canvas::lineTo, "x"_a, "y"_a)
			.def("quad_to", &slughorn::canvas::Canvas::quadTo,
				"cx"_a, "cy"_a, "x"_a, "y"_a
			)
			.def("bezier_to", &slughorn::canvas::Canvas::bezierTo,
				"c1x"_a, "c1y"_a, "c2x"_a, "c2y"_a, "x"_a, "y"_a
			)
			.def("close_path", &slughorn::canvas::Canvas::closePath)

			// Convenience shape helpers ---------------------------------------

			.def("rect", &slughorn::canvas::Canvas::rect,
				"x"_a, "y"_a, "w"_a, "h"_a,
				"Axis-aligned rectangle."
			)
			.def("rounded_rect", &slughorn::canvas::Canvas::roundedRect,
				"x"_a, "y"_a, "w"_a, "h"_a, "r"_a,
				"Rounded rectangle with uniform corner radius r."
			)
			.def("circle", &slughorn::canvas::Canvas::circle,
				"cx"_a, "cy"_a, "r"_a
			)
			.def("ellipse", &slughorn::canvas::Canvas::ellipse,
				"cx"_a, "cy"_a, "rx"_a, "ry"_a
			)
			.def("arc", &slughorn::canvas::Canvas::arc,
				"cx"_a, "cy"_a, "r"_a, "start_angle"_a, "end_angle"_a, "ccw"_a=false,
				"Circular arc. Angles in radians from +X axis, Y-up convention."
			)
			.def("arc_to", &slughorn::canvas::Canvas::arcTo,
				"x1"_a, "y1"_a, "x2"_a, "y2"_a, "r"_a,
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
				"color"_a, "scale"_a=1_cv, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"color"_a, "scale"_a, "key"_a, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"key"_a, "scale"_a=1_cv, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
				"Register the current path as a named Shape (geometry only, no Layer).\n"
				"Returns False if the path was empty."
			)
			// stroke_path() / in-place expand, then commit separately
			.def("stroke_path",
				[](slughorn::canvas::Canvas& c, slug_t width, bool cw) {
					return c.strokePath(width, cw);
				},
				"width"_a, "cw"_a=false,
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
				"width"_a,
				"color"_a,
				"scale"_a=1_cv,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"width"_a,
				"color"_a,
				"scale"_a,
				"key"_a,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a,
				"color"_a,
				"scale"_a=1_cv,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a,
				"color"_a,
				"scale"_a,
				"key"_a,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a, "key"_a, "scale"_a=1_cv, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a,
				"width"_a,
				"color"_a,
				"scale"_a=1_cv,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a,
				"width"_a,
				"color"_a,
				"scale"_a,
				"key"_a,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"path"_a,
				"handle"_a,
				"scale"_a=1_cv,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"x0"_a, "y0"_a, "x1"_a, "y1"_a, "stops"_a,
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
				"cx"_a, "cy"_a, "r0"_a, "r1"_a, "stops"_a,
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
				"cx"_a, "cy"_a, "start_angle"_a, "end_angle"_a, "stops"_a,
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
				"handle"_a, "scale"_a=1_cv, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"handle"_a, "scale"_a, "key"_a, "origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"width"_a,
				"handle"_a,
				"scale"_a=1_cv,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
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
				"width"_a,
				"handle"_a,
				"scale"_a,
				"key"_a,
				"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
				"Expand the current path as a stroke outline with a gradient, registering under key."
			)

			// CompositeShape management ---------------------------------------

			.def("begin_composite", &slughorn::canvas::Canvas::beginComposite,
				"Discard all accumulated layers and start a fresh composite."
			)
			.def("set_advance", &slughorn::canvas::Canvas::setAdvance,
				"advance"_a,
				"Set the horizontal advance of the composite being built."
			)
			.def("finalize",
				py::overload_cast<>(&slughorn::canvas::Canvas::finalize),
				"Return the completed CompositeShape and reset internal state."
			)
			.def("finalize",
				py::overload_cast<slughorn::Key>(&slughorn::canvas::Canvas::finalize),
				"key"_a,
				"Register the completed CompositeShape in the Atlas under key and reset."
			)
			.def("text",
				&slughorn::canvas::Canvas::text,
				"s"_a,
				"font_size"_a,
				"x"_a,
				"y"_a,
				"color"_a,
				"metrics"_a,
				"anchor_y"_a=slughorn::canvas::TextAnchorY::Baseline,
				"align_x"_a=slughorn::canvas::TextAlignX::Left,
				"Place glyphs from s into the current composite.\n\n"
				"The atlas must already contain the requested codepoints (loaded via a\n"
				"freetype function before atlas.build()). Handles em-space conversion,\n"
				"vertical anchoring, and optional horizontal alignment internally.\n\n"
				"anchor_y controls what y refers to (baseline, cap center, cap top, x-center).\n"
				"align_x LEFT is single-pass; CENTER and RIGHT do a measure pass first."
			)

			.def("stroke_text",
				&slughorn::canvas::Canvas::strokeText,
				"s"_a,
				"font_size"_a,
				"stroke_width"_a,
				"x"_a,
				"y"_a,
				"color"_a,
				"metrics"_a,
				"anchor_y"_a=slughorn::canvas::TextAnchorY::Baseline,
				"align_x"_a=slughorn::canvas::TextAlignX::Left,
				"Stroke glyph outlines from s into the current composite.\n\n"
				"Unlike text(), this tessellates each glyph's contours as stroked paths.\n"
				"Must be called before atlas.build() - stroke shapes are registered via\n"
				"addShape() which is disabled post-build.\n\n"
				"For fill+stroke in one CompositeShape, call stroke_text() first (outline\n"
				"underneath), then text() (fill on top), then finalize().\n\n"
				"anchor_y and align_x work identically to text()."
			)

			.def("text_on_path",
				&slughorn::canvas::Canvas::textOnPath,
				"path"_a,
				"s"_a,
				"font_size"_a,
				"start_frac"_a,
				"color"_a,
				"metrics"_a,
				"anchor_y"_a=slughorn::canvas::TextAnchorY::Baseline,
				"Place filled glyphs from s along path, each rotated to follow the tangent.\n\n"
				"path is a slughorn.canvas.Path built with arc/move_to/etc.\n"
				"start_frac in [0,1] is the normalized arc-length start position;\n"
				"use 0.0 for left-aligned or compute (arc_len - text_width) / (2*arc_len)\n"
				"for centered. Glyphs that extend past the path end are dropped.\n\n"
				"The atlas must already contain the requested codepoints (loaded via\n"
				"slughorn.freetype.load_font_glyphs before atlas.build())."
			)

			// Accessors -------------------------------------------------------

			.def_property("auto_metrics",
				&slughorn::canvas::Canvas::getAutoMetrics,
				&slughorn::canvas::Canvas::setAutoMetrics,
				"When True (default), shapes use tight curve bbox for quad sizing and band "
				"calibration. Set to False to keep curves in [0,1] canvas space with the full "
				"unit square as both the layout extent and band spatial range - required for "
				"artifact-free GPU tiling via fract(). Pair with layer.expand = 0."
			)
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
		"path"_a,
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
		"atlas"_a, "path"_a,
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
		"Unicode 15.1 RGI emoji lookup table (974 single-codepoint entries).\n"
		"Names are CLDR short names, lower-case, spaces replaced with underscores."
	);

	emoji.def("name_to_codepoint",
		[](std::string_view name) -> std::optional<uint32_t> {
			return slughorn::emoji::nameToCodepoint(name);
		}, "name"_a,
		"Return the codepoint for a normalised CLDR short name, or None.\n"
		"Example: slughorn.emoji.name_to_codepoint('dragon') -> 0x1F409"
	);

	emoji.def("codepoint_to_name",
		[](uint32_t cp) -> std::optional<std::string> {
			auto sv = slughorn::emoji::codepointToName(cp);

			if(!sv) return std::nullopt;

			return std::string(*sv);
		}, "codepoint"_a,
		"Return the CLDR short name for a codepoint, or None."
	);

	emoji.def("codepoint_at_index",
		&slughorn::emoji::codepointAtIndex,
		"index"_a,
		"Return the codepoint at position index in the sorted table.\n"
		"Pair with table_size() to iterate the full table."
	);

	emoji.def("strip_colons",
		[](std::string_view name) -> std::string {
			return std::string(slughorn::emoji::stripColons(name));
		}, "name"_a,
		"Strip leading/trailing colons: ':dragon:' -> 'dragon'."
	);

	emoji.def("slack_name_to_codepoint",
		[](std::string_view name) -> std::optional<uint32_t> {
			return slughorn::emoji::slackNameToCodepoint(name);
		}, "slack_name"_a,
		"Strip colons then look up. ':dragon:' -> 0x1F409"
	);

	emoji.def("random_codepoint",
		py::overload_cast<>(&slughorn::emoji::randomCodepoint),
		"Return a random codepoint from the table (thread-local RNG, "
		"seeded from random_device on first call)."
	);

	emoji.def("table_size",
		&slughorn::emoji::tableSize,
		"Return the number of entries in the lookup table (974 for Unicode 15.1)."
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
			auto config = detail::makeLoadConfig(strategy, uniform, log);

			return slughorn::freetype::loadAsciiFont(fontPath, atlas, &config);
		},
		"font_path"_a, "atlas"_a, "strategy"_a=py::none(), "uniform"_a=false, "log"_a=py::none(),
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
			auto config = detail::makeLoadConfig(strategy, uniform, log);

			return slughorn::freetype::loadFontGlyphs(fontPath, codepoints, atlas, &config);
		},
		"font_path"_a,
		"codepoints"_a,
		"atlas"_a,
		"strategy"_a=py::none(),
		"uniform"_a=false,
		"log"_a=py::none(),
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
			auto config = detail::makeLoadConfig(strategy, uniform, log);

			return slughorn::freetype::loadAllFontGlyphs(fontPath, atlas, &config);
		},
		"font_path"_a, "atlas"_a, "strategy"_a=py::none(), "uniform"_a=false, "log"_a=py::none(),
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

		auto config = detail::makeLoadConfig(strategy, uniform, log);

		slughorn::freetype::loadEmojiFont(fontPath, codepoints, atlas, colorGlyphs, &config);

		py::dict result;

		for(auto& [cp, cs] : colorGlyphs) result[py::cast(cp)] = std::move(cs);

		return result;
	},
		"font_path"_a,
		"codepoints"_a,
		"atlas"_a,
		"strategy"_a=py::none(),
		"uniform"_a=false,
		"log"_a=py::none(),
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
		"font_path"_a,
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

		py::enum_<slughorn::nanosvg::ShapePolicy>(nanosvg, "ShapePolicy")
			.value("Default", slughorn::nanosvg::ShapePolicy::Default)
			.value("ForceInclude", slughorn::nanosvg::ShapePolicy::ForceInclude)
			.value("ForceExclude", slughorn::nanosvg::ShapePolicy::ForceExclude)
			.value("GeometryOnly", slughorn::nanosvg::ShapePolicy::GeometryOnly)
			.def("__or__", [](
				slughorn::nanosvg::ShapePolicy a,
				slughorn::nanosvg::ShapePolicy b
			) { return a | b; })
			.def("__ror__", [](
				slughorn::nanosvg::ShapePolicy a,
				slughorn::nanosvg::ShapePolicy b
			) { return a | b; })
		;

		py::class_<slughorn::nanosvg::ShapeRule>(nanosvg, "ShapeRule")
			.def(py::init([](
				const std::string& pattern,
				slughorn::nanosvg::ShapePolicy policy,
				std::optional<slughorn::Atlas::ShapeInfo::Origin> origin
			) {
				return slughorn::nanosvg::ShapeRule{std::regex(pattern), policy, origin};
			}),
			"id"_a,
			"policy"_a=slughorn::nanosvg::ShapePolicy::Default,
			"origin"_a=py::none(),
			"id is a regex matched against each SVG shape's id attribute.\n"
			"policy controls whether matched shapes are force-included, excluded,\n"
			"or stored as geometry-only (curves in atlas, no CompositeShape layer).\n"
			"origin overrides LoadConfig.origin for matched shapes (None = inherit).");

		nanosvg.def("load_file",
			[](
				const std::string& path,
				slughorn::Atlas& atlas,
				slughorn::KeyIterator& keys,
				slug_t dpi,
				std::optional<slughorn::nanosvg::LogCallback> log,
				std::vector<slughorn::nanosvg::ShapeRule> rules,
				bool autoMetrics,
				slughorn::Atlas::ShapeInfo::Origin origin
			) {
				auto config = detail::makeNanosvgLoadConfig(log, autoMetrics, origin);

				config.rules = std::move(rules);

				return slughorn::nanosvg::loadFile(path, atlas, keys, dpi, &config);
			},
			"path"_a,
			"atlas"_a,
			"keys"_a=slughorn::KeyIterator(),
			"dpi"_a=96_cv,
			"log"_a=py::none(),
			"rules"_a=std::vector<slughorn::nanosvg::ShapeRule>(),
			"auto_metrics"_a=true,
			"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
			"Parse an SVG file and pack every filled shape into atlas.\n"
			"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
			"to pack multiple SVGs into the same atlas without key collisions.\n"
			"log(level, msg) is called for warnings (level=1) and errors (level=2); "
			"omit to print to stderr.\n"
			"rules is a list of ShapeRule objects applied in order; first match wins.\n"
			"auto_metrics: when True (default) curves are shifted to local origin and shape\n"
			"metrics are derived from the curve bbox; layer.transform.x/y carries the offset\n"
			"(multiply by image width/height to recover authoring coords). When False, curves\n"
			"are stored as-is in SVG canvas space and layer.transform is zero.\n"
			"origin: global origin for all shapes (overridden per-shape by ShapeRule.origin)."
		);

		nanosvg.def("load_string",
			[](
				const std::string& svg,
				slughorn::Atlas& atlas,
				slughorn::KeyIterator& keys,
				slug_t dpi,
				std::optional<slughorn::nanosvg::LogCallback> log,
				std::vector<slughorn::nanosvg::ShapeRule> rules,
				bool autoMetrics,
				slughorn::Atlas::ShapeInfo::Origin origin
			) {
				auto config = detail::makeNanosvgLoadConfig(log, autoMetrics, origin);

				config.rules = std::move(rules);

				return slughorn::nanosvg::loadString(svg, atlas, keys, dpi, &config);
			},
			"svg"_a,
			"atlas"_a,
			"keys"_a=slughorn::KeyIterator(),
			"dpi"_a=96_cv,
			"log"_a=py::none(),
			"rules"_a=std::vector<slughorn::nanosvg::ShapeRule>(),
			"auto_metrics"_a=true,
			"origin"_a=slughorn::Atlas::ShapeInfo::Origin{},
			"Parse an SVG string and pack every filled shape into atlas.\n"
			"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
			"to pack multiple SVGs into the same atlas without key collisions.\n"
			"log(level, msg) is called for warnings (level=1) and errors (level=2); "
			"omit to print to stderr.\n"
			"rules is a list of ShapeRule objects applied in order; first match wins.\n"
			"auto_metrics: when True (default) curves are shifted to local origin and shape\n"
			"metrics are derived from the curve bbox; layer.transform.x/y carries the offset\n"
			"(multiply by image width/height to recover authoring coords). When False, curves\n"
			"are stored as-is in SVG canvas space and layer.transform is zero.\n"
			"origin: global origin for all shapes (overridden per-shape by ShapeRule.origin)."
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
