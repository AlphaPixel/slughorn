#!/usr/bin/env python3
# slughorn_render.py
#
# Software emulator for the Slug rendering algorithm (Lengyel 2017).
#
# Three levels of abstraction, each building on the one below:
#
# Level 1 - Pure math (no slughorn dependency)
# - calc_root_code, solve_horiz_poly, solve_vert_poly, calc_coverage
# - render_sample: ground truth, iterates all curves, no bands
# - render_sample_banded: band-accelerated, mirrors GPU shader exactly
#
# Level 2 - Atlas bridge (requires compiled slughorn module)
# AtlasView: decodes a built Atlas into Python-native structures that render_sample_banded can
# consume. Constructed once per (atlas, key) pair.
#
# Level 3 - Grid samplers + image output
# - sample_grid: reference path (pure Python Curve list)
# - sample_grid_from_atlas: banded path (AtlasView)
# - save_image, print_grid

import math
import struct
import numpy as np

from dataclasses import dataclass
from typing import List, Tuple
from PIL import Image, ImageDraw

# =============================================================================
# Constants
# =============================================================================

EPS = 1.0 / 65536.0 # must match shader

# Band texture width (must match Atlas::TEX_WIDTH and kLogBandTextureWidth)
BAND_TEX_WIDTH = 512
LOG_BAND_TEX_WIDTH = 9 # 2^9 == 512

# =============================================================================
# Shared transform helpers
# =============================================================================

class EmTransform:
	"""
	Shared em-space <-> pixel-space mapping used by:

	This is the single source of truth for coordinate mapping so the debug
	renderer and the samplers cannot drift apart.
	"""

	def __init__(self, shape, width: int, height: int, margin: float=0.0):
		ox, oy = shape.em_origin
		sx, sy = shape.em_size

		# Margin is expressed as a fraction of the shape's em extents.
		ox -= margin * sx
		oy -= margin * sy
		sx *= (1.0 + 2.0 * margin)
		sy *= (1.0 + 2.0 * margin)

		self.ox = ox
		self.oy = oy
		self.sx = sx
		self.sy = sy

		self.width = width
		self.height = height

	def px_to_em(self, i: int, j: int) -> Tuple[float, float]:
		# Pixel-center sampling.
		u = (i + 0.5) / self.width
		v = (j + 0.5) / self.height

		return self.ox + u * self.sx, self.oy + v * self.sy

	def em_to_px(self, ex: float, ey: float) -> Tuple[int, int]:
		u = (ex - self.ox) / self.sx
		v = (ey - self.oy) / self.sy

		return int(u * self.width), int(v * self.height)

def compute_render_size(shape, size_hint: int=256) -> Tuple[int, int]:
	"""
	Use size_hint as the maximum output dimension while preserving aspect ratio.
	"""
	w = shape.width
	h = shape.height

	if w <= 0.0 or h <= 0.0:
		raise ValueError(f"Invalid shape dimensions: width={w}, height={h}")

	scale = size_hint / max(w, h)
	out_w = max(1, int(round(w * scale)))
	out_h = max(1, int(round(h * scale)))

	return out_w, out_h

# ================================================================================================
# Level 1 - Pure math
# ================================================================================================

# When working with a compiled Atlas, you can convert a slughorn.Curve to this with:
# `Curve(*c.to_tuple())`, or just build AtlasView which does it for you.
@dataclass
class Curve:
	x1: float; y1: float
	x2: float; y2: float
	x3: float; y3: float

	def to_tuple(self) -> Tuple[float, ...]:
		return self.x1, self.y1, self.x2, self.y2, self.x3, self.y3

	@staticmethod
	def from_slughorn(c) -> "Curve":
		"""Convert a slughorn.Curve (C++ binding) to a pure-Python Curve."""

		return Curve(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3)

def float_bits_to_uint32(x: float) -> int:
	return struct.unpack(">I", struct.pack(">f", x))[0]

def clamp(x: float, lo: float, hi: float) -> float:
	return lo if x < lo else hi if x > hi else x

# ------------------------------------------------------------------------------------------------
# slug_CalcRootCode - exact port of the GLSL function
# ------------------------------------------------------------------------------------------------

def calc_root_code(y1: float, y2: float, y3: float) -> int:
	i1 = float_bits_to_uint32(y1) >> 31
	i2 = float_bits_to_uint32(y2) >> 30
	i3 = float_bits_to_uint32(y3) >> 29

	shift = (i2 & 0x2) | (i1 & ~0x2)
	shift = (i3 & 0x4) | (shift & ~0x4)

	return (0x2E74 >> shift) & 0x0101

