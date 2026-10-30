"""
Tests for slughorn/canvas.hpp — canvas.Path and canvas.Canvas.
"""

import math
import pytest
import slughorn


# ---------------------------------------------------------------------------
# canvas.Path (standalone, no Atlas)
# ---------------------------------------------------------------------------

def test_path_decomposer_access():
	p = slughorn.canvas.Path()
	d = p.decomposer()
	d.tolerance = 1e-4
	assert p.decomposer().tolerance == pytest.approx(1e-4)

def test_path_move_line_close():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	p.line_to(1.0, 1.0)
	p.close_path()
	assert p.has_pending_path is True

def test_path_quad_to():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.quad_to(0.5, 1.0, 1.0, 0.0)
	assert p.has_pending_path is True
	assert p.arc_length() > 0.0

def test_path_bezier_to():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.bezier_to(0.25, 1.0, 0.75, 1.0, 1.0, 0.0)
	assert p.has_pending_path is True

def test_path_arc_to():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.5)
	p.arc_to(0.5, 1.0, 1.0, 0.5, 0.2)
	assert p.arc_length() > 0.0

def test_path_arc():
	p = slughorn.canvas.Path()
	p.arc(0.5, 0.5, 0.4, 0.0, math.pi)
	assert p.has_pending_path is True
	assert p.arc_length() > 0.0

def test_path_circle():
	p = slughorn.canvas.Path()
	p.circle(0.5, 0.5, 0.4)
	assert p.has_pending_path is True
	assert p.arc_length() > 0.0

def test_path_ellipse():
	p = slughorn.canvas.Path()
	p.ellipse(0.5, 0.5, 0.4, 0.2)
	assert p.has_pending_path is True

def test_path_rect():
	p = slughorn.canvas.Path()
	p.rect(0.0, 0.0, 1.0, 1.0)
	assert p.has_pending_path is True

def test_path_rounded_rect():
	p = slughorn.canvas.Path()
	p.rounded_rect(0.1, 0.1, 0.8, 0.8, 0.1)
	assert p.has_pending_path is True

def test_path_add_path():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	q = slughorn.canvas.Path()
	q.add_path(p)
	assert q.arc_length() == pytest.approx(p.arc_length(), rel=1e-5)

def test_path_add_path_transform():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(0.2, 0.0)
	q = slughorn.canvas.Path()
	q.add_path(p, slughorn.Matrix.translate(0.5, 0.0))
	q.add_path(p, slughorn.Matrix.translate(0.8, 0.0))
	# arc_length includes the implicit gap between the two stamped segments, so it
	# is >= 2 * p.arc_length(), not exactly equal.
	assert q.arc_length() >= 2 * p.arc_length() - 1e-4

def test_path_sample_fields():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	s = p.sample(0.5)
	assert math.isfinite(s.x)
	assert math.isfinite(s.y)
	assert math.isfinite(s.angle)

def test_path_sample_midpoint():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	s = p.sample(0.5)
	assert s.x == pytest.approx(0.5, abs=0.01)
	assert s.y == pytest.approx(0.0, abs=0.01)

def test_path_sample_repr():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	assert "Path.Sample" in repr(p.sample(0.5))

def test_path_arc_length_nonzero():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	assert p.arc_length() == pytest.approx(1.0, rel=1e-5)

def test_path_clear():
	p = slughorn.canvas.Path()
	p.rect(0.0, 0.0, 1.0, 1.0)
	p.clear()
	assert p.has_pending_path is False

def test_path_from_curves(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	# Shape.curves returns a (N, 6) float32 memoryview; Path accepts it directly.
	p = slughorn.canvas.Path(s.curves)
	assert p.arc_length() > 0.0

def test_path_stroke_path():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.5)
	p.line_to(1.0, 0.5)
	result = p.stroke_path(0.1)
	# Returns False if path was empty; must return True for a non-empty path.
	assert result is True
	assert p.has_pending_path is True

def test_path_transform_stack():
	p = slughorn.canvas.Path()
	p.save()
	p.translate(0.5, 0.5)
	p.move_to(0.0, 0.0)
	p.line_to(0.1, 0.0)
	p.restore()
	assert p.arc_length() > 0.0

def test_path_reset_transform():
	p = slughorn.canvas.Path()
	p.translate(1.0, 1.0)
	p.reset_transform()
	p.move_to(0.0, 0.0)
	p.line_to(0.1, 0.0)
	s = p.sample(0.5)
	assert s.x == pytest.approx(0.05, abs=0.01)

def test_path_repr():
	assert repr(slughorn.canvas.Path()) != ""


# ---------------------------------------------------------------------------
# canvas.Path chaining
# ---------------------------------------------------------------------------

def test_path_chain_returns_self():
	p = slughorn.canvas.Path()
	r = p.move_to(0.1, 0.1).line_to(0.9, 0.1).line_to(0.9, 0.9).close_path()
	assert r is p

