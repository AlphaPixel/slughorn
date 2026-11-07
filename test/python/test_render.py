"""
Tests for slughorn/render.hpp — slughorn.render submodule.

render submodule free functions (atlas as first arg):
  render.decode(atlas, key)    -> Sampler

Atlas convenience wrappers (key as first arg, atlas implicit):
  atlas.decode(key)            -> Sampler

(SDF/MSDF tile baking - render.field(), set_sdf/request_sdf, Shape.sdf - lives in test_sdf.py.)
"""

import math
import pytest
import slughorn
from conftest import rect_curves


# ---------------------------------------------------------------------------
# Module / class availability
# ---------------------------------------------------------------------------

def test_render_submodule_exists():
	assert hasattr(slughorn, "render")

def test_sample_class_exists():
	assert hasattr(slughorn.render, "Sample")

def test_sampler_class_exists():
	assert hasattr(slughorn.render, "Sampler")

def test_render_decode_function_exists():
	assert hasattr(slughorn.render, "decode")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _make_built_atlas():
	atlas = slughorn.Atlas()
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.line_to(1.0, 0.0)
	d.line_to(1.0, 1.0)
	d.line_to(0.0, 1.0)
	d.close()
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	atlas.add_shape(slughorn.Key("rect"), info)
	atlas.build()
	return atlas


# ---------------------------------------------------------------------------
# render.decode
# ---------------------------------------------------------------------------

def test_render_decode_returns_sampler():
	atlas = _make_built_atlas()
	sampler = slughorn.render.decode(atlas, slughorn.Key("rect"))
	assert isinstance(sampler, slughorn.render.Sampler)

def test_atlas_decode_convenience_wrapper():
	# atlas.decode(key) must return the same type as render.decode(atlas, key).
	atlas = _make_built_atlas()
	sampler = atlas.decode(slughorn.Key("rect"))
	assert isinstance(sampler, slughorn.render.Sampler)

def test_render_decode_str_key():
	atlas = _make_built_atlas()
	sampler = slughorn.render.decode(atlas, "rect")
	assert isinstance(sampler, slughorn.render.Sampler)


# ---------------------------------------------------------------------------
# Sampler properties
# ---------------------------------------------------------------------------

def test_sampler_curves_nonempty():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert len(sampler.curves) > 0

def test_sampler_curve_buffer_shape():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	# curve_buffer is a 2-D float32 array; second axis is always 6.
	assert sampler.curve_buffer.shape[1] == 6

def test_sampler_curve_buffer_rows_match_curves():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert sampler.curve_buffer.shape[0] == len(sampler.curves)

def test_sampler_indir_x_length():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert sampler.indir_x.shape[0] == 32

def test_sampler_indir_y_length():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert sampler.indir_y.shape[0] == 32

def test_sampler_hband_offsets_nonempty():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert len(sampler.hband_offsets) > 0

def test_sampler_vband_offsets_nonempty():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert len(sampler.vband_offsets) > 0

def test_sampler_hband_indices_nonempty():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert len(sampler.hband_indices) > 0

def test_sampler_vband_indices_nonempty():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert len(sampler.vband_indices) > 0

def test_sampler_repr():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	r = repr(sampler)
	assert "Sampler(" in r
	assert "curves=" in r


# ---------------------------------------------------------------------------
# Sampler.get_hband / get_vband
# ---------------------------------------------------------------------------

def test_sampler_get_hband_returns_list():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert isinstance(sampler.get_hband(0), list)

def test_sampler_get_vband_returns_list():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	assert isinstance(sampler.get_vband(0), list)

def test_sampler_get_hband_out_of_range():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	with pytest.raises((IndexError, Exception)):
		sampler.get_hband(9999)

def test_sampler_get_vband_out_of_range():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	with pytest.raises((IndexError, Exception)):
		sampler.get_vband(9999)


# ---------------------------------------------------------------------------
# render_sample / render_sample_banded
# ---------------------------------------------------------------------------

def test_render_sample_fill_range():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	s = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	assert 0.0 <= s.fill <= 1.0

def test_render_sample_banded_fill_range():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	s = sampler.render_sample_banded(0.5, 0.5, 16.0, 16.0)
	assert 0.0 <= s.fill <= 1.0

def test_render_sample_matches_banded():
	# Both paths must agree to within a small tolerance.
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	ref    = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	banded = sampler.render_sample_banded(0.5, 0.5, 16.0, 16.0)
	assert ref.fill == pytest.approx(banded.fill, abs=1e-5)

def test_render_sample_outside_shape():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	# Well outside the unit square — coverage should be ~0.
	s = sampler.render_sample(5.0, 5.0, 16.0, 16.0)
	assert s.fill == pytest.approx(0.0, abs=0.01)

def test_render_sample_inside_shape():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	# Centre of the unit square — coverage should be ~1.
	s = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	assert s.fill == pytest.approx(1.0, abs=0.05)


# ---------------------------------------------------------------------------
# aa_ramp / EmJacobian
# ---------------------------------------------------------------------------

def _make_diamond_sampler():
	# Edges at 45 degrees in em space: the lower-left one lies on x + y = 0.5.
	atlas = slughorn.Atlas()
	d = slughorn.CurveDecomposer()
	d.move_to(0.5, 0.0)
	d.line_to(1.0, 0.5)
	d.line_to(0.5, 1.0)
	d.line_to(0.0, 0.5)
	d.close()
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	atlas.add_shape(slughorn.Key("diamond"), info)
	atlas.build()
	return atlas.decode("diamond")

