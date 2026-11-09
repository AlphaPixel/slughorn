"""
Tests for centerline strokes: shapes whose curves are a centerline (ShapeInfo.stroke_radius > 0),
covered within a distance of those curves rather than by winding.

render.distance_to_curve(curve, (x, y)) -> em distance to the nearest point of one quadratic.
"""

import math
import pytest
import slughorn

distance = slughorn.render.distance_to_curve


# ---------------------------------------------------------------------------
# Hand-checkable curves
# ---------------------------------------------------------------------------

# A horizontal segment (0,0) -> (1,0) the way lineTo() stores it: control point = endpoint.
LINE = slughorn.Curve(0.0, 0.0, 1.0, 0.0, 1.0, 0.0)

# The same segment with its control point at the chord midpoint: the degenerate (C = 0) branch.
LINE_MID = slughorn.Curve(0.0, 0.0, 0.5, 0.0, 1.0, 0.0)

# B(t) = (2t - 1, (2t - 1)^2): the parabola y = x^2 for x in [-1, 1].
PARABOLA = slughorn.Curve(-1.0, 1.0, 0.0, -1.0, 1.0, 1.0)

@pytest.mark.parametrize("curve", [LINE, LINE_MID], ids=["endpoint-control", "midpoint-control"])
@pytest.mark.parametrize("x, y, expected", [
	(0.25, 0.0, 0.0), # on the segment
	(0.5, 0.3, 0.3), # beside it: the vertical distance
	(0.5, -0.2, 0.2), # either side
	(-0.3, 0.0, 0.3), # past the start, on its line
	(1.3, 0.4, 0.5), # past the end: the 3-4-5 corner distance to (1, 0)
])
def test_distance_to_line(curve, x, y, expected):
	assert distance(curve, (x, y)) == pytest.approx(expected, abs=1e-6)

def test_distance_to_parabola_vertex():
	# Below the vertex: (0, 0) is nearest (one real root).
	assert distance(PARABOLA, (0.0, -0.5)) == pytest.approx(0.5, abs=1e-6)

def test_distance_to_parabola_inside():
	# Above the vertex, at (0, 1): x^2 + (x^2 - 1)^2 is least at x^2 = 1/2, distance sqrt(3) / 2.
	# Three real roots (x = 0 and x = +-1/sqrt(2)); the vertex root is a local maximum of nearness.
	assert distance(PARABOLA, (0.0, 1.0)) == pytest.approx(math.sqrt(3.0) / 2.0, abs=1e-6)

def test_distance_to_parabola_past_end():
	# The curve stops at (1, 1), so a point up and to the right is nearest that endpoint.
	assert distance(PARABOLA, (2.0, 4.0)) == pytest.approx(math.sqrt(10.0), abs=1e-5)


# ---------------------------------------------------------------------------
# Brute force: the closed form against a dense sampling of t
# ---------------------------------------------------------------------------

# The two halves of slughorn-test-canvas's Pattern 24 S-curve.
SCURVE = [
	slughorn.Curve(0.2, 0.85, 0.1, 0.5, 0.5, 0.5),
	slughorn.Curve(0.5, 0.5, 0.9, 0.5, 0.8, 0.15),
]

def _brute_distance(c, x, y, steps=4000):
	best = math.inf

	for i in range(steps + 1):
		t = i / steps
		u = 1.0 - t
		bx = u * u * c.x1 + 2.0 * u * t * c.x2 + t * t * c.x3
		by = u * u * c.y1 + 2.0 * u * t * c.y2 + t * t * c.y3
		best = min(best, math.hypot(bx - x, by - y))

	return best

@pytest.mark.parametrize("curve", SCURVE, ids=["first-half", "second-half"])
def test_distance_matches_brute_force(curve):
	for j in range(11):
		for i in range(11):
			x, y = i / 10.0, j / 10.0
			assert distance(curve, (x, y)) == pytest.approx(_brute_distance(curve, x, y), abs=1e-4), (x, y)


# ---------------------------------------------------------------------------
# Stroke shapes: ShapeInfo.stroke_radius, Sampler coverage
# ---------------------------------------------------------------------------

PPE = 1000.0 # pixels per em: one pixel = 0.001 em

def _stroke_atlas(key, build_path, radius):
	d = slughorn.CurveDecomposer()
	build_path(d)
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	info.stroke_radius = radius
	atlas = slughorn.Atlas()
	atlas.add_shape(slughorn.Key(key), info)
	atlas.build()
	return atlas

