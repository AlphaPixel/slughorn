"""
Tests for Atlas::append() (slughorn/slughorn.hpp) — merging another, already-built Atlas's
shapes and composites into this one.

Covers:
    - Codepoint keys re-keyed via mask, real codepoint preserved
    - Name keys re-keyed via "prefix:name", empty prefix leaves them untouched
    - Metrics/curves copied verbatim (autoMetrics=false), not recomputed
    - CompositeShape and its Layer::key entries remapped consistently
    - Silent no-op once the target Atlas is already built (matches addShape())
    - The real target scenario: build a "font", round-trip it through slughorn.write()/read()
      (a .slug file) or write_string()/read_string() (an in-memory JSON string, e.g. a
      compile-time literal embedded in a header), then append() the reloaded Atlas
"""

import pytest
import slughorn

HAS_SERIAL = hasattr(slughorn, "read")
skip_serial = pytest.mark.skipif(not HAS_SERIAL, reason="built without SLUGHORN_SERIAL=ON")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _square_curves(x0, y0, x1, y1):
	d = slughorn.CurveDecomposer()
	d.move_to(x0, y0)
	d.line_to(x1, y0)
	d.line_to(x1, y1)
	d.line_to(x0, y1)
	d.close()
	return d.get_curves()

def _ascii_font_atlas():
	"""Stand-in 'font': one built shape per uppercase ASCII letter, keyed by codepoint."""
	atlas = slughorn.Atlas()

	for cp in range(ord("A"), ord("Z") + 1):
		info = slughorn.ShapeInfo()
		info.curves = _square_curves(0.0, 0.0, 0.6, 1.0)
		atlas.add_shape(slughorn.Key(cp), info)

	atlas.build()

	return atlas

def _named_icon_atlas():
	"""Built Atlas with two Name-keyed shapes and a composite referencing both."""
	atlas = slughorn.Atlas()

	for name, size in [("gear", 1.0), ("bolt", 0.5)]:
		info = slughorn.ShapeInfo()
		info.curves = _square_curves(0.0, 0.0, size, size)
		atlas.add_shape(slughorn.Key(name), info)

	cs = slughorn.CompositeShape()
	cs.layers.append(slughorn.Layer(slughorn.Key("gear"), slughorn.Color(1.0, 1.0, 1.0, 1.0)))
	cs.layers.append(slughorn.Layer(slughorn.Key("bolt"), slughorn.Color(1.0, 0.0, 0.0, 1.0)))
	atlas.add_composite_shape(slughorn.Key("icon"), cs)
	atlas.build()

	return atlas


# ---------------------------------------------------------------------------
# Codepoint keys / mask
# ---------------------------------------------------------------------------

def test_append_masks_codepoint_keys():
	font = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.append(font, 5)
	atlas.build()

	assert atlas.has_key(slughorn.Key(ord("A"), 5))
	assert not atlas.has_key(slughorn.Key(ord("A")))

def test_append_default_mask_zero():
	font = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.append(font)
	atlas.build()

	assert atlas.has_key(slughorn.Key(ord("A")))

def test_append_preserves_real_codepoint():
	font = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.append(font, 5)
	atlas.build()

	k = slughorn.Key(ord("A"), 5)
	assert k.real_codepoint == ord("A")
	assert k.mask == 5

def test_append_two_sources_distinct_masks_no_collision():
	font_a = _ascii_font_atlas()
	font_b = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.append(font_a, 1)
	atlas.append(font_b, 2)
	atlas.build()

	assert atlas.has_key(slughorn.Key(ord("A"), 1))
	assert atlas.has_key(slughorn.Key(ord("A"), 2))


# ---------------------------------------------------------------------------
# Metrics / curves fidelity
# ---------------------------------------------------------------------------

def test_append_preserves_metrics():
	font = _ascii_font_atlas()
	src_shape = font.get_shape(slughorn.Key(ord("A")))

	atlas = slughorn.Atlas()
	atlas.append(font, 5)
	atlas.build()

	dst_shape = atlas.get_shape(slughorn.Key(ord("A"), 5))
	assert dst_shape.width == pytest.approx(src_shape.width, abs=1e-5)
	assert dst_shape.height == pytest.approx(src_shape.height, abs=1e-5)
	assert dst_shape.advance == pytest.approx(src_shape.advance, abs=1e-5)

def test_append_preserves_curve_count():
	font = _ascii_font_atlas()
	src_shape = font.get_shape(slughorn.Key(ord("A")))

	atlas = slughorn.Atlas()
	atlas.append(font, 5)
	atlas.build()

	dst_shape = atlas.get_shape(slughorn.Key(ord("A"), 5))
	assert len(dst_shape.curves) == len(src_shape.curves)


# ---------------------------------------------------------------------------
# Name keys / prefix
# ---------------------------------------------------------------------------

def test_append_prefixes_name_keys():
	icons = _named_icon_atlas()
	atlas = slughorn.Atlas()
	atlas.append(icons, 0, "foo")
	atlas.build()

	assert atlas.has_key(slughorn.Key("foo:gear"))
	assert not atlas.has_key(slughorn.Key("gear"))

def test_append_empty_prefix_leaves_name_keys_untouched():
	icons = _named_icon_atlas()
	atlas = slughorn.Atlas()
	atlas.append(icons)
	atlas.build()

	assert atlas.has_key(slughorn.Key("gear"))

def test_append_composite_present_under_prefix():
	icons = _named_icon_atlas()
	atlas = slughorn.Atlas()
	atlas.append(icons, 0, "foo")
	atlas.build()

	cs = atlas.get_composite_shape(slughorn.Key("foo:icon"))
	assert cs is not None
	assert len(cs.layers) == 2

