#include "slughorn-python.hpp"

#include "slughorn/nanosvg.hpp"

namespace detail {

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

}