def _scurve(d):
	d.move_to(0.2, 0.85)
	d.quad_to(0.1, 0.5, 0.5, 0.5)
	d.quad_to(0.9, 0.5, 0.8, 0.15)

# 20 segments between y = 0 and y = 0.5: enough curves for several bands on each axis.
def _zigzag(d):
	d.move_to(0.0, 0.0)
	for i in range(1, 21):
		d.line_to(i / 20.0, 0.5 if i % 2 else 0.0)

def test_stroke_radius_defaults_to_fill():
	assert slughorn.ShapeInfo().stroke_radius == 0.0

def test_stroke_radius_kept_and_grows_metrics():
	shape = _stroke_atlas("s", _scurve, 0.04).get_shape(slughorn.Key("s"))
	assert shape.stroke_radius == pytest.approx(0.04)
	# The control-point box (0.1..0.9 x 0.15..0.85) grown by 0.04 on every side.
	assert shape.width == pytest.approx(0.88, abs=1e-5)
	assert shape.height == pytest.approx(0.78, abs=1e-5)
	assert shape.bearing == pytest.approx((0.06, 0.89), abs=1e-5)

def test_stroke_coverage_scurve():
	sampler = _stroke_atlas("s", _scurve, 0.04).decode(slughorn.Key("s"))

	def coverage(x, y):
		return sampler.render_sample_banded(x, y, PPE, PPE).fill

	# On the centerline (the S's middle point and both ends).
	assert coverage(0.5, 0.5) == pytest.approx(1.0)
	assert coverage(0.2, 0.85) == pytest.approx(1.0)
	assert coverage(0.8, 0.15) == pytest.approx(1.0)

	# Well clear of it.
	assert coverage(0.9, 0.9) == 0.0
	assert coverage(0.1, 0.1) == 0.0

	# Past the start, straight up from (0.2, 0.85): the round cap. Inside at 0.03, outside at 0.05.
	assert coverage(0.2, 0.88) == pytest.approx(1.0)
	assert coverage(0.2, 0.90) == 0.0

def test_stroke_coverage_matches_distance():
	# Coverage is the distance converted to pixels: clamp((radius - d) * PPE + 0.5, 0, 1).
	sampler = _stroke_atlas("s", _scurve, 0.04).decode(slughorn.Key("s"))

	for j in range(41):
		for i in range(41):
			x, y = i / 40.0, j / 40.0
			d = min(distance(c, (x, y)) for c in SCURVE)
			expected = min(max((0.04 - d) * PPE + 0.5, 0.0), 1.0)
			got = sampler.render_sample_banded(x, y, PPE, PPE).fill
			assert got == pytest.approx(expected, abs=2e-3), (x, y)

def test_stroke_half_coverage_at_radius():
	# Straight up from the middle of a horizontal line, the boundary sits exactly 0.04 away.
	sampler = _stroke_atlas("l", lambda d: (d.move_to(0.0, 0.0), d.line_to(1.0, 0.0)), 0.04).decode(slughorn.Key("l"))
	assert sampler.render_sample_banded(0.5, 0.04, PPE, PPE).fill == pytest.approx(0.5, abs=1e-3)

def test_stroke_banded_matches_unbanded():
	# Every band must hold every curve within reach of it: walking one band list has to give the
	# same coverage as walking all curves.
	sampler = _stroke_atlas("z", _zigzag, 0.02).decode(slughorn.Key("z"))
	assert len(sampler.hband_offsets) - 1 > 1
	assert len(sampler.vband_offsets) - 1 > 1

	banded = sampler.render_grid(128, 0.0, True)
	unbanded = sampler.render_grid(128, 0.0, False)
	b = memoryview(banded).cast('B').cast('f')
	u = memoryview(unbanded).cast('B').cast('f')
	assert max(u) == pytest.approx(1.0)
	assert all(abs(b[i] - u[i]) < 1e-6 for i in range(len(b)))

# Eight horizontal lines at uneven heights, so band edges fall at every distance from them.
BARS = [0.2589, 0.3033, 0.4049, 0.4206, 0.5113, 0.7580, 0.7838, 0.8444]

def _bars(d):
	for y in BARS:
		d.move_to(0.0, y)
		d.line_to(1.0, y)

