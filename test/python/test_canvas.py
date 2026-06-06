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