def test_path_chain_geometry_matches_stepwise():
	chained = slughorn.canvas.Path()
	chained.move_to(0.1, 0.1).line_to(0.9, 0.1).line_to(0.9, 0.9).line_to(0.1, 0.9).close_path()

	stepwise = slughorn.canvas.Path()
	stepwise.move_to(0.1, 0.1)
	stepwise.line_to(0.9, 0.1)
	stepwise.line_to(0.9, 0.9)
	stepwise.line_to(0.1, 0.9)
	stepwise.close_path()

	assert chained.arc_length() == pytest.approx(stepwise.arc_length(), rel=1e-6)

def test_path_chain_helpers():
	p = slughorn.canvas.Path()
	p.clear().rect(0.0, 0.0, 1.0, 1.0)
	assert p.has_pending_path is True

	q = slughorn.canvas.Path()
	q.clear().circle(0.5, 0.5, 0.4)
	assert q.has_pending_path is True


# ---------------------------------------------------------------------------
# canvas.Canvas construction
# ---------------------------------------------------------------------------

def test_canvas_default_key_iterator(atlas):
	# Canvas(atlas) without a KeyIterator must not crash.
	c = slughorn.canvas.Canvas(atlas)
	c.begin_path()
	c.rect(0, 0, 1, 1)
	layer = c.fill(slughorn.Color(1, 0, 0))
	assert layer.key is not None

def test_canvas_decomposer_access(canvas):
	d = canvas.decomposer()
	d.tolerance = 5e-4
	assert canvas.decomposer().tolerance == pytest.approx(5e-4)

def test_canvas_repr(canvas):
	assert repr(canvas) != ""


# ---------------------------------------------------------------------------
# canvas.Canvas CTM
# ---------------------------------------------------------------------------

def test_canvas_save_restore(canvas, atlas):
	canvas.save()
	canvas.translate(0.5, 0.5)
	canvas.rect(0, 0, 0.2, 0.2)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("ctm_rect"))
	canvas.restore()
	atlas.build()
	assert atlas.get_shape(slughorn.Key("ctm_rect")) is not None

