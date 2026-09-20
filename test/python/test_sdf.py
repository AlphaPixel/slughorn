"""
Tests for slughorn's baked SDF/MSDF tiles - Atlas::SDF (slughorn.SDF in Python).

Covers the whole pipeline: render.field() (the generator), Atlas.set_sdf()/request_sdf() (authoring),
build()'s packing (Shape.sdf tiles + Atlas.sdf texture), and the tile CONTRACT documented on
Atlas::SDF in slughorn.hpp. The contract tests sample the baked texture at em-space points using
nothing but that contract (texel = (em - em_origin) * texels_per_em within the tile, row 0 =
bottom, values clamped to [0, 1] with edge = 0.5 and +/-range em spanning [0, 1]) - so a wrong
origin, scale, or Y orientation fails them.

Serialization round trips live in test_serial.py.

Skipped automatically when slughorn is built without SLUGHORN_SDF=ON (no generator).
"""

import json
import math
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest
import slughorn

HAS_SDF = hasattr(slughorn.render, "field")
skip_sdf = pytest.mark.skipif(not HAS_SDF, reason="built without SLUGHORN_SDF=ON")
skip_serial = pytest.mark.skipif(not hasattr(slughorn, "write"), reason="built without SLUGHORN_SERIAL=ON")

CLI = Path(__file__).resolve().parents[2] / "bin" / "slughorn"

Type = slughorn.SDF.Type
Coloring = slughorn.SDF.Coloring


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _add_polygon(atlas, name, points):
	d = slughorn.CurveDecomposer()
	d.move_to(*points[0])
	for pt in points[1:]:
		d.line_to(*pt)
	d.close()
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	atlas.add_shape(slughorn.Key(name), info)


def _add_rect(atlas, name, w=1.0, h=1.0):
	_add_polygon(atlas, name, [(0, 0), (w, 0), (w, h), (0, h)])


# A 1 x 0.5 right triangle (hypotenuse: x + 2y = 1). Deliberately NOT symmetric across y = x or
# across its own horizontal midline, so a Y flip or an x/y swap changes the answer.
TRIANGLE = [(0.0, 0.0), (1.0, 0.0), (0.0, 0.5)]


def _sdf_atlas(names=("rect",), type=Type.MSDF, tile_size=32, **config):
	"""Atlas with unit-square shapes, every one requested, configured, and BUILT."""
	atlas = slughorn.Atlas()
	atlas.set_sdf(slughorn.SDF.Config(type=type, tile_size=tile_size, **config))
	for name in names:
		_add_rect(atlas, name)
		atlas.request_sdf(name)
	atlas.build()
	return atlas


def _triangle_atlas(type=Type.SDF, tile_size=64, range=0.1):
	"""Atlas with the asymmetric TRIANGLE shape "tri", requested, configured, and BUILT."""
	atlas = slughorn.Atlas()
	atlas.set_sdf(slughorn.SDF.Config(type=type, tile_size=tile_size, range=range))
	_add_polygon(atlas, "tri", TRIANGLE)
	atlas.request_sdf("tri")
	atlas.build()
	return atlas


def _linspace(lo, hi, n):
	return [lo + (hi - lo) * i / (n - 1) for i in range(n)]


def _median3(r, g, b):
	return max(min(r, g), min(max(r, g), b))


def _pixels(atlas):
	"""The baked texture as (height, width, channels) float32; row 0 = bottom."""
	tex = atlas.sdf.texture
	channels = 3 if tex.format == "RGB32F" else 1
	raw = np.frombuffer(bytes(memoryview(tex.bytes)), dtype=np.float32)
	return raw.reshape(tex.height, tex.width, channels)


def _sample(atlas, key, ex, ey):
	"""Bilinear-sample the baked field at em point (ex, ey) using ONLY the documented contract."""
	tile = atlas.get_shape(key).sdf
	px = _pixels(atlas)
	# Continuous texel coordinates in the texture; texel i has its center at i + 0.5.
	u = (ex - tile.em_origin[0]) * tile.texels_per_em + tile.x - 0.5
	v = (ey - tile.em_origin[1]) * tile.texels_per_em + tile.y - 0.5
	x0, y0 = math.floor(u), math.floor(v)
	tx, ty = u - x0, v - y0

	def at(x, y):
		x = min(max(x, tile.x), tile.x + tile.w - 1)
		y = min(max(y, tile.y), tile.y + tile.h - 1)
		return px[y, x]

	top = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx
	bottom = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx
	value = bottom * (1 - ty) + top * ty

	return float(_median3(*value)) if value.shape[0] == 3 else float(value[0])


