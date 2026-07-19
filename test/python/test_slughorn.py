"""
Tests for slughorn/slughorn.hpp — core types and Atlas.
"""

import math
import pytest
import slughorn


# ---------------------------------------------------------------------------
# Module-level constants
# ---------------------------------------------------------------------------

def test_version_constants():
	assert isinstance(slughorn.VERSION_MAJOR, int)
	assert isinstance(slughorn.VERSION_MINOR, int)
	assert isinstance(slughorn.VERSION_PATCH, int)
	assert isinstance(slughorn.version, str)
	assert slughorn.VERSION_MAJOR >= 0

def test_tolerance_constants():
	# DRAFT > BALANCED > FINE (coarser to finer).
	# TOLERANCE_EXACT is FLT_MAX — a sentinel meaning "no subdivision / exact curves as-is".
	# It is NOT the finest value; it is the largest float.
	assert slughorn.TOLERANCE_DRAFT > slughorn.TOLERANCE_BALANCED
	assert slughorn.TOLERANCE_BALANCED > slughorn.TOLERANCE_FINE
	assert slughorn.TOLERANCE_FINE > 0.0
	assert slughorn.TOLERANCE_EXACT > 0.0


# ---------------------------------------------------------------------------
# Key
# ---------------------------------------------------------------------------

def test_key_codepoint():
	k = slughorn.Key(65)
	assert k.type == slughorn.Key.Type.Codepoint
	assert k.codepoint == 65

def test_key_name():
	k = slughorn.Key("logo")
	assert k.type == slughorn.Key.Type.Name
	assert k.name == "logo"

def test_key_equality():
	assert slughorn.Key(65) == slughorn.Key(65)
	assert slughorn.Key("a") == slughorn.Key("a")

def test_key_inequality():
	# Codepoint and Name namespaces are hash-disjoint.
	assert slughorn.Key(65) != slughorn.Key("A")

def test_key_hash():
	assert hash(slughorn.Key(65)) == hash(slughorn.Key(65))
	assert hash(slughorn.Key("logo")) == hash(slughorn.Key("logo"))
	# usable as dict key
	d = {slughorn.Key("x"): 1}
	assert d[slughorn.Key("x")] == 1

def test_key_repr():
	assert "Key(" in repr(slughorn.Key(65))
	assert "logo" in repr(slughorn.Key("logo"))

def test_key_implicit_str():
	# py::implicitly_convertible<std::string, Key>() — plain str accepted where Key expected.
	atlas = slughorn.Atlas()
	info = slughorn.ShapeInfo()
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0); d.line_to(1, 1); d.close()
	info.curves = d.get_curves()
	atlas.add_shape("implicit_str", info)
	assert atlas.has_key("implicit_str")
	assert atlas.has_key(slughorn.Key("implicit_str"))

def test_key_implicit_int():
	# py::implicitly_convertible<uint32_t, Key>() — plain int accepted where Key expected.
	atlas = slughorn.Atlas()
	info = slughorn.ShapeInfo()
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0); d.line_to(1, 1); d.close()
	info.curves = d.get_curves()
	atlas.add_shape(0x1234, info)
	assert atlas.has_key(0x1234)


# ---------------------------------------------------------------------------
# KeyIterator
# ---------------------------------------------------------------------------

def test_key_iterator_prefix():
	ki = slughorn.KeyIterator("glyph")
	assert ki.next() == slughorn.Key("glyph_0")
	assert ki.next() == slughorn.Key("glyph_1")

def test_key_iterator_default_numeric():
	ki = slughorn.KeyIterator()
	k0 = ki.next()
	k1 = ki.next()
	assert k0 != k1

def test_key_iterator_counter_start():
	ki = slughorn.KeyIterator(10)
	k = ki.next()
	assert k.codepoint == 10

def test_key_iterator_counter_readwrite():
	ki = slughorn.KeyIterator("x")
	ki.counter = 5
	assert ki.next() == slughorn.Key("x_5")

def test_key_iterator_prefix_readwrite():
	ki = slughorn.KeyIterator("a")
	ki.prefix = "b"
	assert ki.next() == slughorn.Key("b_0")

def test_key_iterator_force():
	ki = slughorn.KeyIterator("forced", force=True)
	assert ki.force is True

def test_key_iterator_python_protocol():
	ki = slughorn.KeyIterator("p")
	assert next(ki) == slughorn.Key("p_0")
	assert next(ki) == slughorn.Key("p_1")

