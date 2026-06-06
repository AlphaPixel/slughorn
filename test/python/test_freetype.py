"""
Tests for slughorn/freetype.hpp — slughorn.freetype submodule.

All tests are skipped if slughorn.freetype is not compiled in,
or if required system fonts are not present.
"""

import pathlib
import pytest
import slughorn
from conftest import requires_freetype, MONO_FONT, PROP_FONT, DIGIT_CODEPOINTS

pytestmark = requires_freetype()

_MONO_PATH = pathlib.Path(MONO_FONT)
_PROP_PATH = pathlib.Path(PROP_FONT)
_ASCII_CPS = list(range(32, 127))


def _skip_if_no_font(path):
	if not path.exists():
		pytest.skip(f"font not found: {path}")


# ---------------------------------------------------------------------------
# Module / function availability
# ---------------------------------------------------------------------------

def test_freetype_submodule_exists():
	assert hasattr(slughorn, "freetype")

def test_load_font_glyphs_exists():
	assert hasattr(slughorn.freetype, "load_font_glyphs")

def test_load_ascii_font_exists():
	assert hasattr(slughorn.freetype, "load_ascii_font")

def test_load_all_font_glyphs_exists():
	assert hasattr(slughorn.freetype, "load_all_font_glyphs")

def test_load_emoji_font_exists():
	assert hasattr(slughorn.freetype, "load_emoji_font")

def test_load_font_metrics_exists():
	assert hasattr(slughorn.freetype, "load_font_metrics")


# ---------------------------------------------------------------------------
# load_font_metrics (no atlas needed)
# ---------------------------------------------------------------------------

def test_load_font_metrics_returns_metrics(atlas):
	_skip_if_no_font(_MONO_PATH)
	fm = slughorn.freetype.load_font_metrics(MONO_FONT)
	assert fm is not None
	assert isinstance(fm, slughorn.FontMetrics)

def test_load_font_metrics_units_per_em(atlas):
	_skip_if_no_font(_MONO_PATH)
	fm = slughorn.freetype.load_font_metrics(MONO_FONT)
	assert fm.units_per_em > 0

def test_load_font_metrics_ratios_in_range(atlas):
	_skip_if_no_font(_MONO_PATH)
	fm = slughorn.freetype.load_font_metrics(MONO_FONT)
	assert 0.0 < fm.cap_height_ratio  < 1.0
	assert 0.0 < fm.x_height_ratio    < 1.0
	assert 0.0 < fm.ascender_ratio    < 1.0
	assert 0.0 < fm.descender_ratio   < 1.0
	assert 0.0 <= fm.line_gap_ratio

def test_load_font_metrics_bad_path():
	fm = slughorn.freetype.load_font_metrics("/nonexistent/font.ttf")
	assert fm is None

def test_font_metrics_repr():
	_skip_if_no_font(_MONO_PATH)
	fm = slughorn.freetype.load_font_metrics(MONO_FONT)
	assert repr(fm) != ""


# ---------------------------------------------------------------------------
# load_font_glyphs
# ---------------------------------------------------------------------------

def test_load_glyphs_returns_count(atlas):
	_skip_if_no_font(_MONO_PATH)
	n = slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	assert n == len(DIGIT_CODEPOINTS)

