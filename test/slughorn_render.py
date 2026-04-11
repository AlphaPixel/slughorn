#!/usr/bin/env python3
# slughorn_render.py
#
# Software emulator for the Slug rendering algorithm (Lengyel 2017).
#
# Three levels of abstraction, each building on the one below:
#
#   Level 1 — Pure math  (no slughorn dependency)
#     calc_root_code, solve_horiz_poly, solve_vert_poly, calc_coverage
#     render_sample          — ground truth, iterates all curves, no bands
#     render_sample_banded   — band-accelerated, mirrors GPU shader exactly
#
#   Level 2 — Atlas bridge  (requires compiled slughorn module)
#     AtlasView              — decodes a built Atlas into Python-native
#                              structures that render_sample_banded can consume.
#                              Constructed once per (atlas, key) pair.
#
#   Level 3 — Grid samplers + image output
#     sample_grid            — reference path (pure Python Curve list)
#     sample_grid_from_atlas — banded path (AtlasView)
#     save_image, print_grid

import math
import struct
import numpy as np

from dataclasses import dataclass, field
from typing import List, Tuple, Optional
from PIL import Image

# =============================================================================
# Constants
# =============================================================================

EPS = 1.0 / 65536.0  # must match shader

# Band texture width (must match Atlas::TEX_WIDTH and kLogBandTextureWidth)
BAND_TEX_WIDTH     = 512
LOG_BAND_TEX_WIDTH = 9   # 2^9 == 512

# =============================================================================
# Level 1 — Pure math
# =============================================================================

# -----------------------------------------------------------------------------
# Curve (pure-Python version — used by render_sample / sample_grid)
#
# When working with a compiled Atlas, you can convert a slughorn.Curve to this
# with:  Curve(*c.to_tuple())   or just build AtlasView which does it for you.
# -----------------------------------------------------------------------------

@dataclass
class Curve:
    x1: float; y1: float
    x2: float; y2: float
    x3: float; y3: float

    def to_tuple(self) -> Tuple[float, ...]:
        return (self.x1, self.y1, self.x2, self.y2, self.x3, self.y3)

    @staticmethod
    def from_slughorn(c) -> "Curve":
        """Convert a slughorn.Curve (C++ binding) to a pure-Python Curve."""
        return Curve(c.x1, c.y1, c.x2, c.y2, c.x3, c.y3)


# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def float_bits_to_uint32(x: float) -> int:
    return struct.unpack(">I", struct.pack(">f", x))[0]


def clamp(x: float, lo: float, hi: float) -> float:
    return lo if x < lo else hi if x > hi else x


# -----------------------------------------------------------------------------
# slug_CalcRootCode — exact port of the GLSL function
#
# Takes the Y (or X) coordinates of the three control points of a quadratic
# Bézier, all shifted so that the sample point is at the origin.  Returns a
# two-bit code that tells the caller which roots (if any) of the equation
# B(t) = 0 are in [0, 1]:
#
#   bit 0 set  →  root r1 is relevant (positive winding contribution)
#   bit 1 set  →  root r2 is relevant (negative winding contribution)
#   0          →  no roots cross the scan line; skip this curve entirely
#
# The magic constant 0x2E74 is a 16-entry lookup table packed into a uint16.
# Each 2-bit entry encodes the root-code for one sign pattern of (y1, y2, y3).
# -----------------------------------------------------------------------------

def calc_root_code(y1: float, y2: float, y3: float) -> int:
    i1 = float_bits_to_uint32(y1) >> 31        # sign bit of y1
    i2 = float_bits_to_uint32(y2) >> 30        # sign bit of y2, shifted to bit 1
    i3 = float_bits_to_uint32(y3) >> 29        # sign bit of y3, shifted to bit 2

    shift = (i2 & 0x2) | (i1 & ~0x2)
    shift = (i3 & 0x4) | (shift & ~0x4)

    return (0x2E74 >> shift) & 0x0101


# -----------------------------------------------------------------------------
# solve_horiz_poly — find the X intercept(s) of a quadratic Bézier at Y=0
#
# Called after the sample point has been shifted to the origin, so "Y=0" is
# literally "the horizontal scan line through the current fragment".
#
# Returns (x1, x2) — the X positions of the two roots.  When there is only
# one root (the near-linear degenerate case), both values are identical.
# The caller uses calc_root_code to decide which of the two to actually use.
# -----------------------------------------------------------------------------

