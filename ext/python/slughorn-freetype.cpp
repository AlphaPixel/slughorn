#include "slughorn-python.hpp"

#include "slughorn/freetype.hpp"

namespace detail {

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

} // namespace detail

namespace slughorn_python {

void bind_freetype(py::module_& freetype) {
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
}

}
