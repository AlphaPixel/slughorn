#!/usr/bin/env python3
#vimrun! pytest ../test/slughorn-test-python.py

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

	# Make sure keys are "implictly convertible"...
	# shape_key = slughorn.Key("rect")
	shape_key = 0x1234
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
			slughorn.Transform(),
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
	assert shape.compute_quad(slughorn.Transform()).x1 > shape.compute_quad(slughorn.Transform()).x0
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

	fill_layer = canvas.fill(
		slughorn.Color(1.0, 0.0, 0.0, 1.0),
		1.0,
		# Again, make sure that the slughorn.Key ctor is "imlicitly" called...
		# slughorn.Key("fill_rect"),
		"fill_rect",
		slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered),
	)

	path = slughorn.canvas.Path()

	path.circle(0.5, 0.5, 0.35)

	assert canvas.define_shape(path, slughorn.Key("circle_shape")) is True

	stroke_layer = canvas.stroke(
		path,
		0.1,
		slughorn.Color(0.0, 1.0, 0.0, 1.0),
		1.0,
		slughorn.Key("stroke_circle"),
	)

	canvas.begin_path()
	canvas.rounded_rect(0.1, 0.1, 0.8, 0.8, 0.15)

	gradient = canvas.create_linear_gradient(0.0, 0.0, 1.0, 1.0, _gradient_stops())
	gradient_layer = canvas.fill_gradient(gradient, 1.0, slughorn.Key("gradient_rect"))

	canvas.begin_path()
	canvas.move_to(0.2, 0.2)
	canvas.line_to(0.8, 0.8)

	stroke_gradient_layer = canvas.stroke_gradient(
		0.05,
		canvas.create_sweep_gradient(0.5, 0.5, 0.0, math.pi, _gradient_stops()),
		1.0,
		slughorn.Key("gradient_stroke"),
	)

	canvas.set_advance(2.0)
	canvas.finalize(slughorn.Key("canvas_composite"))

	assert fill_layer.key == slughorn.Key("fill_rect")
	assert stroke_layer.key == slughorn.Key("stroke_circle")
	assert gradient_layer.key == slughorn.Key("gradient_rect")
	assert stroke_gradient_layer.key == slughorn.Key("gradient_stroke")

	atlas.build()

	assert atlas.get_shape(slughorn.Key("circle_shape")) is not None
	assert atlas.get_shape(fill_layer.key) is not None
	assert atlas.get_shape(stroke_layer.key) is not None
	assert atlas.get_shape(gradient_layer.key) is not None
	assert atlas.get_shape(stroke_gradient_layer.key) is not None

	composite = atlas.get_composite_shape(slughorn.Key("canvas_composite"))

	assert composite is not None
	assert len(composite) == 4
	assert composite.advance == pytest.approx(2.0)

	gradients = atlas.get_gradients()

	assert len(gradients) == 2
	assert atlas.gradient_texture.width > 0

def test_key_implicit_str_conversion():
	# All canvas methods that accept a Key should also accept a plain Python str.
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator())

	# fill() with str key
	canvas.begin_path()
	canvas.rect(0.1, 0.1, 0.9, 0.9)
	fill_layer = canvas.fill(slughorn.Color(1.0, 0.0, 0.0, 1.0), 1.0, "implicit_fill")

	assert fill_layer.key == slughorn.Key("implicit_fill")

	# stroke() (current-path) with str key
	canvas.begin_path()
	canvas.circle(0.5, 0.5, 0.3)
	stroke_layer = canvas.stroke(0.05, slughorn.Color(0.0, 1.0, 0.0, 1.0), 1.0, "implicit_stroke")

	assert stroke_layer.key == slughorn.Key("implicit_stroke")

	# stroke() (explicit Path) with str key
	path = slughorn.canvas.Path()
	path.circle(0.5, 0.5, 0.2)
	stroke_path_layer = canvas.stroke(path, 0.05, slughorn.Color(0.0, 0.0, 1.0, 1.0), 1.0, "implicit_stroke_path")

	assert stroke_path_layer.key == slughorn.Key("implicit_stroke_path")

	atlas.build()

	assert atlas.get_shape("implicit_fill") is not None
	assert atlas.get_shape("implicit_stroke") is not None
	assert atlas.get_shape("implicit_stroke_path") is not None


