#pragma once

// ================================================================================================
// slughorn-canvas.hpp - HTML Canvas-style drawing context for slughorn
//
// Provides a stateful 2-D path API (beginPath / moveTo / lineTo / quadTo / bezierTo / closePath)
// plus arc primitives (arc, arcTo) and convenience shape helpers (rect, roundedRect, circle,
// ellipse) that accumulate into a
// CompositeShape - one Layer per fill() call - which can then be registered in an Atlas.
//
// The API is modelled on the HTML Canvas / Web 2D Context drawing vocabulary, which is also shared
// by NanoVG, Cairo, Skia, and most other 2-D vector drawing libraries. Anyone who has written
// Canvas or SVG path code will feel immediately at home.
//
// NOTE: This header has NO dependency on NanoVG or any other external drawing library. The naming
// is purely about API familiarity. It is a slughorn-native drawing context.
//
// RELATIONSHIP BETWEEN Shape, Layer, AND CompositeShape
// ------------------------------------------------------
// - Atlas::Shape - pure geometry (packed curve + band textures). Stateless. Identified by Key.
// - Layer - one reference to a Shape + color + transform + scale + effectId.
// - CompositeShape - an ordered stack of Layers (drawn back-to-front) plus an advance.
//
// Canvas is therefore a CompositeShape *builder* with a path accumulator on the side.
// Each fill() call:
// 1. Computes the bounding box of the pending curves.
// 2. Shifts all curves to local origin (tight atlas bands, zero wasted band space).
// 3. Stores the canvas-space offset in Layer::transform (dx/dy), matching the convention
// used by slughorn-cairo.hpp and slughorn-nanosvg.hpp.
// 4. Registers the geometry in the Atlas under an auto-generated Key.
// 5. Pushes the resulting Layer onto the in-progress CompositeShape.
//
// SCALE CONTRACT (matches every other slughorn backend)
// -----------------------------------------------------
// The `scale` parameter passed to fill() / defineShape() is the *backend normalization scale*;
// it converts your authoring-space coordinates into slughorn's em-space [0,1]. It is NEVER stored
// in Layer::scale. Layer::scale is reserved for FreeType2 font-size scaling and should be left
// at its default (1.0) for all geometry authored through this backend.
//
// CUBIC DECOMPOSITION
// -------------------
// bezierTo() routes through CurveDecomposer::cubicTo(), which uses adaptive De Casteljau
// subdivision to produce high-quality quadratic approximations. The `tolerance` field on the
// internal CurveDecomposer controls flatness; the default (1e-4) suits em-normalized [0,1]
// geometry. Access it via canvas.decomposer().tolerance if you need to tune it.
//
// USAGE
// -----
// No implementation guard needed - this header is pure C++ with no external dependencies.
// Just #include "slughorn-canvas.hpp" wherever you need it.
//
// Refer to `test/slughorn-test-canvas.cpp` for examples.
// ================================================================================================

#include "slughorn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace slughorn {
namespace canvas {

// ================================================================================================
// Canvas
// ================================================================================================

class Canvas {
public:
	// @p atlas - the Atlas to register shapes into (must outlive the Canvas).
	// @p baseKey - reference to a key counter; incremented once per fill() / defineShape() call.
	// Pass a fresh namespace per Canvas (e.g. 0xE0000) or share one across multiple Canvas
	// instances that write to the same Atlas.
	Canvas(Atlas& atlas, uint32_t& baseKey):
	_atlas(atlas),
	_baseKey(baseKey),
	_decomposer(_pendingCurves) {
	}

	// -------------------------------------------------------------------------
	// CurveDecomposer access
	//
	// Exposes the internal decomposer so callers can tune `tolerance` for
	// their coordinate space without rebuilding the Canvas.
	//
	// Example - 1000-unit em square authoring space:
	//
	// canvas.decomposer().tolerance = 0.1f;
	// -------------------------------------------------------------------------

	CurveDecomposer& decomposer() { return _decomposer; }
	const CurveDecomposer& decomposer() const { return _decomposer; }

	// -------------------------------------------------------------------------
	// Path commands
	//
	// Call beginPath() to discard any accumulated path state and start fresh,
	// then issue path commands, then call fill() or defineShape() to commit.
	// -------------------------------------------------------------------------

	void beginPath() {
		_pendingCurves.clear();

		_decomposer._x = _decomposer._y = 0.0_cv;
		_decomposer._sx = _decomposer._sy = 0.0_cv;
	}

	void moveTo(slug_t x, slug_t y) {
		_decomposer.moveTo(x, y);
	}

