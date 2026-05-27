//vimrun! ./slughorn-test-cairo

#define SLUGHORN_CAIRO_IMPLEMENTATION
#include "slughorn/cairo.hpp"

#include <iostream>

using namespace slughorn::literals;
using slughorn::slug_t;

void test_Shape() {
	cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 100, 100);
	cairo_t* cr = cairo_create(surf);

	// TODO: Cairo totally ignores transformations when you copy the path and decompose it! It might
	// be possible to FETCH the matrix and apply it somehow, but we'll punt that issue off until a
	// later date...
	// cairo_scale(cr, 100, 100);
	cairo_move_to(cr, 0, 0);
	cairo_line_to(cr, 100, 0);
	cairo_line_to(cr, 0, 100);
	cairo_close_path(cr);

	// -------------------------------------------------------------------------
	// Step 1: raw decomposition
	// -------------------------------------------------------------------------
	auto [info, transform] = slughorn::cairo::decomposePath(cr, 1_cv / 100_cv);

	std::cout << "Matrix: " << transform << std::endl;
	std::cout << "Curves: " << info.curves.size() << std::endl;

	for(size_t i = 0; i < info.curves.size(); i++) std::cout
		<< "  [" << i << "] " << info.curves[i] << std::endl
	;

	// -------------------------------------------------------------------------
	// Step 2: round-trip through atlas
	// -------------------------------------------------------------------------
	slughorn::Atlas atlas;

#if 0
	slughorn::Atlas::ShapeInfo info;

	// TODO: This is SO OFTEN `true` (except in FreeType2 contexts) that constantly setting it
	// manually is kind of janky. Consider making it `true` by default, and just letting the FT2
	// backend manually set it to `false` instead.
	info.autoMetrics = true;

	info.curves = curves;
#endif

	atlas.addShape(1u, info);
	atlas.build();

	const slughorn::Atlas::Shape* shape = atlas.getShape(1u);

	std::cout << std::endl << "=== Atlas::Shape ===" << std::endl;

	if(shape) {
		std::cout << *shape << std::endl;

		auto quad = shape->computeQuad(transform);

		std::cout << quad << std::endl;
	}

	else std::cout << "ERROR: shape not found" << std::endl;

	// Create a "reference image" for comparison.
	cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1.0);
	cairo_fill(cr);
	cairo_surface_write_to_png(surf, "slughorn-test-cairo_Shape.png");

	cairo_destroy(cr);
	cairo_surface_destroy(surf);
}

void test_CompositeShape() {
	cairo_surface_t* surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 300, 100);
	cairo_t* cr = cairo_create(surf);

	slughorn::Atlas atlas;
	slughorn::CompositeShape compositeShape;

	for(double i = 0.0; i < 3.0; i += 1.0) {
		/* double x = i * 100.0;

		cairo_save(cr);
		cairo_new_path(cr);
		cairo_move_to(cr, x, 0.0);
		cairo_line_to(cr, x + 100.0, 0.0);
		cairo_line_to(cr, x, 100.0);
		cairo_close_path(cr);
		cairo_restore(cr); */

		double x = i * 100.0;
		// double y = (2.0 - i) * 100.0;
		double y = i * 100.0;

		cairo_save(cr);
		cairo_new_path(cr);
		cairo_move_to(cr, x, y);
		cairo_line_to(cr, x + 100.0, y);
		cairo_line_to(cr, x, y + 100.0);
		cairo_close_path(cr);
		cairo_restore(cr);

		// auto [curves, transform] = slughorn::cairo::decomposePath(cr, 1_cv / 100_cv);
		auto [info, transform] = slughorn::cairo::decomposePath(cr, 1_cv);

		std::cout << "Matrix: " << transform << std::endl;
		std::cout << "Curves: " << info.curves.size() << std::endl;

		for(size_t j = 0; j < info.curves.size(); j++) std::cout
			<< "  [" << j << "] " << info.curves[j] << std::endl
		;

		// slughorn::Atlas::ShapeInfo info;

		// info.autoMetrics = true;
		// info.curves = curves;

		uint32_t key = static_cast<uint32_t>(i);

		atlas.addShape(key, info);

		compositeShape.layers.push_back({key, {1_cv, 1_cv, 1_cv, 1_cv}, transform});
	}

	// -------------------------------------------------------------------------
	atlas.addCompositeShape(100u, compositeShape);
	atlas.build();

	auto* cs = atlas.getCompositeShape(100u);

	std::cout << std::endl << "=== Atlas::CompositeShape ===" << std::endl;

	for(const auto& layer : cs->layers) {
		const slughorn::Atlas::Shape* shape = atlas.getShape(layer.key);

		if(shape) {
			std::cout << *shape << std::endl;

			auto quad = shape->computeQuad(layer.transform);

			std::cout << quad << std::endl;
		}

		else std::cout << "ERROR: shape not found" << std::endl;

	}

	// Create a "reference image" for comparison.
	cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1.0);
	cairo_fill(cr);
	cairo_surface_write_to_png(surf, "slughorn-test-cairo_CompositeShape.png");

	cairo_destroy(cr);
	cairo_surface_destroy(surf);
}

int main(int argc, char** argv) {
	test_Shape();
	test_CompositeShape();

	return 0;
}