# ------------------------------------------------------------------------------------------------
# solve_horiz_poly - find the X intercept(s) of a quadratic Bezier at Y=0
# ------------------------------------------------------------------------------------------------

def solve_horiz_poly(
	p1: Tuple[float, float],
	p2: Tuple[float, float],
	p3: Tuple[float, float],
) -> Tuple[float, float]:
	ax = p1[0] - 2.0 * p2[0] + p3[0]
	ay = p1[1] - 2.0 * p2[1] + p3[1]
	bx = p1[0] - p2[0]
	by = p1[1] - p2[1]

	if abs(ay) < EPS:
		t = p1[1] * (0.5 / by) if abs(by) >= EPS else 0.0
		x = (ax * t - 2.0 * bx) * t + p1[0]

		return x, x

	d = math.sqrt(max(by * by - ay * p1[1], 0.0))
	t1 = (by - d) / ay
	t2 = (by + d) / ay
	x1 = (ax * t1 - 2.0 * bx) * t1 + p1[0]
	x2 = (ax * t2 - 2.0 * bx) * t2 + p1[0]

	return x1, x2

# ------------------------------------------------------------------------------------------------
# solve_vert_poly - find the Y intercept(s) of a quadratic Bezier at X=0
# ------------------------------------------------------------------------------------------------

def solve_vert_poly(
	p1: Tuple[float, float],
	p2: Tuple[float, float],
	p3: Tuple[float, float],
) -> Tuple[float, float]:
	ax = p1[0] - 2.0 * p2[0] + p3[0]
	ay = p1[1] - 2.0 * p2[1] + p3[1]
	bx = p1[0] - p2[0]
	by = p1[1] - p2[1]

	if abs(ax) < EPS:
		t = p1[0] * (0.5 / bx) if abs(bx) >= EPS else 0.0
		y = (ay * t - 2.0 * by) * t + p1[1]

		return y, y

	d = math.sqrt(max(bx * bx - ax * p1[0], 0.0))
	t1 = (bx - d) / ax
	t2 = (bx + d) / ax
	y1 = (ay * t1 - 2.0 * by) * t1 + p1[1]
	y2 = (ay * t2 - 2.0 * by) * t2 + p1[1]

	return y1, y2

# ------------------------------------------------------------------------------------------------
# calc_coverage - combine horizontal and vertical winding accumulators
# ------------------------------------------------------------------------------------------------

def calc_coverage(
	xcov: float, ycov: float,
	xwgt: float, ywgt: float,
) -> float:
	weighted = abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, EPS)
	conservative = min(abs(xcov), abs(ycov))
	return clamp(max(weighted, conservative), 0.0, 1.0)

# ------------------------------------------------------------------------------------------------
# render_sample - ground-truth renderer, NO bands
# ------------------------------------------------------------------------------------------------

