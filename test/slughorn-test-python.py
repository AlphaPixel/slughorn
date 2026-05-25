#!/usr/bin/env python3

from __future__ import annotations

import math
import sys
from pathlib import Path

import pytest

def _import_slughorn():
	root = Path(__file__).resolve().parents[1]
	build_dir = root / "BUILD"

	if not build_dir.exists():
		raise RuntimeError(f"Missing build directory: {build_dir}")

	sys.path.insert(0, str(build_dir))

	import slughorn as module

	return module

slughorn = _import_slughorn()

def _rect_curves(x0: float = 0.0, y0: float = 0.0, x1: float = 1.0, y1: float = 1.0):
	d = slughorn.CurveDecomposer()

	d.move_to(x0, y0)
	d.line_to(x1, y0)
	d.line_to(x1, y1)
	d.line_to(x0, y1)
	d.close()

	return d.get_curves()

def _gradient_stops():
	return [
		slughorn.GradientStop(0.0, slughorn.Color(1.0, 0.0, 0.0, 1.0)),
		slughorn.GradientStop(1.0, slughorn.Color(0.0, 0.0, 1.0, 1.0)),
	]

def test_core_types_and_curve_decomposer():
	cp_key = slughorn.Key(65)
	name_key = slughorn.Key("logo")

	assert cp_key == slughorn.Key(65)
	assert cp_key != name_key
	assert hash(cp_key) == hash(slughorn.Key(65))
	assert "Key(" in repr(cp_key)
	assert "logo" in repr(name_key)

	iterator = slughorn.KeyIterator("glyph")

	assert iterator.next() == slughorn.Key("glyph_0")
	assert next(iterator) == slughorn.Key("glyph_1")

	color = slughorn.Color(0.25, 0.5, 0.75, 1.0)

	assert "Color(" in repr(color)

	matrix = slughorn.Matrix()
	matrix.dx = 2.0
	matrix.dy = -3.0

	assert matrix.apply(1.0, 2.0) == pytest.approx((3.0, -1.0))
	assert matrix.is_identity() is False
	assert "Matrix(" in repr(matrix)

	stop = slughorn.GradientStop(0.5, color)
	info = slughorn.GradientInfo()
	info.type = slughorn.GradientInfo.Type.Linear
	info.stops = [stop]
	info.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)

	assert "GradientStop(" in repr(stop)
	assert info.stops[0].t == pytest.approx(0.5)

	decomposer = slughorn.CurveDecomposer()

	decomposer.tolerance = 1e-3
	decomposer.move_to(0.0, 0.0)
	decomposer.line_to(1.0, 0.0)
	decomposer.quad_to(1.0, 0.5, 1.0, 1.0)
	decomposer.cubic_to(0.75, 1.25, 0.25, 1.25, 0.0, 1.0)
	decomposer.close()

	curves = decomposer.get_curves()

	assert len(decomposer) == len(curves)
	assert len(curves) >= 4

	splits_x, splits_y = slughorn.Atlas.compute_uniform_splits(curves, 3, 2)

	assert splits_x == pytest.approx([1.0 / 3.0, 2.0 / 3.0])
	assert splits_y == pytest.approx([0.5])

	decomposer.clear()

	assert len(decomposer) == 0