def _polygon_sd(pt, poly):
	"""Brute-force signed distance to a CCW polygon (negative outside, matching the field's sign:
	positive inside)."""
	px, py = pt
	best = float("inf")
	inside = True
	n = len(poly)
	for i in range(n):
		ax, ay = poly[i]
		bx, by = poly[(i + 1) % n]
		abx, aby = bx - ax, by - ay
		t = max(0.0, min(1.0, ((px - ax) * abx + (py - ay) * aby) / (abx * abx + aby * aby)))
		best = min(best, math.hypot(px - (ax + t * abx), py - (ay + t * aby)))
		if abx * (py - ay) - aby * (px - ax) < 0:
			inside = False
	return best if inside else -best


def _expected_value(sd, range):
	"""distance +/-range em spans [0, 1]; edge = 0.5; clamped."""
	return min(1.0, max(0.0, 0.5 + sd / (2 * range)))



# ---------------------------------------------------------------------------
# render.field - the generator
# ---------------------------------------------------------------------------

def _rect_atlas():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	atlas.build()
	return atlas


@skip_sdf
@pytest.mark.parametrize("type,channels", [(Type.SDF, 1), (Type.MSDF, 3)])
def test_field_channels_follow_type(type, channels):
	field = slughorn.render.field(_rect_atlas(), "rect", slughorn.SDF.Config(type=type, tile_size=32))
	assert isinstance(field, slughorn.render.Field)
	assert field.channels == channels
	assert memoryview(field).shape == (field.height, field.width, channels)

@skip_sdf
def test_field_defaults_to_atlas_config():
	field = slughorn.render.field(_rect_atlas(), "rect")  # SDF.Config() defaults: MSDF, 128
	assert field.channels == 3
	assert max(field.width, field.height) == 128

@skip_sdf
def test_field_values_are_clamped():
	arr = np.asarray(slughorn.render.field(_rect_atlas(), "rect", slughorn.SDF.Config(tile_size=32)))
	assert arr.min() >= 0.0 and arr.max() <= 1.0

@skip_sdf
def test_field_frame_of_a_unit_square():
	# Bounds are the shape's (0,0)-(1,1) expanded by range on every side: [-0.1, 1.1].
	f = slughorn.render.field(_rect_atlas(), "rect", slughorn.SDF.Config(tile_size=32, range=0.1))
	assert f.em_origin == pytest.approx((-0.1, -0.1), abs=1e-5)
	assert f.texels_per_em == pytest.approx(32 / 1.2, rel=1e-4)
	assert (f.width, f.height) == (32, 32)
	assert f.range == pytest.approx(0.1, abs=1e-6)

@skip_sdf
def test_field_keeps_aspect_ratio_with_one_uniform_scale():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "wide", w=2.0, h=1.0)
	atlas.build()
	f = slughorn.render.field(atlas, "wide", slughorn.SDF.Config(tile_size=64, range=0.1))
	assert f.width == 64
	assert f.height == pytest.approx(64 * 1.2 / 2.2, abs=1)  # ONE scale for both axes

@skip_sdf
def test_field_does_not_touch_the_atlas():
	atlas = _rect_atlas()
	slughorn.render.field(atlas, "rect", slughorn.SDF.Config(tile_size=32))
	assert atlas.sdf.empty
	assert atlas.get_shape("rect").sdf is None

@skip_sdf
def test_field_of_an_empty_shape_has_no_width():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	try:
		atlas.add_shape(slughorn.Key("empty"), slughorn.ShapeInfo())
	except Exception:
		pytest.skip("add_shape rejects curve-less shapes")
	atlas.build()
	assert slughorn.render.field(atlas, "empty", slughorn.SDF.Config()).width == 0


# ---------------------------------------------------------------------------
# SDF.Config / enums
# ---------------------------------------------------------------------------

def test_enums_exist():
	assert Type.SDF != Type.MSDF
	assert Coloring.Simple != Coloring.ByDistance

def test_config_defaults():
	c = slughorn.SDF.Config()
	assert c.type == Type.MSDF
	assert c.tile_size == 128
	assert c.atlas_width == 2048
	assert c.gutter == 2
	assert c.range == pytest.approx(0.1)
	assert c.coloring == Coloring.ByDistance