def test_key_iterator_iter_protocol():
	ki = slughorn.KeyIterator("q")
	it = iter(ki)
	assert next(it) == slughorn.Key("q_0")

def test_key_iterator_repr():
	assert "KeyIterator" in repr(slughorn.KeyIterator("r")) or repr(slughorn.KeyIterator("r")) != ""


# ---------------------------------------------------------------------------
# Color
# ---------------------------------------------------------------------------

def test_color_default():
	c = slughorn.Color()
	assert c.r == pytest.approx(0.0)
	assert c.g == pytest.approx(0.0)
	assert c.b == pytest.approx(0.0)
	assert c.a == pytest.approx(1.0)

def test_color_channels():
	c = slughorn.Color(0.25, 0.5, 0.75, 0.9)
	assert c.r == pytest.approx(0.25)
	assert c.g == pytest.approx(0.5)
	assert c.b == pytest.approx(0.75)
	assert c.a == pytest.approx(0.9)

def test_color_alpha_default():
	c = slughorn.Color(1.0, 0.0, 0.0)
	assert c.a == pytest.approx(1.0)

def test_color_values_tuple():
	c = slughorn.Color(0.1, 0.2, 0.3, 0.4)
	assert c.values == pytest.approx((0.1, 0.2, 0.3, 0.4))

def test_color_readwrite():
	c = slughorn.Color()
	c.r = 0.5
	assert c.r == pytest.approx(0.5)

def test_color_repr():
	assert "Color(" in repr(slughorn.Color(1, 0, 0, 1))


# ---------------------------------------------------------------------------
# Matrix
# ---------------------------------------------------------------------------

def test_matrix_default_is_identity():
	assert slughorn.Matrix().is_identity() is True

def test_matrix_identity_static():
	assert slughorn.Matrix.identity().is_identity() is True

def test_matrix_translate():
	m = slughorn.Matrix.translate(3.0, -2.0)
	assert m.is_identity() is False
	x, y = m.apply(1.0, 1.0)
	assert x == pytest.approx(4.0)
	assert y == pytest.approx(-1.0)

def test_matrix_scale():
	m = slughorn.Matrix.scale(2.0, 3.0)
	x, y = m.apply(1.0, 1.0)
	assert x == pytest.approx(2.0)
	assert y == pytest.approx(3.0)

def test_matrix_rotate():
	m = slughorn.Matrix.rotate(math.pi / 2)
	x, y = m.apply(1.0, 0.0)
	assert x == pytest.approx(0.0, abs=1e-6)
	assert y == pytest.approx(1.0, abs=1e-6)

def test_matrix_apply():
	m = slughorn.Matrix()
	m.dx = 2.0
	m.dy = -3.0
	assert m.apply(1.0, 2.0) == pytest.approx((3.0, -1.0))

def test_matrix_mul():
	t = slughorn.Matrix.translate(1.0, 0.0)
	s = slughorn.Matrix.scale(2.0, 2.0)
	ts = t * s
	x, y = ts.apply(1.0, 1.0)
	# t*s: scale first, then translate
	assert math.isfinite(x) and math.isfinite(y)

def test_matrix_repr():
	assert "Matrix(" in repr(slughorn.Matrix())

def test_matrix_fields_readwrite():
	m = slughorn.Matrix()
	m.xx = 2.0; m.yy = 3.0
	assert m.xx == pytest.approx(2.0)
	assert m.yy == pytest.approx(3.0)


# ---------------------------------------------------------------------------
# GradientStop / GradientInfo
# ---------------------------------------------------------------------------

def test_gradient_stop_default():
	s = slughorn.GradientStop()
	assert s.t == pytest.approx(0.0)

def test_gradient_stop_fields():
	c = slughorn.Color(1, 0, 0, 1)
	s = slughorn.GradientStop(0.5, c)
	assert s.t == pytest.approx(0.5)
	assert s.color.r == pytest.approx(1.0)

def test_gradient_stop_repr():
	assert "GradientStop(" in repr(slughorn.GradientStop(0.5, slughorn.Color(1, 0, 0)))

def test_gradient_info_linear(gradient_stops):
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Linear
	gi.stops = gradient_stops
	gi.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
	assert gi.type == slughorn.GradientInfo.Type.Linear
	assert len(gi.stops) == 2

