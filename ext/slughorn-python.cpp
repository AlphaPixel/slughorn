// =============================================================================
// slughorn_python.cpp — pybind11 bindings for slughorn
//
// Covers the core slughorn.hpp API:
//   slughorn.Color, slughorn.Matrix
//   slughorn.ColorLayer, slughorn.ColorGlyph
//   slughorn.Atlas  (Curve, ShapeInfo, Shape, TextureData, + full Atlas class)
//   slughorn.CurveDecomposer
//   slughorn.emoji  (submodule — nameToCodepoint, codepointToName, etc.)
//
// Texture data is exposed as py::memoryview (zero-copy, numpy-compatible).
//
// Backend submodules (ft2, skia, cairo) are stubbed at the bottom — uncomment
// and implement when you add those bindings.
//
// BUILD
// -----
// CMakeLists.txt snippet:
//
//   find_package(pybind11 REQUIRED)
//   add_library(slughorn_py MODULE slughorn_python.cpp slughorn.cpp)
//   target_link_libraries(slughorn_py PRIVATE pybind11::module slughorn)
//   set_target_properties(slughorn_py PROPERTIES
//       OUTPUT_NAME "slughorn"
//       PREFIX      ""
//   )
//
// Then: import slughorn  in Python.
//
// OWNERSHIP NOTES
// ---------------
// Atlas is heap-allocated and managed by shared_ptr so that Python's GC and
// C++ ref-counting cooperate safely.  ShapeInfo / Shape / TextureData are
// copied across the boundary (they are value types).
//
// memoryview returned from get_curve_texture_data() / get_band_texture_data()
// borrows from the Atlas's internal buffer — keep the Atlas alive for the
// duration of any view over its data.
// =============================================================================

#define SLUGHORN_EMOJI_IMPLEMENTATION
#include "slughorn.hpp"
#include "slughorn-emoji.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>       // std::vector, std::optional, std::map
#include <pybind11/functional.h>

namespace py = pybind11;

// =============================================================================
// Helpers
// =============================================================================

// Return a zero-copy memoryview over a vector<uint8_t>.
// The vector must outlive the memoryview — caller's responsibility.
static py::memoryview bytesView(const std::vector<uint8_t>& v) {
    return py::memoryview::from_memory(
        const_cast<uint8_t*>(v.data()),
        static_cast<py::ssize_t>(v.size())
    );
}

// =============================================================================
// Module definition
// =============================================================================