def test_atlas_build_decode_and_render_surface():
	atlas = slughorn.Atlas()

	shape_key = slughorn.Key("rect")
	composite_key = slughorn.Key("rect_composite")

	info = slughorn.ShapeInfo()
	info.curves = _rect_curves()
	info.origin = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)

	ginfo = slughorn.GradientInfo()
	ginfo.type = slughorn.GradientInfo.Type.Linear
	ginfo.stops = _gradient_stops()
	ginfo.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)

	gradient_id = atlas.add_gradient(ginfo)

	assert gradient_id == 1

	atlas.add_shape(shape_key, info)

	composite = slughorn.CompositeShape()

	composite.layers.append(
		slughorn.Layer(
			shape_key,
			slughorn.Color(0.2, 0.4, 0.8, 1.0),
			slughorn.Matrix(),
			1.0,
			0,
			gradient_id,
		)
	)

	composite.advance = 1.25

	atlas.add_composite_shape(composite_key, composite)

	assert atlas.has_key(shape_key) is True
	assert atlas.get_composite_shape(composite_key).advance == pytest.approx(1.25)

	atlas.build()

	assert atlas.is_built is True

	shape = atlas.get_shape(shape_key)

	assert shape is not None
	assert shape.width > 0.0
	assert shape.height > 0.0
	assert shape.compute_quad(slughorn.Matrix()).x1 > shape.compute_quad(slughorn.Matrix()).x0
	assert shape.em_to_uv(*shape.em_origin) == pytest.approx((0.0, 0.0))

	curve_texture = atlas.curve_texture
	band_texture = atlas.band_texture
	gradient_texture = atlas.gradient_texture

	assert curve_texture.format == "RGBA32F"
	assert band_texture.format == "RGBA16UI"
	assert gradient_texture.format == "RGBA8"
	assert len(bytes(curve_texture.bytes)) > 0
	assert len(bytes(band_texture.bytes)) > 0
	assert len(bytes(gradient_texture.bytes)) > 0

	stats = atlas.packing_stats

	assert stats.curve_texels_total > 0
	assert stats.band_texels_total > 0
	assert stats.gradient_count == 1

	sampler = atlas.decode(shape_key)

	assert len(sampler.curves) > 0
	assert sampler.curve_buffer.shape[1] == 6
	assert sampler.indir_x.shape[0] == 32
	assert sampler.indir_y.shape[0] == 32
	assert isinstance(sampler.get_hband(0), list)
	assert isinstance(sampler.get_vband(0), list)

	ref = sampler.render_sample(0.5, 0.5, 16.0, 16.0)
	banded = sampler.render_sample_banded(0.5, 0.5, 16.0, 16.0)

	assert 0.0 <= ref.fill <= 1.0
	assert 0.0 <= banded.fill <= 1.0
	assert ref.fill == pytest.approx(banded.fill, abs=1e-5)

	grid = sampler.render_grid(16, 0.0, True)

	assert tuple(grid.shape) == (16, 16)
	assert float(grid.max()) <= 1.0 + 1e-6

def test_canvas_path_and_canvas_decomposer_access():
	path = slughorn.canvas.Path()

	path_decomposer = path.decomposer()
	path_decomposer.tolerance = 1e-4

	assert path.decomposer().tolerance == pytest.approx(1e-4)

	path.move_to(0.0, 0.0)
	path.line_to(1.0, 0.0)
	path.quad_to(1.0, 0.5, 1.0, 1.0)
	path.arc_to(0.5, 1.25, 0.0, 1.0, 0.2)

	assert path.has_pending_path is True
	assert path.arc_length() > 0.0

	sample = path.sample(0.5)

	assert isinstance(sample.angle, float)
	assert math.isfinite(sample.x)
	assert math.isfinite(sample.y)
	assert "Path.Sample" in repr(sample)

	path_copy = slughorn.canvas.Path()
	path_copy.add_path(path)

	assert path_copy.arc_length() == pytest.approx(path.arc_length(), rel=1e-5)

def test_canvas_commit_paths_gradients_and_composites():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("canvas"))

	canvas_decomposer = canvas.decomposer()
	canvas_decomposer.tolerance = 5e-4

	assert canvas.decomposer().tolerance == pytest.approx(5e-4)

	canvas.begin_composite()
	canvas.rect(0.0, 0.0, 1.0, 1.0)

	fill_key = canvas.fill(
		slughorn.Color(1.0, 0.0, 0.0, 1.0),
		1.0,
		slughorn.Key("fill_rect"),
		slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered),
	)

	path = slughorn.canvas.Path()

	path.circle(0.5, 0.5, 0.35)

	assert canvas.define_shape(path, slughorn.Key("circle_shape")) is True

	stroke_key = canvas.stroke(
		path,
		0.1,
		slughorn.Color(0.0, 1.0, 0.0, 1.0),
		1.0,
		slughorn.Key("stroke_circle"),
	)

	canvas.begin_path()
	canvas.rounded_rect(0.1, 0.1, 0.8, 0.8, 0.15)

	gradient = canvas.create_linear_gradient(0.0, 0.0, 1.0, 1.0, _gradient_stops())
	gradient_key = canvas.fill_gradient(gradient, 1.0, slughorn.Key("gradient_rect"))

	canvas.begin_path()
	canvas.move_to(0.2, 0.2)
	canvas.line_to(0.8, 0.8)

	stroke_gradient_key = canvas.stroke_gradient(
		0.05,
		canvas.create_sweep_gradient(0.5, 0.5, 0.0, math.pi, _gradient_stops()),
		1.0,
		slughorn.Key("gradient_stroke"),
	)

	canvas.set_advance(2.0)
	canvas.finalize(slughorn.Key("canvas_composite"))

	assert fill_key == slughorn.Key("fill_rect")
	assert stroke_key == slughorn.Key("stroke_circle")
	assert gradient_key == slughorn.Key("gradient_rect")
	assert stroke_gradient_key == slughorn.Key("gradient_stroke")

	atlas.build()

	assert atlas.get_shape(slughorn.Key("circle_shape")) is not None
	assert atlas.get_shape(fill_key) is not None
	assert atlas.get_shape(stroke_key) is not None
	assert atlas.get_shape(gradient_key) is not None
	assert atlas.get_shape(stroke_gradient_key) is not None

	composite = atlas.get_composite_shape(slughorn.Key("canvas_composite"))

	assert composite is not None
	assert len(composite) == 4
	assert composite.advance == pytest.approx(2.0)

	gradients = atlas.get_gradients()

	assert len(gradients) == 2
	assert atlas.gradient_texture.width > 0