def test_canvas_translate(canvas, atlas):
	canvas.translate(0.5, 0.0)
	canvas.rect(0, 0, 0.2, 0.2)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("trans_rect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("trans_rect")) is not None

def test_canvas_rotate(canvas, atlas):
	canvas.rotate(math.pi / 4)
	canvas.rect(-0.1, -0.1, 0.2, 0.2)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("rot_rect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("rot_rect")) is not None

def test_canvas_scale(canvas, atlas):
	canvas.scale(2.0, 2.0)
	canvas.rect(0, 0, 0.2, 0.2)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("scale_rect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("scale_rect")) is not None

def test_canvas_ctm_chain(canvas):
	canvas.save().translate(0.5, 0.5).rotate(math.pi / 4)
	canvas.restore()
	# Just must not crash.

def test_canvas_reset_transform(canvas, atlas):
	canvas.translate(5.0, 5.0)
	canvas.reset_transform()
	canvas.rect(0, 0, 0.3, 0.3)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("reset_rect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("reset_rect")) is not None

def test_canvas_set_transform(canvas, atlas):
	canvas.set_transform(slughorn.Matrix.translate(0.1, 0.1))
	canvas.rect(0, 0, 0.3, 0.3)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("settrans_rect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("settrans_rect")) is not None


# ---------------------------------------------------------------------------
# canvas.Canvas geometry helpers
# ---------------------------------------------------------------------------

def test_canvas_begin_path(canvas):
	canvas.rect(0, 0, 1, 1)
	canvas.begin_path()
	assert canvas.has_pending_path is False

def test_canvas_has_pending_path(canvas):
	assert canvas.has_pending_path is False
	canvas.rect(0, 0, 1, 1)
	assert canvas.has_pending_path is True

def test_canvas_move_line(canvas, atlas):
	canvas.begin_path()
	canvas.move_to(0.0, 0.0)
	canvas.line_to(1.0, 0.0)
	canvas.line_to(1.0, 1.0)
	canvas.close_path()
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("move_line"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("move_line")) is not None

def test_canvas_quad_to(canvas, atlas):
	canvas.begin_path()
	canvas.move_to(0.0, 0.0)
	canvas.quad_to(0.5, 1.0, 1.0, 0.0)
	layer = canvas.stroke(0.05, slughorn.Color(1, 0, 0), 1.0, slughorn.Key("quad_stroke"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("quad_stroke")) is not None

def test_canvas_bezier_to(canvas, atlas):
	canvas.begin_path()
	canvas.move_to(0.0, 0.0)
	canvas.bezier_to(0.25, 1.0, 0.75, 1.0, 1.0, 0.0)
	layer = canvas.stroke(0.05, slughorn.Color(1, 0, 0), 1.0, slughorn.Key("bezier_stroke"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("bezier_stroke")) is not None

def test_canvas_rect_helper(canvas, atlas):
	canvas.rect(0.0, 0.0, 1.0, 1.0)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("rect_helper"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("rect_helper")) is not None

def test_canvas_circle_helper(canvas, atlas):
	canvas.circle(0.5, 0.5, 0.4)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("circle_helper"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("circle_helper")) is not None

def test_canvas_arc_helper(canvas, atlas):
	canvas.arc(0.5, 0.5, 0.4, 0.0, math.pi * 1.5)
	layer = canvas.stroke(0.05, slughorn.Color(1, 0, 0), 1.0, slughorn.Key("arc_helper"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("arc_helper")) is not None

def test_canvas_arc_to(canvas, atlas):
	canvas.begin_path()
	canvas.move_to(0.0, 0.5)
	canvas.arc_to(0.5, 1.0, 1.0, 0.5, 0.3)
	layer = canvas.stroke(0.05, slughorn.Color(1, 0, 0), 1.0, slughorn.Key("arc_to"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("arc_to")) is not None

def test_canvas_rounded_rect_helper(canvas, atlas):
	canvas.rounded_rect(0.1, 0.1, 0.8, 0.8, 0.1)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("rrect"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("rrect")) is not None

def test_canvas_ellipse_helper(canvas, atlas):
	canvas.ellipse(0.5, 0.5, 0.4, 0.2)
	layer = canvas.fill(slughorn.Color(0, 1, 0), 1.0, slughorn.Key("ellipse"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("ellipse")) is not None

def test_canvas_path_accessor(canvas):
	canvas.rect(0, 0, 1, 1)
	p = canvas.path()
	assert isinstance(p, slughorn.canvas.Path)
	assert p.arc_length() > 0.0

def test_canvas_layer_count(canvas):
	assert canvas.layer_count == 0
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0))
	assert canvas.layer_count == 1


# ---------------------------------------------------------------------------
# canvas.Canvas commit verbs
# ---------------------------------------------------------------------------

def test_canvas_fill_returns_layer(canvas):
	canvas.rect(0, 0, 1, 1)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("fill_key"))
	assert isinstance(layer, slughorn.Layer)
	assert layer.key == slughorn.Key("fill_key")

def test_canvas_fill_auto_key(canvas):
	canvas.rect(0, 0, 1, 1)
	layer = canvas.fill(slughorn.Color(1, 0, 0))
	assert layer.key is not None

def test_canvas_fill_shape_in_atlas(canvas, atlas):
	canvas.rect(0, 0, 1, 1)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("fill_atlas"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("fill_atlas")) is not None

def test_canvas_fill_with_centered_origin(canvas, atlas):
	Origin = slughorn.ShapeInfo.Origin
	canvas.rect(0, 0, 1, 1)
	layer = canvas.fill(
		slughorn.Color(1, 0, 0), 1.0,
		slughorn.Key("fill_centered"),
		Origin(Origin.Type.Centered),
	)
	atlas.build()
	s = atlas.get_shape(slughorn.Key("fill_centered"))
	assert s.origin_x == pytest.approx(s.width / 2, rel=1e-3)
	assert s.origin_y == pytest.approx(s.height / 2, rel=1e-3)

def test_canvas_fill_explicit_path(canvas, atlas):
	p = slughorn.canvas.Path()
	p.rect(0, 0, 1, 1)
	layer = canvas.fill(p, slughorn.Color(1, 0, 0), 1.0, slughorn.Key("fill_path"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("fill_path")) is not None

def test_canvas_stroke_current_path(canvas, atlas):
	canvas.begin_path()
	canvas.circle(0.5, 0.5, 0.4)
	layer = canvas.stroke(0.05, slughorn.Color(0, 1, 0), 1.0, slughorn.Key("stroke_cur"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("stroke_cur")) is not None

def test_canvas_stroke_auto_key(canvas):
	canvas.circle(0.5, 0.5, 0.3)
	layer = canvas.stroke(0.05, slughorn.Color(0, 1, 0))
	assert layer.key is not None

def test_canvas_stroke_explicit_path(canvas, atlas):
	p = slughorn.canvas.Path()
	p.circle(0.5, 0.5, 0.4)
	layer = canvas.stroke(p, 0.05, slughorn.Color(0, 1, 0), 1.0, slughorn.Key("stroke_path"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("stroke_path")) is not None

def test_canvas_define_shape_from_path(canvas, atlas):
	p = slughorn.canvas.Path()
	p.circle(0.5, 0.5, 0.4)
	result = canvas.define_shape(p, slughorn.Key("defined_shape"))
	assert result is True
	atlas.build()
	assert atlas.get_shape(slughorn.Key("defined_shape")) is not None

def test_canvas_define_shape_from_current_path(canvas, atlas):
	canvas.circle(0.5, 0.5, 0.4)
	result = canvas.define_shape(slughorn.Key("defined_cur"), 1.0)
	assert result is True
	atlas.build()
	assert atlas.get_shape(slughorn.Key("defined_cur")) is not None

def test_canvas_stroke_path_in_place(canvas, atlas):
	canvas.begin_path()
	canvas.move_to(0.0, 0.5)
	canvas.line_to(1.0, 0.5)
	result = canvas.stroke_path(0.1)
	assert result is True
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, slughorn.Key("stroke_path_fill"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("stroke_path_fill")) is not None

def test_canvas_auto_metrics_property(canvas):
	assert canvas.auto_metrics is True
	canvas.auto_metrics = False
	assert canvas.auto_metrics is False


# ---------------------------------------------------------------------------
# canvas.Canvas gradient commits
# ---------------------------------------------------------------------------

def test_canvas_fill_gradient(canvas, atlas, gradient_stops):
	canvas.rect(0, 0, 1, 1)
	gh = canvas.create_linear_gradient(0.0, 0.0, 1.0, 0.0, gradient_stops)
	layer = canvas.fill_gradient(gh, 1.0, slughorn.Key("fill_grad"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("fill_grad")) is not None

def test_canvas_stroke_gradient(canvas, atlas, gradient_stops):
	canvas.begin_path()
	canvas.move_to(0.2, 0.2)
	canvas.line_to(0.8, 0.8)
	gh = canvas.create_sweep_gradient(0.5, 0.5, 0.0, math.pi, gradient_stops)
	layer = canvas.stroke_gradient(0.05, gh, 1.0, slughorn.Key("stroke_grad"))
	atlas.build()
	assert atlas.get_shape(slughorn.Key("stroke_grad")) is not None

def test_canvas_create_radial_gradient(canvas, gradient_stops):
	gh = canvas.create_radial_gradient(0.5, 0.5, 0.1, 0.5, gradient_stops)
	assert gh is not None

def test_canvas_gradient_registered_in_atlas(canvas, atlas, gradient_stops):
	canvas.rect(0, 0, 1, 1)
	gh = canvas.create_linear_gradient(0.0, 0.0, 1.0, 0.0, gradient_stops)
	canvas.fill_gradient(gh, 1.0, slughorn.Key("grad_atlas"))
	atlas.build()
	assert len(atlas.get_gradients()) == 1

def test_canvas_fill_gradient_explicit_path(canvas, atlas, gradient_stops):
	p = slughorn.canvas.Path()
	p.rect(0, 0, 1, 1)
	gh = canvas.create_linear_gradient(0.0, 0.0, 1.0, 0.0, gradient_stops)
	layer = canvas.fill_gradient(p, gh, 1.0)
	atlas.build()
	assert layer.key is not None


# ---------------------------------------------------------------------------
# canvas.Canvas composite / finalize
# ---------------------------------------------------------------------------

def test_canvas_finalize_returns_composite(canvas):
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0))
	comp = canvas.finalize()
	assert isinstance(comp, slughorn.CompositeShape)
	assert len(comp) == 1

def test_canvas_finalize_resets_state(canvas):
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0))
	canvas.finalize()
	assert canvas.layer_count == 0

def test_canvas_begin_composite_resets_layers(canvas):
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0))
	assert canvas.layer_count == 1
	canvas.begin_composite()
	assert canvas.layer_count == 0

def test_canvas_set_advance(canvas):
	canvas.set_advance(2.5)
	comp = canvas.finalize()
	assert comp.advance == pytest.approx(2.5)

def test_canvas_finalize_with_key(canvas, atlas):
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0))
	canvas.finalize(slughorn.Key("my_comp"))
	assert atlas.get_composite_shape(slughorn.Key("my_comp")) is not None


# ---------------------------------------------------------------------------
# canvas.Canvas implicit Key conversion
# ---------------------------------------------------------------------------

def test_canvas_fill_str_key(canvas, atlas):
	canvas.rect(0, 0, 1, 1)
	layer = canvas.fill(slughorn.Color(1, 0, 0), 1.0, "str_fill")
	assert layer.key == slughorn.Key("str_fill")
	atlas.build()
	assert atlas.get_shape("str_fill") is not None

def test_canvas_stroke_str_key(canvas, atlas):
	canvas.circle(0.5, 0.5, 0.3)
	layer = canvas.stroke(0.05, slughorn.Color(0, 1, 0), 1.0, "str_stroke")
	assert layer.key == slughorn.Key("str_stroke")

def test_canvas_stroke_path_str_key(canvas, atlas):
	p = slughorn.canvas.Path()
	p.circle(0.5, 0.5, 0.2)
	layer = canvas.stroke(p, 0.05, slughorn.Color(0, 0, 1), 1.0, "str_stroke_path")
	assert layer.key == slughorn.Key("str_stroke_path")

def test_canvas_get_shape_str(canvas, atlas):
	canvas.rect(0, 0, 1, 1)
	canvas.fill(slughorn.Color(1, 0, 0), 1.0, "lookup_str")
	atlas.build()
	assert atlas.get_shape("lookup_str") is not None


# ---------------------------------------------------------------------------
# canvas.Canvas method chaining
# ---------------------------------------------------------------------------

def test_canvas_chain_begin_move_line_stroke(canvas):
	layer = (canvas.begin_path()
		.move_to(0.1, 0.5)
		.line_to(0.9, 0.5)
		.stroke(0.05, slughorn.Color(1, 1, 1)))
	assert layer.key is not None

def test_canvas_chain_circle_fill(canvas):
	fill_layer = canvas.circle(0.5, 0.5, 0.4).fill(slughorn.Color(0, 0, 1), 1.0, slughorn.Key("chain_circle"))
	assert fill_layer.key == slughorn.Key("chain_circle")

def test_canvas_chain_ctm(canvas):
	canvas.save().translate(0.5, 0.5).rotate(math.pi / 4)
	canvas.rect(-0.1, -0.1, 0.2, 0.2)
	layer = canvas.fill(slughorn.Color(1, 0.75, 0), 1.0, slughorn.Key("chain_rotated"))
	canvas.restore()
	assert layer.key == slughorn.Key("chain_rotated")


# ---------------------------------------------------------------------------
# Multi-subpath accumulation (regression)
# ---------------------------------------------------------------------------

def _three_rects_raw(atlas):
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.begin_path()
	for x in (0.1, 0.4, 0.7):
		canvas.move_to(x,       0.25)
		canvas.line_to(x + 0.2, 0.25)
		canvas.line_to(x + 0.2, 0.75)
		canvas.line_to(x,       0.75)
		canvas.close_path()
	return canvas.fill(slughorn.Color(1, 1, 1), 1.0, "three_rects_raw")

def _three_rects_helper(atlas):
	canvas = slughorn.canvas.Canvas(atlas)
	canvas.begin_path()
	canvas.rect(0.1, 0.25, 0.2, 0.5)
	canvas.rect(0.4, 0.25, 0.2, 0.5)
	canvas.rect(0.7, 0.25, 0.2, 0.5)
	return canvas.fill(slughorn.Color(1, 1, 1), 1.0, "three_rects_helper")

def test_multisubpath_raw_commands(atlas):
	layer = _three_rects_raw(atlas)
	atlas.build()
	s = atlas.get_shape(layer.key)
	assert s is not None
	assert s.width  == pytest.approx(0.8, abs=1e-4)
	assert s.height == pytest.approx(0.5, abs=1e-4)

def test_multisubpath_rect_helper(atlas):
	layer = _three_rects_helper(atlas)
	atlas.build()
	s = atlas.get_shape(layer.key)
	assert s is not None
	assert s.width  == pytest.approx(0.8, abs=1e-4)
	assert s.height == pytest.approx(0.5, abs=1e-4)

def test_multisubpath_coverage_raw(atlas):
	layer = _three_rects_raw(atlas)
	atlas.build()
	grid = atlas.decode(layer.key).render_grid(32, 0.0, True)
	assert max(memoryview(grid).cast('B').cast('f')) > 0.5

def test_multisubpath_coverage_helper(atlas):
	layer = _three_rects_helper(atlas)
	atlas.build()
	grid = atlas.decode(layer.key).render_grid(32, 0.0, True)
	assert max(memoryview(grid).cast('B').cast('f')) > 0.5

def test_add_path_transform(atlas):
	canvas = slughorn.canvas.Canvas(atlas)
	rect = slughorn.canvas.Path()
	rect.move_to(0.0, 0.0)
	rect.line_to(0.2, 0.0)
	rect.line_to(0.2, 0.5)
	rect.line_to(0.0, 0.5)
	rect.close_path()

	canvas.begin_path()
	canvas.add_path(rect, slughorn.Matrix.translate(0.1, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.4, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.7, 0.25))
	layer = canvas.fill(slughorn.Color(1, 1, 1), 1.0, "add_path_xform")
	atlas.build()

	s = atlas.get_shape(layer.key)
	assert s is not None
	assert s.width  == pytest.approx(0.8, abs=1e-4)
	assert s.height == pytest.approx(0.5, abs=1e-4)

def test_add_path_transform_coverage(atlas):
	canvas = slughorn.canvas.Canvas(atlas)
	rect = slughorn.canvas.Path()
	rect.move_to(0.0, 0.0)
	rect.line_to(0.2, 0.0)
	rect.line_to(0.2, 0.5)
	rect.line_to(0.0, 0.5)
	rect.close_path()

	canvas.begin_path()
	canvas.add_path(rect, slughorn.Matrix.translate(0.1, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.4, 0.25))
	canvas.add_path(rect, slughorn.Matrix.translate(0.7, 0.25))
	layer = canvas.fill(slughorn.Color(1, 1, 1), 1.0, "add_path_cov")
	atlas.build()

	grid = atlas.decode(layer.key).render_grid(32, 0.0, True)
	assert max(memoryview(grid).cast('B').cast('f')) > 0.5


# ---------------------------------------------------------------------------
# Arc + stroke + Centered origin (rotation pivot regression)
# ---------------------------------------------------------------------------

def test_arc_stroke_centered_origin(atlas):
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator())
	Origin = slughorn.ShapeInfo.Origin
	canvas.begin_path()
	canvas.arc(0.5, 0.5, 0.4, math.pi / 12, 2 * math.pi - math.pi / 12)
	layer = canvas.stroke(0.01, slughorn.Color(0.6, 0.85, 1.0), 1.0, "arc_ring",
		Origin(Origin.Type.Centered))
	atlas.build()
	s = atlas.get_shape("arc_ring")
	assert s is not None
	assert s.origin_x == pytest.approx(s.width  / 2, rel=1e-3)
	assert s.origin_y == pytest.approx(s.height / 2, rel=1e-3)

def test_arc_stroke_compute_quad(atlas):
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator())
	Origin = slughorn.ShapeInfo.Origin
	CX, CY = 0.5, 0.5
	canvas.begin_path()
	canvas.arc(CX, CY, 0.4, math.pi / 12, 2 * math.pi - math.pi / 12)
	canvas.stroke(0.01, slughorn.Color(0.6, 0.85, 1.0), 1.0, "arc_quad",
		Origin(Origin.Type.Centered))
	atlas.build()
	s = atlas.get_shape("arc_quad")
	t = slughorn.Transform()
	t.x = CX
	t.y = CY
	q = s.compute_quad(t)
	mid_x = (q.x0 + q.x1) / 2
	mid_y = (q.y0 + q.y1) / 2
	assert mid_x == pytest.approx(CX, abs=0.02)
	assert mid_y == pytest.approx(CY, abs=0.02)


# ---------------------------------------------------------------------------
# canvas.TextAnchorY / TextAlignX enums
# ---------------------------------------------------------------------------

def test_text_anchor_y_values():
	ta = slughorn.canvas.TextAnchorY
	assert hasattr(ta, "BASELINE")
	assert hasattr(ta, "CAP_CENTER")
	assert hasattr(ta, "CAP_TOP")
	assert hasattr(ta, "X_CENTER")

def test_text_align_x_values():
	ta = slughorn.canvas.TextAlignX
	assert hasattr(ta, "LEFT")
	assert hasattr(ta, "CENTER")
	assert hasattr(ta, "RIGHT")

# ---------------------------------------------------------------------------
# canvas.Path.resample
# ---------------------------------------------------------------------------

def _l_path():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(4.0, 0.0)
	p.line_to(4.0, 3.0)
	return p

def test_path_resample_spacing_and_angles():
	samples = _l_path().resample(1.0)

	assert len(samples) == 8
	assert (samples[2].x, samples[2].y) == pytest.approx((2.0, 0.0), abs=1e-5)
	assert samples[2].angle == pytest.approx(0.0, abs=1e-5)
	assert (samples[6].x, samples[6].y) == pytest.approx((4.0, 2.0), abs=1e-5)
	assert samples[6].angle == pytest.approx(math.pi / 2.0, abs=1e-5)

def test_path_resample_phase_wraps():
	a = _l_path().resample(1.0, 0.25)
	b = _l_path().resample(1.0, 2.25)
	c = _l_path().resample(1.0, -0.75)

	assert a[0].x == pytest.approx(0.25, abs=1e-5)
	assert [s.x for s in a] == pytest.approx([s.x for s in b])
	assert [s.x for s in a] == pytest.approx([s.x for s in c])

def test_path_resample_degenerate():
	assert _l_path().resample(0.0) == []
	assert slughorn.canvas.Path().resample(1.0) == []

def _two_subpaths():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(3.0, 0.0)
	p.move_to(10.0, 5.0)
	p.line_to(10.0, 7.5)
	return p

def test_path_arc_length_skips_subpath_jumps():
	p = _two_subpaths()

	assert p.arc_length() == pytest.approx(5.5)

	s = p.sample(4.0 / 5.5)

	assert (s.x, s.y) == pytest.approx((10.0, 6.0), abs=1e-5)
	assert s.angle == pytest.approx(math.pi / 2.0, abs=1e-5)

def test_path_resample_restarts_per_subpath():
	samples = _two_subpaths().resample(1.0, 0.5)

	assert [(s.x, s.y) for s in samples] == pytest.approx(
		[(0.5, 0.0), (1.5, 0.0), (2.5, 0.0), (10.0, 5.5), (10.0, 6.5), (10.0, 7.5)],
		abs=1e-5
	)
	assert [s.angle for s in samples[3:]] == pytest.approx([math.pi / 2.0] * 3, abs=1e-5)

def test_path_sample_arc_length_per_subpath():
	p = _two_subpaths()

	assert p.sample(0.5).arc_length == pytest.approx(2.75)
	assert p.sample(4.0 / 5.5).arc_length == pytest.approx(1.0)
	assert [s.arc_length for s in p.resample(1.0, 0.5)] == pytest.approx([0.5, 1.5, 2.5, 0.5, 1.5, 2.5])
	assert "arcLength=" in repr(p.sample(0.5))

def test_path_polylines_even_spacing_and_ends():
	lines = [points for points, _ in _two_subpaths().polylines(0.4)]

	assert len(lines) == 2

	# 3.0 / 0.4 -> 8 segments of 0.375; 2.5 / 0.4 -> 7 segments of 2.5 / 7.
	assert len(lines[0]) == 9
	assert len(lines[1]) == 8
	assert (lines[0][-1].x, lines[0][-1].y) == pytest.approx((3.0, 0.0), abs=1e-5)
	assert (lines[1][0].x, lines[1][0].y) == pytest.approx((10.0, 5.0), abs=1e-5)
	assert (lines[1][-1].x, lines[1][-1].y) == pytest.approx((10.0, 7.5), abs=1e-5)
	assert [s.arc_length for s in lines[0]] == pytest.approx([k * 0.375 for k in range(9)], abs=1e-5)
	assert lines[1][-1].arc_length == pytest.approx(2.5, abs=1e-5)

def test_path_polylines_arc_length_is_chord_sum():
	p = slughorn.canvas.Path()
	p.arc(0.0, 0.0, 10.0, 0.0, math.pi)

	line = p.polylines(5.0)[0].points

	chords = sum(math.hypot(b.x - a.x, b.y - a.y) for a, b in zip(line, line[1:]))

	assert line[-1].arc_length == pytest.approx(chords)
	assert line[-1].arc_length < p.arc_length()

def test_path_polylines_degenerate():
	assert _two_subpaths().polylines(0.0) == []
	assert slughorn.canvas.Path().polylines(1.0) == []

def _triangle(close):
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(4.0, 0.0)
	p.line_to(2.0, 3.0)
	if close:
		p.close_path()
	else:
		p.line_to(0.0, 0.0)
	return p

def test_path_polylines_closed_only_by_close_path():
	closed = _triangle(True).polylines(0.5)
	returned = _triangle(False).polylines(0.5)

	assert [l.closed for l in closed] == [True]
	assert [l.closed for l in returned] == [False]

	# Both still end on a repeat of their first point.
	for points, _ in closed + returned:
		assert (points[-1].x, points[-1].y) == pytest.approx((points[0].x, points[0].y), abs=1e-5)

	assert "closed=true" in repr(closed[0])

def test_path_polylines_closed_per_subpath():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(3.0, 0.0)
	p.rect(10.0, 0.0, 2.0, 2.0)
	p.move_to(20.0, 0.0)
	p.line_to(20.0, 4.0)

	assert [l.closed for l in p.polylines(0.5)] == [False, True, False]

def test_path_polylines_closed_survives_add_path():
	src = _triangle(True)
	src.move_to(10.0, 0.0)
	src.line_to(12.0, 0.0)

	dst = slughorn.canvas.Path()
	dst.rect(-5.0, 0.0, 1.0, 1.0)
	dst.add_path(src)
	dst.add_path(src, slughorn.Matrix.translate(0.0, 10.0))

	assert [l.closed for l in dst.polylines(0.5)] == [True, True, False, True, False]

def test_path_polylines_raw_curves_closed_when_ends_meet():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.rect(0.0, 0.0, 1.0, 1.0)
	canvas.rect(0.25, 0.25, 0.5, 0.5)
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, slughorn.Key("rects"))

	path = slughorn.canvas.Path(atlas.get_shape(slughorn.Key("rects")).curves)

	assert [l.closed for l in path.polylines(0.1)] == [True, True]