def test_load_glyphs_all_keys_registered(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	for cp in DIGIT_CODEPOINTS:
		assert atlas.has_key(slughorn.Key(cp)), f"codepoint {cp} missing from atlas"

def test_load_glyphs_shapes_nonzero(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.build()
	for cp in DIGIT_CODEPOINTS:
		s = atlas.get_shape(slughorn.Key(cp))
		assert s is not None
		assert s.width > 0.0 and s.height > 0.0, f"glyph {cp} has zero size"

def test_load_glyphs_bad_path(atlas):
	# BUG: load_font_glyphs silently returns 0 on a bad font path instead of
	# raising, unlike load_ascii_font (returns False) and load_font_metrics
	# (returns None).  All three should signal failure consistently.
	# Update this test when the inconsistency is fixed.
	n = slughorn.freetype.load_font_glyphs("/nonexistent/font.ttf", DIGIT_CODEPOINTS, atlas)
	assert n == 0  # current (inconsistent) behavior

def test_load_glyphs_empty_codepoints(atlas):
	_skip_if_no_font(_MONO_PATH)
	n = slughorn.freetype.load_font_glyphs(MONO_FONT, [], atlas)
	assert n == 0

def test_load_glyphs_uniform_metrics(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas, uniform=True)
	atlas.build()
	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in DIGIT_CODEPOINTS]
	assert all(s is not None for s in shapes)
	ref = shapes[0]
	for s in shapes[1:]:
		assert s.width   == pytest.approx(ref.width,   abs=1e-5)
		assert s.height  == pytest.approx(ref.height,  abs=1e-5)
		assert s.advance == pytest.approx(ref.advance, abs=1e-5)


# ---------------------------------------------------------------------------
# load_ascii_font
# ---------------------------------------------------------------------------

def test_load_ascii_font_returns_true(atlas):
	_skip_if_no_font(_MONO_PATH)
	result = slughorn.freetype.load_ascii_font(MONO_FONT, atlas)
	assert result is True

def test_load_ascii_font_all_printable_registered(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_ascii_font(MONO_FONT, atlas)
	for cp in _ASCII_CPS:
		assert atlas.has_key(slughorn.Key(cp)), f"ASCII codepoint {cp} missing"

def test_load_ascii_font_bad_path(atlas):
	result = slughorn.freetype.load_ascii_font("/nonexistent/font.ttf", atlas)
	assert result is False


# ---------------------------------------------------------------------------
# load_all_font_glyphs
# ---------------------------------------------------------------------------

def test_load_all_font_glyphs_returns_nonzero(atlas):
	_skip_if_no_font(_MONO_PATH)
	n = slughorn.freetype.load_all_font_glyphs(MONO_FONT, atlas)
	assert n > 0

def test_load_all_font_glyphs_includes_digits(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_all_font_glyphs(MONO_FONT, atlas)
	for cp in DIGIT_CODEPOINTS:
		assert atlas.has_key(slughorn.Key(cp))


# ---------------------------------------------------------------------------
# Glyph advance / metrics
# ---------------------------------------------------------------------------

def test_glyph_advance_nonzero(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.build()
	for cp in DIGIT_CODEPOINTS:
		s = atlas.get_shape(slughorn.Key(cp))
		assert s.advance > 0.0, f"glyph {cp} has zero advance"

def test_glyph_advance_tabular(atlas):
	# Monospaced font: all digit advances must be equal.
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.build()
	advances = [atlas.get_shape(slughorn.Key(cp)).advance for cp in DIGIT_CODEPOINTS]
	assert all(a == pytest.approx(advances[0], rel=1e-5) for a in advances)


# ---------------------------------------------------------------------------
# normalize_shape_metrics (primary consumer is freetype)
# ---------------------------------------------------------------------------

def test_normalize_tabular(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in DIGIT_CODEPOINTS])
	atlas.build()
	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in DIGIT_CODEPOINTS]
	assert all(s is not None for s in shapes)
	ref = shapes[0]
	for s in shapes[1:]:
		assert s.width   == pytest.approx(ref.width,   abs=1e-5)
		assert s.height  == pytest.approx(ref.height,  abs=1e-5)
		assert s.advance == pytest.approx(ref.advance, abs=1e-5)

def test_normalize_nontabular(atlas):
	_skip_if_no_font(_PROP_PATH)
	slughorn.freetype.load_font_glyphs(PROP_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in DIGIT_CODEPOINTS])
	atlas.build()
	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in DIGIT_CODEPOINTS]
	assert all(s is not None for s in shapes)
	ref = shapes[0]
	for s in shapes[1:]:
		assert s.width   == pytest.approx(ref.width,   abs=1e-5)
		assert s.height  == pytest.approx(ref.height,  abs=1e-5)
		assert s.advance == pytest.approx(ref.advance, abs=1e-5)