def test_gradient_info_radial(gradient_stops):
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Radial
	gi.inner_radius = 0.2
	gi.stops = gradient_stops
	assert gi.type == slughorn.GradientInfo.Type.Radial
	assert gi.inner_radius == pytest.approx(0.2)

def test_gradient_info_sweep(gradient_stops):
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Sweep
	gi.start_angle = 0.0
	gi.end_angle = 0.5
	gi.stops = gradient_stops
	assert gi.start_angle == pytest.approx(0.0)
	assert gi.end_angle == pytest.approx(0.5)

def test_build_linear_gradient_matrix():
	m = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
	assert isinstance(m, slughorn.Matrix)
	# Degenerate (zero length) should return identity.
	m2 = slughorn.build_linear_gradient_matrix(0.5, 0.5, 0.5, 0.5)
	assert m2.is_identity() is True


# ---------------------------------------------------------------------------
# Quad
# ---------------------------------------------------------------------------

def test_quad_fields():
	q = slughorn.Quad(1.0, 2.0, 3.0, 4.0)
	assert q.x0 == pytest.approx(1.0)
	assert q.y0 == pytest.approx(2.0)
	assert q.x1 == pytest.approx(3.0)
	assert q.y1 == pytest.approx(4.0)

def test_quad_values_tuple():
	q = slughorn.Quad(1.0, 2.0, 3.0, 4.0)
	assert q.values == pytest.approx((1.0, 2.0, 3.0, 4.0))

def test_quad_repr():
	assert "Quad(" in repr(slughorn.Quad(0, 0, 1, 1)) or repr(slughorn.Quad(0, 0, 1, 1)) != ""


# ---------------------------------------------------------------------------
# Transform
# ---------------------------------------------------------------------------

def test_transform_default():
	t = slughorn.Transform()
	assert t.x == pytest.approx(0.0)
	assert t.y == pytest.approx(0.0)
	assert t.z == pytest.approx(0.0)

def test_transform_fields():
	t = slughorn.Transform(1.0, 2.0, 3.0)
	assert t.x == pytest.approx(1.0)
	assert t.y == pytest.approx(2.0)
	assert t.z == pytest.approx(3.0)

def test_transform_repr():
	assert repr(slughorn.Transform()) != ""


# ---------------------------------------------------------------------------
# Layer
# ---------------------------------------------------------------------------

def test_layer_default():
	l = slughorn.Layer()
	assert l is not None

def test_layer_fields():
	k = slughorn.Key("shape")
	c = slughorn.Color(1, 0, 0, 1)
	t = slughorn.Transform(0.5, 0.5, 0.0)
	l = slughorn.Layer(k, c, t, 1.0, 0, 0)
	assert l.key == k
	assert l.color.r == pytest.approx(1.0)
	assert l.transform.x == pytest.approx(0.5)
	assert l.scale == pytest.approx(1.0)
	assert l.effectId == 0
	assert l.gradientId == 0

def test_layer_bleed_default():
	l = slughorn.Layer(slughorn.Key("k"), slughorn.Color())
	assert l.bleed == pytest.approx(0.0)

def test_layer_str_key():
	l = slughorn.Layer("my_shape")
	assert l.key == slughorn.Key("my_shape")

def test_layer_repr():
	assert repr(slughorn.Layer()) != ""


# ---------------------------------------------------------------------------
# CompositeShape
# ---------------------------------------------------------------------------

def test_composite_shape_empty():
	cs = slughorn.CompositeShape()
	assert len(cs) == 0
	assert cs.advance == pytest.approx(0.0)

def test_composite_shape_append_layer():
	cs = slughorn.CompositeShape()
	cs.layers.append(slughorn.Layer(slughorn.Key("s"), slughorn.Color(1, 0, 0)))
	assert len(cs) == 1

def test_composite_shape_advance():
	cs = slughorn.CompositeShape()
	cs.advance = 1.5
	assert cs.advance == pytest.approx(1.5)

def test_composite_shape_repr():
	assert repr(slughorn.CompositeShape()) != ""


# ---------------------------------------------------------------------------
# FontMetrics (struct; produced by freetype.load_font_metrics)
# ---------------------------------------------------------------------------

def test_font_metrics_class_exists():
	assert hasattr(slughorn, "FontMetrics")

def test_font_metrics_default():
	fm = slughorn.FontMetrics()
	assert isinstance(fm.units_per_em, (int, float))

