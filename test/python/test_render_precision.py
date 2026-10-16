"""
Magnitude-robustness regression for the Slug root solver (slughorn/render.hpp, mirrored by the
GPU fragment shader).

The solver treats a curve as linear only when a = p0 - 2*p1 + p2 falls below a fixed 1/65536.
Above that it divides by a, and when a is tiny relative to the curve's extent the two roots
cancel catastrophically - scattered coverage dropouts along an edge that should be solid. Three
edge encodings are exercised:

  endpoint - straight edge, control point duplicating the endpoint (CurveDecomposer::lineTo());
             a is as large as the edge itself, never near zero
  midpoint - straight edge, control point at the float32 midpoint (Canvas's stroke outliner);
             a is exactly zero only when the midpoint rounds exactly, otherwise one ulp of the
             coordinate magnitude, which reaches 1/65536 once coordinates pass 128
  bulge    - a genuine quadratic, control point pushed outward by BULGE x the edge length; the
             shape of a nearly straight piece of a subdivided cubic or a large-radius arc

Every shape is re-origined (bounding-box minimum at 0,0), so it is perfectly conditioned: only
the absolute magnitude changes between parameter sets. Coverage is checked against an exact
double-precision inside/outside test of the straight polygon at pixel centres at least
EDGE_CLEARANCE_PX from any edge, so the AA ramp itself is never judged - only fills that should
be solid 0 or 1. BULGE moves the edge by far less than a pixel at every magnitude.
"""

import math
import random
import pytest
import slughorn

MAGNITUDES = (1.0, 100.0, 1000.0, 10000.0)
ENCODINGS = ("endpoint", "midpoint", "bulge")
SHAPE_COUNT = 12
GRID_PX = 64
EDGE_CLEARANCE_PX = 1.0
SOLID_TOLERANCE = 0.01
BULGE = 1.0e-6

# Random convex polygons in unit space, CCW: 3-6 vertices on a jittered ellipse, so no edge is
# axis-aligned by construction and midpoints are almost never exactly representable.
def _unit_polygons():
	rng = random.Random(0x5106)
	polys = []

	for _ in range(SHAPE_COUNT):
		n = rng.randint(3, 6)
		angles = sorted(rng.uniform(0.0, 2.0 * math.pi) for _ in range(n))
		rx = rng.uniform(0.2, 0.5)
		ry = rng.uniform(0.2, 0.5)

		polys.append([(rx * math.cos(t), ry * math.sin(t)) for t in angles])

	return polys

def _reorigin(poly, scale):
	pts = [(x * scale, y * scale) for x, y in poly]
	mx = min(x for x, _ in pts)
	my = min(y for _, y in pts)

	return [(x - mx, y - my) for x, y in pts]

def _curve(p0, p2, encoding):
	x0, y0 = p0
	x2, y2 = p2

	if encoding == "endpoint":
		return slughorn.Curve(x0, y0, x2, y2, x2, y2)

	bulge = BULGE if encoding == "bulge" else 0.0

	# (y2 - y0, x0 - x2) is the outward normal (scaled by edge length) for a CCW polygon.
	return slughorn.Curve(
		x0, y0,
		(x0 + x2) * 0.5 + (y2 - y0) * bulge, (y0 + y2) * 0.5 + (x0 - x2) * bulge,
		x2, y2
	)

def _build(poly, encoding):
	info = slughorn.ShapeInfo()
	info.curves = [_curve(poly[i], poly[(i + 1) % len(poly)], encoding) for i in range(len(poly))]

	atlas = slughorn.Atlas()
	atlas.add_shape(slughorn.Key("poly"), info)
	atlas.build()

	return atlas.decode("poly")

# Min signed distance over the edge lines (positive inside for CCW); negative means outside.
def _edge_distance(poly, x, y):
	best = math.inf

	for i in range(len(poly)):
		x0, y0 = poly[i]
		x1, y1 = poly[(i + 1) % len(poly)]
		ex = x1 - x0
		ey = y1 - y0

		best = min(best, ((x - x0) * -ey + (y - y0) * ex) / math.hypot(ex, ey))

	return best