def _diamond_point(inside_px, ppe):
	# A point inside_px screen pixels inside the lower-left edge's midpoint, along its normal.
	o = inside_px / ppe / math.sqrt(2.0)
	return 0.25 + o, 0.25 + o

def test_aa_ramp_default_edge():
	sampler = _make_built_atlas().decode("rect")
	assert sampler.aa_ramp == slughorn.render.AARamp.Edge
	sampler.aa_ramp = slughorn.render.AARamp.Axis
	assert sampler.aa_ramp == slughorn.render.AARamp.Axis

def test_em_jacobian_from_pixels_per_em():
	em = slughorn.render.EmJacobian.from_pixels_per_em(16.0, 32.0)
	assert em.dx_x == pytest.approx(1.0 / 16.0)
	assert em.dy_y == pytest.approx(1.0 / 32.0)
	assert em.dx_y == 0.0 and em.dy_x == 0.0
	assert em.ppe_x == pytest.approx(16.0)
	assert em.ppe_y == pytest.approx(32.0)

def test_em_jacobian_from_rotation_ppe():
	# Rotated 45 degrees, fwidth sees |cos| + |sin| = sqrt(2) em-pixels per pixel per axis.
	em = slughorn.render.EmJacobian.from_rotation(math.pi / 4.0, 64.0)
	assert em.ppe_x == pytest.approx(64.0 / math.sqrt(2.0))
	assert em.ppe_y == pytest.approx(64.0 / math.sqrt(2.0))

@pytest.mark.parametrize("ramp", ["Axis", "Edge"])
def test_render_sample_jacobian_matches_ppe(ramp):
	# An axis-aligned Jacobian gives exactly the per-axis entry points' result in both modes.
	sampler = _make_diamond_sampler()
	sampler.aa_ramp = getattr(slughorn.render.AARamp, ramp)
	em = slughorn.render.EmJacobian.from_pixels_per_em(64.0, 64.0)
	for px in (-0.4, 0.0, 0.3):
		x, y = _diamond_point(px, 64.0)
		assert sampler.render_sample(x, y, em).fill == pytest.approx(
			sampler.render_sample(x, y, 64.0, 64.0).fill, abs=1e-6
		)
		assert sampler.render_sample_banded(x, y, em).fill == pytest.approx(
			sampler.render_sample_banded(x, y, 64.0, 64.0).fill, abs=1e-6
		)

def test_render_sample_edge_unrotated_ramp():
	# A 45 degree edge on screen: Edge ramps over |cos| + |sin| = sqrt(2) pixels.
	sampler = _make_diamond_sampler()
	sampler.aa_ramp = slughorn.render.AARamp.Edge
	x, y = _diamond_point(0.3, 64.0)
	assert sampler.render_sample(x, y, 64.0, 64.0).fill == pytest.approx(
		0.3 / math.sqrt(2.0) + 0.5, abs=1e-3
	)

def test_render_sample_edge_rotated_jacobian():
	# The diamond's 45 degree edge rotated 45 degrees on screen is screen-aligned: its ramp is 1
	# pixel wide. Per-axis rates alone (what fwidth gives) see both rotations and ramp over 2.
	sampler = _make_diamond_sampler()
	sampler.aa_ramp = slughorn.render.AARamp.Edge
	em = slughorn.render.EmJacobian.from_rotation(math.pi / 4.0, 64.0)
	x, y = _diamond_point(0.3, 64.0)
	assert sampler.render_sample(x, y, em).fill == pytest.approx(0.8, abs=1e-3)
	assert sampler.render_sample_banded(x, y, em).fill == pytest.approx(0.8, abs=1e-3)
	assert sampler.render_sample(x, y, em.ppe_x, em.ppe_y).fill == pytest.approx(0.65, abs=1e-3)


# ---------------------------------------------------------------------------
# Sample fields and repr
# ---------------------------------------------------------------------------

def test_sample_fields_finite():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	s = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	assert math.isfinite(s.fill)
	assert math.isfinite(s.xcov)
	assert math.isfinite(s.ycov)
	assert math.isfinite(s.xwgt)
	assert math.isfinite(s.ywgt)
	assert isinstance(s.iters, int)

def test_sample_repr():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	s = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	r = repr(s)
	assert "Sample(" in r
	assert "fill=" in r


# ---------------------------------------------------------------------------
# render_grid
# ---------------------------------------------------------------------------

def test_render_grid_shape():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	grid = sampler.render_grid(16, 0.0, True)
	assert (grid.height, grid.width) == (16, 16)

def test_render_grid_value_range():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	grid = sampler.render_grid(16, 0.0, True)
	flat = memoryview(grid).cast('B').cast('f')
	assert min(flat) >= 0.0
	assert max(flat) <= 1.0 + 1e-6

def test_render_grid_solid_rect_coverage():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	grid = sampler.render_grid(32, 0.0, True)
	assert max(memoryview(grid).cast('B').cast('f')) > 0.9, "solid unit rect should have near-full coverage at centre"

def test_render_grid_unbanded_matches_banded():
	atlas = _make_built_atlas()
	sampler = atlas.decode("rect")
	banded   = sampler.render_grid(16, 0.0, True)
	unbanded = sampler.render_grid(16, 0.0, False)
	b = memoryview(banded).cast('B').cast('f')
	u = memoryview(unbanded).cast('B').cast('f')
	assert all(abs(b[i] - u[i]) < 1e-4 for i in range(len(b)))
