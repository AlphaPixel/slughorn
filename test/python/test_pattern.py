"""
Tests for fill patterns: slughorn.PatternInfo, Atlas.add_pattern(), Layer.patternId,
Canvas.define_pattern() / fill_pattern(), Matrix.inverse(), and serial round trips.

The key invariant: PatternInfo.transform maps a point's region em-coordinate to the same point's
tile em-coordinate, whatever auto_metrics did to either shape.
"""

import math
import pytest
import slughorn

Repeat = slughorn.PatternInfo.Repeat

HEX_RADIUS = 0.4

HAS_SERIAL = hasattr(slughorn, "read")

skip_serial = pytest.mark.skipif(not HAS_SERIAL, reason="built without SLUGHORN_SERIAL=ON")

def _hexagon(cvs, cx=0.5, cy=0.5):
	for i in range(6):
		angle = math.pi / 6.0 + i * math.pi / 3.0
		x = cx + HEX_RADIUS * math.cos(angle)
		y = cy + HEX_RADIUS * math.sin(angle)

		if i == 0:
			cvs.move_to(x, y)

		else:
			cvs.line_to(x, y)

	cvs.close_path()

def _hex_min():
	return 0.5 - HEX_RADIUS * math.cos(math.pi / 6.0), 0.5 - HEX_RADIUS

def _define_hex_pattern(cvs, **kwargs):
	cvs.begin_path()

	_hexagon(cvs)

	return cvs.define_pattern(0.0, 0.0, 1.0, 1.0, **kwargs)

# ---------------------------------------------------------------------------
# PatternInfo / Atlas
# ---------------------------------------------------------------------------

def test_pattern_info_defaults():
	p = slughorn.PatternInfo()

	assert (p.cell_x, p.cell_y, p.cell_width, p.cell_height) == (0.0, 0.0, 1.0, 1.0)
	assert p.transform.is_identity()
	assert p.repeat_x == Repeat.Repeat
	assert p.repeat_y == Repeat.Repeat

def test_pattern_info_kwargs():
	p = slughorn.PatternInfo(tile="hex", cell_width=2.0, repeat_y=Repeat.Reflect)

	assert p.tile == slughorn.Key("hex")
	assert p.cell_width == pytest.approx(2.0)
	assert p.repeat_x == Repeat.Repeat
	assert p.repeat_y == Repeat.Reflect
	assert "PatternInfo(" in repr(p)

def test_add_pattern_ids_are_one_based():
	atlas = slughorn.Atlas()

	assert atlas.add_pattern(slughorn.PatternInfo(tile="a")) == 1
	assert atlas.add_pattern(slughorn.PatternInfo(tile="b")) == 2
	assert [p.tile for p in atlas.get_patterns()] == [slughorn.Key("a"), slughorn.Key("b")]

def test_layer_pattern_id_default_and_kwarg():
	assert slughorn.Layer("x").patternId == 0
	assert slughorn.Layer("x", patternId=3).patternId == 3

# ---------------------------------------------------------------------------
# Matrix.inverse
# ---------------------------------------------------------------------------

def test_matrix_inverse_roundtrip():
	m = slughorn.Matrix.translate(3.0, -2.0) * slughorn.Matrix.rotate(0.7) * slughorn.Matrix.scale(2.0, 0.5)
	inv = m.inverse()

	x, y = m.apply(1.25, -4.0)

	assert inv.apply(x, y) == pytest.approx((1.25, -4.0))

def test_matrix_inverse_singular_is_none():
	assert slughorn.Matrix.scale(0.0, 1.0).inverse() is None

# ---------------------------------------------------------------------------
# Canvas.define_pattern
# ---------------------------------------------------------------------------

def test_define_pattern_auto_metrics_off_keeps_authored_cell():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	cvs.auto_metrics = False

	h = _define_hex_pattern(cvs)

	assert (h.tile_offset_x, h.tile_offset_y) == (0.0, 0.0)
	assert (h.info.cell_x, h.info.cell_y) == (0.0, 0.0)
	assert (h.info.cell_width, h.info.cell_height) == pytest.approx((1.0, 1.0))

def test_define_pattern_auto_metrics_on_shifts_cell():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)
	min_x, min_y = _hex_min()

	h = _define_hex_pattern(cvs)

	assert (h.tile_offset_x, h.tile_offset_y) == pytest.approx((min_x, min_y))
	assert (h.info.cell_x, h.info.cell_y) == pytest.approx((-min_x, -min_y))
	assert (h.info.cell_width, h.info.cell_height) == pytest.approx((1.0, 1.0))

