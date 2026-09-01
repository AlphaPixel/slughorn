#include "slughorn-python.hpp"

#include "slughorn/freetype.hpp"

namespace slughorn_python {

void bind_freetype(py::module_& freetype) {
	py::class_<slughorn::freetype::LoadConfig>(freetype, "LoadConfig")
		.def(py::init([](py::kwargs kwargs) {
			slughorn::freetype::LoadConfig config;

			for(auto item : kwargs) {
				auto key = item.first.cast<std::string>();

				if(key == "strategy") config.strategy = item.second.cast<slughorn::Atlas::SplitStrategy>();
				else if(key == "log") config.log = item.second.cast<slughorn::freetype::LogCallback>();
				else if(key == "uniform") config.uniform = item.second.cast<bool>();
				else if(key == "mask") config.mask = item.second.cast<uint8_t>();
				else if(key == "metrics") config.metrics = item.second.cast<slughorn::FontMetrics>();
				else if(key == "family_name") config.familyName = item.second.cast<std::string>();
				else if(key == "style_name") config.styleName = item.second.cast<std::string>();
				else throw py::type_error("LoadConfig got an unexpected keyword argument '" + key + "'");
			}

			return config;
		}),
		"Construct with optional field=value kwargs, e.g. LoadConfig(uniform=True, mask=3)."
		)
		.def_readwrite("strategy", &slughorn::freetype::LoadConfig::strategy,
			"Optional callable(curves) -> (splits_x, splits_y), e.g.:\n"
			"    lambda c: slughorn.Atlas.compute_adaptive_splits(c, 8, 8)"
		)
		.def_readwrite("log", &slughorn::freetype::LoadConfig::log,
			"Optional callable(level: int, msg: str) for load-time diagnostics."
		)
		.def_readwrite("uniform", &slughorn::freetype::LoadConfig::uniform,
			"If True, all glyphs in a batch share the same em-space bounding box\n"
			"(required for setLayerShapeIndex glyph-swap cycling)."
		)
		.def_readwrite("mask", &slughorn::freetype::LoadConfig::mask,
			"Opt-in Key namespace (0-255) packed into every loaded glyph's Key,\n"
			"e.g. to load a second font's variant of the same codepoints without\n"
			"colliding with a previously-loaded font (see slughorn.Key)."
		)
		.def_readwrite("metrics", &slughorn::freetype::LoadConfig::metrics,
			"Output: em-space font metrics, populated by the high-level load*_font\n"
			"functions (not by the FT_Face-taking overloads)."
		)
		.def_readwrite("family_name", &slughorn::freetype::LoadConfig::familyName,
			"Output: populated by the high-level load*_font functions."
		)
		.def_readwrite("style_name", &slughorn::freetype::LoadConfig::styleName,
			"Output: populated by the high-level load*_font functions."
		)
		.def("__repr__", [](const slughorn::freetype::LoadConfig& c) { return streamRepr(c); })
	;

	freetype.def("load_ascii_font",
		&slughorn::freetype::loadAsciiFont,
		"font_path"_a, "atlas"_a, "config"_a=nullptr,
		"Load printable ASCII (codepoints 32-126) from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"config: optional LoadConfig; its output fields (metrics, family_name,\n"
		"    style_name) are populated in place on return.\n"
		"Returns True on success, False if the font cannot be opened."
	);

	freetype.def("load_font_glyphs",
		&slughorn::freetype::loadFontGlyphs,
		"font_path"_a,
		"codepoints"_a,
		"atlas"_a,
		"config"_a=nullptr,
		"Load an explicit list of Unicode codepoints from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"config: optional LoadConfig; its output fields (metrics, family_name,\n"
		"    style_name) are populated in place on return.\n"
		"Returns the number of glyphs successfully added."
	);

	freetype.def("load_all_font_glyphs",
		&slughorn::freetype::loadAllFontGlyphs,
		"font_path"_a, "atlas"_a, "config"_a=nullptr,
		"Load every mapped codepoint from font_path into atlas.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"config: optional LoadConfig; its output fields (metrics, family_name,\n"
		"    style_name) are populated in place on return.\n"
		"Returns the number of glyphs successfully added."
	);

	freetype.def("load_emoji_font", [](
		const std::string& fontPath,
		const std::vector<uint32_t>& codepoints,
		slughorn::Atlas& atlas,
		slughorn::freetype::LoadConfig* config
	) -> py::dict {
		std::map<uint32_t, slughorn::CompositeShape> colorGlyphs;

		slughorn::freetype::loadEmojiFont(fontPath, codepoints, atlas, colorGlyphs, config);

		py::dict result;

		for(auto& [cp, cs] : colorGlyphs) result[py::cast(cp)] = std::move(cs);

		return result;
	},
		"font_path"_a,
		"codepoints"_a,
		"atlas"_a,
		"config"_a=nullptr,
		"Load COLR emoji from font_path for the given codepoints into atlas.\n"
		"codepoints is a list of uint32_t Unicode codepoints.\n"
		"Creates and destroys an FT_Library/FT_Face internally.\n"
		"config: optional LoadConfig; its output fields (metrics, family_name,\n"
		"    style_name) are populated in place on return.\n"
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