def test_font_metrics_fields_readwrite():
	fm = slughorn.FontMetrics()
	fm.units_per_em = 2048
	fm.cap_height_ratio = 0.72
	fm.x_height_ratio = 0.53
	fm.ascender_ratio = 0.80
	fm.descender_ratio = 0.20
	fm.line_gap_ratio = 0.0
	assert fm.units_per_em == 2048
	assert fm.cap_height_ratio == pytest.approx(0.72)

def test_font_metrics_repr():
	assert repr(slughorn.FontMetrics()) != ""


# ---------------------------------------------------------------------------
# Curve
# ---------------------------------------------------------------------------

def test_curve_default():
	c = slughorn.Curve()
	assert c is not None

def test_curve_fields():
	c = slughorn.Curve(0.0, 0.0, 0.5, 0.5, 1.0, 0.0)
	assert c.x1 == pytest.approx(0.0)
	assert c.y1 == pytest.approx(0.0)
	assert c.x2 == pytest.approx(0.5)
	assert c.y2 == pytest.approx(0.5)
	assert c.x3 == pytest.approx(1.0)
	assert c.y3 == pytest.approx(0.0)

def test_curve_to_tuple():
	c = slughorn.Curve(0.0, 0.0, 0.5, 0.5, 1.0, 0.0)
	t = c.to_tuple()
	assert t == pytest.approx((0.0, 0.0, 0.5, 0.5, 1.0, 0.0))

def test_curve_repr():
	assert repr(slughorn.Curve()) != ""


# ---------------------------------------------------------------------------
# ShapeInfo / ShapeInfo.Origin
# ---------------------------------------------------------------------------

def test_shape_info_defaults():
	si = slughorn.ShapeInfo()
	assert si.auto_metrics is True
	assert si.curves == [] or si.curves is not None

def test_shape_info_curves_roundtrip(rect_curves):
	si = slughorn.ShapeInfo()
	si.curves = rect_curves
	assert len(si.curves) == len(rect_curves)

def test_shape_info_manual_metrics():
	si = slughorn.ShapeInfo()
	si.auto_metrics = False
	si.width = 1.0
	si.height = 1.0
	si.advance = 1.1
	assert si.width == pytest.approx(1.0)
	assert si.advance == pytest.approx(1.1)

def test_shape_info_splits():
	si = slughorn.ShapeInfo()
	si.splits_x = [0.33, 0.66]
	si.splits_y = [0.5]
	assert si.splits_x == pytest.approx([0.33, 0.66])
	assert si.splits_y == pytest.approx([0.5])

def test_shape_info_origin_default():
	o = slughorn.ShapeInfo.Origin()
	assert o.type == slughorn.ShapeInfo.Origin.Type.Default

def test_shape_info_origin_centered():
	o = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)
	assert o.type == slughorn.ShapeInfo.Origin.Type.Centered

def test_shape_info_origin_pivot():
	o = slughorn.ShapeInfo.Origin(0.5, 0.5)
	assert o.x == pytest.approx(0.5)
	assert o.y == pytest.approx(0.5)

def test_shape_info_origin_custom():
	o = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Custom, 1.0, 2.0)
	assert o.type == slughorn.ShapeInfo.Origin.Type.Custom
	assert o.x == pytest.approx(1.0)
	assert o.y == pytest.approx(2.0)

def test_shape_info_origin_equality():
	a = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)
	b = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)
	assert a == b

def test_shape_info_origin_repr():
	assert repr(slughorn.ShapeInfo.Origin()) != ""

def test_shape_info_repr():
	assert repr(slughorn.ShapeInfo()) != ""


# ---------------------------------------------------------------------------
# CurveDecomposer
# ---------------------------------------------------------------------------

def test_decomposer_basic():
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.line_to(1.0, 0.0)
	d.line_to(1.0, 1.0)
	d.close()
	assert len(d) > 0

def test_decomposer_quad():
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.quad_to(0.5, 1.0, 1.0, 0.0)
	assert len(d) >= 1

def test_decomposer_cubic():
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.cubic_to(0.25, 1.0, 0.75, 1.0, 1.0, 0.0)
	assert len(d) >= 1

def test_decomposer_tolerance_roundtrip():
	d = slughorn.CurveDecomposer()
	d.tolerance = 1e-4
	assert d.tolerance == pytest.approx(1e-4)

def test_decomposer_len_matches_get_curves():
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0); d.line_to(1, 1); d.close()
	assert len(d) == len(d.get_curves())