@pytest.mark.parametrize("size", [128, 512])
def test_stroke_banded_matches_unbanded_edge_fringe(size):
	# Coverage ramps out to half a pixel past the radius, beyond the band padding: a pixel near a
	# band edge must still see a curve filed only in the neighboring band.
	sampler = _stroke_atlas("b", _bars, 0.04).decode(slughorn.Key("b"))
	assert len(sampler.hband_offsets) - 1 > 1

	banded = sampler.render_grid(size, 0.0, True)
	unbanded = sampler.render_grid(size, 0.0, False)
	b = memoryview(banded).cast('B').cast('f')
	u = memoryview(unbanded).cast('B').cast('f')
	assert max(abs(b[i] - u[i]) for i in range(len(b))) < 1e-6

def test_stroke_banded_walks_fewer_curves():
	sampler = _stroke_atlas("z", _zigzag, 0.02).decode(slughorn.Key("z"))
	banded = unbanded = 0

	for j in range(32):
		for i in range(32):
			x, y = (i + 0.5) / 32.0, (j + 0.5) / 64.0
			banded += sampler.render_sample_banded(x, y, PPE, PPE).iters
			unbanded += sampler.render_sample(x, y, PPE, PPE).iters

	assert unbanded == 32 * 32 * 20
	assert banded < unbanded / 2


# ---------------------------------------------------------------------------
# SDF / MSDF tiles: render.field() bakes strokeRadius - distance, not an msdfgen outline
# ---------------------------------------------------------------------------

HAS_SDF = hasattr(slughorn.render, "field")
skip_sdf = pytest.mark.skipif(not HAS_SDF, reason="built without SLUGHORN_SDF=ON")

def _stroke_sdf_atlas(type, radius=0.05, range=0.1, tile_size=32):
	d = slughorn.CurveDecomposer()
	_scurve(d)
	info = slughorn.ShapeInfo()
	info.curves = d.get_curves()
	info.stroke_radius = radius
	atlas = slughorn.Atlas()
	atlas.set_sdf(slughorn.SDF.Config(type=type, tile_size=tile_size, range=range))
	atlas.add_shape(slughorn.Key("s"), info)
	atlas.request_sdf("s")
	atlas.build()
	return atlas

def _texels(atlas):
	"""(channels, flat float list) of the baked texture; row 0 = bottom."""
	tex = atlas.sdf.texture
	channels = 3 if tex.format == "RGB32F" else 1
	return channels, memoryview(tex.bytes).cast('B').cast('f')

@skip_sdf
@pytest.mark.parametrize("type", [slughorn.SDF.Type.SDF, slughorn.SDF.Type.MSDF], ids=["sdf", "msdf"])
def test_stroke_sdf_is_exact_distance(type):
	radius, rng = 0.05, 0.1
	atlas = _stroke_sdf_atlas(type, radius, rng)
	tile = atlas.get_shape(slughorn.Key("s")).sdf
	channels, px = _texels(atlas)
	width = atlas.sdf.texture.width
	assert max(tile.w, tile.h) == 32

	inside = 0

	for row in range(tile.h):
		for col in range(tile.w):
			# Texel centers through the tile contract: em = em_origin + (texel + 0.5) / texels_per_em.
			x = tile.em_origin[0] + (col + 0.5) / tile.texels_per_em
			y = tile.em_origin[1] + (row + 0.5) / tile.texels_per_em
			d = min(distance(c, (x, y)) for c in SCURVE)
			expected = min(max(0.5 + (radius - d) / (2.0 * rng), 0.0), 1.0)
			base = ((tile.y + row) * width + tile.x + col) * channels

			for ch in range(channels):
				assert px[base + ch] == pytest.approx(expected, abs=1e-5), (row, col, ch)

			inside += expected > 0.5

	assert inside > 0

@skip_sdf
def test_stroke_sdf_tile_edge_is_exterior():
	# Bounds grow by stroke_radius + range, and a border texel's center is half a texel inside them,
	# so it is at least range - half a texel outside the stroke.
	rng = 0.1
	atlas = _stroke_sdf_atlas(slughorn.SDF.Type.SDF, range=rng)
	tile = atlas.get_shape(slughorn.Key("s")).sdf
	_, px = _texels(atlas)
	width = atlas.sdf.texture.width
	limit = (0.5 / tile.texels_per_em) / (2.0 * rng) + 1e-6

	for col in range(tile.w):
		for row in (0, tile.h - 1):
			assert px[(tile.y + row) * width + tile.x + col] <= limit, (row, col)

	for row in range(tile.h):
		for col in (0, tile.w - 1):
			assert px[(tile.y + row) * width + tile.x + col] <= limit, (row, col)