def test_canvas_arc_stroke_current_path_with_centered_origin():
	# Exercises the canvas.arc() + canvas.stroke() (current-path, named key, Centered origin)
	# pattern used by zora.py. Verifies: shape exists, origin is non-zero (Centered was applied),
	# and a Layer built from the shape has the correct pivot for vertex-shader rotation.
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator())

	Origin = slughorn.ShapeInfo.Origin
	centered = Origin(Origin.Type.Centered)

	CX, CY, R = 0.5, 0.5, 0.4
	GAP = math.pi / 6

	canvas.begin_path()
	canvas.arc(CX, CY, R, GAP / 2, 2 * math.pi - GAP / 2)

	# Both str and Key forms must work here (implicit conversion test).
	layer = canvas.stroke(0.01, slughorn.Color(0.6, 0.85, 1.0, 1.0), 1.0, "arc_ring", centered)

	assert layer.key == slughorn.Key("arc_ring")

	atlas.build()

	shape = atlas.get_shape("arc_ring")

	assert shape is not None
	assert shape.width > 0.0
	assert shape.height > 0.0

	# Centered origin: origin_x/y should equal half the bbox width/height (≈ R),
	# not zero as the default origin would give.
	assert shape.origin_x == pytest.approx(shape.width / 2, rel=1e-3)
	assert shape.origin_y == pytest.approx(shape.height / 2, rel=1e-3)

	# A Layer built from this shape with transform.x/y = CX/CY should place its
	# pivot at (CX, CY) — verify that computeQuad centers correctly.
	t = slughorn.Transform()
	t.x = CX
	t.y = CY
	quad = shape.compute_quad(t)

	mid_x = (quad.x0 + quad.x1) / 2
	mid_y = (quad.y0 + quad.y1) / 2

	assert mid_x == pytest.approx(CX, abs=0.02)
	assert mid_y == pytest.approx(CY, abs=0.02)


def test_canvas_method_chaining():
    """Path and Canvas builder methods return self, enabling fluent call chains."""

    # -- Path chaining --------------------------------------------------------
    # Build a square by chaining move_to / line_to / close_path.
    p = slughorn.canvas.Path()
    result = p.move_to(0.1, 0.1) \
               .line_to(0.9, 0.1) \
               .line_to(0.9, 0.9) \
               .line_to(0.1, 0.9) \
               .close_path()

    # Returned object wraps the same C++ Path — geometry is visible on both sides.
    assert result.has_pending_path is True
    assert p.has_pending_path is True

    # A path built step-by-step (no chaining) must have the same arc length.
    ref = slughorn.canvas.Path()
    ref.move_to(0.1, 0.1)
    ref.line_to(0.9, 0.1)
    ref.line_to(0.9, 0.9)
    ref.line_to(0.1, 0.9)
    ref.close_path()

    assert result.arc_length() == pytest.approx(ref.arc_length(), rel=1e-6)

    # Additional Path helpers must also be chainable.
    q = slughorn.canvas.Path()
    q.clear().rect(0.0, 0.0, 1.0, 1.0)
    assert q.has_pending_path is True

    q2 = slughorn.canvas.Path()
    q2.clear().circle(0.5, 0.5, 0.4)
    assert q2.has_pending_path is True

    # -- Canvas chaining ------------------------------------------------------
    atlas = slughorn.Atlas()
    canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("ch"))

    # Geometry chain: begin_path → move_to → line_to → stroke (terminal, returns Layer).
    layer = canvas.begin_path() \
                  .move_to(0.1, 0.5) \
                  .line_to(0.9, 0.5) \
                  .stroke(0.05, slughorn.Color(1, 1, 1, 1))

    assert layer.key is not None
    canvas.finalize(slughorn.Key("chain_line_comp"))

    # Shape helper chain: circle → fill (returns Layer).
    fill_layer = canvas.circle(0.5, 0.5, 0.4) \
                       .fill(slughorn.Color(0, 0, 1, 1), 1.0, slughorn.Key("chain_circle"))

    assert fill_layer.key == slughorn.Key("chain_circle")
    canvas.finalize(slughorn.Key("chain_circle_comp"))

    # Transform chain: save → translate → rotate (all return Canvas&), then commit.
    canvas.save() \
          .translate(0.5, 0.5) \
          .rotate(math.pi / 4)

    rot_layer = canvas.rect(-0.3, -0.3, 0.6, 0.6) \
                      .fill(slughorn.Color(1, 0.75, 0, 1), 1.0, slughorn.Key("chain_rotated"))

    canvas.restore()
    canvas.finalize(slughorn.Key("chain_rot_comp"))

    assert rot_layer.key == slughorn.Key("chain_rotated")

    atlas.build()

    assert atlas.get_shape(slughorn.Key("chain_circle")) is not None
    assert atlas.get_shape(slughorn.Key("chain_rotated")) is not None


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
	keys = slughorn.KeyIterator()
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas, keys)

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
	composite = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)

	assert len(composite) == 2