def test_config_kwargs_and_attrs():
	c = slughorn.SDF.Config(type=Type.SDF, tile_size=64, gutter=4)
	assert (c.type, c.tile_size, c.gutter) == (Type.SDF, 64, 4)
	c.range = 0.25
	assert c.range == pytest.approx(0.25)

def test_mask_sdf_tile():
	m = slughorn.Mask.sdf_tile("rect", True)
	assert m.type == slughorn.Mask.Type.SDFTile
	assert m.key is not None
	assert m.invert is True
	assert slughorn.Mask().type == slughorn.Mask.Type.SDFTile, "the default mask kind is the baked tile"
	assert m.params[2] == 1.0, "scale defaults to 1, not 0 (a zero scale would mask everything out)"


# ---------------------------------------------------------------------------
# Atlas.set_sdf / request_sdf lifecycle
# ---------------------------------------------------------------------------

@skip_sdf
def test_set_sdf_after_build_raises():
	with pytest.raises(RuntimeError):
		_rect_atlas().set_sdf(slughorn.SDF.Config())

@skip_sdf
def test_set_sdf_rejects_bad_config():
	atlas = slughorn.Atlas()
	with pytest.raises(ValueError):
		atlas.set_sdf(slughorn.SDF.Config(tile_size=0))
	with pytest.raises(ValueError):
		atlas.set_sdf(slughorn.SDF.Config(tile_size=128, atlas_width=64))

@skip_sdf
def test_request_sdf_after_build_raises():
	with pytest.raises(RuntimeError):
		_rect_atlas().request_sdf("rect")

@skip_sdf
def test_request_sdf_unknown_key_raises():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	with pytest.raises(IndexError):
		atlas.request_sdf("nope")

@skip_sdf
def test_request_sdf_batch_is_atomic():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "a")
	with pytest.raises(IndexError):
		atlas.request_sdf(["a", "missing"])
	atlas.build()
	assert atlas.get_shape("a").sdf is None, "a bad key must leave nothing half-requested"

@skip_sdf
def test_request_sdf_is_idempotent_first_range_wins():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	atlas.request_sdf("rect", 0.15)
	atlas.request_sdf("rect", 0.05)
	atlas.build()
	assert atlas.get_shape("rect").sdf.range == pytest.approx(0.15, abs=1e-6)
	assert atlas.packing_stats.sdf.tile_count == 1

@skip_sdf
def test_request_sdf_range_override():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	atlas.request_sdf("rect", 0.2)
	atlas.build()
	assert atlas.get_shape("rect").sdf.range == pytest.approx(0.2, abs=1e-6)

@skip_sdf
def test_default_range_is_resolved_at_bake_time():
	"""A request with no range uses the config that is current when build() runs, so set_sdf() may
	come AFTER request_sdf()."""
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	atlas.request_sdf("rect")
	atlas.set_sdf(slughorn.SDF.Config(range=0.2))
	atlas.build()
	assert atlas.get_shape("rect").sdf.range == pytest.approx(0.2, abs=1e-6)

@skip_sdf
def test_unrequested_shape_has_no_tile():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	atlas.build()
	assert atlas.get_shape("rect").sdf is None
	assert atlas.sdf.empty
	assert atlas.packing_stats.sdf.tile_count == 0

@skip_sdf
def test_shape_without_geometry_gets_no_tile():
	atlas = slughorn.Atlas()
	_add_rect(atlas, "rect")
	try:
		atlas.add_shape(slughorn.Key("empty"), slughorn.ShapeInfo())
	except Exception:
		pytest.skip("add_shape rejects curve-less shapes")
	atlas.request_sdf(["rect", "empty"])
	atlas.build()
	assert atlas.get_shape("rect").sdf is not None
	assert atlas.get_shape("empty").sdf is None


# ---------------------------------------------------------------------------
# Baked atlas: layout and stats
# ---------------------------------------------------------------------------

@skip_sdf
def test_tile_after_build():
	tile = _sdf_atlas(tile_size=32, range=0.1).get_shape("rect").sdf
	assert (tile.w, tile.h) == (32, 32)
	assert tile.range == pytest.approx(0.1, abs=1e-6)
	assert tile.em_origin == pytest.approx((-0.1, -0.1), abs=1e-5)
	assert tile.pixel_range == pytest.approx(2 * tile.range * tile.texels_per_em, rel=1e-6)

