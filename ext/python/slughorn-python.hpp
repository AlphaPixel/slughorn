#pragma once

// Shared umbrella header for the split slughorn Python bindings (see slughorn.cpp for the
// PYBIND11_MODULE entry point). Deliberately does NOT include freetype/nanosvg/tessellate/serial
// headers -- those are heavy and/or gated behind SLUGHORN_HAS_* feature macros, and only the one
// matching slughorn-*.cpp file needs them (see that file's own local #include). emoji.hpp is
// likewise NOT included here: SLUGHORN_EMOJI_IMPLEMENTATION must land in exactly one translation
// unit, and that's slughorn-emoji.cpp's job alone. Splitting was a straight build-time win:
// previously any change anywhere in the single ~2900-line ext/slughorn-python.cpp forced a full
// serial recompile; now each bind_*() lives in its own translation unit, so touching one doesn't
// force the others to recompile, and a from-scratch build parallelizes across `make -j#`.

#include "slughorn/canvas.hpp"
#include "slughorn/render.hpp"

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl_bind.h>
#include <pybind11/functional.h>

#include <array>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <sstream>

using namespace slughorn::literals;
using slughorn::slug_t;

namespace py = pybind11;
using namespace py::literals;

PYBIND11_MAKE_OPAQUE(std::vector<slughorn::Layer>);

// ================================================================================================
// detail - internal helpers and Python trampolines shared across bind_*() translation units.
// Submodule-local helpers (detail::makeLoadConfig, detail::makeNanosvgLoadConfig) are NOT here --
// they live in slughorn-freetype.cpp/slughorn-nanosvg.cpp respectively, which reopen this same
// `detail` namespace to add them.
// ================================================================================================

namespace detail {

// Zero-copy memoryview over a vector<uint8_t>.
// The vector must outlive the view - caller's responsibility.
inline py::memoryview bytesView(const std::vector<uint8_t>& v) {
	return py::memoryview::from_memory(
		const_cast<uint8_t*>(v.data()),
		static_cast<py::ssize_t>(v.size())
	);
}

// pybind11's memoryview::from_buffer stores the raw `format` pointer directly in the Py_buffer
// struct (view.format = format) and PyMemoryView_FromBuffer does not copy the string it points
// to - it must stay valid for the memoryview object's entire lifetime, not just this call. A
// stack-local std::string (even via format_descriptor<T>::format()) is destroyed on return,
// leaving a dangling pointer that "works" until something else reuses that stack slot. Must be
// a function-local static (one instance per T) so the backing storage lives forever.
template<typename T>
const std::string& formatOf() {
	static const std::string fmt = py::format_descriptor<T>::format();

	return fmt;
}

template<typename T>
py::memoryview vectorView1D(const std::vector<T>& v) {
	const std::vector<py::ssize_t> shape = {static_cast<py::ssize_t>(v.size())};
	const std::vector<py::ssize_t> strides = {static_cast<py::ssize_t>(sizeof(T))};

	return py::memoryview::from_buffer(
		static_cast<const void*>(v.data()),
		sizeof(T),
		formatOf<T>().c_str(),
		shape,
		strides
	);
}

template<typename T, size_t N>
py::memoryview arrayView1D(const std::array<T, N>& v) {
	const std::vector<py::ssize_t> shape = {static_cast<py::ssize_t>(N)};
	const std::vector<py::ssize_t> strides = {static_cast<py::ssize_t>(sizeof(T))};

	return py::memoryview::from_buffer(
		static_cast<const void*>(v.data()),
		sizeof(T),
		formatOf<T>().c_str(),
		shape,
		strides
	);
}

// Views `rows` rows of `cols` T-elements each, starting at `data`, as a strided 2-D buffer with
// no copy. `rowStrideBytes` is separate from `cols * sizeof(T)` so callers whose row type has
// padding (or is a struct, like Curve) can pass the real row size.
template<typename T>
py::memoryview flatView2D(const void* data, size_t rows, size_t cols, size_t rowStrideBytes) {
	const std::vector<py::ssize_t> shape = {
		static_cast<py::ssize_t>(rows), static_cast<py::ssize_t>(cols)
	};
	const std::vector<py::ssize_t> strides = {
		static_cast<py::ssize_t>(rowStrideBytes),
		static_cast<py::ssize_t>(sizeof(T))
	};

	return py::memoryview::from_buffer(data, sizeof(T), formatOf<T>().c_str(), shape, strides);
}

template<typename T>
py::memoryview flatView2D(const std::vector<T>& v, size_t cols) {
	return flatView2D<T>(
		static_cast<const void*>(v.data()), v.size() / cols, cols, cols * sizeof(T)
	);
}

inline py::memoryview curveView2D(const std::vector<slughorn::Atlas::Curve>& curves) {
	return flatView2D<slughorn::slug_t>(
		static_cast<const void*>(curves.data()), curves.size(), 6, sizeof(slughorn::Atlas::Curve)
	);
}

// Use the C++ operator<< to build a repr string for any type that has one.
template<typename T>
std::string streamRepr(const T& v, const std::string& prefix="") {
	std::ostringstream ss;

	if(!prefix.empty()) ss << prefix << ".";

	ss << v;

	return ss.str();
}

// CSR view over Atlas::getShapeContours(): all curves concatenated into one flat buffer, plus
// row offsets splitting it back into per-contour ranges (contour i = curves[offsets[i]:offsets[i+1]]).
// Avoids building nested Python lists/tuples for what can be a large, jagged curve list (GIS
// polygons especially); mirrors the offsets+indices CSR pattern Sampler already uses for bands.
struct PyShapeContours {
	slughorn::Atlas::Curves curves;
	std::vector<uint32_t> offsets;
};

// Reconstructs Atlas::Contours (vector<Curves>) from the CSR form above - used by tessellate()/
// extrude() bindings, which take the jagged C++ form.
inline slughorn::Atlas::Contours contoursFromCSR(const PyShapeContours& sc) {
	slughorn::Atlas::Contours result;

	if(sc.offsets.empty()) return result;

	result.reserve(sc.offsets.size() - 1);

	for(size_t i = 0; i + 1 < sc.offsets.size(); i++) {
		result.emplace_back(sc.curves.begin() + sc.offsets[i], sc.curves.begin() + sc.offsets[i + 1]);
	}

	return result;
}

// Python-friendly CurveDecomposer
//
// Owns its Curves vector so Python's GC cannot collect it out from under us. The C++
// CurveDecomposer holds a Curves& - that's fine in C++ but unsafe to expose directly to Python.
// ================================================================================================
struct PyCurveDecomposer {
	slughorn::Atlas::Curves curves;
	slughorn::CurveDecomposer decomposer;

