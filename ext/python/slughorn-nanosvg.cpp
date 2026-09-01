#include "slughorn-python.hpp"

#include "slughorn/nanosvg.hpp"

namespace slughorn_python {

void bind_nanosvg(py::module_& nanosvg) {
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

	py::class_<slughorn::nanosvg::LoadConfig>(nanosvg, "LoadConfig")
		.def(py::init([](py::kwargs kwargs) {
			slughorn::nanosvg::LoadConfig config;

			for(auto item : kwargs) {
				auto key = item.first.cast<std::string>();

				if(key == "log") config.log = item.second.cast<slughorn::nanosvg::LogCallback>();
				else if(key == "rules") {
					config.rules = item.second.cast<std::vector<slughorn::nanosvg::ShapeRule>>();
				}
				else if(key == "auto_metrics") config.autoMetrics = item.second.cast<bool>();
				else if(key == "origin") {
					config.origin = item.second.cast<slughorn::Atlas::ShapeInfo::Origin>();
				}
				else if(key == "width") config.width = item.second.cast<slug_t>();
				else if(key == "height") config.height = item.second.cast<slug_t>();
				else if(key == "height_em") config.heightEm = item.second.cast<slug_t>();
				else throw py::type_error("LoadConfig got an unexpected keyword argument '" + key + "'");
			}

			return config;
		}),
		"Construct with optional field=value kwargs, e.g. LoadConfig(auto_metrics=False)."
		)
		.def_readwrite("log", &slughorn::nanosvg::LoadConfig::log,
			"Optional callable(level: int, msg: str) for load-time diagnostics; "
			"omit (None) to print warnings/errors to stderr."
		)
		.def_readwrite("rules", &slughorn::nanosvg::LoadConfig::rules,
			"List of ShapeRule objects applied in order; first match wins."
		)
		.def_readwrite("auto_metrics", &slughorn::nanosvg::LoadConfig::autoMetrics,
			"If True (default), curves are shifted to local origin and shape metrics\n"
			"are derived from the curve bbox; layer.transform.x/y carries the offset\n"
			"(multiply by image width/height to recover authoring coords). If False,\n"
			"curves are stored as-is in SVG canvas space and layer.transform is zero."
		)
		.def_readwrite("origin", &slughorn::nanosvg::LoadConfig::origin,
			"Global origin for all shapes (overridden per-shape by ShapeRule.origin)."
		)
		.def_readwrite("width", &slughorn::nanosvg::LoadConfig::width,
			"Output: raw SVG width, populated by loadImage/loadFile/loadString."
		)
		.def_readwrite("height", &slughorn::nanosvg::LoadConfig::height,
			"Output: raw SVG height, populated by loadImage/loadFile/loadString."
		)
		.def_readwrite("height_em", &slughorn::nanosvg::LoadConfig::heightEm,
			"Output: SVG viewport height in em-space, populated by "
			"loadImage/loadFile/loadString."
		)
		.def("__repr__", [](const slughorn::nanosvg::LoadConfig& c) { return streamRepr(c); })
	;

	nanosvg.def("load_file",
		&slughorn::nanosvg::loadFile,
		"path"_a,
		"atlas"_a,
		"keys"_a=slughorn::KeyIterator(),
		"dpi"_a=96_cv,
		"config"_a=nullptr,
		"Parse an SVG file and pack every filled shape into atlas.\n"
		"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
		"to pack multiple SVGs into the same atlas without key collisions.\n"
		"config: optional LoadConfig; its output fields (width, height, height_em)\n"
		"    are populated in place on return."
	);

	nanosvg.def("load_string",
		&slughorn::nanosvg::loadString,
		"svg"_a,
		"atlas"_a,
		"keys"_a=slughorn::KeyIterator(),
		"dpi"_a=96_cv,
		"config"_a=nullptr,
		"Parse an SVG string and pack every filled shape into atlas.\n"
		"keys is advanced in-place; pass the same KeyIterator to subsequent calls\n"
		"to pack multiple SVGs into the same atlas without key collisions.\n"
		"config: optional LoadConfig; its output fields (width, height, height_em)\n"
		"    are populated in place on return."
	);
}

}