# ---------------------------------------------------------------------------
# Canvas: the Stroke type and Stroke.Method.CENTERLINE on every stroke*() verb
# ---------------------------------------------------------------------------

Stroke = slughorn.canvas.Stroke
CENTERLINE = Stroke.Method.CENTERLINE
WHITE = slughorn.Color(1.0, 1.0, 1.0, 1.0)

def test_stroke_type_defaults():
	s = Stroke()
	assert s.width == pytest.approx(1.0)
	assert s.method == Stroke.Method.OUTLINE
	assert s.join is None
	assert s.cap is None
	assert s.miter_limit == pytest.approx(4.0)

def test_stroke_type_kwargs_and_repr():
	s = Stroke(width=0.1, method=CENTERLINE, join=slughorn.canvas.LineJoin.ROUND)
	assert s.width == pytest.approx(0.1)
	assert s.method == CENTERLINE
	assert s.join == slughorn.canvas.LineJoin.ROUND
	assert "Centerline" in repr(s) and "join=Round" in repr(s)

def _canvas_scurve(c, x=0.0, y=0.0):
	c.begin_path()
	c.move_to(x + 0.2, y + 0.85)
	c.quad_to(x + 0.1, y + 0.5, x + 0.5, y + 0.5)
	c.quad_to(x + 0.9, y + 0.5, x + 0.8, y + 0.15)

def _rows(shape):
	return [tuple(r) for r in memoryview(shape.curves).tolist()]

def test_canvas_centerline_stores_the_centerline():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	layer = c.stroke(Stroke(width=0.08, method=CENTERLINE), WHITE, 1.0, slughorn.Key("s"))
	atlas.build()

	shape = atlas.get_shape(slughorn.Key("s"))
	assert shape.stroke_radius == pytest.approx(0.04)
	assert len(_rows(shape)) == 2

	# The stored curves are the S-curve itself, re-origined: adding the layer offset back gives SCURVE.
	t = layer.transform
	for got, want in zip(_rows(shape), SCURVE):
		back = [v + (t.x if i % 2 == 0 else t.y) for i, v in enumerate(got)]
		assert back == pytest.approx([want.x1, want.y1, want.x2, want.y2, want.x3, want.y3], abs=1e-6)

def test_canvas_outline_is_a_fill():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	c.stroke(Stroke(width=0.08), WHITE, 1.0, slughorn.Key("s"))
	atlas.build()

	shape = atlas.get_shape(slughorn.Key("s"))
	assert shape.stroke_radius == 0.0
	assert len(_rows(shape)) > 2

def test_canvas_centerline_radius_follows_scale_and_ctm():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	c.stroke(Stroke(width=0.08, method=CENTERLINE), WHITE, 10.0, slughorn.Key("scaled"))
	c.scale(2.0, 2.0)
	_canvas_scurve(c)
	c.stroke(Stroke(width=0.08, method=CENTERLINE), WHITE, 1.0, slughorn.Key("ctm"))
	atlas.build()

	assert atlas.get_shape(slughorn.Key("scaled")).stroke_radius == pytest.approx(0.4)
	assert atlas.get_shape(slughorn.Key("ctm")).stroke_radius == pytest.approx(0.08)

def test_canvas_centerline_leaves_the_path():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	c.stroke(Stroke(width=0.08, method=CENTERLINE), WHITE, 1.0, slughorn.Key("a"))
	c.stroke(Stroke(width=0.02, method=CENTERLINE), WHITE, 1.0, slughorn.Key("b"))
	atlas.build()

	assert _rows(atlas.get_shape(slughorn.Key("a"))) == _rows(atlas.get_shape(slughorn.Key("b")))
	assert atlas.get_shape(slughorn.Key("b")).stroke_radius == pytest.approx(0.01)

def test_canvas_centerline_explicit_path():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	p = slughorn.canvas.Path()
	p.move_to(0.0, 0.0)
	p.line_to(1.0, 0.0)
	c.stroke(p, Stroke(width=0.1, method=CENTERLINE), WHITE, 1.0, slughorn.Key("p"))
	atlas.build()

	shape = atlas.get_shape(slughorn.Key("p"))
	assert shape.stroke_radius == pytest.approx(0.05)
	assert len(_rows(shape)) == 1
	assert p.has_pending_path