	void lineTo(slug_t x, slug_t y) {
		_decomposer.lineTo(x, y);
	}

	// Quadratic Bezier: current point -> (cx,cy) control -> (x,y) end.
	void quadTo(slug_t cx, slug_t cy, slug_t x, slug_t y) {
		_decomposer.quadTo(cx, cy, x, y);
	}

	// Cubic Bezier: current point -> (c1x,c1y) -> (c2x,c2y) -> (x,y).
	// Adaptively subdivided into quadratics by CurveDecomposer::cubicTo.
	void bezierTo(slug_t c1x, slug_t c1y, slug_t c2x, slug_t c2y, slug_t x, slug_t y) {
		_decomposer.cubicTo(c1x, c1y, c2x, c2y, x, y);
	}

	void closePath() {
		_decomposer.close();
	}

	// -------------------------------------------------------------------------
	// Convenience shape helpers
	//
	// Each helper calls beginPath() implicitly - they are self-contained path
	// descriptions that replace whatever path was previously being built.
	//
	// If you need multiple primitives in a single shape (e.g. a rect with a
	// circular hole), call beginPath() manually and use the path commands above.
	// -------------------------------------------------------------------------

	// Axis-aligned rectangle. Wound counter-clockwise (Y-up convention).
	void rect(slug_t x, slug_t y, slug_t w, slug_t h) {
		beginPath();

		moveTo(x, y);
		lineTo(x + w, y);
		lineTo(x + w, y + h);
		lineTo(x, y + h);

		closePath();
	}

	// Rounded rectangle with uniform corner radius r.
	// r is clamped to half the smaller dimension so it never exceeds the shape.
	void roundedRect(slug_t x, slug_t y, slug_t w, slug_t h, slug_t r) {
		r = std::min(r, std::min(w, h) * 0.5_cv);

		if(r <= 0.0_cv) { rect(x, y, w, h); return; }

		// Cubic approximation constant for a quarter-circle arc (= 4*(?2-1)/3).
		const slug_t k = 0.5522847498_cv;
		const slug_t kr = k * r;

		beginPath();

		// Bottom edge -> right
		moveTo(x + r, y);
		lineTo(x + w - r, y);

		// Bottom-right corner
		bezierTo(
			x + w - r + kr, y,
			x + w, y + r - kr,
			x + w, y + r
		);

		// Right edge -> up
		lineTo(x + w, y + h - r);

		// Top-right corner
		bezierTo(
			x + w, y + h - r + kr,
			x + w - r + kr, y + h,
			x + w - r, y + h
		);

		// Top edge -> left
		lineTo(x + r, y + h);

		// Top-left corner
		bezierTo(
			x + r - kr, y + h,
			x, y + h - r + kr,
			x, y + h - r
		);

		// Left edge -> down
		lineTo(x, y + r);

		// Bottom-left corner
		bezierTo(
			x, y + r - kr,
			x + r - kr, y,
			x + r, y
		);

		closePath();
	}

	// Circle approximated by four cubic arcs.
	void circle(slug_t cx, slug_t cy, slug_t r) {
		ellipse(cx, cy, r, r);
	}

	// Ellipse approximated by four cubic arcs (Bezier constant k = 4*(?2-1)/3).
	void ellipse(slug_t cx, slug_t cy, slug_t rx, slug_t ry) {
		const slug_t k = 0.5522847498_cv;
		const slug_t kx = k * rx;
		const slug_t ky = k * ry;

		beginPath();

		moveTo (cx + rx, cy);
		bezierTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
		bezierTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
		bezierTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
		bezierTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);

		closePath();
	}

	// Arc - circular arc centred at (cx,cy) with radius r, from startAngle to endAngle (radians,
	// measured from the +X axis, Y-up convention).
	//
	// @p ccw - if true, sweep counter-clockwise; clockwise otherwise. Matches HTML Canvas arc()
	// exactly.
	//
	// The arc is decomposed into segments of at most ?/2 (quarter-circle) each. Each segment is
	// emitted as a cubic via bezierTo(), which routes through the adaptive CurveDecomposer - so
	// quality scales automatically with curvature.
	//
	// arc() does NOT call beginPath(). It appends to the current path, so you can combine arcs with
	// lineTo() to build pie slices, stadium shapes, etc. If you want a standalone arc shape, call
	// beginPath() first.
	//
	// Example - a full circle arc (equivalent to circle(), but via arc()):
	//
	// canvas.beginPath();
	// canvas.arc(cx, cy, r, 0, 2*M_PI);
	// canvas.closePath();
	// canvas.fill(color);
	void arc(slug_t cx, slug_t cy, slug_t r, slug_t startAngle, slug_t endAngle, bool ccw = false) {
		// Normalise the sweep so it is always positive and <= 2?. HTML Canvas semantics: if ccw,
		// sweep is endAngle->startAngle going CCW.
		slug_t sweep;

		if(ccw) {
			// Swap so we always march from lower to higher angle, then negate to signal direction
			// to _arcSegments.
			sweep = startAngle - endAngle;

			if(sweep < 0.0_cv) sweep += cv(2.0 * M_PI);

			sweep = -sweep; // negative sweep = CCW
		}

		else {
			sweep = endAngle - startAngle;

			if(sweep < 0.0_cv) sweep += cv(2.0 * M_PI);
		}

		_arcSegments(cx, cy, r, startAngle, sweep);
	}