def solve_horiz_poly(
    p1: Tuple[float, float],
    p2: Tuple[float, float],
    p3: Tuple[float, float],
) -> Tuple[float, float]:
    ax = p1[0] - 2.0 * p2[0] + p3[0]   # quadratic coefficient in X
    ay = p1[1] - 2.0 * p2[1] + p3[1]   # quadratic coefficient in Y
    bx = p1[0] - p2[0]                  # linear coefficient in X
    by = p1[1] - p2[1]                  # linear coefficient in Y

    if abs(ay) < EPS:
        # Degenerate: curve is nearly linear in Y → single root
        t = p1[1] * (0.5 / by) if abs(by) >= EPS else 0.0
        x = (ax * t - 2.0 * bx) * t + p1[0]
        return x, x

    d  = math.sqrt(max(by * by - ay * p1[1], 0.0))
    t1 = (by - d) / ay
    t2 = (by + d) / ay

    x1 = (ax * t1 - 2.0 * bx) * t1 + p1[0]
    x2 = (ax * t2 - 2.0 * bx) * t2 + p1[0]

    return x1, x2


# -----------------------------------------------------------------------------
# solve_vert_poly — find the Y intercept(s) of a quadratic Bézier at X=0
#
# Symmetric to solve_horiz_poly; roles of X and Y are swapped.
# -----------------------------------------------------------------------------

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

    d  = math.sqrt(max(bx * bx - ax * p1[0], 0.0))
    t1 = (bx - d) / ax
    t2 = (bx + d) / ax

    y1 = (ay * t1 - 2.0 * by) * t1 + p1[1]
    y2 = (ay * t2 - 2.0 * by) * t2 + p1[1]

    return y1, y2


# -----------------------------------------------------------------------------
# calc_coverage — combine horizontal and vertical winding accumulators
#
# xcov/ycov are signed coverage sums from the horizontal/vertical passes.
# xwgt/ywgt are confidence weights (how close the nearest crossing was to the
# scan line, in pixel units).
#
# The blend of "weighted average" and "conservative minimum" is the key insight
# in Lengyel's algorithm: when both passes agree (both weights high) you get a
# clean average; when one pass is unreliable (weight near 0, e.g. a near-
# horizontal edge on the horizontal pass) the other dominates.
# -----------------------------------------------------------------------------

def calc_coverage(
    xcov: float, ycov: float,
    xwgt: float, ywgt: float,
) -> float:
    weighted     = abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, EPS)
    conservative = min(abs(xcov), abs(ycov))
    return clamp(max(weighted, conservative), 0.0, 1.0)