def test_nanosvg_load_file_gradient():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	svg_path = _SVG_DIR / "gradient-test.svg"

	atlas = slughorn.Atlas()
	composite = slughorn.nanosvg.load_file(str(svg_path), atlas)

	# gradient-test.svg has 3 rects: 2 linear + 1 radial gradient
	assert len(composite) == 3

	atlas.build()

	for layer in composite.layers:
		assert atlas.get_shape(layer.key) is not None

def test_nanosvg_load_file_inkscape():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	svg_path = _SVG_DIR / "inkscape-test.svg"

	atlas = slughorn.Atlas()
	composite = slughorn.nanosvg.load_file(str(svg_path), atlas)

	# inkscape-test.svg has 1 ellipse with a linear gradient
	assert len(composite) == 1

def test_nanosvg_key_chaining():
	if not _nanosvg_available():
		pytest.skip("slughorn.nanosvg not available (build without SLUGHORN_NANOSVG)")

	atlas = slughorn.Atlas()
	keys = slughorn.KeyIterator()

	composite_a = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas, keys)
	composite_b = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas, keys)

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
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)

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
	composite = slughorn.nanosvg.load_file("/nonexistent/path.svg", atlas)

	assert len(composite) == 0

# ============================================================================
# normalize_shape_metrics
# ============================================================================

def _freetype_available():
	return hasattr(slughorn, "freetype")

_MONO_FONT   = "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
_PROP_FONT   = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
_DIGIT_CPS   = [ord(c) for c in "0123456789"]

def test_normalize_canvas_shapes():
	"""Two canvas shapes with different sizes → normalize → identical declared metrics post-build."""
	atlas  = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	# Small square: curves span ~[0, 0.3] x [0, 0.3]
	canvas.begin_path()
	canvas.rect(0, 0, 0.3, 0.3)
	l_small = canvas.fill(slughorn.Color(1, 0, 0), 1.0)
	canvas.finalize()

	# Large square: curves span ~[0, 0.7] x [0, 0.7]
	canvas.begin_path()
	canvas.rect(0, 0, 0.7, 0.7)
	l_large = canvas.fill(slughorn.Color(0, 1, 0), 1.0)
	canvas.finalize()

	# Shapes have different widths before normalization.
	# After normalization they must share identical declared metrics.
	atlas.normalize_shape_metrics([l_small.key, l_large.key])
	atlas.build()

	s_small = atlas.get_shape(l_small.key)
	s_large = atlas.get_shape(l_large.key)

	assert s_small is not None and s_large is not None

	assert s_small.width   == pytest.approx(s_large.width,   abs=1e-5)
	assert s_small.height  == pytest.approx(s_large.height,  abs=1e-5)
	assert s_small.advance == pytest.approx(s_large.advance, abs=1e-5)

	# Band transforms must differ — each shape still has its own coverage data.
	assert s_small.band_scale_x != pytest.approx(s_large.band_scale_x, abs=1e-5)

	# Both shapes must still render (non-zero coverage).
	for key in (l_small.key, l_large.key):
		grid = atlas.decode(key).render_grid(32, 0.0, True)
		assert float(grid.max()) > 0.1, f"shape {key} should have non-zero coverage"

def test_normalize_freetype_tabular():
	"""Monospaced font digits: normalize produces a shared cell; band transforms differ."""
	if not _freetype_available():
		pytest.skip("slughorn.freetype not available")

	from pathlib import Path
	if not Path(_MONO_FONT).exists():
		pytest.skip(f"font not found: {_MONO_FONT}")

	atlas = slughorn.Atlas()
	slughorn.freetype.load_font_glyphs(_MONO_FONT, _DIGIT_CPS, atlas)  # no uniform=True

	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in _DIGIT_CPS])
	atlas.build()

	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in _DIGIT_CPS]
	assert all(s is not None for s in shapes)

	ref = shapes[0]
	for s in shapes[1:]:
		assert s.width   == pytest.approx(ref.width,   abs=1e-5)
		assert s.height  == pytest.approx(ref.height,  abs=1e-5)
		assert s.advance == pytest.approx(ref.advance, abs=1e-5)

def test_normalize_freetype_nontabular():
	"""Proportional font digits: non-tabular path; all shapes get the union cell."""
	if not _freetype_available():
		pytest.skip("slughorn.freetype not available")

	from pathlib import Path
	if not Path(_PROP_FONT).exists():
		pytest.skip(f"font not found: {_PROP_FONT}")

	atlas = slughorn.Atlas()
	slughorn.freetype.load_font_glyphs(_PROP_FONT, _DIGIT_CPS, atlas)

	atlas.normalize_shape_metrics([slughorn.Key(cp) for cp in _DIGIT_CPS])
	atlas.build()

	shapes = [atlas.get_shape(slughorn.Key(cp)) for cp in _DIGIT_CPS]
	assert all(s is not None for s in shapes)

	ref = shapes[0]
	for s in shapes[1:]:
		assert s.width   == pytest.approx(ref.width,   abs=1e-5)
		assert s.height  == pytest.approx(ref.height,  abs=1e-5)
		assert s.advance == pytest.approx(ref.advance, abs=1e-5)

