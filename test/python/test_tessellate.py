"""
Tests for slughorn/tessellate.hpp - slughorn.tessellate submodule.

Mirrors test/slughorn-test-tessellate.cpp: builds the same three shapes (single exterior,
exterior+hole, disjoint multi-exterior), triangulates via tessellate(), and checks the summed
triangle area against the known analytic area - the same correctness check that caught the
memoryview dangling-format-pointer bug via test_path_from_curves, applied here directly against
the new bindings.
"""

import math
import numpy as np
import pytest
import slughorn
from conftest import requires_tessellate


def _triangle_atlas():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.move_to(0.5, 0.1)
	canvas.line_to(0.9, 0.9)
	canvas.line_to(0.1, 0.9)
	canvas.close_path()
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, slughorn.Key("triangle"))
	canvas.finalize(slughorn.Key("triangle_comp"))

	atlas.build()
	return atlas


def _donut_atlas():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.circle(0.5, 0.5, 0.4)
	canvas.move_to(0.35, 0.35)
	canvas.line_to(0.35, 0.65)
	canvas.line_to(0.65, 0.65)
	canvas.line_to(0.65, 0.35)
	canvas.close_path()
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, slughorn.Key("donut"))
	canvas.finalize(slughorn.Key("donut_comp"))

	atlas.build()
	return atlas


def _two_dots_atlas():
	atlas = slughorn.Atlas()
	canvas = slughorn.canvas.Canvas(atlas)

	canvas.begin_path()
	canvas.circle(0.3, 0.5, 0.15)
	canvas.circle(0.7, 0.5, 0.15)
	canvas.fill(slughorn.Color(1, 1, 1, 1), 1.0, slughorn.Key("two_dots"))
	canvas.finalize(slughorn.Key("two_dots_comp"))

	atlas.build()
	return atlas


def _mesh_area(mesh):
	pos = np.asarray(mesh.positions)
	idx = np.asarray(mesh.indices)
	tris = pos[idx]
	cross = np.cross(tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0])
	return float(np.abs(cross).sum() * 0.5)


# ---------------------------------------------------------------------------
# Module / class availability
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_tessellate_submodule_exists():
	assert hasattr(slughorn, "tessellate")
	assert hasattr(slughorn.tessellate, "Mesh2D")
	assert hasattr(slughorn.tessellate, "Mesh3D")
	assert hasattr(slughorn.tessellate, "tessellate")
	assert hasattr(slughorn.tessellate, "extrude")


# ---------------------------------------------------------------------------
# tessellate() - triangle (single exterior, no holes)
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_tessellate_triangle_returns_mesh2d():
	atlas = _triangle_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("triangle"))
	mesh = slughorn.tessellate.tessellate(contours)
	assert isinstance(mesh, slughorn.tessellate.Mesh2D)

@requires_tessellate()
def test_tessellate_triangle_area():
	atlas = _triangle_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("triangle"))
	mesh = slughorn.tessellate.tessellate(contours)

	# Exact (straight edges, no curve flattening error).
	expected = 0.5 * abs(0.5 * (0.9 - 0.9) + 0.9 * (0.9 - 0.1) + 0.1 * (0.1 - 0.9))
	assert _mesh_area(mesh) == pytest.approx(expected, abs=1e-4)


# ---------------------------------------------------------------------------
# tessellate() - donut (one exterior + one hole)
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_tessellate_donut_area():
	atlas = _donut_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("donut"))
	assert len(contours) == 2  # exterior circle + square hole

	mesh = slughorn.tessellate.tessellate(contours)
	expected = math.pi * 0.4 * 0.4 - 0.3 * 0.3
	assert _mesh_area(mesh) == pytest.approx(expected, abs=0.01)


# ---------------------------------------------------------------------------
# tessellate() - two_dots (disjoint multi-exterior, no holes)
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_tessellate_two_dots_area():
	atlas = _two_dots_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("two_dots"))
	assert len(contours) == 2  # two disjoint exteriors

	mesh = slughorn.tessellate.tessellate(contours)
	expected = 2 * math.pi * 0.15 * 0.15
	assert _mesh_area(mesh) == pytest.approx(expected, abs=0.01)


# ---------------------------------------------------------------------------
# tessellate() - buffer shape / index-bounds invariants
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_tessellate_mesh2d_buffer_shapes():
	atlas = _donut_atlas()
	mesh = slughorn.tessellate.tessellate(atlas.get_shape_contours(slughorn.Key("donut")))

	positions = np.asarray(mesh.positions)
	indices = np.asarray(mesh.indices)

	assert positions.ndim == 2 and positions.shape[1] == 2
	assert indices.ndim == 2 and indices.shape[1] == 3
	assert indices.max() < positions.shape[0]


# ---------------------------------------------------------------------------
# extrude()
# ---------------------------------------------------------------------------

@requires_tessellate()
def test_extrude_returns_mesh3d():
	atlas = _donut_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("donut"))
	solid = slughorn.tessellate.extrude(contours, 0.2)
	assert isinstance(solid, slughorn.tessellate.Mesh3D)

@requires_tessellate()
def test_extrude_mesh3d_buffer_shapes():
	atlas = _donut_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("donut"))
	solid = slughorn.tessellate.extrude(contours, 0.2)

	positions = np.asarray(solid.positions)
	indices = np.asarray(solid.indices)

	assert positions.ndim == 2 and positions.shape[1] == 3
	assert indices.ndim == 2 and indices.shape[1] == 3
	assert indices.max() < positions.shape[0]

@requires_tessellate()
def test_extrude_has_more_vertices_than_cap():
	atlas = _donut_atlas()
	contours = atlas.get_shape_contours(slughorn.Key("donut"))

	cap = slughorn.tessellate.tessellate(contours)
	solid = slughorn.tessellate.extrude(contours, 0.2)

	cap_vertex_count = np.asarray(cap.positions).shape[0]
	solid_vertex_count = np.asarray(solid.positions).shape[0]

	# Two caps (top + bottom) plus wall vertices per ring - strictly more than the flat cap alone.
	assert solid_vertex_count > cap_vertex_count * 2