	// arcTo - tangential arc from the current point.
	//
	// Draws a circular arc of radius @p r that is tangent to both:
	//
	// - the line from the current point toward (x1, y1)
	// - the line from (x1, y1) toward (x2, y2)
	//
	// Then draws a straight line from the current point to the arc start, and
	// leaves the current point at the arc end - matching HTML Canvas arcTo().
	//
	// This is the natural primitive for manually-built rounded corners:
	//
	// canvas.beginPath();
	// canvas.moveTo(x, y);
	// canvas.arcTo(cornerX, cornerY, nextX, nextY, radius);
	// ...
	//
	// Degenerate cases (zero-length tangent legs, r <= 0, collinear points):
	// fall back to a plain lineTo(x1, y1) with no arc, matching Canvas spec.
	void arcTo(slug_t x1, slug_t y1, slug_t x2, slug_t y2, slug_t r) {
		if(r <= 0.0_cv) { lineTo(x1, y1); return; }

		const slug_t p0x = _decomposer._x;
		const slug_t p0y = _decomposer._y;

		// Vectors from the corner point (x1,y1) toward p0 and toward p2.
		const slug_t d0x = p0x - x1;
		const slug_t d0y = p0y - y1;
		const slug_t d1x = x2 - x1;
		const slug_t d1y = y2 - y1;
		const slug_t len0 = std::sqrt(d0x*d0x + d0y*d0y);
		const slug_t len1 = std::sqrt(d1x*d1x + d1y*d1y);

		// Degenerate: either leg has zero length.
		if(len0 < 1e-6_cv || len1 < 1e-6_cv) { lineTo(x1, y1); return; }

		// Unit vectors
		const slug_t u0x = d0x / len0, u0y = d0y / len0;
		const slug_t u1x = d1x / len1, u1y = d1y / len1;

		// Cross product (sine of angle between the two legs).
		const slug_t cross = u0x*u1y - u0y*u1x;

		// Degenerate: legs are (nearly) collinear - no arc possible.
		if(std::abs(cross) < 1e-6_cv) { lineTo(x1, y1); return; }

		// Distance from corner to the two tangent points.
		const slug_t dot = u0x*u1x + u0y*u1y;
		const slug_t tanHalf = std::abs(cross) / (1.0_cv + dot);

		if(tanHalf < 1e-6_cv) { lineTo(x1, y1); return; }

		const slug_t tangentDist = r / tanHalf;

		// Tangent points: step back from the corner along each leg.
		const slug_t t0x = x1 + u0x * tangentDist; // arc start (on leg toward p0)
		const slug_t t0y = y1 + u0y * tangentDist;
		const slug_t t1x = x1 + u1x * tangentDist; // arc end (on leg toward p2)
		const slug_t t1y = y1 + u1y * tangentDist;

		// Centre of the arc: perpendicular from the corner bisects the chord.
		// The centre lies along the bisector of the two legs at distance r/|sin(halfAngle)|.
		// Equivalent: offset t0 perpendicular to u0 by r (sign from cross product).
		const slug_t sign = (cross > 0.0_cv) ? 1.0_cv : -1.0_cv;
		const slug_t cenX = t0x + sign * u0y * r; // u0 rotated 90?
		const slug_t cenY = t0y - sign * u0x * r;

		// Angles from centre to the two tangent points.
		const slug_t a0 = std::atan2(t0y - cenY, t0x - cenX);
		const slug_t a1 = std::atan2(t1y - cenY, t1x - cenX);

		// Line from current point to arc start tangent point.
		lineTo(t0x, t0y);

		// Arc from a0 to a1. Winding: if cross > 0 the arc goes CCW (Y-up).
		arc(cenX, cenY, r, a0, a1, cross > 0.0_cv);
	}

	// -------------------------------------------------------------------------
	// Commit the current path
	// -------------------------------------------------------------------------