def test_canvas_centerline_gradient():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	gradient = c.create_linear_gradient(0.0, 0.0, 1.0, 0.0, [
		slughorn.GradientStop(0.0, slughorn.Color(1, 0, 0, 1)),
		slughorn.GradientStop(1.0, slughorn.Color(0, 0, 1, 1)),
	])
	layer = c.stroke_gradient(Stroke(width=0.08, method=CENTERLINE), gradient, 1.0, slughorn.Key("g"))
	atlas.build()

	assert layer.gradient_id > 0
	assert atlas.get_shape(slughorn.Key("g")).stroke_radius == pytest.approx(0.04)
	assert not c.has_pending_path

@pytest.mark.parametrize("kwargs", [
	{"join": slughorn.canvas.LineJoin.MITER},
	{"join": slughorn.canvas.LineJoin.BEVEL},
	{"cap": slughorn.canvas.LineCap.BUTT},
	{"cap": slughorn.canvas.LineCap.SQUARE},
], ids=["miter", "bevel", "butt", "square"])
def test_canvas_centerline_rejects_other_joins_and_caps(kwargs):
	c = slughorn.canvas.Canvas(slughorn.Atlas())
	_canvas_scurve(c)

	with pytest.raises(ValueError):
		c.stroke(Stroke(width=0.08, method=CENTERLINE, **kwargs), WHITE)

def test_canvas_centerline_accepts_round():
	atlas = slughorn.Atlas()
	c = slughorn.canvas.Canvas(atlas)
	_canvas_scurve(c)
	s = Stroke(width=0.08, method=CENTERLINE, join=slughorn.canvas.LineJoin.ROUND, cap=slughorn.canvas.LineCap.ROUND)
	assert c.stroke(s, WHITE, 1.0, slughorn.Key("r")).key == slughorn.Key("r")

# A stand-in glyph for stroke_text(): one closed unit-square contour whose FIRST point is the
# (0, 0) corner, registered under the codepoint key stroke_text() looks up.
def _square_glyph_atlas():
	atlas = slughorn.Atlas()
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.line_to(1.0, 0.0)
	d.line_to(1.0, 1.0)
	d.line_to(0.0, 1.0)
	d.close()
	atlas.add_shape(slughorn.Key(ord("A")), slughorn.ShapeInfo(curves=d.get_curves()))
	return atlas

def _coverage_at(atlas, layer, x, y):
	"""Coverage of a committed layer's shape at canvas point (x, y)."""
	sampler = atlas.decode(layer.key)
	return sampler.render_sample_banded(x - layer.transform.x, y - layer.transform.y, PPE, PPE).fill

def test_stroke_text_outline_joins_the_contour_start():
	# A glyph contour is closed, so its start corner gets a join like every other corner. Before
	# the fix it got two butt caps, leaving the outer corner square empty.
	atlas = _square_glyph_atlas()
	c = slughorn.canvas.Canvas(atlas)
	c.stroke_text("A", 1.0, Stroke(width=0.1), 0.0, 0.0, WHITE, slughorn.FontMetrics())
	layers = c.finalize().layers
	atlas.build()

	assert len(layers) == 1
	# Just outside the start corner (0, 0), inside its miter; and the same spot at the (1, 1) corner.
	assert _coverage_at(atlas, layers[0], -0.03, -0.03) == pytest.approx(1.0)
	assert _coverage_at(atlas, layers[0], 1.03, 1.03) == pytest.approx(1.0)

def test_stroke_text_centerline_one_shape_per_glyph():
	atlas = _square_glyph_atlas()
	c = slughorn.canvas.Canvas(atlas)
	c.stroke_text("AA", 1.0, Stroke(width=0.1, method=CENTERLINE), 0.0, 0.0, WHITE, slughorn.FontMetrics())
	layers = c.finalize().layers
	atlas.build()

	assert len(layers) == 2

	for layer in layers:
		shape = atlas.get_shape(layer.key)
		assert shape.stroke_radius == pytest.approx(0.05)
		assert len(_rows(shape)) == 4

	# The round join at the start corner: inside within 0.05 of (0, 0), outside its miter square.
	assert _coverage_at(atlas, layers[0], -0.03, 0.0) == pytest.approx(1.0)
	assert _coverage_at(atlas, layers[0], -0.045, -0.045) == 0.0