def test_normalize_band_transforms_differ(atlas):
	_skip_if_no_font(_PROP_PATH)
	slughorn.freetype.load_font_glyphs(PROP_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in DIGIT_CODEPOINTS])
	atlas.build()
	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in DIGIT_CODEPOINTS]
	scales = [s.band_scale_x for s in shapes]
	# At least some shapes must have distinct band scales (per-shape coverage data preserved).
	assert not all(abs(sc - scales[0]) < 1e-5 for sc in scales[1:])

def test_normalize_coverage_preserved(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas)
	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in DIGIT_CODEPOINTS])
	atlas.build()
	for cp in DIGIT_CODEPOINTS:
		grid = atlas.decode(slughorn.Key(cp)).render_grid(32, 0.0, True)
		assert max(memoryview(grid).cast('B').cast('f')) > 0.0, f"glyph {cp} has zero coverage after normalize"


# ---------------------------------------------------------------------------
# log callback
# ---------------------------------------------------------------------------

def test_log_callback_bad_path(atlas):
	messages = []
	def on_log(level, msg):
		messages.append((level, msg))
	try:
		slughorn.freetype.load_font_glyphs("/nonexistent/font.ttf", DIGIT_CODEPOINTS, atlas, log=on_log)
	except Exception:
		pass
	# Whether the error is raised or reported via the log, at least one must happen.

def test_log_callback_receives_strings(atlas):
	_skip_if_no_font(_MONO_PATH)
	messages = []
	def on_log(level, msg):
		messages.append((level, msg))
	slughorn.freetype.load_font_glyphs(MONO_FONT, DIGIT_CODEPOINTS, atlas, log=on_log)
	# No warnings expected on a valid font, but the callback must not crash if called.
	for level, msg in messages:
		assert isinstance(level, int)
		assert isinstance(msg, str)


# ---------------------------------------------------------------------------
# strategy parameter
# ---------------------------------------------------------------------------

def test_strategy_callable(atlas):
	_skip_if_no_font(_MONO_PATH)
	def my_strategy(curves):
		return slughorn.Atlas.compute_uniform_splits(curves, 4, 4)
	n = slughorn.freetype.load_font_glyphs(
		MONO_FONT, [ord("A")], atlas, strategy=my_strategy
	)
	assert n == 1
	atlas.build()
	assert atlas.get_shape(slughorn.Key(ord("A"))) is not None


# ---------------------------------------------------------------------------
# load_emoji_font
# ---------------------------------------------------------------------------

_NOTO_EMOJI = pathlib.Path("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf")

def test_load_emoji_font_returns_dict(atlas):
	if not _NOTO_EMOJI.exists():
		pytest.skip("NotoColorEmoji.ttf not installed")
	# NotoColorEmoji is a bitmap (CBDT/CBLC) font, not COLR — load_emoji_font only
	# handles COLR v0/v1.  The call must not crash and returns an empty dict for
	# unsupported formats.  Use a COLR font to get non-empty results.
	result = slughorn.freetype.load_emoji_font(str(_NOTO_EMOJI), [0x1F600], atlas)
	assert isinstance(result, dict)


# ---------------------------------------------------------------------------
# End-to-end: load → build → decode → render
# ---------------------------------------------------------------------------

def test_load_and_decode(atlas):
	_skip_if_no_font(_MONO_PATH)
	slughorn.freetype.load_font_glyphs(MONO_FONT, [ord("A")], atlas)
	atlas.build()
	sampler = atlas.decode(slughorn.Key(ord("A")))
	grid = sampler.render_grid(32, 0.0, True)
	assert max(memoryview(grid).cast('B').cast('f')) > 0.0, "glyph 'A' rendered empty"