	// Commit the current path as a new Layer in the in-progress CompositeShape.
	//
	// @p color - fill color for the layer.
	// @p scale - normalizes authoring-space coords into em-space (default 1.0). NEVER stored in
	// Layer::scale; see Scale Contract in the file header.
	//
	// The curves are shifted to local origin (tight atlas bands); the canvas-space offset is stored
	// in Layer::transform (dx/dy). An internal key is auto-generated and _baseKey is incremented.
	//
	// Returns the auto-generated Key, or Key(0u) if the current path is empty.
	Key fill(Color color, slug_t scale = 1.0_cv) {
		if(_pendingCurves.empty()) return Key(0u);

		Atlas::Curves scaled = _scaleCurves(_pendingCurves, scale);

		Matrix transform;
		Atlas::Curves local = _toLocalOrigin(scaled, transform);

		if(local.empty()) return Key(0u);

		const Key key(_baseKey++);

		Atlas::ShapeInfo info;

		// info.autoMetrics = true;
		info.curves = std::move(local);

		_atlas.addShape(key, info);

		Layer layer;

		layer.key = key;
		layer.color = color;
		layer.transform = transform;
		// layer.scale intentionally left at 1.0 - see Scale Contract

		_composite.layers.push_back(layer);

		return key;
	}

	// Register the current path as a named Shape (geometry only, no color or Layer).
	//
	// Use when you want to reference the same geometry from multiple Layers
	// with different colors or transforms, managed by the caller.
	//
	// @p scale - same normalization convention as fill(). NOT stored on any Layer.
	//
	// Returns false and does nothing if the current path is empty.
	bool defineShape(Key key, slug_t scale = 1.0_cv) {
		if(_pendingCurves.empty()) return false;

		Atlas::Curves scaled = _scaleCurves(_pendingCurves, scale);

		Matrix transform; // discarded - caller manages placement

		Atlas::Curves local = _toLocalOrigin(scaled, transform);

		if(local.empty()) return false;

		Atlas::ShapeInfo info;

		// info.autoMetrics = true;
		info.curves = std::move(local);

		_atlas.addShape(key, info);

		return true;
	}

	// -------------------------------------------------------------------------
	// CompositeShape management
	// -------------------------------------------------------------------------

	// Discard all accumulated layers and start a fresh composite.
	// Any pending (uncommitted) path is also cleared.
	void beginComposite() {
		_composite = CompositeShape{};

		beginPath();
	}

	// Set the advance (horizontal width hint) of the composite being built.
	// Rarely needed for pure geometry scenes; more useful when integrating with
	// text layout where advance drives cursor advancement.
	void setAdvance(slug_t advance) {
		_composite.advance = advance;
	}

	// Return the in-progress CompositeShape and reset internal state.
	// The returned composite contains all Layers accumulated since the last
	// beginComposite() call (or since construction).
	CompositeShape finalize() {
		CompositeShape result = std::move(_composite);

		beginComposite();

		return result;
	}

	// Register the in-progress CompositeShape in the Atlas under @p key and reset internal state.
	// Equivalent to:
	//
	// atlas.addCompositeShape(key, canvas.finalize());
	void finalize(Key key) {
		_atlas.addCompositeShape(key, finalize());
	}

	// -------------------------------------------------------------------------
	// Accessors
	// -------------------------------------------------------------------------

	// Number of Layers accumulated in the current composite.
	size_t layerCount() const { return _composite.layers.size(); }

	// True if the pending path has any curves.
	bool hasPendingPath() const { return !_pendingCurves.empty(); }

private:
	// -------------------------------------------------------------------------
	// Internal helpers
	// -------------------------------------------------------------------------