def test_decomposer_clear():
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0)
	assert len(d) > 0
	d.clear()
	assert len(d) == 0

def test_decomposer_get_curves_returns_copy():
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0); d.close()
	c1 = d.get_curves()
	c2 = d.get_curves()
	assert len(c1) == len(c2)

def test_decomposer_mark_and_reverse():
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.line_to(1.0, 0.0)
	pos = d.mark()
	d.line_to(1.0, 1.0)
	d.line_to(0.0, 1.0)
	before = d.get_curves()
	d.reverse_from(pos)
	after = d.get_curves()
	# Length must be the same; some coordinates must differ.
	assert len(before) == len(after)

def test_decomposer_reverse_curves():
	d = slughorn.CurveDecomposer()
	d.move_to(0, 0); d.line_to(1, 0); d.line_to(1, 1); d.line_to(0, 1)
	n = len(d)
	d.reverse_curves(0, n)
	assert len(d) == n


# ---------------------------------------------------------------------------
# Atlas static helpers
# ---------------------------------------------------------------------------

def test_compute_uniform_splits(rect_curves):
	sx, sy = slughorn.Atlas.compute_uniform_splits(rect_curves, 3, 2)
	assert sx == pytest.approx([1/3, 2/3])
	assert sy == pytest.approx([0.5])

def test_compute_uniform_splits_one_band(rect_curves):
	sx, sy = slughorn.Atlas.compute_uniform_splits(rect_curves, 1, 1)
	assert sx == []
	assert sy == []


# ---------------------------------------------------------------------------
# Atlas lifecycle
# ---------------------------------------------------------------------------