# =============================================================================
# NanoSVG backend
# =============================================================================

_SVG_DIR = Path(__file__).resolve().parent

# Minimal inline SVGs used for controlled unit tests.
_SVG_ONE_SOLID = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <rect x="10" y="10" width="80" height="80" fill="#ff0000"/>
</svg>
"""

_SVG_TWO_SOLIDS = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
  <rect x="0"   y="0" width="100" height="100" fill="#00ff00"/>
  <rect x="100" y="0" width="100" height="100" fill="#0000ff"/>
</svg>
"""

def _nanosvg_available():
	return hasattr(slughorn, "nanosvg")

def test_nanosvg_load_string_single_solid():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()
	composite, next_key = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)

	assert next_key == 1, "one shape -> next_key must be 1"
	assert len(composite) == 1, "one rect -> one layer"

	layer = composite.layers[0]

	# colorFromNSVG unpacks 0xAABBGGRR; #ff0000 => R=1, G=0, B=0, A=1
	assert layer.color.r == pytest.approx(1.0, abs=1e-3)
	assert layer.color.g == pytest.approx(0.0, abs=1e-3)
	assert layer.color.b == pytest.approx(0.0, abs=1e-3)
	assert layer.color.a == pytest.approx(1.0, abs=1e-3)

	atlas.build()

	assert atlas.get_shape(layer.key) is not None

def test_nanosvg_load_string_two_solids():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()
	composite, next_key = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)

	assert next_key == 2, "two shapes -> next_key must be 2"
	assert len(composite) == 2

def test_nanosvg_load_file_gradient():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	svg_path = _SVG_DIR / "gradient-test.svg"

	atlas = slughorn.Atlas()
	composite, next_key = slughorn.nanosvg.load_file(str(svg_path), atlas)

	# gradient-test.svg has 3 rects: 2 linear + 1 radial gradient
	assert next_key == 3, "gradient-test.svg has 3 shapes"
	assert len(composite) == 3

	atlas.build()

	for layer in composite.layers:
		assert atlas.get_shape(layer.key) is not None

def test_nanosvg_load_file_inkscape():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	svg_path = _SVG_DIR / "inkscape-test.svg"

	atlas = slughorn.Atlas()
	composite, next_key = slughorn.nanosvg.load_file(str(svg_path), atlas)

	# inkscape-test.svg has 1 ellipse with a linear gradient
	assert next_key == 1, "inkscape-test.svg has 1 shape"
	assert len(composite) == 1

def test_nanosvg_key_chaining():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()

	composite_a, k = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas, base_key=0)
	composite_b, k = slughorn.nanosvg.load_string(_SVG_ONE_SOLID,   atlas, base_key=k)

	assert k == 3, "2 + 1 shapes = next_key 3"
	assert len(composite_a) == 2
	assert len(composite_b) == 1

	# Keys must not overlap
	keys_a = {layer.key for layer in composite_a.layers}
	keys_b = {layer.key for layer in composite_b.layers}

	assert keys_a.isdisjoint(keys_b), "chained loads must not share keys"

	atlas.build()

	for layer in list(composite_a.layers) + list(composite_b.layers):
		assert atlas.get_shape(layer.key) is not None

def test_nanosvg_renderable():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()
	composite, _ = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)

	atlas.build()

	layer = composite.layers[0]
	sampler = atlas.decode(layer.key)

	grid = sampler.render_grid(32, 0.0, True)

	assert tuple(grid.shape) == (32, 32)
	assert float(grid.max()) > 0.5, "solid filled rect must have high coverage at centre"
	assert float(grid.min()) >= 0.0

def test_nanosvg_bad_path_returns_empty():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()
	composite, next_key = slughorn.nanosvg.load_file("/nonexistent/path.svg", atlas)

	# NanoSVG returns an empty CompositeShape on parse failure; next_key unchanged
	assert next_key == 0
	assert len(composite) == 0
