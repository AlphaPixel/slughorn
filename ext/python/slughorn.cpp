// PYBIND11_MODULE entry point for the slughorn Python module. Deliberately thin: every actual
// binding lives in its own slughorn-*.cpp translation unit (see slughorn-python.hpp for the
// bind_* declarations), each compiled into the slughorn_python_bindings static library
// (CMakeLists.txt). This file just wires the submodules together, which keeps its own compile
// time (and therefore the cost of touching it) trivial, and means a change to one binding group
// no longer forces a full serial recompile of everything else.
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
// slughorn.ShapeContours (CSR view - flat curves buffer + offsets buffer, zero-copy memoryviews)
// slughorn.Atlas (add_shape, add_composite_shape, build, get_shape, get_composite_shape,
// get_shape_contours, has_key, is_built property, curve_texture, band_texture)
// slughorn.CurveDecomposer (owns its Curves internally - safe for Python GC)
//
// slughorn.render / slughorn.canvas / slughorn.emoji (always present)
// slughorn.freetype / slughorn.nanosvg / slughorn.tessellate (present per SLUGHORN_HAS_* build)
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

#include "slughorn-python.hpp"

PYBIND11_MODULE(slughorn, m) {
	m.doc() = "slughorn - GPU-native vector shape renderer (Slug algorithm, Lengyel 2017)";

	slughorn_python::bind_core(m);

	auto m_render = m.def_submodule("render",
		"Software decode and rendering helpers built on top of a compiled slughorn.Atlas.\n\n"
		"Provides a decoded per-shape view plus native reference and banded sample paths."
	);

	slughorn_python::bind_render(m_render);

	auto m_canvas = m.def_submodule("canvas",
		"HTML Canvas-style drawing context for slughorn.\n\n"
		"Build CompositeShapes from 2-D path commands (moveTo, lineTo, quadTo, "
		"bezierTo, closePath) plus arc primitives and convenience shape helpers "
		"(rect, roundedRect, circle, ellipse).\n\n"
		"Each fill() call commits the current path as a new Layer. "
		"Call finalize() to retrieve the completed CompositeShape."
	);

	slughorn_python::bind_canvas(m_canvas);

	auto m_emoji = m.def_submodule("emoji",
		"Unicode 15.1 RGI emoji lookup table (974 single-codepoint entries).\n"
		"Names are CLDR short names, lower-case, spaces replaced with underscores."
	);

	slughorn_python::bind_emoji(m_emoji);

#ifdef SLUGHORN_HAS_FREETYPE
	auto m_freetype = m.def_submodule("freetype",
		"FreeType backend - decompose TrueType/OpenType outlines and COLR emoji "
		"into Atlas shapes.\n\n"
		"High-level functions manage their own FT_Library/FT_Face lifetime; "
		"no FreeType handles are exposed to Python."
	);

	slughorn_python::bind_freetype(m_freetype);
#endif

#ifdef SLUGHORN_HAS_NANOSVG
	auto m_nanosvg = m.def_submodule("nanosvg",
		"NanoSVG backend - parse SVG files or strings into Atlas shapes.\n\n"
		"Produces a CompositeShape with one Layer per filled SVG shape, "
		"back-to-front order preserved.\n\n"
		"Both functions accept an optional KeyIterator that is advanced in-place "
		"as shapes are registered. Pass the same KeyIterator across multiple calls "
		"to pack several SVGs into one atlas without key collisions."
	);

	slughorn_python::bind_nanosvg(m_nanosvg);
#endif

#ifdef SLUGHORN_HAS_TESSELLATE
	auto m_tessellate = m.def_submodule("tessellate",
		"Polygon-with-holes triangulation and linear extrusion (earcut-backed).\n\n"
		"tessellate() and extrude() both accept `contours` as a slughorn.ShapeContours\n"
		"(the return value of Atlas.get_shape_contours())."
	);

	slughorn_python::bind_tessellate(m_tessellate);
#endif

	// auto m_skia = m.def_submodule("skia",
	// "Skia path backend - decompose SkPath objects, stroke-to-fill expansion.");
	// TODO: bind slughorn::skia::decomposePath, strokeToFill, loadShape, etc.

	// auto m_cairo = m.def_submodule("cairo",
	// "Cairo path backend - decompose cairo_t paths.");
	// TODO: bind slughorn::cairo::decomposePath, loadShape.
}