PYBIND11_MODULE(slughorn, m) {
    m.doc() = "slughorn — GPU-native vector shape renderer (Slug algorithm)";

    // =========================================================================
    // slughorn.Color
    // =========================================================================
    py::class_<slughorn::Color>(m, "Color")
        .def(py::init<>())
        .def(py::init([](slug_t r, slug_t g, slug_t b, slug_t a) {
            return slughorn::Color{r, g, b, a};
        }), py::arg("r"), py::arg("g"), py::arg("b"), py::arg("a") = 1.0f)
        .def_readwrite("r", &slughorn::Color::r)
        .def_readwrite("g", &slughorn::Color::g)
        .def_readwrite("b", &slughorn::Color::b)
        .def_readwrite("a", &slughorn::Color::a)
        .def("__repr__", [](const slughorn::Color& c) {
            return "Color(r=" + std::to_string(c.r) +
                   ", g=" + std::to_string(c.g) +
                   ", b=" + std::to_string(c.b) +
                   ", a=" + std::to_string(c.a) + ")";
        })
        .def("to_tuple", [](const slughorn::Color& c) {
            return py::make_tuple(c.r, c.g, c.b, c.a);
        }, "Return (r, g, b, a) as a Python tuple.")
    ;

    // =========================================================================
    // slughorn.Matrix
    // =========================================================================
    py::class_<slughorn::Matrix>(m, "Matrix")
        .def(py::init<>())
        .def_static("identity", &slughorn::Matrix::identity)
        .def_readwrite("xx", &slughorn::Matrix::xx)
        .def_readwrite("yx", &slughorn::Matrix::yx)
        .def_readwrite("xy", &slughorn::Matrix::xy)
        .def_readwrite("yy", &slughorn::Matrix::yy)
        .def_readwrite("dx", &slughorn::Matrix::dx)
        .def_readwrite("dy", &slughorn::Matrix::dy)
        .def("is_identity", &slughorn::Matrix::isIdentity)
        .def("apply", [](const slughorn::Matrix& mat, slug_t x, slug_t y) {
            slug_t ox, oy;
            mat.apply(x, y, ox, oy);
            return py::make_tuple(ox, oy);
        }, py::arg("x"), py::arg("y"),
           "Apply the matrix to a point, returning (x', y').")
        .def("__mul__", &slughorn::Matrix::operator*, py::arg("rhs"),
            "Concatenate two matrices (rhs applied first).")
        .def("__repr__", [](const slughorn::Matrix& mat) {
            return "Matrix(xx=" + std::to_string(mat.xx) +
                   " xy=" + std::to_string(mat.xy) +
                   " yx=" + std::to_string(mat.yx) +
                   " yy=" + std::to_string(mat.yy) +
                   " dx=" + std::to_string(mat.dx) +
                   " dy=" + std::to_string(mat.dy) + ")";
        })
    ;

    // =========================================================================
    // slughorn.ColorLayer / slughorn.ColorGlyph
    // =========================================================================
    py::class_<slughorn::ColorLayer>(m, "ColorLayer")
        .def(py::init<>())
        .def_readwrite("key",   &slughorn::ColorLayer::key)
        .def_readwrite("color", &slughorn::ColorLayer::color)
        .def("__repr__", [](const slughorn::ColorLayer& l) {
            return "ColorLayer(key=0x" + [&]{
                char buf[16]; snprintf(buf, sizeof(buf), "%X", l.key); return std::string(buf);
            }() + ")";
        })
    ;

    py::class_<slughorn::ColorGlyph>(m, "ColorGlyph")
        .def(py::init<>())
        .def_readwrite("layers",  &slughorn::ColorGlyph::layers)
        .def_readwrite("advance", &slughorn::ColorGlyph::advance)
        .def("__len__", [](const slughorn::ColorGlyph& g) {
            return g.layers.size();
        })
        .def("__repr__", [](const slughorn::ColorGlyph& g) {
            return "ColorGlyph(" + std::to_string(g.layers.size()) + " layers)";
        })
    ;

    // =========================================================================
    // slughorn.Atlas  (nested types first, then the class itself)
    // =========================================================================

    // -- Atlas.Curve ----------------------------------------------------------
    py::class_<slughorn::Atlas::Curve>(m, "Curve")
        .def(py::init<>())
        .def(py::init([](slug_t x1, slug_t y1,
                         slug_t x2, slug_t y2,
                         slug_t x3, slug_t y3) {
            return slughorn::Atlas::Curve{x1, y1, x2, y2, x3, y3};
        }), py::arg("x1"), py::arg("y1"),
            py::arg("x2"), py::arg("y2"),
            py::arg("x3"), py::arg("y3"))
        .def_readwrite("x1", &slughorn::Atlas::Curve::x1)
        .def_readwrite("y1", &slughorn::Atlas::Curve::y1)
        .def_readwrite("x2", &slughorn::Atlas::Curve::x2)
        .def_readwrite("y2", &slughorn::Atlas::Curve::y2)
        .def_readwrite("x3", &slughorn::Atlas::Curve::x3)
        .def_readwrite("y3", &slughorn::Atlas::Curve::y3)
        .def("__repr__", [](const slughorn::Atlas::Curve& c) {
            return "Curve((" + std::to_string(c.x1) + "," + std::to_string(c.y1) +
                   ") ctrl=(" + std::to_string(c.x2) + "," + std::to_string(c.y2) +
                   ") end=(" + std::to_string(c.x3) + "," + std::to_string(c.y3) + "))";
        })
    ;

    // -- Atlas.ShapeInfo ------------------------------------------------------
    py::class_<slughorn::Atlas::ShapeInfo>(m, "ShapeInfo")
        .def(py::init<>())
        .def_readwrite("curves",       &slughorn::Atlas::ShapeInfo::curves)
        .def_readwrite("auto_metrics", &slughorn::Atlas::ShapeInfo::autoMetrics)
        .def_readwrite("bearing_x",    &slughorn::Atlas::ShapeInfo::bearingX)
        .def_readwrite("bearing_y",    &slughorn::Atlas::ShapeInfo::bearingY)
        .def_readwrite("width",        &slughorn::Atlas::ShapeInfo::width)
        .def_readwrite("height",       &slughorn::Atlas::ShapeInfo::height)
        .def_readwrite("advance",      &slughorn::Atlas::ShapeInfo::advance)
        .def_readwrite("num_bands",    &slughorn::Atlas::ShapeInfo::numBands)
    ;

    // -- Atlas.Shape ----------------------------------------------------------
    py::class_<slughorn::Atlas::Shape>(m, "Shape")
        .def_readonly("band_tex_x",   &slughorn::Atlas::Shape::bandTexX)
        .def_readonly("band_tex_y",   &slughorn::Atlas::Shape::bandTexY)
        .def_readonly("band_max_x",   &slughorn::Atlas::Shape::bandMaxX)
        .def_readonly("band_max_y",   &slughorn::Atlas::Shape::bandMaxY)
        .def_readonly("band_scale_x", &slughorn::Atlas::Shape::bandScaleX)
        .def_readonly("band_scale_y", &slughorn::Atlas::Shape::bandScaleY)
        .def_readonly("band_offset_x",&slughorn::Atlas::Shape::bandOffsetX)
        .def_readonly("band_offset_y",&slughorn::Atlas::Shape::bandOffsetY)
        .def_readonly("bearing_x",    &slughorn::Atlas::Shape::bearingX)
        .def_readonly("bearing_y",    &slughorn::Atlas::Shape::bearingY)
        .def_readonly("width",        &slughorn::Atlas::Shape::width)
        .def_readonly("height",       &slughorn::Atlas::Shape::height)
        .def_readonly("advance",      &slughorn::Atlas::Shape::advance)
        .def("__repr__", [](const slughorn::Atlas::Shape& s) {
            return "Shape(advance=" + std::to_string(s.advance) +
                   " size=" + std::to_string(s.width) +
                   "x" + std::to_string(s.height) + ")";
        })
    ;

    // -- Atlas.TextureData ----------------------------------------------------
    py::class_<slughorn::Atlas::TextureData>(m, "TextureData")
        .def_readonly("width",  &slughorn::Atlas::TextureData::width)
        .def_readonly("height", &slughorn::Atlas::TextureData::height)
        .def_property_readonly("format", [](const slughorn::Atlas::TextureData& td) {
            return td.format == slughorn::Atlas::TextureData::Format::RGBA32F
                ? "RGBA32F" : "RGBA16UI";
        })
        .def_property_readonly("bytes", [](const slughorn::Atlas::TextureData& td) {
            return bytesView(td.bytes);
        }, "Zero-copy memoryview of the raw pixel data. "
           "Keep the Atlas alive for the duration of any view.")
        .def("__repr__", [](const slughorn::Atlas::TextureData& td) {
            return "TextureData(" + std::to_string(td.width) +
                   "x" + std::to_string(td.height) +
                   " format=" + (td.format == slughorn::Atlas::TextureData::Format::RGBA32F
                       ? "RGBA32F" : "RGBA16UI") + ")";
        })
    ;

    // -- Atlas ----------------------------------------------------------------
    py::class_<slughorn::Atlas, std::shared_ptr<slughorn::Atlas>>(m, "Atlas")
        .def(py::init<>())

        // Population
        .def("add_shape", &slughorn::Atlas::addShape,
            py::arg("key"), py::arg("info"),
            "Register a shape under key.  Must be called before build().")

        // Build
        .def("build", &slughorn::Atlas::build,
            "Pack all registered shapes into the texture buffers.  "
            "Call once; subsequent calls are no-ops.")
        .def("is_built", &slughorn::Atlas::isBuilt)

        // Accessors (valid after build())
        .def("get_shape", [](const slughorn::Atlas& a, uint32_t key)
            -> std::optional<slughorn::Atlas::Shape>
        {
            const auto* s = a.getShape(key);
            if(!s) return std::nullopt;
            return *s;
        }, py::arg("key"),
           "Return the Shape for key, or None if not found.")

        .def("has_key", &slughorn::Atlas::hasKey, py::arg("key"))

        .def("get_curve_texture_data",
            [](const slughorn::Atlas& a) -> py::memoryview {
                return bytesView(a.getCurveTextureData().bytes);
            },
            "Zero-copy memoryview of the RGBA32F curve texture bytes.")

        .def("get_band_texture_data",
            [](const slughorn::Atlas& a) -> py::memoryview {
                return bytesView(a.getBandTextureData().bytes);
            },
            "Zero-copy memoryview of the RGBA16UI band texture bytes.")

        .def_property_readonly("curve_texture",
            [](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
                return a.getCurveTextureData();
            }, py::return_value_policy::reference_internal)

        .def_property_readonly("band_texture",
            [](const slughorn::Atlas& a) -> const slughorn::Atlas::TextureData& {
                return a.getBandTextureData();
            }, py::return_value_policy::reference_internal)
    ;

    // =========================================================================
    // slughorn.CurveDecomposer
    // =========================================================================
    py::class_<slughorn::CurveDecomposer>(m, "CurveDecomposer",
        "Stateful path sink: accepts moveTo/lineTo/quadTo/cubicTo and appends "
        "quadratic Bezier segments to a Curves list.")
        .def(py::init([](slughorn::Atlas::Curves& curves) {
            return std::make_unique<slughorn::CurveDecomposer>(curves);
        }), py::arg("curves"),
            "Construct with a reference to a Curves list to append into. "
            "The list must outlive the decomposer.")
        .def("move_to",  &slughorn::CurveDecomposer::moveTo,
            py::arg("x"), py::arg("y"))
        .def("line_to",  &slughorn::CurveDecomposer::lineTo,
            py::arg("x3"), py::arg("y3"))
        .def("quad_to",  &slughorn::CurveDecomposer::quadTo,
            py::arg("cx"), py::arg("cy"), py::arg("x3"), py::arg("y3"))
        .def("cubic_to", &slughorn::CurveDecomposer::cubicTo,
            py::arg("c1x"), py::arg("c1y"),
            py::arg("c2x"), py::arg("c2y"),
            py::arg("x3"),  py::arg("y3"))
    ;

    // =========================================================================
    // slughorn.emoji  submodule
    // =========================================================================
    py::module_ emoji = m.def_submodule("emoji",
        "Emoji name <-> codepoint lookup table (Unicode 15.1 CLDR short names).");

    emoji.def("name_to_codepoint",
        [](std::string_view name) -> std::optional<uint32_t> {
            return slughorn::emoji::nameToCodepoint(name);
        }, py::arg("name"),
        "Return the codepoint for a normalised CLDR short name, or None.\n"
        "Example: slughorn.emoji.name_to_codepoint('dragon') -> 0x1F409");

    emoji.def("codepoint_to_name",
        [](uint32_t cp) -> std::optional<std::string> {
            auto sv = slughorn::emoji::codepointToName(cp);
            if(!sv) return std::nullopt;
            return std::string(*sv);
        }, py::arg("codepoint"),
        "Return the CLDR short name for a codepoint, or None.");

    emoji.def("strip_colons", [](std::string_view name) -> std::string {
        return std::string(slughorn::emoji::stripColons(name));
    }, py::arg("name"),
        "Strip leading/trailing colons: ':dragon:' -> 'dragon'.");

    emoji.def("slack_name_to_codepoint",
        [](std::string_view name) -> std::optional<uint32_t> {
            return slughorn::emoji::slackNameToCodepoint(name);
        }, py::arg("slack_name"),
        "Strip colons then look up. ':dragon:' -> 0x1F409");

    emoji.def("table_size", &slughorn::emoji::tableSize,
        "Return the number of entries in the lookup table.");

    // =========================================================================
    // Submodule stubs — uncomment and implement as you add each backend
    // =========================================================================

    // py::module_ ft2 = m.def_submodule("ft2",
    //     "FreeType 2 backend — decompose TrueType/OpenType outlines and COLR emoji.");
    // TODO: bind slughorn::ft2::loadAsciiFont, loadEmojiFont, loadGlyph, etc.

    // py::module_ skia = m.def_submodule("skia",
    //     "Skia path backend — decompose SkPath objects, stroke-to-fill expansion.");
    // TODO: bind slughorn::skia::decomposePath, strokeToFill, loadShape, etc.

    // py::module_ cairo = m.def_submodule("cairo",
    //     "Cairo path backend — decompose cairo_t paths.");
    // TODO: bind slughorn::cairo::decomposePath, loadShape.
}