# ============================================================================
# Multi-subpath accumulation (BUG-1 regression)
# ============================================================================

def _build_three_rect_atlas(use_rect_helper: bool):
	"""
	Build an atlas with a single shape made of three side-by-side rects.
	x: 0.1→0.3, 0.4→0.6, 0.7→0.9  |  y: 0.25→0.75
	Union bounding box: width=0.8, height=0.5

	use_rect_helper=True  → canvas.rect() calls (BUG-1 regression path)
	use_rect_helper=False → raw moveTo/lineTo/closePath (reference path)
	"""
	atlas  = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	def add_rect(x, y, w, h):
		canvas.move_to(x,     y)
		canvas.line_to(x + w, y)
		canvas.line_to(x + w, y + h)
		canvas.line_to(x,     y + h)
		canvas.close_path()

	canvas.begin_path()

	if use_rect_helper:
		canvas.rect(0.1, 0.25, 0.2, 0.5)
		canvas.rect(0.4, 0.25, 0.2, 0.5)
		canvas.rect(0.7, 0.25, 0.2, 0.5)
	else:
		add_rect(0.1, 0.25, 0.2, 0.5)
		add_rect(0.4, 0.25, 0.2, 0.5)
		add_rect(0.7, 0.25, 0.2, 0.5)

	layer = canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, "three_rects")
	comp  = canvas.finalize()

	atlas.add_composite_shape(slughorn.Key("three_rects_comp"), comp)
	atlas.build()

	return atlas, layer.key, comp


def test_multi_subpath_raw_commands():
	"""Three sub-paths via raw moveTo/lineTo/closePath produce one shape spanning all three rects."""
	atlas, key, comp = _build_three_rect_atlas(use_rect_helper=False)

	# One fill() call → one layer in the composite.
	assert len(comp) == 1

	shape = atlas.get_shape(key)
	assert shape is not None

	# Union bbox: x spans 0.1→0.9 (width 0.8), y spans 0.25→0.75 (height 0.5).
	assert shape.width  == pytest.approx(0.8, abs=1e-4)
	assert shape.height == pytest.approx(0.5, abs=1e-4)

	# Render and confirm non-zero coverage (shape is actually filled, not empty).
	grid = atlas.decode(key).render_grid(32, 0.0, True)
	assert float(grid.max()) > 0.5, "compound shape must have high coverage inside rects"


def test_multi_subpath_rect_helper():
	"""Three canvas.rect() calls after begin_path() must accumulate — BUG-1 regression."""
	atlas, key, comp = _build_three_rect_atlas(use_rect_helper=True)

	# Same assertions as the raw-command version — results must be identical.
	assert len(comp) == 1

	shape = atlas.get_shape(key)
	assert shape is not None

	assert shape.width  == pytest.approx(0.8, abs=1e-4)
	assert shape.height == pytest.approx(0.5, abs=1e-4)

	grid = atlas.decode(key).render_grid(32, 0.0, True)
	assert float(grid.max()) > 0.5, "rect() helpers must accumulate sub-paths, not wipe them"


def test_add_path_with_transform():
	"""addPath(path, transform) stamps a path at three positions — BUG-2 regression."""
	atlas  = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	# A single 0.2 x 0.5 rect authored at the origin.
	rect = slughorn.canvas.Path()
	rect.move_to(0.0,  0.0)
	rect.line_to(0.2,  0.0)
	rect.line_to(0.2,  0.5)
	rect.line_to(0.0,  0.5)
	rect.close_path()

	# Stamp it three times via translate transforms — same geometry as the multi-subpath tests.
	canvas.begin_path()
	canvas.add_path(rect, slughorn.Matrix.translate(0.1, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.4, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.7, 0.25))

	layer = canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, "add_path_xform")
	comp  = canvas.finalize()

	atlas.add_composite_shape(slughorn.Key("add_path_xform_comp"), comp)
	atlas.build()

	assert len(comp) == 1

	shape = atlas.get_shape(layer.key)
	assert shape is not None

	# Union bbox must match the three-rect tests: width=0.8, height=0.5.
	assert shape.width  == pytest.approx(0.8, abs=1e-4)
	assert shape.height == pytest.approx(0.5, abs=1e-4)

	grid = atlas.decode(layer.key).render_grid(32, 0.0, True)
	assert float(grid.max()) > 0.5, "addPath(path, transform) must place geometry at correct positions"