# -----------------------------------------------------------------------------
# render_sample — ground-truth renderer, NO bands, NO sorting, NO early-exit
#
# Parameters
# ----------
# curves        : list of Curve (pure-Python)
# render_coord  : (x, y) in em-space (same space as the curve coordinates)
# pixels_per_em : how many output pixels span one em unit — controls the
#                 antialiasing kernel width.  Pass (size, size) for a shape
#                 that fills a size×size pixel image.
#
# Returns a dict with fill + all intermediate accumulators (useful for
# debugging individual samples).
#
# This is the reference to validate render_sample_banded against.
# -----------------------------------------------------------------------------

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

        # Shift so the sample point is at the origin (CRITICAL to the algorithm)
        p1 = (c.x1 - rx, c.y1 - ry)
        p2 = (c.x2 - rx, c.y2 - ry)
        p3 = (c.x3 - rx, c.y3 - ry)

        # --- Horizontal pass (solve Y=0, accumulate X coverage) ---
        code = calc_root_code(p1[1], p2[1], p3[1])

        if code:
            r1, r2 = solve_horiz_poly(p1, p2, p3)
            r1 *= ppe_x
            r2 *= ppe_x

            if code & 0x01:
                xcov += clamp(r1 + 0.5, 0.0, 1.0)
                xwgt  = max(xwgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

            if code & 0x100:
                xcov -= clamp(r2 + 0.5, 0.0, 1.0)
                xwgt  = max(xwgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

        # --- Vertical pass (solve X=0, accumulate Y coverage) ---
        code = calc_root_code(p1[0], p2[0], p3[0])

        if code:
            r1, r2 = solve_vert_poly(p1, p2, p3)
            r1 *= ppe_y
            r2 *= ppe_y

            if code & 0x01:
                ycov -= clamp(r1 + 0.5, 0.0, 1.0)
                ywgt  = max(ywgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

            if code & 0x100:
                ycov += clamp(r2 + 0.5, 0.0, 1.0)
                ywgt  = max(ywgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

    return {
        "fill":  calc_coverage(xcov, ycov, xwgt, ywgt),
        "xcov":  xcov, "ycov":  ycov,
        "xwgt":  xwgt, "ywgt":  ywgt,
        "iters": iters,
    }


# NOTE on the `code` bit test above
# ----------------------------------
# calc_root_code returns values from the table (0x2E74 >> shift) & 0x0101.
# The possible non-zero results are:
#
#   0x0001  →  only root r1 is relevant
#   0x0100  →  only root r2 is relevant
#   0x0101  →  both roots are relevant
#
# The original code used `code & 1` / `code > 1` which works numerically
# but obscures the intent.  `code & 0x01` / `code & 0x100` is explicit.


# -----------------------------------------------------------------------------
# render_sample_banded — band-accelerated renderer
#
# Mirrors the GPU shader exactly, including:
#   - Band index computation (same formula as the GLSL)
#   - Early-exit on sorted curve lists (curves sorted by decreasing max-X/Y,
#     so once a curve is entirely to the left/below the sample it can't
#     contribute — everything after it in the list is even further away)
#
# Parameters
# ----------
# curves        : flat list of (x1,y1, x2,y2, x3,y3) tuples, indexed by the
#                 band lists.  Build this from AtlasView.curves.
# hbands_idx    : list-of-lists; hbands_idx[band_y] → curve indices for the
#                 horizontal pass of that Y band.
# vbands_idx    : list-of-lists; vbands_idx[band_x] → curve indices for the
#                 vertical pass of that X band.
# render_coord  : (x, y) in em-space
# pixels_per_em : (ppe_x, ppe_y) — same convention as render_sample
# band_*        : band transform parameters taken directly from slughorn.Shape
# -----------------------------------------------------------------------------

def render_sample_banded(
    curves:       List[Tuple],
    hbands_idx:   List[List[int]],
    vbands_idx:   List[List[int]],
    render_coord: Tuple[float, float],
    pixels_per_em: Tuple[float, float],
    band_scale_x:  float,
    band_scale_y:  float,
    band_offset_x: float,
    band_offset_y: float,
    band_max_x:    int,
    band_max_y:    int,
) -> dict:
    rx, ry   = render_coord
    ppe_x, ppe_y = pixels_per_em

    # Band index — same formula as the GLSL vertex/fragment shader
    band_x = int(clamp(rx * band_scale_x + band_offset_x, 0, band_max_x))
    band_y = int(clamp(ry * band_scale_y + band_offset_y, 0, band_max_y))

    xcov = xwgt = ycov = ywgt = 0.0
    iters = 0

    # --- Horizontal pass (hband → Y roots → X coverage) ---
    for ci in hbands_idx[band_y]:
        iters += 1
        c = curves[ci]

        p1 = (c[0] - rx, c[1] - ry)
        p2 = (c[2] - rx, c[3] - ry)
        p3 = (c[4] - rx, c[5] - ry)

        # Early-exit: curves are sorted by decreasing max-X.
        # Once max-X of a curve (in pixel units, relative to sample) is < -0.5,
        # this curve and all subsequent ones are entirely to the left and cannot
        # cross the Y=0 scan line in the region that matters.
        if max(p1[0], p2[0], p3[0]) * ppe_x < -0.5:
            break

        code = calc_root_code(p1[1], p2[1], p3[1])

        if code:
            r1, r2 = solve_horiz_poly(p1, p2, p3)
            r1 *= ppe_x
            r2 *= ppe_x

            if code & 0x01:
                xcov += clamp(r1 + 0.5, 0.0, 1.0)
                xwgt  = max(xwgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

            if code & 0x100:
                xcov -= clamp(r2 + 0.5, 0.0, 1.0)
                xwgt  = max(xwgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

    # --- Vertical pass (vband → X roots → Y coverage) ---
    for ci in vbands_idx[band_x]:
        iters += 1
        c = curves[ci]

        p1 = (c[0] - rx, c[1] - ry)
        p2 = (c[2] - rx, c[3] - ry)
        p3 = (c[4] - rx, c[5] - ry)

        # Early-exit: curves sorted by decreasing max-Y.
        if max(p1[1], p2[1], p3[1]) * ppe_y < -0.5:
            break

        code = calc_root_code(p1[0], p2[0], p3[0])

        if code:
            r1, r2 = solve_vert_poly(p1, p2, p3)
            r1 *= ppe_y
            r2 *= ppe_y

            if code & 0x01:
                ycov -= clamp(r1 + 0.5, 0.0, 1.0)
                ywgt  = max(ywgt, clamp(1.0 - abs(r1) * 2.0, 0.0, 1.0))

            if code & 0x100:
                ycov += clamp(r2 + 0.5, 0.0, 1.0)
                ywgt  = max(ywgt, clamp(1.0 - abs(r2) * 2.0, 0.0, 1.0))

    return {
        "fill":  calc_coverage(xcov, ycov, xwgt, ywgt),
        "xcov":  xcov, "ycov":  ycov,
        "xwgt":  xwgt, "ywgt":  ywgt,
        "iters": iters,
    }


# =============================================================================
# Level 2 — Atlas bridge
# =============================================================================

class AtlasView:
    """
    Decodes a built slughorn.Atlas for a single shape key into Python-native
    structures that render_sample_banded can consume directly.

    Construct once per (atlas, key) pair — the decode is done in __init__
    and the results are cached as plain Python lists.

    Attributes
    ----------
    shape       : the slughorn.Shape object (band transform, metrics, etc.)
    curves      : flat list of (x1,y1, x2,y2, x3,y3) float tuples, one per
                  curve in the atlas.  Indexed by the band index lists.
    hbands_idx  : list-of-lists; hbands_idx[band_y] → list of curve indices
    vbands_idx  : list-of-lists; vbands_idx[band_x] → list of curve indices
    curve_list  : same curves as pure-Python Curve objects (for render_sample)

    Usage
    -----
        view = AtlasView(atlas, slughorn.Key(ord('F')))
        fill = view.render(0.5, 0.3, pixels_per_em=(128, 128))
    """

    def __init__(self, atlas, key):
        self.shape = atlas.get_shape(key)

        if self.shape is None:
            raise KeyError(f"Key {key!r} not found in atlas (or atlas not built yet)")

        self.curves     = self._decode_curve_texture(atlas.curve_texture)
        self.curve_list = [Curve(*c) for c in self.curves]

        self.hbands_idx, self.vbands_idx = self._decode_band_texture(
            atlas.band_texture, self.shape
        )

    # -------------------------------------------------------------------------
    # Convenience render methods
    # -------------------------------------------------------------------------

    def render(
        self,
        em_x: float,
        em_y: float,
        pixels_per_em: Tuple[float, float],
        banded: bool = True,
    ) -> dict:
        """
        Render a single sample at em-space coordinate (em_x, em_y).

        pixels_per_em controls the antialiasing kernel width — pass
        (image_width, image_height) when the shape fills the whole image,
        or scale accordingly.

        Set banded=False to use the reference (brute-force) renderer instead,
        which is useful for validating that the banded path gives the same result.
        """
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

    # -------------------------------------------------------------------------
    # Decode helpers (private)
    # -------------------------------------------------------------------------

    @staticmethod
    def _decode_curve_texture(tex) -> List[Tuple]:
        """
        Decode the RGBA32F curve texture into a flat list of curve tuples.

        The atlas packs each curve into two consecutive texels (always aligned
        to an even X position within the texture row):
            texel N+0: (x1, y1, x2, y2)
            texel N+1: (x3, y3, _, _)

        Returns a list indexed 0..N-1 where index i corresponds to the i-th
        curve in packing order.  The band index lists store indices into this
        list.
        """
        data = np.frombuffer(tex.bytes, dtype=np.float32).reshape(
            (tex.height, tex.width, 4)
        )

        curves = []

        for y in range(tex.height):
            for x in range(0, tex.width, 2):  # step by 2: each curve = 2 texels
                t0 = data[y, x]
                if x + 1 >= tex.width:
                    continue

                t1 = data[y, x + 1]

                # Skip empty slots (both texels all-zero)
                if np.allclose(t0, 0.0) and np.allclose(t1, 0.0):
                    continue

                x1, y1, x2, y2 = t0
                x3, y3, _,  _  = t1

                curves.append((float(x1), float(y1),
                                float(x2), float(y2),
                                float(x3), float(y3)))

        return curves

    @staticmethod
    def _decode_band_texture(tex, shape) -> Tuple[List[List[int]], List[List[int]]]:
        """
        Decode the RGBA16UI band texture for one shape into two lists of
        curve-index lists.

        Band texture layout (per shape):
            [ header_0 | header_1 | ... | header_N-1 | ... curve index lists ... ]

        Each header texel: (count, offset, 0, 0)
            count  — number of curves in this band's list
            offset — texel offset from the shape's band_tex start to the list

        Texel addressing wraps within the texture the same way the GLSL shader
        does it:
            abs_idx = start + relative_offset
            y = abs_idx >> LOG_BAND_TEX_WIDTH   (i.e. abs_idx // TEX_WIDTH)
            x = abs_idx &  (TEX_WIDTH - 1)      (i.e. abs_idx %  TEX_WIDTH)

        Headers 0 .. num_h-1 are the horizontal bands (indexed by band_y).
        Headers num_h .. num_h+num_v-1 are the vertical bands (indexed by band_x).

        Returns (hbands_idx, vbands_idx) where each is a list-of-lists of ints.
        The ints are indices into the curve list returned by _decode_curve_texture.
        They are NOT (cx, cy) texel coordinates — the conversion to curve indices
        happens here so that render_sample_banded never has to think about textures.
        """
        data = np.frombuffer(tex.bytes, dtype=np.uint16).reshape(
            (tex.height, tex.width, 4)
        )

        start = shape.band_tex_y * BAND_TEX_WIDTH + shape.band_tex_x
        num_h = shape.band_max_y + 1
        num_v = shape.band_max_x + 1
        num_headers = num_h + num_v

        # -----------------------------------------------------------------
        # Build curve-location → index lookup
        # Each curve occupies two texels; its "address" is the texel index
        # of its first (even-X) texel in the CURVE texture.
        # We store the address as (cx, cy) so it matches what the band
        # texture records.
        # -----------------------------------------------------------------
        # We don't have the curve texture here, but we can build the lookup
        # from the curve count we already decoded: curve i lives at texel
        # index i*2 (they are densely packed at even offsets, two per curve).
        # That gives us (cx, cy) = (i*2 % TEX_WIDTH, i*2 // TEX_WIDTH).
        # The inverse map is: index = (cy * TEX_WIDTH + cx) // 2
        def loc_to_index(cx: int, cy: int) -> int:
            return (cy * BAND_TEX_WIDTH + cx) // 2

        def read_texel(relative_offset: int) -> np.ndarray:
            abs_idx = start + relative_offset
            ty = abs_idx >> LOG_BAND_TEX_WIDTH
            tx = abs_idx &  (BAND_TEX_WIDTH - 1)
            return data[ty, tx]

        # -----------------------------------------------------------------
        # Read all headers
        # -----------------------------------------------------------------
        headers = []

        for i in range(num_headers):
            texel  = read_texel(i)
            count  = int(texel[0])
            offset = int(texel[1])
            headers.append((count, offset))

        # -----------------------------------------------------------------
        # Decode band lists into curve indices
        # -----------------------------------------------------------------
        def decode_band(count: int, offset: int) -> List[int]:
            result = []
            for i in range(count):
                texel = read_texel(offset + i)
                cx    = int(texel[0])
                cy    = int(texel[1])
                result.append(loc_to_index(cx, cy))
            return result

        hbands_idx = [decode_band(*headers[i])           for i in range(num_h)]
        vbands_idx = [decode_band(*headers[num_h + i])   for i in range(num_v)]

        return hbands_idx, vbands_idx


# =============================================================================
# Level 3 — Grid samplers + image I/O
# =============================================================================

def _sample_coords(size: int, margin: float = 0.1):
    """Generate (px, py) em-space sample coordinates for a size×size grid."""
    scale = 1.0 - 2.0 * margin
    for y in range(size):
        for x in range(size):
            yield (
                x,
                y,
                margin + scale * (x / (size - 1)),
                margin + scale * (y / (size - 1)),
            )


def sample_grid(curves: List[Curve], size: int = 40, margin: float = 0.1) -> List[List[float]]:
    """
    Render a size×size grid using the reference (brute-force) renderer.

    curves        — pure-Python Curve list
    size          — output resolution in pixels
    margin        — fraction of the em-square to leave as padding on each side

    Returns a 2-D list [y][x] of fill values in [0, 1].
    """
    ppe = (float(size), float(size))

    grid = [[0.0] * size for _ in range(size)]

    for x, y, px, py in _sample_coords(size, margin):
        grid[y][x] = render_sample(curves, (px, py), ppe)["fill"]

    return grid


def sample_grid_from_atlas(
    atlas,
    key,
    size:   int   = 128,
    margin: float = 0.1,
    banded: bool  = True,
) -> List[List[float]]:
    """
    Render a size×size grid from a built slughorn.Atlas.

    atlas  — a built slughorn.Atlas (or any object with .get_shape() /
              .curve_texture / .band_texture)
    key    — slughorn.Key (or uint32_t codepoint — implicit conversion works)
    size   — output resolution in pixels
    margin — fraction of the em-square to leave as padding on each side
    banded — if True (default) use render_sample_banded;
              if False use render_sample for ground-truth comparison

    Returns a 2-D list [y][x] of fill values in [0, 1].
    Raises KeyError if the key is not found in the atlas.
    """
    view = AtlasView(atlas, key)
    ppe  = (float(size), float(size))

    grid = [[0.0] * size for _ in range(size)]

    for x, y, px, py in _sample_coords(size, margin):
        grid[y][x] = view.render(px, py, ppe, banded=banded)["fill"]

    return grid


# -----------------------------------------------------------------------------
# Debug output
# -----------------------------------------------------------------------------

def print_grid(grid: List[List[float]]) -> None:
    chars = " .:-=+*#%@"
    for row in grid:
        print("".join(chars[int(v * (len(chars) - 1))] for v in row))


def save_image(grid: List[List[float]], filename: str = "out.png", flip_y: bool = True) -> None:
    if flip_y:
        grid = list(reversed(grid))
    h, w = len(grid), len(grid[0])
    img  = Image.new("L", (w, h))
    px   = img.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = int(clamp(grid[y][x], 0.0, 1.0) * 255)
    img.save(filename)
    print(f"Saved {w}x{h} image → {filename}")


# =============================================================================
# Self-test
# =============================================================================

if __name__ == "__main__":
    # -------------------------------------------------------------------------
    # Test 1: reference renderer — pure Python, no slughorn import needed
    # -------------------------------------------------------------------------
    print("Test 1: reference renderer (pure Python)")

    curves = [
        Curve(0.0, 0.0, 0.5, 0.35, 1.0, 0.0),
        Curve(1.0, 0.0, 0.75, 0.35, 0.5, 0.7),
        Curve(0.5, 0.7, 0.25, 0.35, 0.0, 0.0),
    ]

    grid = sample_grid(curves, size=128)
    save_image(grid, "grid_reference.png")

    # -------------------------------------------------------------------------
    # Test 2: Atlas round-trip — build via slughorn, render via AtlasView
    # -------------------------------------------------------------------------
    print("Test 2: Atlas round-trip (requires compiled slughorn module)")

    try:
        import slughorn

        atlas = slughorn.Atlas()
        info  = slughorn.ShapeInfo()

        info.num_bands = 2
        info.curves = [
            slughorn.Curve(0.0, 0.0, 0.5, 0.35, 1.0, 0.0),
            slughorn.Curve(1.0, 0.0, 0.75, 0.35, 0.5, 0.7),
            slughorn.Curve(0.5, 0.7, 0.25, 0.35, 0.0, 0.0),
        ]

        key = slughorn.Key(ord('F'))

        atlas.add_shape(key, info)
        atlas.build()

        # Banded render
        grid_banded = sample_grid_from_atlas(atlas, key, size=128, banded=True)
        save_image(grid_banded, "grid_banded.png")

        # Reference render via AtlasView (for diff comparison)
        grid_ref = sample_grid_from_atlas(atlas, key, size=128, banded=False)
        save_image(grid_ref, "grid_atlas_ref.png")

        # Quick sanity: max per-pixel difference between banded and reference
        diffs = [
            abs(grid_banded[y][x] - grid_ref[y][x])
            for y in range(128) for x in range(128)
        ]
        print(f"  Max banded/reference diff: {max(diffs):.6f}")
        print(f"  Mean diff:                 {sum(diffs)/len(diffs):.6f}")

    except ImportError:
        print("  slughorn module not available — skipping atlas test")