def test_path_from_shape_keeps_touching_subpaths():
	# The second subpath starts exactly where the first ends. The Shape's recorded boundaries keep
	# them apart; raw curves can only guess from coordinate gaps and merge them.
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.move_to(0.0, 0.0)
	canvas.line_to(10.0, 0.0)
	canvas.move_to(10.0, 0.0)
	canvas.line_to(10.0, 10.0)
	canvas.define_shape(slughorn.Key("touching"))

	shape = atlas.get_shape(slughorn.Key("touching"))

	assert [l.closed for l in slughorn.canvas.Path(shape).polylines(1.0)] == [False, False]
	assert len(slughorn.canvas.Path(shape.curves).polylines(1.0)) == 1

def test_path_from_shape_closed_when_ends_meet():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.circle(500.0, 500.0, 200.0)
	canvas.move_to(0.0, 0.0)
	canvas.line_to(100.0, 0.0)
	canvas.define_shape(slughorn.Key("ring_and_line"))

	path = slughorn.canvas.Path(atlas.get_shape(slughorn.Key("ring_and_line")))

	assert [l.closed for l in path.polylines(5.0)] == [True, False]

def test_path_raw_curves_closed_at_large_coordinates():
	# A loop whose end misses its start by float rounding at ~1000 units: an absolute 1e-6
	# tolerance called it open; the extent-relative one calls it closed.
	x, y = 1000.0, 1000.0
	off = 1000.0001
	curves = [
		slughorn.Curve(x, y, x + 50.0, y, x + 100.0, y),
		slughorn.Curve(x + 100.0, y, x + 100.0, y + 50.0, x + 100.0, y + 100.0),
		slughorn.Curve(x + 100.0, y + 100.0, x + 50.0, y + 50.0, off, y),
	]

	assert [l.closed for l in slughorn.canvas.Path(curves).polylines(5.0)] == [True]

