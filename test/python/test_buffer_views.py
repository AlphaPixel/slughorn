"""
Zero-copy memoryviews keep the object they view alive.

Every view the bindings return (Shape.curves, ShapeContours, TextureData.bytes, tessellate meshes,
...) points straight into a C++ object's storage. Reading one off a temporary - e.g.
`atlas.get_shape(key).curves` - must still see valid data after that temporary is gone.
"""

import gc
import slughorn
from conftest import requires_tessellate

def _circle_atlas(build=False):
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.circle(0.5, 0.5, 0.4)
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, slughorn.Key("circle"))

	if build:
		atlas.build()

	return atlas

def _churn(nbytes):
	"""Collect garbage, then reallocate and overwrite freed blocks of about `nbytes`."""
	gc.collect()

	return [bytearray(b"\xff" * nbytes) for _ in range(64)]

def test_shape_curves_from_temporary():
	atlas = _circle_atlas()
	key = slughorn.Key("circle")
	expected = atlas.get_shape(key).curves.tolist()

	view = atlas.get_shape(key).curves
	junk = _churn(len(expected) * 24)

	assert view.tolist() == expected
	assert len(junk) == 64

def test_path_from_temporary_shape_curves():
	atlas = _circle_atlas()
	key = slughorn.Key("circle")
	shape = atlas.get_shape(key)

	path = slughorn.canvas.Path(atlas.get_shape(key).curves)

	assert path.arc_length() == slughorn.canvas.Path(shape.curves).arc_length()

def test_shape_contours_from_temporary():
	atlas = _circle_atlas()
	key = slughorn.Key("circle")
	held = atlas.get_shape_contours(key)

	curves = atlas.get_shape_contours(key).curves
	offsets = atlas.get_shape_contours(key).offsets
	first = atlas.get_shape_contours(key)[0]
	junk = _churn(len(held.curves.tolist()) * 24)

	assert curves.tolist() == held.curves.tolist()
	assert offsets.tolist() == held.offsets.tolist()
	assert first.tolist() == held[0].tolist()
	assert len(junk) == 64

def test_texture_bytes_from_temporary():
	atlas = _circle_atlas(build=True)
	expected = bytes(atlas.curve_texture.bytes)

	view = atlas.curve_texture.bytes
	junk = _churn(len(expected))

	assert bytes(view) == expected
	assert len(junk) == 64

@requires_tessellate()
def test_tessellate_mesh_from_temporary():
	atlas = _circle_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("circle"))
	held = slughorn.tessellate.tessellate(contours)

	positions = slughorn.tessellate.tessellate(contours).positions
	indices = slughorn.tessellate.tessellate(contours).indices
	junk = _churn(len(held.positions.tolist()) * 8)

	assert positions.tolist() == held.positions.tolist()
	assert indices.tolist() == held.indices.tolist()
	assert len(junk) == 64
