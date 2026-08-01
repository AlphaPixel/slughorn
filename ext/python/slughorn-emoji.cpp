#define SLUGHORN_EMOJI_IMPLEMENTATION
#include "slughorn/emoji.hpp"

#include "slughorn-python.hpp"

namespace slughorn_python {

void bind_emoji(py::module_& emoji) {
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
}

}