@skip_sdf
@pytest.mark.parametrize("type,fmt,channels", [(Type.SDF, "R32F", 1), (Type.MSDF, "RGB32F", 3)])
def test_texture_format_follows_type(type, fmt, channels):
	atlas = _sdf_atlas(type=type)
	tex = atlas.sdf.texture
	assert tex.format == fmt
	assert len(memoryview(tex.bytes)) == tex.width * tex.height * channels * 4
	assert atlas.sdf.config.type == type
	assert atlas.packing_stats.sdf.format == fmt

@skip_sdf
def test_texture_width_is_atlas_width():
	assert _sdf_atlas(tile_size=32, atlas_width=256).sdf.texture.width == 256

@skip_sdf
def test_tiles_respect_gutter_and_do_not_overlap():
	names = [f"s{i}" for i in range(9)]
	atlas = _sdf_atlas(names, tile_size=32, atlas_width=128, gutter=3)
	tex = atlas.sdf.texture
	tiles = [atlas.get_shape(n).sdf for n in names]
	assert all(t is not None for t in tiles)
	for t in tiles:
		assert t.x >= 3 and t.y >= 3
		assert t.x + t.w + 3 <= tex.width
		assert t.y + t.h + 3 <= tex.height
	for i, a in enumerate(tiles):
		for b in tiles[i + 1:]:
			separated = (
				a.x + a.w + 3 <= b.x or b.x + b.w + 3 <= a.x or
				a.y + a.h + 3 <= b.y or b.y + b.h + 3 <= a.y
			)
			assert separated, "tiles must keep a gutter between them"

@skip_sdf
def test_many_shapes_of_different_sizes_all_pack():
	atlas = slughorn.Atlas()
	atlas.set_sdf(slughorn.SDF.Config(type=Type.SDF, tile_size=48, atlas_width=512))
	names = []
	for i in range(40):
		w, h = 0.3 + (i * 7 % 11) / 10.0, 0.3 + (i * 5 % 13) / 10.0
		names.append(f"r{i}")
		_add_rect(atlas, names[-1], w, h)
	atlas.request_sdf(names)
	atlas.build()
	tex = atlas.sdf.texture
	rects = []
	for n in names:
		t = atlas.get_shape(n).sdf
		assert t is not None
		assert t.x + t.w <= tex.width and t.y + t.h <= tex.height
		rects.append((t.x, t.y, t.x + t.w, t.y + t.h))
	for i, a in enumerate(rects):
		for b in rects[i + 1:]:
			assert a[2] <= b[0] or b[2] <= a[0] or a[3] <= b[1] or b[3] <= a[1], "tiles overlap"
	assert atlas.packing_stats.sdf.tile_count == 40

@skip_sdf
def test_packing_stats():
	atlas = _sdf_atlas(["a", "b"], tile_size=32, atlas_width=256)
	st = atlas.packing_stats.sdf
	tex = atlas.sdf.texture
	assert st.tile_count == 2
	assert st.texels_total == tex.width * tex.height
	assert st.texels_used + st.texels_padding == st.texels_total
	assert 0.0 < st.utilization() <= 1.0
	assert st.bytes() == len(memoryview(tex.bytes))

@skip_sdf
def test_total_bytes_includes_the_sdf_atlas():
	with_sdf = _sdf_atlas(tile_size=32).packing_stats
	plain = _rect_atlas().packing_stats
	assert with_sdf.total_bytes() - plain.total_bytes() == with_sdf.sdf.bytes()

@skip_sdf
def test_baking_is_deterministic():
	a = _sdf_atlas(["a", "b", "c"], tile_size=32, atlas_width=128)
	b = _sdf_atlas(["a", "b", "c"], tile_size=32, atlas_width=128)
	assert bytes(memoryview(a.sdf.texture.bytes)) == bytes(memoryview(b.sdf.texture.bytes))
	for n in "abc":
		ta, tb = a.get_shape(n).sdf, b.get_shape(n).sdf
		assert (ta.x, ta.y, ta.w, ta.h) == (tb.x, tb.y, tb.w, tb.h)