def render_sample(
	curves: List[Curve],
	render_coord: Tuple[float, float],
	pixels_per_em: Tuple[float, float],
) -> dict:
	rx, ry = render_coord
	ppe_x, ppe_y = pixels_per_em
	xcov = xwgt = ycov = ywgt = 0.0
	iters = 0

	for c in curves:
		iters += 1

		p1 = (c.x1 - rx, c.y1 - ry)
		p2 = (c.x2 - rx, c.y2 - ry)
		p3 = (c.x3 - rx, c.y3 - ry)

		code = calc_root_code(p1[1], p2[1], p3[1])

		if code:
			r1, r2 = solve_horiz_poly(p1, p2, p3)
			r1 *= ppe_x
			r2 *= ppe_x

			if code & 0x01:
				xcov += clamp(r1 + 0.5, 0.0, 1.0)
				xwgt = max(xwgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

			if code & 0x100:
				xcov -= clamp(r2 + 0.5, 0.0, 1.0)
				xwgt = max(xwgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

		code = calc_root_code(p1[0], p2[0], p3[0])

		if code:
			r1, r2 = solve_vert_poly(p1, p2, p3)
			r1 *= ppe_y
			r2 *= ppe_y

			if code & 0x01:
				ycov -= clamp(r1 + 0.5, 0.0, 1.0)
				ywgt = max(ywgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

			if code & 0x100:
				ycov += clamp(r2 + 0.5, 0.0, 1.0)
				ywgt = max(ywgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

	return {
		"fill": calc_coverage(xcov, ycov, xwgt, ywgt),
		"xcov": xcov, "ycov": ycov,
		"xwgt": xwgt, "ywgt": ywgt,
		"iters": iters,
	}

# ------------------------------------------------------------------------------------------------
# render_sample_banded - band-accelerated renderer
# ------------------------------------------------------------------------------------------------

def render_sample_banded(
	curves: List[Tuple[float, float, float, float, float, float]],
	hbands_idx: List[List[int]],
	vbands_idx: List[List[int]],
	render_coord: Tuple[float, float],
	pixels_per_em: Tuple[float, float],
	band_scale_x: float,
	band_scale_y: float,
	band_offset_x: float,
	band_offset_y: float,
	band_max_x: int,
	band_max_y: int,
) -> dict:
	rx, ry = render_coord
	ppe_x, ppe_y = pixels_per_em
	band_x = int(clamp(rx * band_scale_x + band_offset_x, 0, band_max_x))
	band_y = int(clamp(ry * band_scale_y + band_offset_y, 0, band_max_y))
	xcov = xwgt = ycov = ywgt = 0.0
	iters = 0

	for ci in hbands_idx[band_y]:
		iters += 1
		c = curves[ci]

		p1 = (c[0] - rx, c[1] - ry)
		p2 = (c[2] - rx, c[3] - ry)
		p3 = (c[4] - rx, c[5] - ry)

		if max(p1[0], p2[0], p3[0]) * ppe_x < -0.5:
			break

		code = calc_root_code(p1[1], p2[1], p3[1])

		if code:
			r1, r2 = solve_horiz_poly(p1, p2, p3)
			r1 *= ppe_x
			r2 *= ppe_x

			if code & 0x01:
				xcov += clamp(r1 + 0.5, 0.0, 1.0)
				xwgt = max(xwgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

			if code & 0x100:
				xcov -= clamp(r2 + 0.5, 0.0, 1.0)
				xwgt = max(xwgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

	for ci in vbands_idx[band_x]:
		iters += 1
		c = curves[ci]

		p1 = (c[0] - rx, c[1] - ry)
		p2 = (c[2] - rx, c[3] - ry)
		p3 = (c[4] - rx, c[5] - ry)

		if max(p1[1], p2[1], p3[1]) * ppe_y < -0.5:
			break

		code = calc_root_code(p1[0], p2[0], p3[0])

		if code:
			r1, r2 = solve_vert_poly(p1, p2, p3)
			r1 *= ppe_y
			r2 *= ppe_y

			if code & 0x01:
				ycov -= clamp(r1 + 0.5, 0.0, 1.0)
				ywgt = max(ywgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

			if code & 0x100:
				ycov += clamp(r2 + 0.5, 0.0, 1.0)
				ywgt = max(ywgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

	return {
		"fill": calc_coverage(xcov, ycov, xwgt, ywgt),
		"xcov": xcov, "ycov": ycov,
		"xwgt": xwgt, "ywgt": ywgt,
		"iters": iters,
	}

# ================================================================================================
# Level 2 - Atlas bridge
# ================================================================================================

class AtlasView:
	"""
	Decodes a built slughorn.Atlas for a single shape key into Python-native
	structures that render_sample_banded can consume directly.
	"""

	def __init__(self, atlas, key):
		self.shape = atlas.get_shape(key)

		if self.shape is None:
			raise KeyError(f"Key {key!r} not found in atlas (or atlas not built yet)")

		self.curves = self._decode_curve_texture(atlas.curve_texture)
		self.curve_list = [Curve(*c) for c in self.curves]

		self.hbands_idx, self.vbands_idx = self._decode_band_texture(
			atlas.band_texture, self.shape
		)

	def render(
		self,
		em_x: float,
		em_y: float,
		pixels_per_em: Tuple[float, float],
		banded: bool = True,
	) -> dict:
		if banded:
			return render_sample_banded(
				self.curves,
				self.hbands_idx,
				self.vbands_idx,
				(em_x, em_y),
				pixels_per_em,
				self.shape.band_scale_x,
				self.shape.band_scale_y,
				self.shape.band_offset_x,
				self.shape.band_offset_y,
				self.shape.band_max_x,
				self.shape.band_max_y,
			)

		else:
			return render_sample(self.curve_list, (em_x, em_y), pixels_per_em)

	@staticmethod
	def _decode_curve_texture(tex) -> List[Tuple[float, float, float, float, float, float]]:
		data = np.frombuffer(tex.bytes, dtype=np.float32).reshape(
			(tex.height, tex.width, 4)
		)

		curves = []

		for y in range(tex.height):
			for x in range(0, tex.width, 2):
				t0 = data[y, x]

				if x + 1 >= tex.width:
					continue

				t1 = data[y, x + 1]

				if np.allclose(t0, 0.0) and np.allclose(t1, 0.0):
					continue

				x1, y1, x2, y2 = t0
				x3, y3, _, _ = t1

				curves.append((
					float(x1), float(y1),
					float(x2), float(y2),
					float(x3), float(y3),
				))

		return curves

	@staticmethod
	def _decode_band_texture(tex, shape) -> Tuple[List[List[int]], List[List[int]]]:
		data = np.frombuffer(tex.bytes, dtype=np.uint16).reshape(
			(tex.height, tex.width, 4)
		)

		start = shape.band_tex_y * BAND_TEX_WIDTH + shape.band_tex_x
		num_h = shape.band_max_y + 1
		num_v = shape.band_max_x + 1
		num_headers = num_h + num_v

		def loc_to_index(cx: int, cy: int) -> int:
			return (cy * BAND_TEX_WIDTH + cx) // 2

		def read_texel(relative_offset: int) -> np.ndarray:
			abs_idx = start + relative_offset
			ty = abs_idx >> LOG_BAND_TEX_WIDTH
			tx = abs_idx & (BAND_TEX_WIDTH - 1)

			return data[ty, tx]

		headers = []

		for i in range(num_headers):
			texel = read_texel(i)
			count = int(texel[0])
			offset = int(texel[1])

			headers.append((count, offset))

		def decode_band(count: int, offset: int) -> List[int]:
			result = []

			for i in range(count):
				texel = read_texel(offset + i)
				cx = int(texel[0])
				cy = int(texel[1])

				result.append(loc_to_index(cx, cy))

			return result

		hbands_idx = [decode_band(*headers[i]) for i in range(num_h)]
		vbands_idx = [decode_band(*headers[num_h + i]) for i in range(num_v)]

		return hbands_idx, vbands_idx

# ================================================================================================
# Level 3 - Grid samplers + image I/O
# ================================================================================================

def sample_grid(
	curves: List[Curve],
	shape,
	size: int=128,
	margin: float=0.0,
) -> List[List[float]]:
	"""
	Render a grid using the reference (brute-force) renderer.

	Unlike the old version, this samples in the shape's actual em-space
	using the same transform logic as the atlas path and debug renderer.
	"""

	width, height = compute_render_size(shape, size)
	xf = EmTransform(shape, width, height, margin)
	ppe = (float(width), float(height))
	grid = [[0.0] * width for _ in range(height)]

	for j in range(height):
		for i in range(width):
			ex, ey = xf.px_to_em(i, j)
			grid[j][i] = render_sample(curves, (ex, ey), ppe)["fill"]

	return grid

def sample_grid_from_atlas(
	atlas,
	key,
	size: int=128,
	margin: float=0.0,
	banded: bool=True,
) -> List[List[float]]:
	"""
	Render a grid from a built slughorn.Atlas
	"""

	view = AtlasView(atlas, key)
	shape = view.shape
	width, height = compute_render_size(shape, size)
	xf = EmTransform(shape, width, height, margin)
	ppe = (float(width), float(height))
	grid = [[0.0] * width for _ in range(height)]

	for j in range(height):
		for i in range(width):
			ex, ey = xf.px_to_em(i, j)
			grid[j][i] = view.render(ex, ey, ppe, banded=banded)["fill"]

	return grid

# ------------------------------------------------------------------------------------------------
# Debug output, file writing
# ------------------------------------------------------------------------------------------------

def print_grid(grid: List[List[float]]) -> None:
	chars = " .:-=+*#%@"

	for row in grid:
		print("".join(chars[int(v * (len(chars) - 1))] for v in row))

def save_image(grid: List[List[float]], filename: str = "out.png", flip_y: bool = False) -> None:
	if flip_y:
		grid = list(reversed(grid))

	h, w = len(grid), len(grid[0])
	img = Image.new("L", (w, h))
	px = img.load()

	for y in range(h):
		for x in range(w):
			px[x, y] = int(clamp(grid[y][x], 0.0, 1.0) * 255)

	img.save(filename)

	return f"Saved {w}x{h} image -> {filename}"

def save_curves(
	curves,
	shape,
	filename="curves_debug.png",
	scale: int=1024,
	margin: float=0.0,
	flip_y: bool=True
):
	"""
	Render the raw curve geometry as a diagnostic diagram using the same
	em-space -> pixel-space transform as the samplers.
	"""

	w, h = compute_render_size(shape, scale)
	xf = EmTransform(shape, w, h, margin)
	img = Image.new("RGB", (w, h), (30, 30, 30))
	draw = ImageDraw.Draw(img)

	# def em_to_px(ex, ey):
	# 	return xf.em_to_px(ex, ey)

	def em_to_px(ex, ey):
		px, py = xf.em_to_px(ex, ey)

		if flip_y:
			py = h - py

		return px, py

	def draw_quad_bezier(p1, p2, p3, color, steps=32):
		pts = []

		for i in range(steps + 1):
			t = i / steps
			mt = 1.0 - t
			x = mt * mt * p1[0] + 2 * mt * t * p2[0] + t * t * p3[0]
			y = mt * mt * p1[1] + 2 * mt * t * p2[1] + t * t * p3[1]

			pts.append(em_to_px(x, y))

		for i in range(len(pts) - 1):
			draw.line([pts[i], pts[i + 1]], fill=color, width=1)

	for c in curves:
		x1, y1, x2, y2, x3, y3 = c

		p1 = (x1, y1)
		p2 = (x2, y2)
		p3 = (x3, y3)

		draw_quad_bezier(p1, p2, p3, color=(200, 200, 200))

		draw.line([em_to_px(*p1), em_to_px(*p2)], fill=(80, 80, 180), width=1)
		draw.line([em_to_px(*p2), em_to_px(*p3)], fill=(80, 80, 180), width=1)

		for px, py in [em_to_px(*p1), em_to_px(*p3)]:
			r = 2
			draw.ellipse([px-r, py-r, px+r, py+r], fill=(80, 220, 80))

		px, py = em_to_px(*p2)
		r = 3

		draw.ellipse([px - r, py - r, px + r, py + r], outline=(100, 100, 255), width=1)

	# -------------------------------------------------------------------------
	# Band overlay
	# -------------------------------------------------------------------------
	ox, oy = shape.em_origin
	sx, sy = shape.em_size

	# Horizontal bands
	for i in range(shape.band_max_y + 1):
		y = (i - shape.band_offset_y) / shape.band_scale_y

		x0, y0 = em_to_px(ox, y)
		x1, y1 = em_to_px(ox + sx, y)

		draw.line([(x0, y0), (x1, y1)], fill=(255, 0, 0), width=1)

	# Vertical bands
	for i in range(shape.band_max_x + 1):
		x = (i - shape.band_offset_x) / shape.band_scale_x

		x0, y0 = em_to_px(x, oy)
		x1, y1 = em_to_px(x, oy + sy)

		draw.line([(x0, y0), (x1, y1)], fill=(255, 0, 0), width=1)

	img.save(filename)

	return f"Saved {w}x{h} curve debug -> {filename}"

def save_curves_svg(
	curves,
	shape,
	filename="curves_debug.svg",
	scale: int=1024,
	margin: float=0.0,
	flip_y: bool=True
):
	# --- Size + transform ---
	w, h = compute_render_size(shape, scale)
	xf = EmTransform(shape, w, h, margin)

	ox, oy = shape.em_origin
	sx, sy = shape.em_size

	def em_to_px(ex, ey):
		px, py = xf.em_to_px(ex, ey)

		if flip_y:
			py = h - 1 - py

		return px, py

	# --- SVG builder ---
	elements = []

	def line(x1, y1, x2, y2, color, width=1):
		elements.append(
			f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" '
			f'stroke="{color}" stroke-width="{width}" />'
		)

	def circle(cx, cy, r, fill=None, stroke=None):
		parts = [f'cx="{cx}"', f'cy="{cy}"', f'r="{r}"']

		if fill:
			parts.append(f'fill="{fill}"')

		else:
			parts.append('fill="none"')

		if stroke:
			parts.append(f'stroke="{stroke}"')

		elements.append(f'<circle {" ".join(parts)} />')

	def quad_path(p1, p2, p3, color):
		x1, y1 = em_to_px(*p1)
		x2, y2 = em_to_px(*p2)
		x3, y3 = em_to_px(*p3)

		elements.append(
			f'<path d="M {x1},{y1} Q {x2},{y2} {x3},{y3}" '
			f'stroke="{color}" fill="none" stroke-width="1" />'
		)

	# -------------------------------------------------------------------------
	# Curves
	# -------------------------------------------------------------------------
	for c in curves:
		x1, y1, x2, y2, x3, y3 = c
		p1 = (x1, y1)
		p2 = (x2, y2)
		p3 = (x3, y3)

		# curve
		quad_path(p1, p2, p3, "#CCCCCC")

		# control lines
		px1, py1 = em_to_px(*p1)
		px2, py2 = em_to_px(*p2)
		px3, py3 = em_to_px(*p3)

		line(px1, py1, px2, py2, "#5050B4")
		line(px2, py2, px3, py3, "#5050B4")

		# endpoints
		circle(px1, py1, 2, fill="#50DC50")
		circle(px3, py3, 2, fill="#50DC50")

		# control point
		circle(px2, py2, 3, stroke="#6464FF")

	# -------------------------------------------------------------------------
	# Band overlay
	# -------------------------------------------------------------------------
	for i in range(shape.band_max_y + 1):
		y = (i - shape.band_offset_y) / shape.band_scale_y
		x0, y0 = em_to_px(ox, y)
		x1, y1 = em_to_px(ox + sx, y)
		line(x0, y0, x1, y1, "#FF0000", width=1)

	for i in range(shape.band_max_x + 1):
		x = (i - shape.band_offset_x) / shape.band_scale_x
		x0, y0 = em_to_px(x, oy)
		x1, y1 = em_to_px(x, oy + sy)
		line(x0, y0, x1, y1, "#FF0000", width=1)

	# -------------------------------------------------------------------------
	# Write SVG
	# -------------------------------------------------------------------------
	svg = " ".join((
		'<svg xmlns="http://www.w3.org/2000/svg"',
		f'width="{w}" height="{h}"',
		f'viewBox="0 0 {w} {h}">\n'
	))

	# TODO: Make this background fill OPTIONAL!
	svg += '\t<rect width="100%" height="100%" fill="#1E1E1E"/>\n'
	svg += '\n'.join(f"\t{e}" for e in elements)
	svg += "\n</svg>"

	with open(filename, "w") as f:
		f.write(svg)

	print(f"Saved SVG debug -> {filename}")

# =============================================================================
# Self-test
# =============================================================================

# TODO: DOES NOT WORK (currently)! You'll need to use LEGIT data via `slughorn_export.py` (or write
# your own wrapper, which isn't too terribly hard as long as you understand how to use `AtlasView`
# properly...)
if __name__ == "__main__":
	print("Test 1: reference renderer (pure Python)")

	curves = [
		Curve(0.0, 0.0, 0.5, 0.35, 1.0, 0.0),
		Curve(1.0, 0.0, 0.75, 0.35, 0.5, 0.7),
		Curve(0.5, 0.7, 0.25, 0.35, 0.0, 0.0),
	]

	# Minimal fake shape for pure-Python testing.
	class _ShapeStub:
		width = 1.0
		height = 0.7
		em_origin = (0.0, 0.0)
		em_size = (1.0, 0.7)

	shape = _ShapeStub()

	grid = sample_grid(curves, shape, size=128)

	save_image(grid, "grid_reference.png")

	save_curves([c.to_tuple() for c in curves], shape, "grid_reference_curves.png")

	print("Test 2: Atlas round-trip (requires compiled slughorn module)")

	try:
		import slughorn

		atlas = slughorn.Atlas()
		info = slughorn.ShapeInfo()

		info.num_bands_x = 2
		info.num_bands_y = 2
		info.curves = [
			slughorn.Curve(0.0, 0.0, 0.5, 0.35, 1.0, 0.0),
			slughorn.Curve(1.0, 0.0, 0.75, 0.35, 0.5, 0.7),
			slughorn.Curve(0.5, 0.7, 0.25, 0.35, 0.0, 0.0),
		]

		key = slughorn.Key(ord('F'))

		atlas.add_shape(key, info)
		atlas.build()

		grid_banded = sample_grid_from_atlas(atlas, key, size=128, banded=True)

		save_image(grid_banded, "grid_banded.png")

		grid_ref = sample_grid_from_atlas(atlas, key, size=128, banded=False)

		save_image(grid_ref, "grid_atlas_ref.png")

		diffs = [
			abs(grid_banded[y][x] - grid_ref[y][x])
			for y in range(len(grid_banded))
			for x in range(len(grid_banded[0]))
		]

		print(f"  Max banded/reference diff: {max(diffs):.6f}")
		print(f"  Mean diff:                 {sum(diffs)/len(diffs):.6f}")

	except ImportError:
		print("  slughorn module not available - skipping atlas test")
