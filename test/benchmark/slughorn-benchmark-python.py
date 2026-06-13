#!/usr/bin/env python3
"""
Benchmark: serial vs parallel render_grid

Measures wall-clock time for renderGrid() at varying pixel sizes and shape
complexities. Run after building with -DSLUGHORN_RENDER_PARALLEL=ON to see
actual speedup; without it, parallel=True silently falls back to serial.

Usage:
    python3 test/python/bench_render.py          # auto-finds BUILD/
    python3 test/python/bench_render.py /path/to/BUILD
"""

import sys
import math
import time
from pathlib import Path

BUILD = (
    Path(sys.argv[1])
    if len(sys.argv) > 1
    else Path(__file__).resolve().parents[2] / "BUILD"
)
sys.path.insert(0, str(BUILD))

import slughorn

REPS = 10
SIZES = [64, 128, 256, 512]


def timed(sampler, size, parallel, reps=REPS):
    sampler.render_grid(size, 0.0, True, parallel)  # warm-up
    t0 = time.perf_counter()
    for _ in range(reps):
        sampler.render_grid(size, 0.0, True, parallel)
    return (time.perf_counter() - t0) / reps * 1000.0  # ms


def build_rect_atlas():
    """4 curves — minimal, overhead-dominated."""
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
    return atlas, "rect"


def build_star_atlas(points=32):
    """N*2 edges — realistic rendering workload."""
    atlas = slughorn.Atlas()
    d = slughorn.CurveDecomposer()
    n = points * 2
    for i in range(n):
        angle = 2.0 * math.pi * i / n - math.pi / 2.0
        r = 0.45 if i % 2 == 0 else 0.18
        x = 0.5 + r * math.cos(angle)
        y = 0.5 + r * math.sin(angle)
        if i == 0:
            d.move_to(x, y)
        else:
            d.line_to(x, y)
    d.close()
    info = slughorn.ShapeInfo()
    info.curves = d.get_curves()
    atlas.add_shape(slughorn.Key("star"), info)
    atlas.build()
    return atlas, "star"


def detect_parallel(sampler):
    try:
        g_ser = sampler.render_grid(16, 0.0, True, False)
        g_par = sampler.render_grid(16, 0.0, True, True)
        ser = list(memoryview(g_ser).cast("B").cast("f"))
        par = list(memoryview(g_par).cast("B").cast("f"))
        identical = all(abs(a - b) < 1e-5 for a, b in zip(ser, par))
        return True, identical
    except Exception as e:
        return False, str(e)


def main():
    cases = [
        ("rect  (4 curves)",        build_rect_atlas),
        (f"star  ({32*2} curves)",  build_star_atlas),
    ]

    # Detect parallel support using the rect sampler
    probe_atlas, probe_key = build_rect_atlas()
    probe_sampler = probe_atlas.decode(probe_key)
    has_parallel, output_matches = detect_parallel(probe_sampler)

    print("slughorn render_grid benchmark")
    print(f"  Build:    {BUILD}")
    print(f"  Parallel: {'YES (SLUGHORN_HAS_PARALLEL)' if has_parallel else 'NO — rebuild with -DSLUGHORN_RENDER_PARALLEL=ON'}")
    if has_parallel:
        print(f"  Output match (serial == parallel): {output_matches}")
    print(f"  Reps per measurement: {REPS}  |  banded=True")
    print()

    col_w = 24
    print(f"{'shape':<{col_w}} {'px':>5}  {'serial ms':>10}  {'parallel ms':>12}  {'speedup':>8}")
    print("-" * (col_w + 46))

    for label, builder in cases:
        atlas, key = builder()
        sampler = atlas.decode(key)

        for size in SIZES:
            serial_ms = timed(sampler, size, False)

            if has_parallel:
                par_ms = timed(sampler, size, True)
                speedup = f"{serial_ms / par_ms:.2f}x"
                par_col = f"{par_ms:>12.2f}"
            else:
                par_col = f"{'N/A':>12}"
                speedup = "N/A"

            print(f"{label:<{col_w}} {size:>5}  {serial_ms:>10.2f}  {par_col}  {speedup:>8}")

        print()


if __name__ == "__main__":
    main()