	PyCurveDecomposer(): decomposer(curves) {}

	void moveTo(slug_t x, slug_t y) { decomposer.moveTo(x, y); }
	void lineTo(slug_t x3, slug_t y3) { decomposer.lineTo(x3, y3); }
	void quadTo(slug_t cx, slug_t cy, slug_t x3, slug_t y3) { decomposer.quadTo(cx, cy, x3, y3); }
	void cubicTo(
		slug_t c1x, slug_t c1y,
		slug_t c2x, slug_t c2y,
		slug_t x3, slug_t y3
	) { decomposer.cubicTo(c1x, c1y, c2x, c2y, x3, y3); }

	const slughorn::Atlas::Curves& getCurves() const { return curves; }
	void close() { decomposer.close(); }
	slug_t getTolerance() const { return decomposer.tolerance; }
	void setTolerance(slug_t tolerance) { decomposer.tolerance = tolerance; }
	void clear() { curves.clear(); }

	size_t mark() const { return decomposer.mark(); }
	void reverseFrom(size_t pos) { decomposer.reverseFrom(pos); }
	void reverseCurves(size_t begin, size_t end) {
		slughorn::CurveDecomposer::reverseCurves(curves, begin, end);
	}
};

// Non-owning Python-facing view over a real slughorn::CurveDecomposer.
// Used for Path.decomposer() / Canvas.decomposer(), where the underlying
// C++ object already exists and should be mutated in place.
struct CurveDecomposerRef {
	slughorn::CurveDecomposer* decomposer = nullptr;

	slug_t getTolerance() const { return decomposer ? decomposer->tolerance : 0_cv; }

	void setTolerance(slug_t tolerance) {
		if(decomposer) decomposer->tolerance = tolerance;
	}

	size_t mark() const { return decomposer ? decomposer->mark() : 0; }

	void reverseFrom(size_t pos) {
		if(decomposer) decomposer->reverseFrom(pos);
	}

	void reverseCurves(size_t begin, size_t end) {
		if(decomposer) slughorn::CurveDecomposer::reverseCurves(decomposer->curves, begin, end);
	}
};

} // namespace detail

// Bring detail helpers into file scope so existing call sites in each bind_*() need no change.
// makeLoadConfig/makeNanosvgLoadConfig are intentionally left out; call sites should say
// detail::makeLoadConfig/detail::makeNanosvgLoadConfig explicitly (see slughorn-freetype.cpp/
// slughorn-nanosvg.cpp, which define them locally).
using detail::bytesView;
using detail::vectorView1D;
using detail::arrayView1D;
using detail::curveView2D;
using detail::flatView2D;
using detail::streamRepr;
using detail::PyCurveDecomposer;
using detail::CurveDecomposerRef;
using detail::PyShapeContours;
using detail::contoursFromCSR;

using slughorn::render::Sample;
using slughorn::render::Sampler;
using slughorn::render::Grid;

#ifdef SLUGHORN_HAS_MSDF
using slughorn::render::MSDFGrid;
#endif

namespace slughorn_python {

void bind_core(py::module_& m);
void bind_render(py::module_& m_render);
void bind_canvas(py::module_& m_canvas);
void bind_emoji(py::module_& m_emoji);

#ifdef SLUGHORN_HAS_FREETYPE
void bind_freetype(py::module_& m_freetype);
#endif

#ifdef SLUGHORN_HAS_NANOSVG
void bind_nanosvg(py::module_& m_nanosvg);
#endif

#ifdef SLUGHORN_HAS_TESSELLATE
void bind_tessellate(py::module_& m_tessellate);
#endif

}