def test_canvas_define_geometry(canvas, atlas):
	canvas.begin_path()
	canvas.circle(0.5, 0.5, 0.4)
	canvas.fill(slughorn.Color(1, 0, 0, 1))

	canvas.begin_path()
	canvas.move_to(0.1, 0.1)
	canvas.line_to(0.9, 0.9)
	layer = canvas.define_geometry(slughorn.Key("route"))

	assert layer.key == slughorn.Key("route")
	assert layer.drawMode == slughorn.DrawMode.Geometry

	composite = canvas.finalize()

	assert [l.drawMode for l in composite.layers] == [slughorn.DrawMode.Visible, slughorn.DrawMode.Geometry]
	assert atlas.get_shape(slughorn.Key("route")) is not None

def test_canvas_define_geometry_from_path(canvas, atlas):
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 1.0)

	layer = canvas.define_geometry(p, slughorn.Key("route_path"))

	assert layer.drawMode == slughorn.DrawMode.Geometry
	assert canvas.finalize().layers[-1].drawMode == slughorn.DrawMode.Geometry

def test_canvas_define_geometry_empty(canvas):
	canvas.begin_path()

	layer = canvas.define_geometry(slughorn.Key("nothing"))

	assert layer.drawMode == slughorn.DrawMode.Visible
	assert canvas.layer_count == 0