	// Decompose a circular arc into cubic Bezier segments and emit via bezierTo().
	//
	// @p sweep - signed total sweep in radians. Positive = clockwise (Y-up).
	// Negative = counter-clockwise. Must not be zero.
	//
	// Splits the sweep into segments of at most ?/2 each (quarter-circle), which keeps the cubic
	// approximation error below 0.00027% of r. Each segment is a single bezierTo() ->
	// CurveDecomposer::cubicTo() -> adaptive subdivision.
	//
	// Does NOT call moveTo(). The caller (arc() / arcTo()) is responsible for ensuring the current
	// point is at the arc start before calling this.
	void _arcSegments(slug_t cx, slug_t cy, slug_t r, slug_t startAngle, slug_t sweep) {
		if(r <= 0.0_cv || sweep == 0.0_cv) return;

		// Number of segments: ceil(|sweep| / (?/2)), minimum 1.
		const slug_t absSweep = std::abs(sweep);
		const int nSegs = std::max(1, static_cast<int>(std::ceil(absSweep / cv(M_PI_2))));
		const slug_t segSweep = sweep / cv(nSegs); // signed per-segment sweep

		slug_t angle = startAngle;

		for(int i = 0; i < nSegs; ++i) {
			const slug_t a0 = angle;
			const slug_t a1 = angle + segSweep;

			// Cubic arc approximation for a segment of sweep segSweep.
			// k = (4/3) * tan(segSweep/4)
			const slug_t k = (4.0_cv / 3.0_cv) * std::tan(segSweep * 0.25_cv);

			const slug_t cos0 = std::cos(a0), sin0 = std::sin(a0);
			const slug_t cos1 = std::cos(a1), sin1 = std::sin(a1);

			// Arc start point (should equal current point on first segment,
			// and the end of the previous segment on subsequent ones).
			const slug_t p0x = cx + r * cos0;
			const slug_t p0y = cy + r * sin0;

			// Arc end point.
			const slug_t p3x = cx + r * cos1;
			const slug_t p3y = cy + r * sin1;

			// Control points: offset from start/end tangentially by k*r.
			const slug_t p1x = p0x - k * r * sin0;
			const slug_t p1y = p0y + k * r * cos0;
			const slug_t p2x = p3x + k * r * sin1;
			const slug_t p2y = p3y - k * r * cos1;

			// On the very first segment, snap the start to a moveTo if the path is empty; otherwise
			// emit a lineTo to connect cleanly.
			if(i == 0 && _pendingCurves.empty()) moveTo(p0x, p0y);

			else if(i == 0) {
				// Connect current position to arc start with a line (arc() callers that want a
				// seamless join should ensure their current point is already at the arc start.)
				const slug_t dx = p0x - _decomposer._x;
				const slug_t dy = p0y - _decomposer._y;

				if(dx * dx + dy * dy > 1e-10_cv) lineTo(p0x, p0y);
			}

			bezierTo(p1x, p1y, p2x, p2y, p3x, p3y);

			angle = a1;
		}
	}

	// Apply scale to every coordinate in a curve list. Fast-path returns src unchanged (no copy)
	// when scale == 1.
	static Atlas::Curves _scaleCurves(const Atlas::Curves& src, slug_t scale) {
		if(scale == 1.0_cv) return src;

		Atlas::Curves out;

		out.reserve(src.size());

		for(const auto& c : src) out.push_back({
			c.x1 * scale, c.y1 * scale,
			c.x2 * scale, c.y2 * scale,
			c.x3 * scale, c.y3 * scale
		});

		return out;
	}

	// Shift curves to local origin (bounding-box minimum -> 0,0). The subtracted offset is written
	// to @p outTransform (dx/dy only). Returns an empty vector if the bounding box is degenerate.
	//
	// TODO: I'd rather return an `std::pair` than have this `outTransform` parameter.
	static Atlas::Curves _toLocalOrigin(const Atlas::Curves& src, Matrix& outTransform) {
		outTransform = Matrix::identity();

		if(src.empty()) return {};

		slug_t minX = std::numeric_limits<slug_t>::max();
		slug_t minY = std::numeric_limits<slug_t>::max();
		slug_t maxX = -std::numeric_limits<slug_t>::max();
		slug_t maxY = -std::numeric_limits<slug_t>::max();

		for(const auto& c : src) {
			minX = std::min({minX, c.x1, c.x2, c.x3});
			minY = std::min({minY, c.y1, c.y2, c.y3});
			maxX = std::max({maxX, c.x1, c.x2, c.x3});
			maxY = std::max({maxY, c.y1, c.y2, c.y3});
		}

		if(maxX <= minX || maxY <= minY) return {};

		outTransform.dx = minX;
		outTransform.dy = minY;

		Atlas::Curves out;
		out.reserve(src.size());

		for(const auto& c : src) {
			out.push_back({
				c.x1 - minX, c.y1 - minY,
				c.x2 - minX, c.y2 - minY,
				c.x3 - minX, c.y3 - minY
			});
		}

		return out;
	}

	// -------------------------------------------------------------------------
	// Data
	// -------------------------------------------------------------------------

	Atlas& _atlas;

	// TODO: This should be a TRUE `Key` instance, and support the `uint32_t` approach of just
	// incrementing the value OR--when the value is a "string"--incrementing the number, converting
	// it to a string, and appending it! Important!
	uint32_t& _baseKey;

	// Path accumulator
	Atlas::Curves _pendingCurves;

	CurveDecomposer _decomposer;

	// Composite being built
	CompositeShape _composite;
};

}
}
