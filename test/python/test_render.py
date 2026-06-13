"""
Tests for slughorn/render.hpp — slughorn.render submodule.

render submodule free functions (atlas as first arg):
  render.decode(atlas, key)    -> Sampler
  render.sdf(atlas, key, ...)  -> Grid        (requires SLUGHORN_MSDF)
  render.msdf(atlas, key, ...) -> MSDFGrid    (requires SLUGHORN_MSDF)
  render.msdf_tile(atlas, key, ...) -> MSDFGrid  (square tiles; requires SLUGHORN_MSDF)

Atlas convenience wrappers (key as first arg, atlas implicit):
  atlas.decode(key)            -> Sampler
  atlas.render_sdf(key, ...)   -> Grid        (requires SLUGHORN_MSDF)
  atlas.render_msdf(key, ...)  -> MSDFGrid    (requires SLUGHORN_MSDF)
  atlas.render_msdf_tile(key, ...) -> MSDFGrid   (square tiles; requires SLUGHORN_MSDF)
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


# ---------------------------------------------------------------------------
# SDF / MSDF — render submodule free functions and atlas convenience wrappers
# (skipped when slughorn is built without SLUGHORN_MSDF=ON)
# ---------------------------------------------------------------------------

msdf = pytest.importorskip("slughorn", reason="slughorn module missing")
HAS_MSDF = hasattr(slughorn.render, "sdf")


@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_submodule_sdf_exists():
	assert hasattr(slughorn.render, "sdf")

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_submodule_msdf_exists():
	assert hasattr(slughorn.render, "msdf")

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_submodule_msdf_tile_exists():
	assert hasattr(slughorn.render, "msdf_tile")

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_atlas_render_msdf_tile_exists():
	assert hasattr(slughorn.Atlas, "render_msdf_tile")


# render.sdf

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_sdf_returns_grid():
	atlas = _make_built_atlas()
	grid = slughorn.render.sdf(atlas, "rect", 32)
	assert isinstance(grid, slughorn.render.Grid)

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_sdf_interior_exterior():
	atlas = _make_built_atlas()
	grid = slughorn.render.sdf(atlas, "rect", 32)
	flat = memoryview(grid).cast('B').cast('f')
	assert max(flat) > 0.5, "interior should be > 0.5"
	assert min(flat) < 0.5, "exterior should be < 0.5"


# render.msdf

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_returns_msdf_grid():
	atlas = _make_built_atlas()
	grid = slughorn.render.msdf(atlas, "rect", 32)
	assert isinstance(grid, slughorn.render.MSDFGrid)

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_interior_exterior():
	import numpy as np
	atlas = _make_built_atlas()
	arr = np.asarray(slughorn.render.msdf(atlas, "rect", 32))
	def median3(r, g, b): return max(min(r, g), min(max(r, g), b))
	h, w = arr.shape[:2]
	center = median3(arr[h//2, w//2, 0], arr[h//2, w//2, 1], arr[h//2, w//2, 2])
	corner = median3(arr[0, 0, 0], arr[0, 0, 1], arr[0, 0, 2])
	assert center > 0.5, "center should be interior (>0.5)"
	assert corner < 0.5, "corner should be exterior (<0.5)"

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_matches_atlas_render_msdf():
	import numpy as np
	atlas = _make_built_atlas()
	via_module = np.asarray(slughorn.render.msdf(atlas, "rect", 32))
	via_atlas  = np.asarray(atlas.render_msdf("rect", tile_size=32))
	assert np.allclose(via_module, via_atlas)


# render.msdf_tile / atlas.render_msdf_tile

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_tile_is_square():
	atlas = _make_built_atlas()
	grid = slughorn.render.msdf_tile(atlas, "rect", 64)
	assert grid.width == 64
	assert grid.height == 64

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_tile_interior_exterior():
	import numpy as np
	# Use a diamond shape (vertices at the mid-points of the unit bbox) so that
	# the bbox corners are genuinely exterior.  renderMSDFTile maps the tile
	# exactly to the shape's bounding box with no range expansion, so a rect
	# would put every tile pixel inside-or-on-boundary of the shape.
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
	arr = np.asarray(slughorn.render.msdf_tile(atlas, "diamond", 64))
	def median3(r, g, b): return max(min(r, g), min(max(r, g), b))
	center = median3(arr[32, 32, 0], arr[32, 32, 1], arr[32, 32, 2])
	corner = median3(arr[0, 0, 0], arr[0, 0, 1], arr[0, 0, 2])
	assert center > 0.5, "center should be interior (>0.5)"
	assert corner < 0.5, "corner should be exterior (<0.5)"

@pytest.mark.skipif(not HAS_MSDF, reason="built without SLUGHORN_MSDF=ON")
def test_render_msdf_tile_matches_atlas_wrapper():
	import numpy as np
	atlas = _make_built_atlas()
	via_module = np.asarray(slughorn.render.msdf_tile(atlas, "rect", 64))
	via_atlas  = np.asarray(atlas.render_msdf_tile("rect", tile_size=64))
	assert np.allclose(via_module, via_atlas)
