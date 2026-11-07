#include "slughorn-python.hpp"

namespace slughorn_python {

void bind_render(py::module_& render) {

	py::class_<Grid>(render, "Grid", py::buffer_protocol())
		.def_readonly("width", &Grid::width)
		.def_readonly("height", &Grid::height)
		.def_buffer([](Grid& g) -> py::buffer_info {
			return py::buffer_info(
				g.data.data(),
				sizeof(slug_t),
				py::format_descriptor<slug_t>::format(),
				2,
				{ static_cast<py::ssize_t>(g.height), static_cast<py::ssize_t>(g.width) },
				{
					static_cast<py::ssize_t>(g.width * sizeof(slug_t)),
					static_cast<py::ssize_t>(sizeof(slug_t))
				}
			);
		})
		.def("__repr__", [](const Grid& g) {
			return "Grid(" + std::to_string(g.width) + "x" + std::to_string(g.height) + ")";
		})
	;

#ifdef SLUGHORN_HAS_SDF
	// A baked SDF/MSDF tile straight from the generator (see slughorn.SDF for the tile contract).
	// memoryview(field) / np.asarray(field) is always (height, width, channels) float32,
	// row 0 = BOTTOM; channels == 1 for SDF.Type.SDF, 3 for SDF.Type.MSDF.
	py::class_<Field>(render, "Field", py::buffer_protocol())
		.def_readonly("width", &Field::width)
		.def_readonly("height", &Field::height)
		.def_readonly("channels", &Field::channels)
		.def_readonly("range", &Field::range)
		.def_readonly("texels_per_em", &Field::texelsPerEm)
		.def_property_readonly("em_origin", [](const Field& f) {
			return py::make_tuple(f.emOriginX, f.emOriginY);
		})
		.def_buffer([](Field& f) -> py::buffer_info {
			return py::buffer_info(
				f.data.data(),
				sizeof(float),
				py::format_descriptor<float>::format(),
				3,
				{
					static_cast<py::ssize_t>(f.height),
					static_cast<py::ssize_t>(f.width),
					static_cast<py::ssize_t>(f.channels)
				},
				{
					static_cast<py::ssize_t>(f.width * f.channels * sizeof(float)),
					static_cast<py::ssize_t>(f.channels * sizeof(float)),
					static_cast<py::ssize_t>(sizeof(float))
				}
			);
		})
		.def("__repr__", [](const Field& f) {
			return "Field(" + std::to_string(f.width) + "x" + std::to_string(f.height)
				+ "x" + std::to_string(f.channels) + ")";
		})
	;
#endif

	py::class_<Sample>(render, "Sample")
		.def_readonly("fill", &Sample::fill)
		.def_readonly("xcov", &Sample::xcov)
		.def_readonly("ycov", &Sample::ycov)
		.def_readonly("xwgt", &Sample::xwgt)
		.def_readonly("ywgt", &Sample::ywgt)
		.def_readonly("iters", &Sample::iters)
		.def("__repr__", [](const Sample& r) {
			std::ostringstream ss;

			ss
				<< "Sample(fill=" << r.fill
				<< ", xcov=" << r.xcov
				<< ", ycov=" << r.ycov
				<< ", xwgt=" << r.xwgt
				<< ", ywgt=" << r.ywgt
				<< ", iters=" << r.iters << ")"
			;

			return ss.str();
		})
	;

	py::enum_<AARamp>(render, "AARamp")
		.value("Axis", AARamp::Axis)
		.value("Edge", AARamp::Edge)
	;

	py::class_<EmJacobian>(render, "EmJacobian")
		.def(py::init([](slug_t dxX, slug_t dxY, slug_t dyX, slug_t dyY) {
			return EmJacobian{dxX, dxY, dyX, dyY};
		}), "dx_x"_a=0_cv, "dx_y"_a=0_cv, "dy_x"_a=0_cv, "dy_y"_a=0_cv)
		.def_readwrite("dx_x", &EmJacobian::dxX)
		.def_readwrite("dx_y", &EmJacobian::dxY)
		.def_readwrite("dy_x", &EmJacobian::dyX)
		.def_readwrite("dy_y", &EmJacobian::dyY)
		.def_static("from_pixels_per_em", &EmJacobian::fromPixelsPerEm, "ppe_x"_a, "ppe_y"_a)
		.def_static("from_rotation", &EmJacobian::fromRotation, "radians"_a, "ppe"_a)
		.def_property_readonly("ppe_x", &EmJacobian::ppeX)
		.def_property_readonly("ppe_y", &EmJacobian::ppeY)
		.def("__repr__", [](const EmJacobian& j) {
			std::ostringstream ss;

			ss
				<< "EmJacobian(dx_x=" << j.dxX
				<< ", dx_y=" << j.dxY
				<< ", dy_x=" << j.dyX
				<< ", dy_y=" << j.dyY << ")"
			;

			return ss.str();
		})
	;

	py::class_<Sampler>(render, "Sampler")
		.def_readwrite("aa_ramp", &Sampler::aaRamp)
		.def_property_readonly("shape", [](const Sampler& d) { return d.shape; })
		.def_property_readonly("curves", [](const Sampler& d) { return d.curves; })
		.def_property_readonly("curve_buffer", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return curveView2D(self, d.curves);
		}, "2-D float32 memoryview of decoded curves with shape (num_curves, 6).")
		.def_property_readonly("hband_offsets", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return vectorView1D(self, d.hbandOffsets);
		}, "CSR offsets for horizontal bands.")
		.def_property_readonly("hband_indices", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return vectorView1D(self, d.hbandIndices);
		}, "CSR payload for horizontal bands.")
		.def_property_readonly("vband_offsets", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return vectorView1D(self, d.vbandOffsets);
		}, "CSR offsets for vertical bands.")
		.def_property_readonly("vband_indices", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return vectorView1D(self, d.vbandIndices);
		}, "CSR payload for vertical bands.")
		.def_property_readonly("indir_y", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return arrayView1D(self, d.indirY);
		}, "Band indirection table for Y, length INDIRECTION_SIZE.")
		.def_property_readonly("indir_x", [](py::object self) {
			const auto& d = self.cast<const Sampler&>();

			return arrayView1D(self, d.indirX);
		}, "Band indirection table for X, length INDIRECTION_SIZE.")
		.def("get_hband", [](const Sampler& d, uint32_t i) {
			if(i + 1 >= d.hbandOffsets.size()) {
				throw py::index_error("horizontal band out of range");
			}

			py::list out;

			for(uint32_t j = d.hbandOffsets[i]; j < d.hbandOffsets[i + 1]; j++)
				out.append(d.hbandIndices[j]);

			return out;
		}, "index"_a)
		.def("get_vband", [](const Sampler& d, uint32_t i) {
			if(i + 1 >= d.vbandOffsets.size()) {
				throw py::index_error("vertical band out of range");
			}

			py::list out;

			for(uint32_t j = d.vbandOffsets[i]; j < d.vbandOffsets[i + 1]; j++)
				out.append(d.vbandIndices[j]);

			return out;
		}, "index"_a)
		.def("render_sample",
			[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
				return d.renderSample(x, y, ppeX, ppeY);
			},
			"x"_a, "y"_a, "ppe_x"_a, "ppe_y"_a,
			"Reference software sample using all decoded curves."
		)
		.def("render_sample",
			[](const Sampler& d, slug_t x, slug_t y, const EmJacobian& em) {
				return d.renderSample(x, y, em);
			},
			"x"_a, "y"_a, "em"_a,
			"Reference software sample with a full em Jacobian (read by AARamp.Edge)."
		)
		.def("render_sample_banded",
			[](const Sampler& d, slug_t x, slug_t y, slug_t ppeX, slug_t ppeY) {
				return d.renderSampleBanded(x, y, ppeX, ppeY);
			},
			"x"_a, "y"_a, "ppe_x"_a, "ppe_y"_a,
			"Band-accelerated software sample mirroring the GPU shader path."
		)
		.def("render_sample_banded",
			[](const Sampler& d, slug_t x, slug_t y, const EmJacobian& em) {
				return d.renderSampleBanded(x, y, em);
			},
			"x"_a, "y"_a, "em"_a,
			"Band-accelerated software sample with a full em Jacobian (read by AARamp.Edge)."
		)
		.def("render_grid",
			[](const Sampler& d, uint32_t size, slug_t margin, bool banded, bool parallel) {
				py::gil_scoped_release release;
				return d.renderGrid(size, margin, banded, parallel);
			},
			"size"_a=128, "margin"_a=0_cv, "banded"_a=true, "parallel"_a=false,
			"Render a full grayscale coverage grid.\n"
			"Returns a Grid; use memoryview(grid) for a zero-copy (H, W) float32 view,\n"
			"or np.asarray(grid) for NumPy users.\n"
			"parallel=True uses OpenMP row parallelism (requires SLUGHORN_RENDER_PARALLEL=ON)."
		)
		.def("__repr__", [](const Sampler& d) {
			return "Sampler(curves=" + std::to_string(d.curves.size()) +
				", hbands=" + std::to_string(
					d.hbandOffsets.empty() ? 0 : d.hbandOffsets.size() - 1
				) +
				", vbands=" + std::to_string(
					d.vbandOffsets.empty() ? 0 : d.vbandOffsets.size() - 1
				) + ")";
		})
	;

	render.def("distance_to_curve", &slughorn::render::distanceToCurve,
		"curve"_a, "x"_a, "y"_a,
		"Em distance from (x, y) to the nearest point of a quadratic Curve (t in [0, 1])."
	);

	render.def("decode",
		[](const slughorn::Atlas& atlas, slughorn::Key key) {
			return slughorn::render::decode(atlas, key);
		},
		"atlas"_a, "key"_a,
		"Decode a built atlas shape into a slughorn.render.Sampler."
	);

#ifdef SLUGHORN_HAS_SDF
	render.def("field",
		[](
			const slughorn::Atlas& atlas,
			slughorn::Key key,
			std::optional<slughorn::Atlas::SDF::Config> config,
			std::optional<slug_t> range
		) {
			const slughorn::Atlas::SDF::Config& cfg = config ? *config : atlas.getSDF().config;

			return slughorn::render::field(atlas, key, cfg, range.value_or(cfg.range));
		},
		"atlas"_a, "key"_a, "config"_a=py::none(), "range"_a=py::none(),
		"Bake one shape into an SDF/MSDF Field without touching the Atlas (the same generator\n"
		"build() uses for request_sdf()). config defaults to the Atlas's own SDF.Config; range\n"
		"defaults to config.range. width == 0 means the shape has no geometry.\n"
		"Row 0 is the BOTTOM row (GPU convention); flip it yourself for a human-facing image."
	);
#endif
}

}