def _boundary_distance(poly, x, y):
	best = math.inf

	for i in range(len(poly)):
		x0, y0 = poly[i]
		x1, y1 = poly[(i + 1) % len(poly)]
		ex = x1 - x0
		ey = y1 - y0
		t = max(0.0, min(1.0, ((x - x0) * ex + (y - y0) * ey) / (ex * ex + ey * ey)))

		best = min(best, math.hypot(x - (x0 + t * ex), y - (y0 + t * ey)))

	return best

def _failures(poly, sampler, banded):
	span = max(max(x for x, _ in poly), max(y for _, y in poly))
	ppe = GRID_PX / span
	clearance = EDGE_CLEARANCE_PX / ppe
	sample = sampler.render_sample_banded if banded else sampler.render_sample
	bad = []

	for j in range(GRID_PX):
		for i in range(GRID_PX):
			x = (i + 0.5) / ppe
			y = (j + 0.5) / ppe

			if _boundary_distance(poly, x, y) < clearance:
				continue

			expected = 1.0 if _edge_distance(poly, x, y) > 0.0 else 0.0
			fill = sample(x, y, ppe, ppe).fill

			if abs(fill - expected) > SOLID_TOLERANCE:
				bad.append((i, j, expected, fill))

	return bad

@pytest.mark.parametrize("banded", (False, True), ids=("full", "banded"))
@pytest.mark.parametrize("magnitude", MAGNITUDES)
@pytest.mark.parametrize("encoding", ENCODINGS)
def test_edges_magnitude_independent(encoding, magnitude, banded):
	report = []

	for index, unit in enumerate(_unit_polygons()):
		poly = _reorigin(unit, magnitude)
		bad = _failures(poly, _build(poly, encoding), banded)

		if bad:
			report.append(f"polygon {index}: {len(bad)} bad pixels, first {bad[0]}")

	assert not report, f"{encoding} edges at magnitude {magnitude}:\n" + "\n".join(report)

# The exact Canvas.stroke() shape that exposed the bug: a 4-segment polyline baked at scale=160.
def test_canvas_stroke_scaled_polyline():
	polyline = ((-0.5, -0.1), (-0.15, -0.1), (0.05, 0.1), (0.2, -0.1), (0.45, 0.0))

	atlas = slughorn.Atlas()
	cvs = slughorn.canvas.Canvas(atlas)

	cvs.begin_path()
	cvs.move_to(*polyline[0])

	for x, y in polyline[1:]:
		cvs.line_to(x, y)

	layer = cvs.stroke(0.125, slughorn.Color(1.0, 1.0, 1.0, 1.0), scale=160.0)

	atlas.build()

	sampler = atlas.decode(layer.key)
	cb = sampler.curve_buffer
	poly = [(cb[i, 0], cb[i, 1]) for i in range(len(sampler.curves))]

	# The stroke outline is a simple but non-convex polygon, so classify by crossing number rather
	# than _edge_distance().
	def inside(x, y):
		crossings = 0

		for i in range(len(poly)):
			x0, y0 = poly[i]
			x1, y1 = poly[(i + 1) % len(poly)]

			if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
				crossings += 1

		return crossings % 2 == 1

	span = max(max(x for x, _ in poly), max(y for _, y in poly))
	ppe = GRID_PX * 4 / span
	clearance = EDGE_CLEARANCE_PX / ppe
	bad = 0

	for j in range(GRID_PX * 4):
		for i in range(GRID_PX * 4):
			x = (i + 0.5) / ppe
			y = (j + 0.5) / ppe

			if _boundary_distance(poly, x, y) < clearance:
				continue

			expected = 1.0 if inside(x, y) else 0.0

			if abs(sampler.render_sample(x, y, ppe, ppe).fill - expected) > SOLID_TOLERANCE:
				bad += 1

	assert bad == 0, f"{bad} wrongly covered pixels away from the stroke outline"
