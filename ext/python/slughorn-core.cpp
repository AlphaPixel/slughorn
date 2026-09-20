#include "slughorn-python.hpp"

#ifdef SLUGHORN_HAS_SERIAL
#include "slughorn/serial.hpp"
#include <sstream>
#endif

namespace {

// Shared by ShapeInfo's "curves" property setter and its kwargs constructor: accepts either a
// (N, 6) float32 buffer (memoryview, numpy array, etc.) or a list of Curve objects.
void assignShapeInfoCurves(slughorn::Atlas::ShapeInfo& info, py::handle obj) {
	if(PyObject_CheckBuffer(obj.ptr())) {
		py::buffer_info bi = py::reinterpret_borrow<py::buffer>(obj).request();

		if(
			bi.format != py::format_descriptor<slug_t>::format() ||
			bi.ndim != 2 ||
			bi.shape[1] != 6
		) throw std::runtime_error("ShapeInfo.curves: expected (N, 6) float32 buffer");

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
}

// CompositeShape's "layers" field is PYBIND11_MAKE_OPAQUE'd (see slughorn-python.hpp) so
// cs.layers.append(x) mutates the real C++ vector in place -- but that opts std::vector<Layer>
// out of pybind's normal implicit list<->vector conversion everywhere it appears as a parameter,
// including here. Duck-type instead: accept a single Layer, or anything iterable (a plain list/
// tuple, or an actual Layers instance, which is itself iterable).
std::vector<slughorn::Layer> layersFromObject(py::object obj) {
	std::vector<slughorn::Layer> layers;

	if(py::isinstance<slughorn::Layer>(obj)) layers.push_back(obj.cast<slughorn::Layer>());
	else for(auto item : obj) layers.push_back(item.cast<slughorn::Layer>());

	return layers;
}

}

namespace slughorn_python {

void bind_core(py::module_& m) {
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
	;

	key_
		// Constructors
		.def(py::init<>(), "Default key: codepoint 0.")
		.def(py::init<uint32_t>(), "codepoint"_a,
			"Construct a Codepoint key from a uint32_t (e.g. ord('A'))."
		)
		.def(py::init<uint32_t, uint8_t>(), "codepoint"_a, "mask"_a,
			"Construct a Codepoint key with an opt-in namespace/mask (0-255) packed\n"
			"into otherwise-unused bits [21..28] of the codepoint -- lets two\n"
			"unrelated sources (e.g. two fonts) register the same raw codepoint\n"
			"without one silently overwriting the other's atlas entry.\n"
			"Key(48, 0) is bit-identical to Key(48)."
		)
		.def(py::init<const std::string&>(), "name"_a,
			"Construct a named key from a string (e.g. Key('logo'))."
		)

		// Accessors
		.def_property_readonly("type", &slughorn::Key::type,
			"KeyType.Codepoint or KeyType.Name."
		)
		.def_property_readonly("codepoint", &slughorn::Key::codepoint,
			"The full raw uint32_t codepoint, mask bits included if any -- "
			"Key(k.codepoint) reconstructs an identical Key. "
			"Only valid when type == KeyType.Codepoint."
		)
		.def_property_readonly("real_codepoint", &slughorn::Key::realCodepoint,
			"The real Unicode codepoint with any mask bits stripped. "
			"Only valid when type == KeyType.Codepoint."
		)
		.def_property_readonly("mask", &slughorn::Key::mask,
			"The caller-defined namespace (0-255); 0 if unset. "
			"Only valid when type == KeyType.Codepoint."
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
		}, "x"_a, "y"_a, "Apply the matrix to point (x, y), returning (x', y').")
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
		}), "t"_a, "color"_a, "Construct from position t in [0,1] and RGBA color.")
		.def_readwrite("t", &slughorn::GradientStop::t, "Position along the gradient axis [0, 1].")
		.def_readwrite("color", &slughorn::GradientStop::color)
		.def("__repr__", [](const slughorn::GradientStop& s) { return streamRepr(s); })
	;

	auto gradinfo_ = py::class_<slughorn::GradientInfo>(m, "GradientInfo");

	// Bound before gradinfo_'s own .def() chain below: the kwargs constructor's "type" default
	// value needs GradientInfo::Type already registered with pybind at the point .def() runs, or
	// converting that default to a py::object fails at module-import time (same reasoning as
	// DrawMode/BlendMode needing to be bound before Layer's ctor, further down, which defaults
	// its drawMode/blendMode params the same way).
	py::enum_<slughorn::GradientInfo::Type>(gradinfo_, "Type")
		.value("Linear", slughorn::GradientInfo::Type::Linear)
		.value("Radial", slughorn::GradientInfo::Type::Radial)
		.value("Sweep", slughorn::GradientInfo::Type::Sweep)
		.value("AffineRadial", slughorn::GradientInfo::Type::AffineRadial)
	;

	gradinfo_
		.def(py::init<>(), "Default: linear gradient, no stops.")
		.def(
			py::init([](
				slughorn::GradientInfo::Type type,
				std::vector<slughorn::GradientStop> stops,
				slughorn::Matrix transform,
				slug_t innerRadius,
				slug_t startAngle,
				slug_t endAngle
			) {
				slughorn::GradientInfo info;

				info.type = type;
				info.stops = std::move(stops);
				info.transform = transform;
				info.innerRadius = innerRadius;
				info.startAngle = startAngle;
				info.endAngle = endAngle;

				return info;
			}),
			"type"_a=slughorn::GradientInfo::Type::Linear,
			"stops"_a=std::vector<slughorn::GradientStop>{},
			"transform"_a=slughorn::Matrix{},
			"inner_radius"_a=0_cv,
			"start_angle"_a=0_cv,
			"end_angle"_a=1_cv,
			"Construct with optional field=value kwargs, e.g. "
			"GradientInfo(type=slughorn.GradientInfo.Type.Radial, stops=[...])."
		)
		.def_readwrite("type", &slughorn::GradientInfo::type,
			"GradientInfo.Type.Linear, .Radial, .AffineRadial, or .Sweep."
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
		.value("DstOver", slughorn::BlendMode::DstOver)
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
			"Key of a shape whose baked SDF/MSDF tile is used as coverage. "
			"Required when type == Mask.Type.SDFTile."
		)
		.def_readwrite("type", &slughorn::Mask::type)
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
			"  Circle: cx, cy, r\n"
			"  Rect: x, y, w, h\n"
			"  Capsule: ax, ay, bx, by, r\n"
			"  Arc: cx, cy, r, angle_start, angle_end\n"
			"  ArcBand: cx, cy, r, angle_start, angle_end, stroke_half_width\n"
			"  Hexagon: cx, cy, r, rotation\n"
			"  Octagon: cx, cy, r, rotation\n"
			"  Star: cx, cy, r, points, inner_ratio, rotation\n"
			"  SDFTile: ox, oy, scale (canvas-space position of the tile shape's em origin; scale about its center)"
		)
		.def_readwrite("invert", &slughorn::Mask::invert,
			"If True, inverts coverage so the outside of the mask shape becomes the inside.")
		.def_static("sdf_tile", &slughorn::Mask::sdfTile,
			"key"_a, "invert"_a=false,
			"Construct a baked SDF/MSDF tile mask. key must be requested with atlas.request_sdf()\n"
			"before build(); the tile carries its own em-space frame, so the only params are\n"
			"params[0..1] = ox, oy, the canvas-space position of the shape's em-space origin\n"
			"(Canvas.mask() fills these in; default 0, 0) and params[2] = scale about the tile's\n"
			"own center (default 1) - the cheap way to animate a baked mask.")
		.def_static("circle", &slughorn::Mask::circle,
			"cx"_a, "cy"_a, "r"_a, "invert"_a=false,
			"Analytical circle mask: center (cx, cy), radius r.")
		.def_static("rect", &slughorn::Mask::rect,
			"x"_a, "y"_a, "w"_a, "h"_a, "invert"_a=false,
			"Analytical axis-aligned box mask: corner (x, y), size (w, h).")
		.def_static("capsule", &slughorn::Mask::capsule,
			"ax"_a, "ay"_a, "bx"_a, "by"_a, "r"_a, "invert"_a=false,
			"Analytical capsule mask: endpoints (ax,ay)→(bx,by), radius r.")
		.def_static("arc", &slughorn::Mask::arc,
			"cx"_a, "cy"_a, "r"_a, "a0"_a, "a1"_a, "invert"_a=false,
			"Analytical pie-sector mask: center (cx,cy), radius r, angle range [a0,a1] radians (0=+X, CCW).")
		.def_static("arcBand", &slughorn::Mask::arcBand,
			"cx"_a, "cy"_a, "r"_a, "a0"_a, "a1"_a, "rb"_a, "invert"_a=false,
			"Analytical stroked-arc mask: center (cx,cy), arc radius r, angle range [a0,a1], stroke half-width rb.")
		.def_static("hexagon", &slughorn::Mask::hexagon,
			"cx"_a, "cy"_a, "r"_a, "rotation"_a=0.0f, "invert"_a=false,
			"Analytical regular-hexagon mask: center (cx,cy), radius r, rotation in radians.")
		.def_static("octagon", &slughorn::Mask::octagon,
			"cx"_a, "cy"_a, "r"_a, "rotation"_a=0.0f, "invert"_a=false,
			"Analytical regular-octagon mask: center (cx,cy), radius r, rotation in radians.")
		.def_static("star", &slughorn::Mask::star,
			"cx"_a, "cy"_a, "r"_a,
			"points"_a, "inner_ratio"_a, "rotation"_a=0.0f,
			"invert"_a=false,
			"Analytical n-pointed star mask: center (cx,cy), outer radius r, point count, "
			"inner_ratio in [0,1] (0=sharpest spikes, 1=regular polygon), rotation in radians.")
		.def("__repr__", [](const slughorn::Mask& mk) { return streamRepr(mk); })
	;

	py::enum_<slughorn::Mask::Type>(mask_, "Type")
		.value("SDFTile", slughorn::Mask::Type::SDFTile)
		.value("Circle", slughorn::Mask::Type::Circle)
		.value("Rect", slughorn::Mask::Type::Rect)
		.value("Capsule", slughorn::Mask::Type::Capsule)
		.value("Arc", slughorn::Mask::Type::Arc)
		.value("ArcBand", slughorn::Mask::Type::ArcBand)
		.value("Hexagon", slughorn::Mask::Type::Hexagon)
		.value("Octagon", slughorn::Mask::Type::Octagon)
		.value("Star", slughorn::Mask::Type::Star)
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
				slug_t bleed,
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
				layer.bleed = bleed;
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
			"bleed"_a=0_cv,
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
		.def_readwrite("bleed", &slughorn::Layer::bleed,
			"Extra em-space CONTENT margin on each side of the quad (default 0), for effects "
			"that intentionally draw outside the shape's true bounds (outer glow, drop shadow, "
			"MSDF spread) - print's 'bleed'. NOT an antialiasing margin: the AA margin is the "
			"renderer's responsibility, computed live at pixel scale in the vertex stage.")
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
		.def(
			py::init([](
				py::object layers,
				slug_t advance,
				std::optional<slughorn::Mask> mask
			) {
				slughorn::CompositeShape composite;

				composite.layers = layersFromObject(layers);
				composite.advance = advance;
				composite.mask = std::move(mask);

				return composite;
			}),
			"layers"_a=py::list(),
			"advance"_a=0_cv,
			"mask"_a=py::none(),
			"Construct with optional field=value kwargs, e.g. "
			"CompositeShape(layers=[layer0, layer1], advance=0.6). "
			"layers accepts a single Layer, a list/tuple of Layer, or a Layers instance."
		)
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
		.def(
			py::init([](
				slug_t unitsPerEM,
				slug_t capHeightRatio,
				slug_t xHeightRatio,
				slug_t ascenderRatio,
				slug_t descenderRatio,
				slug_t lineGapRatio
			) {
				slughorn::FontMetrics fm;

				fm.unitsPerEM = unitsPerEM;
				fm.capHeightRatio = capHeightRatio;
				fm.xHeightRatio = xHeightRatio;
				fm.ascenderRatio = ascenderRatio;
				fm.descenderRatio = descenderRatio;
				fm.lineGapRatio = lineGapRatio;

				return fm;
			}),
			"units_per_em"_a=0_cv,
			"cap_height_ratio"_a=0_cv,
			"x_height_ratio"_a=0_cv,
			"ascender_ratio"_a=0_cv,
			"descender_ratio"_a=0_cv,
			"line_gap_ratio"_a=0_cv,
			"Construct with optional field=value kwargs, for supplying synthetic metrics "
			"without a real font file, e.g. FontMetrics(cap_height_ratio=0.7)."
		)
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
		.def(py::init([](py::kwargs kwargs) {
			slughorn::Atlas::ShapeInfo info;

			for(auto item : kwargs) {
				auto key = item.first.cast<std::string>();

				if(key == "curves") assignShapeInfoCurves(info, item.second);
				else if(key == "auto_metrics") info.autoMetrics = item.second.cast<bool>();
				else if(key == "bearing_x") info.bearingX = item.second.cast<slug_t>();
				else if(key == "bearing_y") info.bearingY = item.second.cast<slug_t>();
				else if(key == "width") info.width = item.second.cast<slug_t>();
				else if(key == "height") info.height = item.second.cast<slug_t>();
				else if(key == "advance") info.advance = item.second.cast<slug_t>();
				else if(key == "num_bands_x") info.numBandsX = item.second.cast<int>();
				else if(key == "num_bands_y") info.numBandsY = item.second.cast<int>();
				else if(key == "splits_x") {
					info.splitsX = item.second.cast<std::vector<slug_t>>();
				}
				else if(key == "splits_y") {
					info.splitsY = item.second.cast<std::vector<slug_t>>();
				}
				else if(key == "origin") {
					info.origin = item.second.cast<slughorn::Atlas::ShapeInfo::Origin>();
				}
				else throw py::type_error("ShapeInfo got an unexpected keyword argument '" + key + "'");
			}

			return info;
		}),
			"Construct with optional field=value kwargs, e.g. "
			"ShapeInfo(curves=my_curves, auto_metrics=False)."
		)
		.def_property("curves",
			[](const slughorn::Atlas::ShapeInfo& info) { return info.curves; },
			[](slughorn::Atlas::ShapeInfo& info, py::object obj) {
				assignShapeInfoCurves(info, obj);
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
	;

	// ============================================================================================
	// slughorn.Shape (Atlas::Shape in C++, flat in Python - read-only)
	// ============================================================================================
	// ============================================================================================
	// slughorn.SDF (Atlas::SDF in C++, flat in Python) - baked distance-field tiles
	// ============================================================================================
	auto sdf_ = py::class_<slughorn::Atlas::SDF>(m, "SDF",
		"The Atlas's baked SDF/MSDF tile texture and its Config; see Atlas.set_sdf() and\n"
		"Atlas.request_sdf(). Each shape's tile lives on Shape.sdf."
	);

	// Enums first: pybind11 evaluates default argument values at .def() time.
	py::enum_<slughorn::Atlas::SDF::Type>(sdf_, "Type",
		"SDF: one channel (read .r). MSDF: three channels (median of .rgb); keeps sharp corners."
	)
		.value("SDF", slughorn::Atlas::SDF::Type::SDF)
		.value("MSDF", slughorn::Atlas::SDF::Type::MSDF)
	;

	py::enum_<slughorn::Atlas::SDF::Coloring>(sdf_, "Coloring",
		"MSDF edge-coloring algorithm. ByDistance: fewer corner artifacts, slightly more CPU\n"
		"work (recommended default). Simple: faster, prone to artifacts at acute convex corners."
	)
		.value("Simple", slughorn::Atlas::SDF::Coloring::Simple)
		.value("ByDistance", slughorn::Atlas::SDF::Coloring::ByDistance)
	;

	py::class_<slughorn::Atlas::SDF::Config>(sdf_, "Config",
		"Atlas-wide SDF baking options; pass to Atlas.set_sdf() before build()."
	)
		.def(py::init<>())
		.def(py::init([](
			slughorn::Atlas::SDF::Type type,
			uint32_t tileSize,
			uint32_t atlasWidth,
			uint32_t gutter,
			slug_t range,
			slughorn::Atlas::SDF::Coloring coloring
		) {
			slughorn::Atlas::SDF::Config c;

			c.type = type;
			c.tileSize = tileSize;
			c.atlasWidth = atlasWidth;
			c.gutter = gutter;
			c.range = range;
			c.coloring = coloring;

			return c;
		}),
			"type"_a=slughorn::Atlas::SDF::Type::MSDF,
			"tile_size"_a=128,
			"atlas_width"_a=2048,
			"gutter"_a=2,
			"range"_a=0.1,
			"coloring"_a=slughorn::Atlas::SDF::Coloring::ByDistance
		)
		.def_readwrite("type", &slughorn::Atlas::SDF::Config::type,
			"SDF.Type.SDF or SDF.Type.MSDF (default). One kind per Atlas.")
		.def_readwrite("tile_size", &slughorn::Atlas::SDF::Config::tileSize,
			"Longest tile axis, in texels (default 128).")
		.def_readwrite("atlas_width", &slughorn::Atlas::SDF::Config::atlasWidth,
			"Texture width in texels (default 2048); the atlas grows in height as tiles shelf-pack.")
		.def_readwrite("gutter", &slughorn::Atlas::SDF::Config::gutter,
			"Texels of exterior kept around every tile (default 2).")
		.def_readwrite("range", &slughorn::Atlas::SDF::Config::range,
			"Default em-space distance range (default 0.1); request_sdf() may override per shape.")
		.def_readwrite("coloring", &slughorn::Atlas::SDF::Config::coloring,
			"MSDF only. SDF.Coloring.ByDistance (default) or .Simple.")
	;

	py::class_<slughorn::Atlas::SDF::Tile>(sdf_, "Tile",
		"One shape's baked tile: texels [x, x+w) x [y, y+h) of the SDF texture (row 0 = BOTTOM),\n"
		"a single uniform scale (texels_per_em) and the em-space point at its bottom-left corner.\n"
		"Values are clamped to [0, 1]: edge = 0.5, interior > 0.5; +/-range em spans [0, 1]."
	)
		.def_readonly("x", &slughorn::Atlas::SDF::Tile::x)
		.def_readonly("y", &slughorn::Atlas::SDF::Tile::y)
		.def_readonly("w", &slughorn::Atlas::SDF::Tile::w)
		.def_readonly("h", &slughorn::Atlas::SDF::Tile::h)
		.def_readonly("range", &slughorn::Atlas::SDF::Tile::range,
			"Em-space half-range this tile was baked with.")
		.def_readonly("texels_per_em", &slughorn::Atlas::SDF::Tile::texelsPerEm)
		.def_property_readonly("em_origin", [](const slughorn::Atlas::SDF::Tile& t) {
			return py::make_tuple(t.emOriginX, t.emOriginY);
		}, "Em-space (x, y) at the tile's bottom-left corner.")
		.def_property_readonly("pixel_range", &slughorn::Atlas::SDF::Tile::pixelRange,
			"Total distance range in texels (2 * range * texels_per_em) - msdfgen calls pixelRange.")
		.def("__repr__", [](const slughorn::Atlas::SDF::Tile& t) {
			return "SDF.Tile(x=" + std::to_string(t.x) + ", y=" + std::to_string(t.y)
				+ ", w=" + std::to_string(t.w) + ", h=" + std::to_string(t.h) + ")";
		})
	;

	py::class_<slughorn::Atlas::SDF::Stats>(sdf_, "Stats")
		.def_readonly("type", &slughorn::Atlas::SDF::Stats::type)
		.def_property_readonly("format", [](const slughorn::Atlas::SDF::Stats& st) {
			return streamRepr(st.format);
		}, "Texture format string: 'R32F' (SDF) or 'RGB32F' (MSDF).")
		.def_readonly("tile_count", &slughorn::Atlas::SDF::Stats::tileCount)
		.def_readonly("texels_used", &slughorn::Atlas::SDF::Stats::texelsUsed)
		.def_readonly("texels_padding", &slughorn::Atlas::SDF::Stats::texelsPadding)
		.def_readonly("texels_total", &slughorn::Atlas::SDF::Stats::texelsTotal)
		.def("utilization", &slughorn::Atlas::SDF::Stats::utilization)
		.def("padding_ratio", &slughorn::Atlas::SDF::Stats::paddingRatio)
		.def("bytes", &slughorn::Atlas::SDF::Stats::bytes)
	;

	sdf_
		.def_readonly("config", &slughorn::Atlas::SDF::config)
		.def_readonly("texture", &slughorn::Atlas::SDF::texture,
			"The tile atlas as a TextureData (R32F for SDF, RGB32F for MSDF). Empty if nothing was baked.")
		.def_property_readonly("empty", [](const slughorn::Atlas::SDF& sdf) {
			return sdf.texture.empty();
		}, "True if no tile has been baked.")
	;

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

		.def_property_readonly("sdf", [](const slughorn::Atlas::Shape& shape) {
			return shape.sdf;
		}, "This shape's baked SDF.Tile, or None if request_sdf() was never called for it (or it\n"
			"has no geometry). Valid after build()."
		)

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
			"transform"_a, "scale"_a=1_cv,
			"Compute the world-space bounding quad for this shape. The returned quad is the "
			"TRUE authored quad - no padding, no margin, ever. Renderers add any AA/bleed "
			"room downstream without disturbing these coordinates."
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
				case slughorn::Atlas::TextureData::Format::RGBA16F: return "RGBA16F";
				case slughorn::Atlas::TextureData::Format::RGBA16UI: return "RGBA16UI";
				case slughorn::Atlas::TextureData::Format::RG16UI: return "RG16UI";
				case slughorn::Atlas::TextureData::Format::RGBA8: return "RGBA8";
				case slughorn::Atlas::TextureData::Format::RGB32F: return "RGB32F";
				case slughorn::Atlas::TextureData::Format::R32F: return "R32F";
			}
			return "unknown";
		}, "String: 'RGBA16F' (curve), 'RG16UI' (band), 'RGBA8' (gradient), 'R32F'/'RGB32F' (SDF/MSDF tile atlas).")
		.def_property_readonly("bytes", [](const slughorn::Atlas::TextureData& td) {
			return bytesView(td.bytes);
		}, "Zero-copy memoryview of the raw pixel data (row-major). "
			"Keep the Atlas alive for the duration of any view."
		)
		.def("__repr__", [](const slughorn::Atlas::TextureData& td) { return streamRepr(td); })
	;

	// ============================================================================================
	// slughorn.ShapeContours (PyShapeContours - CSR view, returned by Atlas.get_shape_contours())
	// ============================================================================================
	py::class_<PyShapeContours>(m, "ShapeContours")
		.def_property_readonly("curves", [](const PyShapeContours& c) {
			return curveView2D(c.curves);
		}, "Zero-copy (N, 6) float32 memoryview of every curve across every contour, in order.")
		.def_property_readonly("offsets", [](const PyShapeContours& c) {
			return vectorView1D(c.offsets);
		}, "Zero-copy CSR row offsets into `curves`, length len(self) + 1.\n"
			"Contour i is curves[offsets[i]:offsets[i + 1]]."
		)
		.def("__len__", [](const PyShapeContours& c) {
			return c.offsets.empty() ? size_t(0) : c.offsets.size() - 1;
		}, "Number of contours.")
		.def("__getitem__", [](const PyShapeContours& c, py::ssize_t i) {
			const auto n = static_cast<py::ssize_t>(c.offsets.empty() ? 0 : c.offsets.size() - 1);

			if(i < 0) i += n;
			if(i < 0 || i >= n) throw py::index_error("ShapeContours index out of range");

			const auto start = c.offsets[static_cast<size_t>(i)];
			const auto count = c.offsets[static_cast<size_t>(i) + 1] - start;

			// A FRESH sub-view built directly from a C++ pointer (same helper .curves itself
			// uses), not a Python-level slice of an existing memoryview -- CPython's built-in
			// memoryview does not support slicing a multi-dimensional buffer
			// ("NotImplementedError: multi-dimensional sub-views are not implemented"), so
			// contours.curves[offsets[i]:offsets[i + 1]] can never work no matter how callers
			// write it. This sidesteps that entirely: no slicing happens, ever.
			//
			// TODO: Investigate wrapping in a custom pybind11 type (e.g. ContourCurves) with its
			// own __getitem__/__len__ instead of returning a raw memoryview -- would let callers
			// do `for row in contour: x1, y1, ... = row` directly (memoryview's OWN __getitem__
			// still can't do this for ndim>1, see the comment above; a type we control could).
			// Not worth it yet for bin/slughorn's one debug-tool call site (see
			// [[feedback-pybind11-buffer]] for the memoryview.cast() workaround in use there) --
			// revisit if this same need shows up on a hot path.
			return flatView2D<slughorn::slug_t>(
				static_cast<const void*>(c.curves.data() + start), count, 6, sizeof(slughorn::Atlas::Curve)
			);
		}, "Zero-copy (K, 6) float32 memoryview of contour i's own curves. Supports negative "
			"indices. Combined with __len__, this also makes ShapeContours directly iterable "
			"(`for contour in shape_contours:`) via Python's sequence protocol."
		)
		.def("__repr__", [](const PyShapeContours& c) {
			return "ShapeContours("
				+ std::to_string(c.offsets.empty() ? size_t(0) : c.offsets.size() - 1)
				+ " contours, " + std::to_string(c.curves.size()) + " curves)"
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
		.def_readonly("band_max_count", &slughorn::Atlas::PackingStats::bandMaxCount,
			"Largest single band's curve-index list across all shapes (hard limit 65535).")
		.def_readonly("band_max_offset", &slughorn::Atlas::PackingStats::bandMaxOffset,
			"Largest per-shape cumulative band-data span (hard limit 65535).")
		.def_readonly("gradient_count", &slughorn::Atlas::PackingStats::gradientCount,
			"Number of registered gradients (0 when none).")
		.def_readonly("gradient_texels_total", &slughorn::Atlas::PackingStats::gradientTexelsTotal,
			"Total gradient texture texels (GRADIENT_STRIP_WIDTH * gradient_count).")
		.def_readonly("sdf", &slughorn::Atlas::PackingStats::sdf,
			"SDF.Stats for the baked tile atlas (all zero unless request_sdf() was used).")
		.def("curve_utilization", &slughorn::Atlas::PackingStats::curveUtilization)
		.def("band_utilization", &slughorn::Atlas::PackingStats::bandUtilization)
		.def("curve_padding_ratio", &slughorn::Atlas::PackingStats::curvePaddingRatio)
		.def("band_padding_ratio", &slughorn::Atlas::PackingStats::bandPaddingRatio)
		.def("curve_bytes", &slughorn::Atlas::PackingStats::curveBytes)
		.def("band_bytes", &slughorn::Atlas::PackingStats::bandBytes)
		.def("gradient_bytes", &slughorn::Atlas::PackingStats::gradientBytes)
		.def("total_bytes", &slughorn::Atlas::PackingStats::totalBytes,
			"Total GPU memory across every channel, in bytes "
			"(curve + band + gradient + SDF tile atlas).")
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

		.def("append", &slughorn::Atlas::append,
			"source"_a, "mask"_a=0, "name_prefix"_a="",
			"Merge another, already-built Atlas's shapes and composites into this one.\n\n"
			"Must be called before build() (silently no-ops if this atlas is already\n"
			"built). source must itself already be built - e.g. the result of\n"
			"slughorn.read().\n\n"
			"mask namespaces Codepoint keys via Key's existing bit-packed mask field;\n"
			"the real codepoint (and any auto-key marker bits) pass through untouched.\n"
			"mask=0 is fine for a real, bounded-codepoint source (a font); pick a\n"
			"distinct mask per source for auto-keyed shape libraries, which have no\n"
			"safe default.\n\n"
			"name_prefix independently namespaces Name keys as 'name_prefix:name'.\n"
			"Empty (the default) leaves Name keys unprefixed."
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

		.def("set_curve_texture_format",
			[](slughorn::Atlas& a, const std::string& fmt) {
				slughorn::Atlas::TextureData::Format f;

				if(fmt == "RGBA32F") f = slughorn::Atlas::TextureData::Format::RGBA32F;
				else if(fmt == "RGBA16F") f = slughorn::Atlas::TextureData::Format::RGBA16F;
				else throw std::invalid_argument("expected 'RGBA32F' or 'RGBA16F', got '" + fmt + "'");

				a.setCurveTextureFormat(f);
			},
			"format"_a,
			"Select the curve texture's storage format: 'RGBA32F' (default, full precision) or\n"
			"'RGBA16F' (matches the reference Slug format, halves curve-texture memory, real\n"
			"precision tradeoff -- can visibly degrade shapes under heavy zoom). Must be called\n"
			"before build(); no-op after."
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
				PyShapeContours result;

				result.offsets.push_back(0);

				for(const auto& contour : a.getShapeContours(key)) {
					result.curves.insert(result.curves.end(), contour.begin(), contour.end());
					result.offsets.push_back(static_cast<uint32_t>(result.curves.size()));
				}

				return result;
			},
			"key"_a,
			"Return a ShapeContours: a flat (N, 6) curve buffer plus CSR row offsets splitting\n"
			"it into per-contour ranges (contour i = curves[offsets[i]:offsets[i + 1]]).\n"
			"Contour breaks are detected where p3 of curve[i] != p1 of curve[i+1].\n"
			"Zero contours if the key is not found. Use when building paths for stroking,\n"
			"filling, or triangulation - each contour must be handled independently."
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
			"TextureData for the RGBA16F curve texture (valid after build())."
		)

		.def_property_readonly("band_texture",
			[](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
				return a.getBandTextureData();
			},
			py::return_value_policy::reference_internal,
			"TextureData for the RG16UI band texture (valid after build())."
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

	atlas_
		.def("set_sdf", &slughorn::Atlas::setSDF,
			"config"_a,
			"Configure SDF baking for the whole Atlas (kind, tile size, atlas width, gutter, default\n"
			"range, MSDF coloring). Optional -- the SDF.Config defaults apply otherwise. Call any\n"
			"time before build(); raises RuntimeError afterward."
		)

		.def("request_sdf",
			[](slughorn::Atlas& a, slughorn::Key key, std::optional<slug_t> range) {
				a.requestSDF(key, range);
			},
			"key"_a, "range"_a=py::none(),
			"Opt this shape in to SDF/MSDF tile baking. Must be called BEFORE build(), which renders\n"
			"every requested tile and fills in Shape.sdf and Atlas.sdf. range: em-space distance\n"
			"range, defaulting to SDF.Config.range. Idempotent per key (the first range wins); a\n"
			"shape with no geometry simply gets no tile. Raises RuntimeError after build() or when\n"
			"slughorn was built without SLUGHORN_SDF=ON."
		)

		.def("request_sdf",
			[](slughorn::Atlas& a, const std::vector<slughorn::Key>& keys, std::optional<slug_t> range) {
				a.requestSDF(keys, range);
			},
			"keys"_a, "range"_a=py::none(),
			"Batch overload: request tiles for a list of keys. All keys are validated first, so a\n"
			"bad key leaves nothing half-requested."
		)

		.def_property_readonly("sdf", &slughorn::Atlas::getSDF,
			py::return_value_policy::reference_internal,
			"The baked SDF result (config + tile texture). Per-shape tiles are on Shape.sdf."
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

	m.def("read_string",
		[](const std::string& json) {
			std::istringstream in(json);

			return slughorn::serial::read(in);
		},
		"json"_a,
		"Load an already-built Atlas from an in-memory .slug JSON string - never touches\n"
		"disk. For a JSON blob embedded as a compile-time string literal (e.g. a fallback\n"
		"font or icon library baked into a header), this is the counterpart to write_string()\n"
		"and the intended source for Atlas.append().\n"
		"Raises RuntimeError if the JSON is invalid.\n"
		"Only available when slughorn was compiled with SLUGHORN_SERIAL=ON."
	);

	m.def("write_string",
		[](const slughorn::Atlas& atlas, bool pretty) {
			std::ostringstream out;

			slughorn::serial::writeJSON(atlas, out, pretty);

			return out.str();
		},
		"atlas"_a, "pretty"_a=true,
		"Serialize a built Atlas to an in-memory .slug JSON string - never touches disk.\n"
		"Counterpart to read_string(); useful for embedding the result as a compile-time\n"
		"string literal (e.g. via a bin/slughorn codegen step).\n"
		"Raises RuntimeError if the atlas is not built.\n"
		"Only available when slughorn was compiled with SLUGHORN_SERIAL=ON."
	);
#endif
}

}
