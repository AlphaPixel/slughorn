#include "slughorn-python.hpp"

namespace slughorn_python {

void bind_canvas(py::module_& canvas) {

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

		// MSDF opt-in + mask authoring -------------------------------------

#ifdef SLUGHORN_HAS_MSDF
		.def("set_msdf",
			&slughorn::canvas::Canvas::setMSDF,
			"enabled"_a, "range"_a=0.1, "coloring"_a=slughorn::Atlas::MSDFEdgeColoring::ByDistance,
			"Toggle: when enabled, every subsequent fill()/stroke()/text()/text_on_path()\n"
			"commit also requests an MSDF tile for the shape it just registered (see\n"
			"Atlas.request_msdf()) -- no separate post-build registration loop needed.\n"
			"Persists like fill style, same convention as auto_metrics: applies until\n"
			"set_msdf(False) or a new set_msdf() call."
		)
		.def_property_readonly("msdf", &slughorn::canvas::Canvas::getMSDF,
			"Current set_msdf() enabled state."
		)
#endif

		// mask() - MSDF form: commits the current path as a baked mask shape.
		.def("mask",
			py::overload_cast<slug_t, bool>(&slughorn::canvas::Canvas::mask),
			"range"_a=0.1, "invert"_a=false,
			"Commit the current path as an MSDF-baked mask and stage it onto the composite\n"
			"being built (defineShape() semantics -- no Layer pushed). Auto-generates a key,\n"
			"derives cx/cy/r from the path's own canvas-space bbox, and requests its MSDF\n"
			"tile itself -- no separate atlas.request_msdf() call needed afterward.\n"
			"Returns the constructed Mask (an empty Mask if the path was empty)."
		)
		// mask() - procedural/explicit form: stage an already-built Mask.
		.def("mask",
			py::overload_cast<const slughorn::Mask&>(&slughorn::canvas::Canvas::mask),
			"mask"_a,
			"Stage an already-constructed Mask (e.g. slughorn.Mask.circle(...)) onto the\n"
			"composite being built. Procedural types need no atlas registration at all."
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
			"artifact-free GPU tiling via fract()."
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

}