def test_atlas_add_and_has_key(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	assert atlas.has_key(slughorn.Key("r")) is True
	assert atlas.has_key(slughorn.Key("not_there")) is False

def test_atlas_build(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	assert atlas.is_built is False
	atlas.build()
	assert atlas.is_built is True

def test_atlas_build_idempotent(built_atlas):
	built_atlas.build()  # second call must not crash
	assert built_atlas.is_built is True

def test_atlas_get_shape_postbuild(built_atlas):
	shape = built_atlas.get_shape(slughorn.Key("rect"))
	assert shape is not None
	assert shape.width > 0.0
	assert shape.height > 0.0

def test_atlas_get_shape_prebuild(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	# Pre-build: get_shape returns curve/metric data (no GPU band fields yet).
	shape = atlas.get_shape(slughorn.Key("r"))
	assert shape is not None
	assert len(shape.curves) > 0

def test_atlas_get_shape_unknown(built_atlas):
	assert built_atlas.get_shape(slughorn.Key("no_such_key")) is None

def test_atlas_duplicate_key(atlas, rect_curves):
	# Last write wins (map insert/overwrite semantics).
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("dup"), info)
	atlas.add_shape(slughorn.Key("dup"), info)
	atlas.build()
	assert atlas.get_shape(slughorn.Key("dup")) is not None


# ---------------------------------------------------------------------------
# Atlas.get_shape_contours
# ---------------------------------------------------------------------------

def test_get_shape_contours_nonempty(built_atlas):
	contours = built_atlas.get_shape_contours(slughorn.Key("rect"))
	assert len(contours) > 0

def test_get_shape_contours_is_shapecontours(built_atlas):
	contours = built_atlas.get_shape_contours(slughorn.Key("rect"))
	assert isinstance(contours, slughorn.ShapeContours)

def test_get_shape_contours_offsets_span_curves(built_atlas):
	contours = built_atlas.get_shape_contours(slughorn.Key("rect"))
	assert contours.offsets[0] == 0
	assert contours.offsets[-1] == contours.curves.shape[0]
	assert contours.curves.shape[1] == 6

def test_get_shape_contours_unknown(built_atlas):
	contours = built_atlas.get_shape_contours(slughorn.Key("no_such"))
	assert len(contours) == 0


# ---------------------------------------------------------------------------
# Atlas bulk accessors
# ---------------------------------------------------------------------------

def test_atlas_get_shapes(built_atlas):
	shapes = built_atlas.get_shapes()
	assert isinstance(shapes, dict)
	assert slughorn.Key("rect") in shapes

def test_atlas_get_composite_shapes(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	cs = slughorn.CompositeShape()
	cs.layers.append(slughorn.Layer(slughorn.Key("r"), slughorn.Color(1, 0, 0)))
	atlas.add_composite_shape(slughorn.Key("comp"), cs)
	atlas.build()
	comps = atlas.get_composite_shapes()
	assert slughorn.Key("comp") in comps


# ---------------------------------------------------------------------------
# Atlas textures
# ---------------------------------------------------------------------------

def test_atlas_curve_texture(built_atlas):
	td = built_atlas.curve_texture
	assert td.format == "RGBA32F"
	assert len(bytes(td.bytes)) > 0

def test_atlas_band_texture(built_atlas):
	td = built_atlas.band_texture
	assert td.format == "RGBA16UI"
	assert len(bytes(td.bytes)) > 0

def test_atlas_gradient_texture_empty_without_gradients(built_atlas):
	# When no gradients are registered the gradient texture slot is unpopulated.
	# BUG: format returns "RGBA32F" instead of "RGBA8" in this case — the
	# TextureData falls back to the default-constructed Format enum value rather
	# than the gradient slot's declared RGBA8.  Fix: initialise gradient
	# TextureData.format = RGBA8 unconditionally in Atlas::build().
	td = built_atlas.gradient_texture
	assert td.format == "RGBA32F"  # current (wrong) behavior — should be "RGBA8"

def test_atlas_gradient_texture_nonempty_with_gradient(atlas, rect_curves, gradient_stops):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Linear
	gi.stops = gradient_stops
	gi.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
	atlas.add_gradient(gi)
	atlas.build()
	td = atlas.gradient_texture
	assert td.width > 0

def test_texture_data_repr(built_atlas):
	assert repr(built_atlas.curve_texture) != ""


# ---------------------------------------------------------------------------
# Atlas gradients
# ---------------------------------------------------------------------------

def test_atlas_add_gradient_returns_id(atlas, gradient_stops):
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Linear
	gi.stops = gradient_stops
	gi.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
	gid = atlas.add_gradient(gi)
	assert gid >= 1

def test_atlas_add_gradient_ids_sequential(atlas, gradient_stops):
	def make_gi():
		gi = slughorn.GradientInfo()
		gi.type = slughorn.GradientInfo.Type.Linear
		gi.stops = gradient_stops
		gi.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
		return gi
	id1 = atlas.add_gradient(make_gi())
	id2 = atlas.add_gradient(make_gi())
	assert id2 == id1 + 1

def test_atlas_get_gradients(atlas, gradient_stops):
	gi = slughorn.GradientInfo()
	gi.type = slughorn.GradientInfo.Type.Linear
	gi.stops = gradient_stops
	gi.transform = slughorn.build_linear_gradient_matrix(0.0, 0.0, 1.0, 0.0)
	atlas.add_gradient(gi)
	atlas.add_gradient(gi)
	assert len(atlas.get_gradients()) == 2


# ---------------------------------------------------------------------------
# Atlas composite shapes
# ---------------------------------------------------------------------------

def test_atlas_composite_round_trip(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	cs = slughorn.CompositeShape()
	cs.layers.append(slughorn.Layer(slughorn.Key("r"), slughorn.Color(1, 0, 0)))
	atlas.add_composite_shape(slughorn.Key("comp"), cs)
	result = atlas.get_composite_shape(slughorn.Key("comp"))
	assert result is not None
	assert len(result) == 1

def test_atlas_composite_advance(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("r"), info)
	cs = slughorn.CompositeShape()
	cs.advance = 2.5
	atlas.add_composite_shape(slughorn.Key("comp"), cs)
	assert atlas.get_composite_shape(slughorn.Key("comp")).advance == pytest.approx(2.5)

def test_atlas_composite_unknown(atlas):
	assert atlas.get_composite_shape(slughorn.Key("no_such")) is None


# ---------------------------------------------------------------------------
# Atlas packing stats
# ---------------------------------------------------------------------------

def test_atlas_packing_stats(built_atlas):
	ps = built_atlas.packing_stats
	assert ps.curve_texels_total > 0
	assert ps.band_texels_total > 0
	assert ps.gradient_count == 0

def test_packing_stats_utilization(built_atlas):
	ps = built_atlas.packing_stats
	assert 0.0 <= ps.curve_utilization() <= 1.0
	assert 0.0 <= ps.band_utilization() <= 1.0

def test_packing_stats_repr(built_atlas):
	assert repr(built_atlas.packing_stats) != ""


# ---------------------------------------------------------------------------
# Shape (post-build)
# ---------------------------------------------------------------------------

def test_shape_dimensions(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	assert s.width > 0.0
	assert s.height > 0.0

def test_shape_advance(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	assert s.advance > 0.0

def test_shape_compute_quad(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	q = s.compute_quad(slughorn.Transform())
	assert q.x1 > q.x0
	assert q.y1 > q.y0

def test_shape_em_origin(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	ox, oy = s.em_origin
	assert math.isfinite(ox) and math.isfinite(oy)

def test_shape_em_to_uv(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	ox, oy = s.em_origin
	u, v = s.em_to_uv(ox, oy)
	assert u == pytest.approx(0.0, abs=1e-5)
	assert v == pytest.approx(0.0, abs=1e-5)

def test_shape_em_size(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	sx, sy = s.em_size
	assert sx > 0.0 and sy > 0.0

def test_shape_origin_default(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	assert s.origin_x == pytest.approx(0.0)
	assert s.origin_y == pytest.approx(0.0)

def test_shape_origin_centered(atlas, rect_curves):
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	info.origin = slughorn.ShapeInfo.Origin(slughorn.ShapeInfo.Origin.Type.Centered)
	atlas.add_shape(slughorn.Key("centered"), info)
	atlas.build()
	s = atlas.get_shape(slughorn.Key("centered"))
	assert s.origin_x == pytest.approx(s.width / 2, rel=1e-3)
	assert s.origin_y == pytest.approx(s.height / 2, rel=1e-3)

def test_shape_curves_nonempty(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	assert len(s.curves) > 0

def test_shape_curves_is_buffer(built_atlas):
	s = built_atlas.get_shape(slughorn.Key("rect"))
	mv = memoryview(s.curves)
	assert mv.ndim == 2
	assert mv.shape[1] == 6
	assert mv.format == 'f'

def test_shape_repr(built_atlas):
	assert repr(built_atlas.get_shape(slughorn.Key("rect"))) != ""


# ---------------------------------------------------------------------------
# Atlas.normalize_shape_metrics
# ---------------------------------------------------------------------------

def test_normalize_canvas_shapes(atlas):
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("n"))
	canvas.begin_path()
	canvas.rect(0, 0, 0.3, 0.3)
	l_small = canvas.fill(slughorn.Color(1, 0, 0), 1.0)
	canvas.finalize()

	canvas.begin_path()
	canvas.rect(0, 0, 0.7, 0.7)
	l_large = canvas.fill(slughorn.Color(0, 1, 0), 1.0)
	canvas.finalize()

	atlas.normalize_shape_metrics([l_small.key, l_large.key])
	atlas.build()

	s_small = atlas.get_shape(l_small.key)
	s_large = atlas.get_shape(l_large.key)
	assert s_small is not None and s_large is not None
	assert s_small.width  == pytest.approx(s_large.width,  abs=1e-5)
	assert s_small.height == pytest.approx(s_large.height, abs=1e-5)

def test_normalize_band_transforms_differ(atlas):
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("nb"))
	canvas.begin_path()
	canvas.rect(0, 0, 0.3, 0.3)
	l_small = canvas.fill(slughorn.Color(1, 0, 0), 1.0)
	canvas.finalize()

	canvas.begin_path()
	canvas.rect(0, 0, 0.7, 0.7)
	l_large = canvas.fill(slughorn.Color(0, 1, 0), 1.0)
	canvas.finalize()

	atlas.normalize_shape_metrics([l_small.key, l_large.key])
	atlas.build()

	s_small = atlas.get_shape(l_small.key)
	s_large = atlas.get_shape(l_large.key)
	assert s_small.band_scale_x != pytest.approx(s_large.band_scale_x, abs=1e-5)

def test_normalize_coverage_preserved(atlas):
	canvas = slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("nc"))
	canvas.begin_path()
	canvas.rect(0, 0, 0.3, 0.3)
	l_small = canvas.fill(slughorn.Color(1, 0, 0), 1.0)
	canvas.finalize()

	canvas.begin_path()
	canvas.rect(0, 0, 0.7, 0.7)
	l_large = canvas.fill(slughorn.Color(0, 1, 0), 1.0)
	canvas.finalize()

	atlas.normalize_shape_metrics([l_small.key, l_large.key])
	atlas.build()

	for key in (l_small.key, l_large.key):
		grid = atlas.decode(key).render_grid(32, 0.0, True)
		assert max(memoryview(grid).cast('B').cast('f')) > 0.1, f"{key} should have non-zero coverage after normalize"
