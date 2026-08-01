#include "slughorn-python.hpp"

#include "slughorn/tessellate.hpp"

namespace slughorn_python {

void bind_tessellate(py::module_& tessellate) {

	py::class_<slughorn::tessellate::Mesh2D>(tessellate, "Mesh2D")
		.def_property_readonly("positions", [](const slughorn::tessellate::Mesh2D& mesh) {
			return flatView2D(mesh.positions, 2);
		}, "Zero-copy (N, 2) float32 memoryview of vertex positions (xy).")
		.def_property_readonly("indices", [](const slughorn::tessellate::Mesh2D& mesh) {
			return flatView2D(mesh.indices, 3);
		}, "Zero-copy (N, 3) uint32 memoryview of triangle indices.")
		.def("__repr__", [](const slughorn::tessellate::Mesh2D& mesh) {
			return "Mesh2D(" + std::to_string(mesh.positions.size() / 2) + " vertices, "
				+ std::to_string(mesh.indices.size() / 3) + " triangles)";
		})
	;

	py::class_<slughorn::tessellate::Mesh3D>(tessellate, "Mesh3D")
		.def_property_readonly("positions", [](const slughorn::tessellate::Mesh3D& mesh) {
			return flatView2D(mesh.positions, 3);
		}, "Zero-copy (N, 3) float32 memoryview of vertex positions (xyz).")
		.def_property_readonly("indices", [](const slughorn::tessellate::Mesh3D& mesh) {
			return flatView2D(mesh.indices, 3);
		}, "Zero-copy (N, 3) uint32 memoryview of triangle indices.")
		.def("__repr__", [](const slughorn::tessellate::Mesh3D& mesh) {
			return "Mesh3D(" + std::to_string(mesh.positions.size() / 3) + " vertices, "
				+ std::to_string(mesh.indices.size() / 3) + " triangles)";
		})
	;

	tessellate.def("tessellate",
		[](const PyShapeContours& contours, slug_t tolerance) {
			return slughorn::tessellate::tessellate(contoursFromCSR(contours), tolerance);
		},
		"contours"_a, "tolerance"_a=slughorn::TOLERANCE_BALANCED,
		"Flatten and triangulate `contours` (a ShapeContours, as returned by\n"
		"Atlas.get_shape_contours()). Groups holes with the exterior ring that contains\n"
		"them; a shape may have any number of disjoint exterior rings, each with zero or\n"
		"more holes. Returns a Mesh2D."
	);

	tessellate.def("extrude",
		[](const PyShapeContours& contours, slug_t depth, slug_t tolerance) {
			return slughorn::tessellate::extrude(contoursFromCSR(contours), depth, tolerance);
		},
		"contours"_a, "depth"_a, "tolerance"_a=slughorn::TOLERANCE_BALANCED,
		"Extrude `contours` (a ShapeContours) into a closed 3D solid: a top cap at\n"
		"z=depth, a bottom cap at z=0, and a wall connecting every ring (exterior and\n"
		"hole boundaries alike). Returns a Mesh3D."
	);
}

}