@skip_sdf
def test_baked_texels_match_render_field():
	"""What build() packs into the atlas is exactly what render.field() returns."""
	atlas = _sdf_atlas(type=Type.MSDF, tile_size=32)
	tile = atlas.get_shape("rect").sdf
	field = np.asarray(slughorn.render.field(atlas, "rect", atlas.sdf.config))
	patch = _pixels(atlas)[tile.y:tile.y + tile.h, tile.x:tile.x + tile.w]
	assert np.array_equal(patch, field)

@skip_sdf
def test_gutter_is_deeply_exterior():
	px = _pixels(_sdf_atlas(type=Type.SDF, tile_size=32, gutter=2))
	assert px[0, :].max() == 0.0, "the bottom gutter rows must stay zero (deep exterior)"
	assert px[:, 0].max() == 0.0, "the left gutter columns must stay zero"


# ---------------------------------------------------------------------------
# The tile CONTRACT, checked by sampling the baked texture
# ---------------------------------------------------------------------------

@skip_sdf
@pytest.mark.parametrize("type", [Type.SDF, Type.MSDF])
def test_contract_straight_edges_of_a_square(type):
	atlas = _sdf_atlas(type=type, tile_size=64, range=0.1)
	# 0.05 em from an edge: half the range -> 0.75 inside, 0.25 outside; center is deep inside.
	assert _sample(atlas, "rect", 0.5, 0.5) == pytest.approx(1.0, abs=0.02)
	for name, inside, outside in [
		("left", (0.05, 0.5), (-0.05, 0.5)),
		("right", (0.95, 0.5), (1.05, 0.5)),
		("bottom", (0.5, 0.05), (0.5, -0.05)),
		("top", (0.5, 0.95), (0.5, 1.05)),
	]:
		assert _sample(atlas, "rect", *inside) == pytest.approx(0.75, abs=0.03), name
		assert _sample(atlas, "rect", *outside) == pytest.approx(0.25, abs=0.03), name

@skip_sdf
def test_contract_range_saturates_at_the_range():
	atlas = _sdf_atlas(type=Type.SDF, tile_size=64, range=0.1)
	assert _sample(atlas, "rect", -0.09, 0.5) == pytest.approx(0.05, abs=0.03)
	assert _sample(atlas, "rect", 0.5, 0.5) == pytest.approx(1.0, abs=0.02)

@skip_sdf
@pytest.mark.parametrize("range", [0.05, 0.1, 0.2])
def test_contract_range_maps_to_the_full_0_1_span(range):
	atlas = _sdf_atlas(type=Type.SDF, tile_size=96, range=range)
	assert _sample(atlas, "rect", -range / 2, 0.5) == pytest.approx(0.25, abs=0.04)
	assert _sample(atlas, "rect", range / 2, 0.5) == pytest.approx(0.75, abs=0.04)

@skip_sdf
def test_contract_y_orientation_and_axes_are_not_swapped():
	"""Each pair below is a point inside the triangle and the point a Y flip (about the shape's
	midline, y = 0.25) or an x/y swap would land it on - which is outside. Both must classify the
	right way round."""
	atlas = _triangle_atlas(type=Type.SDF, tile_size=96, range=0.1)
	inside = [(0.8, 0.05), (0.5, 0.05), (0.05, 0.3), (0.05, 0.05)]
	outside = [
		(0.05, 0.8),   # x/y swap of (0.8, 0.05)
		(0.5, 0.45),   # Y flip of (0.5, 0.05)
		(0.05, -0.05),  # below the bottom edge
		(-0.05, 0.05),  # left of the left edge
		(0.9, 0.3),    # beyond the hypotenuse
	]
	for pt in inside:
		assert _sample(atlas, "tri", *pt) > 0.5, f"{pt} should be inside"
	for pt in outside:
		assert _sample(atlas, "tri", *pt) < 0.5, f"{pt} should be outside"

@skip_sdf
def test_contract_matches_brute_force_polygon_distance():
	rng = 0.1
	atlas = _triangle_atlas(type=Type.SDF, tile_size=128, range=rng)
	worst = 0.0
	for gx in _linspace(-0.09, 1.09, 15):
		for gy in _linspace(-0.09, 0.59, 15):
			want = _expected_value(_polygon_sd((gx, gy), TRIANGLE), rng)
			worst = max(worst, abs(_sample(atlas, "tri", gx, gy) - want))
	assert worst < 0.06, f"worst deviation from the true SDF: {worst}"