def test_path_stroke_ends_only_on_unclosed_subpaths():
	# A subpath that returns to its start without close_path() is open: its two butt-capped ends
	# meet a separate line's end at (0, 0), and that 3-way "star" vertex gets an extra round disc.
	# Closed, the triangle has no ends there, so no disc.
	def outline_count(close):
		p = slughorn.canvas.Path()
		p.move_to(-3.0, -1.0)
		p.line_to(0.0, 0.0)
		p.move_to(0.0, 0.0)
		p.line_to(4.0, 0.0)
		p.line_to(2.0, 3.0)
		if close:
			p.close_path()
		else:
			p.line_to(0.0, 0.0)
		p.stroke_path(0.5)
		return len(p.polylines(0.1))

	assert outline_count(True) == 2
	assert outline_count(False) == 3

def test_path_stroke_closed_then_open_subpaths():
	# The closed triangle is pending and the line still active when stroke_path() runs; both must
	# come back as outlines (a triangle's centerline would be one contour of its perimeter, 11.2).
	p = _triangle(True)
	p.move_to(10.0, 0.0)
	p.line_to(13.0, 0.0)
	p.stroke_path(0.5)

	lines = p.polylines(0.05)

	assert len(lines) == 2
	assert all(l.closed for l in lines)
	assert lines[0].points[-1].arc_length > 2.0 * 11.0
	assert lines[1].points[-1].arc_length == pytest.approx(2.0 * 3.0 + 2.0 * 0.5, abs=1e-3)

def test_path_stroke_twice_keeps_earlier_outlines():
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(3.0, 0.0)
	p.stroke_path(0.5)

	first = [l.points[-1].arc_length for l in p.polylines(0.05)]

	p.rect(10.0, 0.0, 2.0, 2.0)
	p.stroke_path(0.5)

	lengths = [l.points[-1].arc_length for l in p.polylines(0.05)]

	assert len(lengths) == 2
	assert lengths[0] == pytest.approx(first[0])
	assert lengths[1] > 2.0 * 8.0

def test_path_stroke_outlines_are_closed():
	p = _triangle(False)
	p.stroke_path(0.5)

	assert all(l.closed for l in p.polylines(0.1))