def test_append_composite_layer_keys_remapped():
	icons = _named_icon_atlas()
	atlas = slughorn.Atlas()
	atlas.append(icons, 0, "foo")
	atlas.build()

	cs = atlas.get_composite_shape(slughorn.Key("foo:icon"))
	assert cs.layers[0].key == slughorn.Key("foo:gear")
	assert cs.layers[1].key == slughorn.Key("foo:bolt")
	# The remapped layer keys must actually resolve in the target atlas.
	assert atlas.has_key(cs.layers[0].key)
	assert atlas.has_key(cs.layers[1].key)


# ---------------------------------------------------------------------------
# Build lifecycle
# ---------------------------------------------------------------------------

def test_append_noop_after_target_built():
	font = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.build()
	atlas.append(font)

	assert not atlas.has_key(slughorn.Key(ord("A")))

def test_append_source_build_state_unaffected():
	font = _ascii_font_atlas()
	atlas = slughorn.Atlas()
	atlas.append(font, 5)

	assert font.is_built
	assert font.has_key(slughorn.Key(ord("A")))


# ---------------------------------------------------------------------------
# Real scenario: serialize a "font", reload it, append it
# ---------------------------------------------------------------------------

@skip_serial
def test_append_via_json_roundtrip(tmp_path):
	font = _ascii_font_atlas()
	path = str(tmp_path / "ascii_font.slug")
	slughorn.write(font, path)

	reloaded = slughorn.read(path)
	assert reloaded.is_built

	atlas = slughorn.Atlas()
	atlas.append(reloaded, 9)
	atlas.build()

	for cp in range(ord("A"), ord("Z") + 1):
		assert atlas.has_key(slughorn.Key(cp, 9))

	src_shape = font.get_shape(slughorn.Key(ord("M")))
	dst_shape = atlas.get_shape(slughorn.Key(ord("M"), 9))
	assert dst_shape.width == pytest.approx(src_shape.width, abs=1e-5)
	assert dst_shape.height == pytest.approx(src_shape.height, abs=1e-5)

@skip_serial
def test_append_via_json_roundtrip_alongside_own_shapes(tmp_path):
	"""The motivating use case: caller's own shapes plus an appended embedded font."""
	font = _ascii_font_atlas()
	path = str(tmp_path / "ascii_font.slug")
	slughorn.write(font, path)

	atlas = slughorn.Atlas()
	own_info = slughorn.ShapeInfo()
	own_info.curves = _square_curves(0.0, 0.0, 2.0, 2.0)
	atlas.add_shape(slughorn.Key("logo"), own_info)

	atlas.append(slughorn.read(path), 9)
	atlas.build()

	assert atlas.has_key(slughorn.Key("logo"))
	assert atlas.has_key(slughorn.Key(ord("A"), 9))


# ---------------------------------------------------------------------------
# Real scenario: embedded JSON string, no disk involved at all
#
# This is the actual target use case: a JSON blob generated once (e.g. by bin/slughorn),
# baked into a header as a compile-time string literal, then loaded and append()ed at
# runtime with zero filesystem access.
# ---------------------------------------------------------------------------

@skip_serial
def test_append_via_json_string():
	font = _ascii_font_atlas()

	# Stand-in for a `constexpr auto FONT = R"SLUGHORN(...)SLUGHORN";` embedded header.
	embedded_font_json = slughorn.write_string(font)
	assert isinstance(embedded_font_json, str)

	atlas = slughorn.Atlas()
	atlas.append(slughorn.read_string(embedded_font_json), 9)
	atlas.build()

	for cp in range(ord("A"), ord("Z") + 1):
		assert atlas.has_key(slughorn.Key(cp, 9))

	src_shape = font.get_shape(slughorn.Key(ord("M")))
	dst_shape = atlas.get_shape(slughorn.Key(ord("M"), 9))
	assert dst_shape.width == pytest.approx(src_shape.width, abs=1e-5)
	assert dst_shape.height == pytest.approx(src_shape.height, abs=1e-5)
	assert len(dst_shape.curves) == len(src_shape.curves)

@skip_serial
def test_append_via_json_string_alongside_own_shapes():
	"""The motivating use case in full: own shapes + an embedded-string font, one atlas."""
	embedded_font_json = slughorn.write_string(_ascii_font_atlas())

	atlas = slughorn.Atlas()
	own_info = slughorn.ShapeInfo()
	own_info.curves = _square_curves(0.0, 0.0, 2.0, 2.0)
	atlas.add_shape(slughorn.Key("logo"), own_info)

	atlas.append(slughorn.read_string(embedded_font_json), 9)
	atlas.build()

	assert atlas.has_key(slughorn.Key("logo"))
	assert atlas.has_key(slughorn.Key(ord("A"), 9))

@skip_serial
def test_append_via_json_string_two_libraries_distinct_masks():
	"""Two independently embedded string blobs, each under its own mask, merge cleanly."""
	font_json = slughorn.write_string(_ascii_font_atlas())
	icons_json = slughorn.write_string(_named_icon_atlas())

	atlas = slughorn.Atlas()
	atlas.append(slughorn.read_string(font_json), 9)
	atlas.append(slughorn.read_string(icons_json), 0, "icons")
	atlas.build()

	assert atlas.has_key(slughorn.Key(ord("A"), 9))
	assert atlas.has_key(slughorn.Key("icons:gear"))
	assert atlas.has_key(slughorn.Key("icons:icon"))
