#!/usr/bin/env python3
#
# Clean end-to-end example using:
# - slughorn (pybind11 bindings)
# - slughorn_render.py (unified em-space renderer)
#
# Usage:
# python slughorn_export.py <file.slug> [key] [output]
# python slughorn_export.py --selftest

import sys
import os
import contextlib
import time

try:
	import slughorn

except ImportError:
	print("ERROR: slughorn module not found.")

	sys.exit(1)

from slughorn_render import (
	AtlasView,
	Curve,
	sample_grid,
	sample_grid_from_atlas,
	save_curves,
	save_curves_svg,
	save_image,
)

# -----------------------------------------------------------------------------
# Helpers
# -----------------------------------------------------------------------------

def make_key(raw: str) -> slughorn.Key:
	try:
		return slughorn.Key(int(raw, 0))

	except ValueError:
		return slughorn.Key.from_string(raw)

@contextlib.contextmanager
def timer(label: str):
	print(f"{label}... ", end="", flush=True)

	start = time.perf_counter()

	yield

	print(f"{time.perf_counter() - start:.3f}s")

# -----------------------------------------------------------------------------
# Self-test (no .slug required)
# -----------------------------------------------------------------------------

# def run_selftest():
# 	print("=== Self-test ===")
#
# 	atlas = slughorn.Atlas()
# 	info = slughorn.ShapeInfo()
#
# 	info.num_bands_x = 2
# 	info.num_bands_y = 2
# 	info.curves = [
# 		slughorn.Curve(0.0, 0.0, 0.5, 0.35, 1.0, 0.0),
# 		slughorn.Curve(1.0, 0.0, 0.75, 0.35, 0.5, 0.7),
# 		slughorn.Curve(0.5, 0.7, 0.25, 0.35, 0.0, 0.0),
# 	]
#
# 	key = slughorn.Key(ord('F'))
#
# 	atlas.add_shape(key, info)
# 	atlas.build()
#
# 	shape = atlas.get_shape(key)
#
# 	print("Shape:", shape)
# 	print("em_origin:", shape.em_origin)
# 	print("em_size:", shape.em_size)
#
# 	view = AtlasView(atlas, key)
#
# 	# Debug geometry
# 	save_curves(view.curves, shape, "selftest_curves.png")
#
# 	# Banded
# 	grid_banded = sample_grid_from_atlas(atlas, key, size=128, banded=True)
# 	save_image(grid_banded, "selftest_banded.png")
#
# 	# Reference
# 	grid_ref = sample_grid_from_atlas(atlas, key, size=128, banded=False)
# 	save_image(grid_ref, "selftest_ref.png")
#
# 	# Diff
# 	diffs = [
# 		abs(grid_banded[y][x] - grid_ref[y][x])
# 		for y in range(len(grid_banded))
# 		for x in range(len(grid_banded[0]))
# 	]
#
# 	print(f"Max diff: {max(diffs):.6f}")
# 	print(f"Mean diff: {sum(diffs)/len(diffs):.6f}")
#
# 	print("Done.")

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

def main():
	if "--selftest" in sys.argv:
		run_selftest()

		return

	if len(sys.argv) < 2:
		print("Usage: slughorn_export.py <file.slug> [key] [output.png]")

		sys.exit(1)

	slug_path = sys.argv[1]
	key_arg = sys.argv[2] if len(sys.argv) > 2 else "axolotl"
	out_path = sys.argv[3] if len(sys.argv) > 3 else "out"

	if not hasattr(slughorn, "read"):
		print("ERROR: slughorn.read() not available (build with SERIAL).")

		sys.exit(2)

	print(f"Loading {slug_path}...")

	atlas = slughorn.read(slug_path)
	key = make_key(key_arg)
	shape = atlas.get_shape(key)

	if shape is None:
		print(f"Key {key!r} not found.")
		print("Available:")

		for k in atlas.get_shapes().keys():
			print(" ", k)

		sys.exit(1)

	print("\nShape:")
	print(" ", shape)
	print("  em_origin:", shape.em_origin)
	print("  em_size:  ", shape.em_size)
	print("")

	view = AtlasView(atlas, key)

	# Debug geometry
	save_curves_svg(view.curves, shape, out_path + ".svg")
	# save_curves(view.curves, shape, out_path + "_lines.png")

	msg = ""

	# Banded render
	with timer("\nRendering banded"):
		grid_banded = sample_grid_from_atlas(atlas, key, size=256, banded=True)

		msg = save_image(grid_banded, out_path + ".png")

	print(f"{msg}\n")

	sys.exit(0)

	# Reference render
	with timer("Rendering reference"):
		grid_ref = sample_grid_from_atlas(atlas, key, size=256, banded=False)

		msg = save_image(grid_ref, out_path + "_ref.png")

	print(f"{msg}\n")

	# Diff
	diffs = [
		abs(grid_banded[y][x] - grid_ref[y][x])
		for y in range(len(grid_banded))
		for x in range(len(grid_banded[0]))
	]

	print(f"Max diff:  {max(diffs):.6f}")
	print(f"Mean diff: {sum(diffs)/len(diffs):.6f}")
	print("\nDone.")

if __name__ == "__main__":
	main()
