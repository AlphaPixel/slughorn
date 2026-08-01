#include "slughorn-python.hpp"

#ifdef SLUGHORN_HAS_SERIAL
#include "slughorn/serial.hpp"
#endif

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

	py::enum_<slughorn::GradientInfo::Type>(gradinfo_, "Type")
		.value("Linear", slughorn::GradientInfo::Type::Linear)
		.value("Radial", slughorn::GradientInfo::Type::Radial)
		.value("Sweep", slughorn::GradientInfo::Type::Sweep)
		.value("AffineRadial", slughorn::GradientInfo::Type::AffineRadial)
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
			"Key of a shape whose MSDF tile is used as coverage. "
			"Required when type == Mask.Type.MSDF."
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
			"  Star: cx, cy, r, points, inner_ratio, rotation"
		)
		.def_readwrite("invert", &slughorn::Mask::invert,
			"If True, inverts coverage so the outside of the mask shape becomes the inside.")
		.def_static("msdf", &slughorn::Mask::msdf,
			py::arg("key"), py::arg("invert") = false,
			"Construct a baked-MSDF mask. key must be requested with atlas.request_msdf().")
		.def_static("circle", &slughorn::Mask::circle,
			py::arg("cx"), py::arg("cy"), py::arg("r"), py::arg("invert") = false,
			"Analytical circle mask: center (cx, cy), radius r.")
		.def_static("rect", &slughorn::Mask::rect,
			py::arg("x"), py::arg("y"), py::arg("w"), py::arg("h"), py::arg("invert") = false,
			"Analytical axis-aligned box mask: corner (x, y), size (w, h).")
		.def_static("capsule", &slughorn::Mask::capsule,
			py::arg("ax"), py::arg("ay"), py::arg("bx"), py::arg("by"), py::arg("r"), py::arg("invert") = false,
			"Analytical capsule mask: endpoints (ax,ay)→(bx,by), radius r.")
		.def_static("arc", &slughorn::Mask::arc,
			py::arg("cx"), py::arg("cy"), py::arg("r"), py::arg("a0"), py::arg("a1"), py::arg("invert") = false,
			"Analytical pie-sector mask: center (cx,cy), radius r, angle range [a0,a1] radians (0=+X, CCW).")
		.def_static("arcBand", &slughorn::Mask::arcBand,
			py::arg("cx"), py::arg("cy"), py::arg("r"), py::arg("a0"), py::arg("a1"), py::arg("rb"), py::arg("invert") = false,
			"Analytical stroked-arc mask: center (cx,cy), arc radius r, angle range [a0,a1], stroke half-width rb.")
		.def_static("hexagon", &slughorn::Mask::hexagon,
			py::arg("cx"), py::arg("cy"), py::arg("r"), py::arg("rotation") = 0.0f, py::arg("invert") = false,
			"Analytical regular-hexagon mask: center (cx,cy), radius r, rotation in radians.")
		.def_static("octagon", &slughorn::Mask::octagon,
			py::arg("cx"), py::arg("cy"), py::arg("r"), py::arg("rotation") = 0.0f, py::arg("invert") = false,
			"Analytical regular-octagon mask: center (cx,cy), radius r, rotation in radians.")
		.def_static("star", &slughorn::Mask::star,
			py::arg("cx"), py::arg("cy"), py::arg("r"),
			py::arg("points"), py::arg("inner_ratio"), py::arg("rotation") = 0.0f,
			py::arg("invert") = false,
			"Analytical n-pointed star mask: center (cx,cy), outer radius r, point count, "
			"inner_ratio in [0,1] (0=sharpest spikes, 1=regular polygon), rotation in radians.")
		.def("__repr__", [](const slughorn::Mask& mk) { return streamRepr(mk); })
	;

	py::enum_<slughorn::Mask::Type>(mask_, "Type")
		.value("MSDF", slughorn::Mask::Type::MSDF)
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
			"-1 if request_msdf() has not been called for this key, or was called pre-build and "
			"is still queued (check after build())."
		)
		.def_readonly("msdf_range", &slughorn::Atlas::Shape::msdfRange,
			"Em-space SDF range used when the MSDF tile was generated. "
			"0.0 if request_msdf() has not rendered a tile for this key yet."
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
		.def_readonly("sdf_tile_count", &slughorn::Atlas::PackingStats::sdfTileCount,
			"Number of shapes with a packed SDF/MSDF atlas tile (0 unless set_sdf_options() was used).")
		.def_readonly("sdf_texels_used", &slughorn::Atlas::PackingStats::sdfTexelsUsed)
		.def_readonly("sdf_texels_padding", &slughorn::Atlas::PackingStats::sdfTexelsPadding)
		.def_readonly("sdf_texels_total", &slughorn::Atlas::PackingStats::sdfTexelsTotal)
		.def_readonly("msdf_layer_count", &slughorn::Atlas::PackingStats::msdfLayerCount,
			"Number of layers registered via request_msdf() (0 unless used).")
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
	;

	atlas_
		.def_property("msdf_tile_size",
			&slughorn::Atlas::getMSDFTileSize,
			&slughorn::Atlas::setMSDFTileSize,
			"Tile size for MSDF tiles (default 128). All layers in a sampler2DArray must be\n"
			"identical - hard GPU constraint. Read any time; write only before the first MSDF\n"
			"tile is actually rendered (see request_msdf()). Setting afterward raises RuntimeError."
		)

		.def("request_msdf",
			[](
				slughorn::Atlas& a,
				slughorn::Key key,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				return a.requestMSDF(key, range, coloring);
			},
			"key"_a, "range"_a=0.1, "coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Opt this shape in to MSDF tile generation. May be called any time -- before build()\n"
			"(the common case, right after the shape itself is authored: queued, and actually\n"
			"rendered inside build() once each shape's packed-atlas position is known) or after\n"
			"build() (rendered immediately, returning the layer index right away).\n"
			"range: em-space SDF spread; controls gradient depth and tile bbox margin.\n"
			"coloring: MSDFEdgeColoring.ByDistance (default, fewer artifacts) or .Simple (faster).\n"
			"Returns the layer index in the resulting Texture2DArray, or -1 if the call was queued\n"
			"(read Shape.msdf_layer after build() to recover it in that case).\n"
			"Shape.msdf_layer and .msdf_range are updated in-place once rendered. Idempotent for\n"
			"repeated keys."
		)

		.def("request_msdf",
			[](
				slughorn::Atlas& a,
				const std::vector<slughorn::Key>& keys,
				slug_t range,
				slughorn::Atlas::MSDFEdgeColoring coloring
			) {
				a.requestMSDF(keys, range, coloring);
			},
			"keys"_a, "range"_a=0.1, "coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Batch overload: request MSDF tiles for a list of keys. May be called any time, same\n"
			"as the single-key overload. Once rendered (either immediately, if already built, or\n"
			"inside build() if requested earlier), tiles are rendered in parallel (when built with\n"
			"SLUGHORN_RENDER_PARALLEL=ON), then committed in deterministic order. Idempotent for\n"
			"already-registered keys."
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
}

}