@skip_sdf
def test_contract_msdf_agrees_in_sign_with_the_true_distance():
	atlas = _triangle_atlas(type=Type.MSDF, tile_size=128, range=0.1)
	for gx in _linspace(-0.09, 1.09, 13):
		for gy in _linspace(-0.09, 0.59, 13):
			sd = _polygon_sd((gx, gy), TRIANGLE)
			if abs(sd) < 0.03:
				continue  # too close to the edge for a stable sign after bilinear filtering
			assert (_sample(atlas, "tri", gx, gy) > 0.5) == (sd > 0), (gx, gy)

@skip_sdf
def test_msdf_matches_sdf_along_straight_edges():
	"""Away from corners a median-of-three MSDF reproduces the true distance."""
	sdf = _sdf_atlas(type=Type.SDF, tile_size=64, range=0.1)
	msdf = _sdf_atlas(type=Type.MSDF, tile_size=64, range=0.1)
	for pt in [(0.5, 0.06), (0.5, -0.06), (0.06, 0.5), (-0.06, 0.5), (0.5, 0.94), (0.94, 0.5)]:
		assert _sample(msdf, "rect", *pt) == pytest.approx(_sample(sdf, "rect", *pt), abs=0.04), pt

@skip_sdf
@pytest.mark.parametrize("coloring", [Coloring.Simple, Coloring.ByDistance])
def test_both_colorings_bake_a_valid_msdf(coloring):
	atlas = _sdf_atlas(type=Type.MSDF, tile_size=64, coloring=coloring)
	assert _sample(atlas, "rect", 0.5, 0.5) == pytest.approx(1.0, abs=0.02)
	assert _sample(atlas, "rect", -0.05, 0.5) == pytest.approx(0.25, abs=0.03)


# ---------------------------------------------------------------------------
# Canvas integration
# ---------------------------------------------------------------------------

@skip_sdf
def test_canvas_set_sdf_requests_each_commit():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.set_sdf(True, 0.1)
	canvas.circle(0.5, 0.5, 0.4)
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, "circle")
	canvas.finalize("comp")
	atlas.build()
	assert canvas.sdf is True
	assert atlas.get_shape("circle").sdf is not None

@skip_sdf
def test_canvas_without_set_sdf_requests_nothing():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.circle(0.5, 0.5, 0.4)
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, "circle")
	canvas.finalize("comp")
	atlas.build()
	assert canvas.sdf is False
	assert atlas.sdf.empty

@skip_sdf
def test_canvas_mask_requests_a_tile_and_uses_the_sdf_tile_type():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.circle(0.5, 0.5, 0.4)
	mask = canvas.mask(0.1, False)
	assert mask.type == slughorn.Mask.Type.SDFTile
	assert mask.key is not None
	canvas.finalize("comp")
	atlas.build()
	assert atlas.get_shape(mask.key).sdf is not None

@skip_sdf
def test_canvas_mask_records_the_canvas_space_origin_of_the_tile_shape():
	# The shape is localized (bbox min -> em origin), so the tile alone cannot say where the mask
	# sits on the canvas; params[0..1] carry that. Circle (0.5, 0.5) r 0.4 -> bbox min (0.1, 0.1).
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.circle(0.5, 0.5, 0.4)
	mask = canvas.mask(0.1, False)
	assert mask.params[0] == pytest.approx(0.1, abs=1e-5)
	assert mask.params[1] == pytest.approx(0.1, abs=1e-5)
	assert mask.params[2] == 1.0


# ---------------------------------------------------------------------------
# `bin/slughorn sdf` - atlas PNG + osgx_sdf glTF manifest
# ---------------------------------------------------------------------------

def _run_cli(tmp_path, *args):
	env = dict(os.environ)
	env["PYTHONPATH"] = str(Path(slughorn.__file__).resolve().parent) + os.pathsep + env.get("PYTHONPATH", "")
	return subprocess.run(
		[sys.executable, str(CLI), *args], cwd=tmp_path, env=env, capture_output=True, text=True
	)


def _cli_atlas_file(tmp_path, type):
	atlas = slughorn.Atlas()
	atlas.set_sdf(slughorn.SDF.Config(type=type, tile_size=64, range=0.1))
	_add_polygon(atlas, "tri", TRIANGLE)
	_add_rect(atlas, "rect")
	atlas.request_sdf(["tri", "rect"])
	atlas.build()
	path = tmp_path / "in.slug"
	slughorn.write(atlas, str(path))
	return atlas, path