def test_define_pattern_repeat_modes():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	h = _define_hex_pattern(cvs, repeat_y=Repeat.Reflect)

	assert h.info.repeat_x == Repeat.Repeat
	assert h.info.repeat_y == Repeat.Reflect

def test_define_pattern_empty_path_returns_empty_handle():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	cvs.begin_path()

	assert cvs.define_pattern(0.0, 0.0, 1.0, 1.0).info.tile == slughorn.Key(0)

# ---------------------------------------------------------------------------
# Canvas.fill_pattern
# ---------------------------------------------------------------------------

def _fill_region(cvs, handle, scale=1.0):
	cvs.begin_path()
	cvs.rect(2.0, 3.0, 5.0, 5.0)

	return cvs.fill_pattern(handle, slughorn.Color(1.0, 0.7, 0.0, 1.0), scale)

@pytest.mark.parametrize("auto_metrics", [True, False])
@pytest.mark.parametrize("scale", [1.0, 2.0])
def test_fill_pattern_transform_maps_region_em_to_tile_em(auto_metrics, scale):
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	cvs.auto_metrics = auto_metrics

	h = _define_hex_pattern(cvs)
	layer = _fill_region(cvs, h, scale)

	assert layer.patternId == 1

	info = atlas.get_patterns()[0]

	assert info.tile == h.info.tile

	# A canvas point inside the region, in the region's em-space and in the tile's em-space.
	cx, cy = 2.25, 3.75
	region_min = (2.0 * scale, 3.0 * scale) if auto_metrics else (0.0, 0.0)
	em = (cx * scale - region_min[0], cy * scale - region_min[1])
	tile_em = (cx - h.tile_offset_x, cy - h.tile_offset_y)

	assert info.transform.apply(*em) == pytest.approx(tile_em)

def test_fill_pattern_handle_transform_applies_inverse():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	cvs.auto_metrics = False

	h = _define_hex_pattern(cvs)

	# Pattern space -> canvas space: the pattern is drawn twice as large.
	h.transform = slughorn.Matrix.scale(2.0, 2.0)

	_fill_region(cvs, h)

	info = atlas.get_patterns()[0]

	assert info.transform.apply(4.0, 6.0) == pytest.approx((2.0, 3.0))

def test_fill_pattern_singular_transform_fails():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	h = _define_hex_pattern(cvs)

	h.transform = slughorn.Matrix.scale(0.0, 0.0)

	layer = _fill_region(cvs, h)

	assert layer.patternId == 0
	assert atlas.get_patterns() == []

def test_fill_pattern_layer_lands_in_composite():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	h = _define_hex_pattern(cvs)
	layer = _fill_region(cvs, h)

	cs = cvs.finalize()

	assert cs.layers[-1].key == layer.key
	assert cs.layers[-1].patternId == 1

def test_fill_pattern_builds():
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	h = _define_hex_pattern(cvs)

	_fill_region(cvs, h)

	atlas.build()

	assert atlas.get_shape(h.info.tile) is not None

def test_canvas_keeps_temporary_atlas_alive():
	import gc

	cvs = slughorn.canvas.Canvas(slughorn.Atlas())

	gc.collect()

	h = _define_hex_pattern(cvs)

	_fill_region(cvs, h)

	assert cvs.finalize().layers[-1].patternId == 1

# ---------------------------------------------------------------------------
# Serial
# ---------------------------------------------------------------------------

@skip_serial
@pytest.mark.parametrize("ext", ["slug", "slugb"])
def test_serial_roundtrip(tmp_path, ext):
	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	h = _define_hex_pattern(cvs, repeat_y=Repeat.Reflect)

	h.transform = slughorn.Matrix.rotate(0.3)

	_fill_region(cvs, h)

	cvs.finalize(slughorn.Key("patterned"))

	atlas.build()

	path = str(tmp_path / f"atlas.{ext}")

	slughorn.write(atlas, path)

	back = slughorn.read(path)
	src = atlas.get_patterns()[0]
	dst = back.get_patterns()[0]

	assert dst.tile == src.tile
	assert (dst.cell_x, dst.cell_y, dst.cell_width, dst.cell_height) == pytest.approx(
		(src.cell_x, src.cell_y, src.cell_width, src.cell_height)
	)
	assert dst.transform.apply(1.0, 2.0) == pytest.approx(src.transform.apply(1.0, 2.0))
	assert dst.repeat_x == Repeat.Repeat
	assert dst.repeat_y == Repeat.Reflect
	assert back.get_composite_shape(slughorn.Key("patterned")).layers[-1].patternId == 1