@skip_sdf
@skip_serial
@pytest.mark.parametrize("type", [Type.SDF, Type.MSDF])
def test_cli_writes_png_and_osgx_sdf_manifest(tmp_path, type):
	Image = pytest.importorskip("PIL.Image")
	atlas, path = _cli_atlas_file(tmp_path, type)
	result = _run_cli(tmp_path, "sdf", str(path), "-o", "out.png")
	assert result.returncode == 0, result.stderr

	manifest = json.loads((tmp_path / "out.gltf").read_text())
	assert manifest["extensionsUsed"] == ["osgx_sdf"]
	assert manifest["asset"]["version"] == "2.0"

	entry = manifest["extensions"]["osgx_sdf"]["data"][0]
	assert entry["type"] == ("MSDF" if type == Type.MSDF else "SDF")
	assert entry["texture"] == {"uri": "out.png"}
	assert set(entry["tiles"]) == {"tri", "rect"}

	image = Image.open(tmp_path / "out.png")
	assert image.mode == ("RGB" if type == Type.MSDF else "L")
	assert image.size == (atlas.sdf.texture.width, atlas.sdf.texture.height)

	for name, tile in entry["tiles"].items():
		baked = atlas.get_shape(name).sdf
		x, y, w, h = tile["rect"]
		assert (w, h) == (baked.w, baked.h)
		assert 0 <= x and x + w <= image.width and 0 <= y and y + h <= image.height
		# rect is measured from the TOP-left (glTF convention); the texture itself is bottom-up.
		assert (x, y) == (baked.x, image.height - baked.y - baked.h)
		assert tile["pixelRange"] == pytest.approx(baked.pixel_range, rel=1e-6)
		assert tile["range"] == pytest.approx(baked.range, rel=1e-6)
		assert tile["texelsPerEm"] == pytest.approx(baked.texels_per_em, rel=1e-6)
		assert tile["emOrigin"] == pytest.approx(baked.em_origin, abs=1e-6)


@skip_sdf
@skip_serial
def test_cli_manifest_matches_the_png_pixels(tmp_path):
	"""Locate em-space points in the saved PNG using ONLY the manifest, and check the picture agrees:
	the image must be upright and `rect`/`emOrigin`/`texelsPerEm` must line up with it."""
	Image = pytest.importorskip("PIL.Image")
	_cli_atlas_file(tmp_path, Type.SDF)
	assert _run_cli(tmp_path, "sdf", "in.slug", "-o", "out.png").returncode == 0
	tile = json.loads((tmp_path / "out.gltf").read_text())["extensions"]["osgx_sdf"]["data"][0]["tiles"]["tri"]
	image = Image.open(tmp_path / "out.png")
	x, y, w, h = tile["rect"]
	ox, oy = tile["emOrigin"]

	def pixel(ex, ey):
		px = x + (ex - ox) * tile["texelsPerEm"]
		py = y + h - (ey - oy) * tile["texelsPerEm"]  # image rows grow DOWNWARD
		return image.getpixel((int(px), int(py))) / 255.0

	for pt in [(0.8, 0.05), (0.05, 0.3), (0.05, 0.05)]:
		assert pixel(*pt) > 0.5, f"{pt} should be inside"
	for pt in [(0.8, 0.3), (0.5, 0.45), (0.05, -0.05)]:
		assert pixel(*pt) < 0.5, f"{pt} should be outside"


@skip_sdf
@skip_serial
def test_cli_no_flip_y_writes_no_manifest(tmp_path):
	pytest.importorskip("PIL.Image")
	_cli_atlas_file(tmp_path, Type.SDF)
	result = _run_cli(tmp_path, "sdf", "in.slug", "-o", "raw.png", "--no-flip-y")
	assert result.returncode == 0, result.stderr
	assert (tmp_path / "raw.png").exists()
	assert not (tmp_path / "raw.gltf").exists(), "a bottom-up dump cannot be described by osgx_sdf"


@skip_sdf
@skip_serial
def test_cli_single_tile_is_cropped(tmp_path):
	Image = pytest.importorskip("PIL.Image")
	atlas, _ = _cli_atlas_file(tmp_path, Type.MSDF)
	result = _run_cli(tmp_path, "sdf", "in.slug", "tri", "tri.png")
	assert result.returncode == 0, result.stderr
	baked = atlas.get_shape("tri").sdf
	assert Image.open(tmp_path / "tri.png").size == (baked.w, baked.h)
