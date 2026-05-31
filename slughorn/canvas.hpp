#pragma once

// ================================================================================================
// canvas.hpp - HTML Canvas-style drawing context for slughorn
//
// Two cooperating types:
//
// Path - owns all geometry state and authoring verbs (moveTo / lineTo / arc / strokePath / etc.)
// plus the arc-length sampling API (sample / arcLength). Can be built standalone and passed
// explicitly to Canvas commit verbs, or used implicitly through the Canvas forwarding API.
// Equivalent to HTML Canvas Path2D.
//
// Canvas - stateful CompositeShape builder. Holds one implicit Path internally and forwards the
// full authoring API to it. Commit verbs (fill / stroke / defineShape / fillGradient) accept either
// the implicit path or an explicit Path argument.
//
// IMPLICIT PATH (existing style - unchanged from caller's perspective):
//
// canvas.beginPath();
// canvas.moveTo(...); canvas.lineTo(...);
// canvas.fill(color);
//
// EXPLICIT PATH (new Path2D style):
//
// slughorn::canvas::Path p;
// p.moveTo(...); p.lineTo(...);
// canvas.fill(p, color); // p is unchanged; can be reused
// canvas.stroke(p, w, color);
//
// PATH SAMPLING:
//
// auto s = p.sample(0.5); // Sample { x, y, angle } at arc-length midpoint
// auto snap = canvas.path(); // copy of internal path; sample or continue building
//
// SCALE CONTRACT
// The `scale` parameter to fill() / defineShape() is the backend normalization scale; it
// converts authoring-space coordinates into slughorn em-space [0,1]. It is NEVER stored in
// Layer::scale (reserved for FreeType2). Leave at 1.0 for all geometry authored here.
//
// No implementation guard needed. Pure C++ header, no external dependencies.
// ================================================================================================

#include "slughorn.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace slughorn {
namespace canvas {

// ================================================================================================
// detail::flattenCurve - shared by Path::strokePath and Path::_rebuildLUT
// ================================================================================================

namespace detail {

inline void flattenCurve(
	slug_t p0x, slug_t p0y,
	slug_t p1x, slug_t p1y,
	slug_t p2x, slug_t p2y,
	slug_t tol, size_t depth,
	std::vector<std::pair<slug_t, slug_t>>& pts
) {
	const slug_t dx = p2x - p0x, dy = p2y - p0y;
	const slug_t lenSq = dx * dx + dy * dy;
	const slug_t cross = (p1x - p0x) * dy - (p1y - p0y) * dx;

	if(lenSq < 1e-12_cv || (cross * cross) <= (tol * tol * lenSq) || depth >= 8) {
		pts.push_back({p2x, p2y});

		return;
	}

	const slug_t m01x = (p0x + p1x) * 0.5_cv, m01y = (p0y + p1y) * 0.5_cv;
	const slug_t m12x = (p1x + p2x) * 0.5_cv, m12y = (p1y + p2y) * 0.5_cv;
	const slug_t mx = (m01x + m12x) * 0.5_cv, my = (m01y + m12y) * 0.5_cv;

	flattenCurve(p0x, p0y, m01x, m01y, mx, my, tol, depth + 1, pts);
	flattenCurve(mx, my, m12x, m12y, p2x, p2y, tol, depth + 1, pts);
}

} // namespace detail

// ================================================================================================
// Path
// ================================================================================================

class Canvas;

class Path {
public:
	struct Sample { slug_t x, y, angle; };

	friend std::ostream& operator<<(std::ostream& os, const Sample& s);
	friend std::ostream& operator<<(std::ostream& os, const Path& p);

	Path(): _decomposer(_activeCurves) {}

	// Custom copy/move needed: CurveDecomposer.curves is a reference that must stay
	// bound to THIS object's _activeCurves, not the source's.

	Path(const Path& o):
	_activeCurves(o._activeCurves),
	_pendingCurves(o._pendingCurves),
	_penX(o._penX), _penY(o._penY),
	_ctm(o._ctm), _ctmStack(o._ctmStack),
	_decomposer(_activeCurves) {
		_decomposer.tolerance = o._decomposer.tolerance;
		_decomposer._x = o._decomposer._x;
		_decomposer._y = o._decomposer._y;
		_decomposer._sx = o._decomposer._sx;
		_decomposer._sy = o._decomposer._sy;
	}

	Path& operator=(const Path& o) {
		if(this == &o) return *this;

		_activeCurves = o._activeCurves;
		_pendingCurves = o._pendingCurves;
		_penX = o._penX; _penY = o._penY;
		_ctm = o._ctm; _ctmStack = o._ctmStack;
		_decomposer.tolerance = o._decomposer.tolerance;
		_decomposer._x = o._decomposer._x;
		_decomposer._y = o._decomposer._y;
		_decomposer._sx = o._decomposer._sx;
		_decomposer._sy = o._decomposer._sy;
		_lutDirty = true;

		return *this;
	}

	Path(Path&& o) noexcept:
	_activeCurves(std::move(o._activeCurves)),
	_pendingCurves(std::move(o._pendingCurves)),
	_penX(o._penX), _penY(o._penY),
	_ctm(o._ctm), _ctmStack(std::move(o._ctmStack)),
	_decomposer(_activeCurves) {
		_decomposer.tolerance = o._decomposer.tolerance;
		_decomposer._x = o._decomposer._x;
		_decomposer._y = o._decomposer._y;
		_decomposer._sx = o._decomposer._sx;
		_decomposer._sy = o._decomposer._sy;
	}

	Path& operator=(Path&& o) noexcept {
		if(this == &o) return *this;

		_activeCurves = std::move(o._activeCurves);
		_pendingCurves = std::move(o._pendingCurves);
		_penX = o._penX; _penY = o._penY;
		_ctm = o._ctm; _ctmStack = std::move(o._ctmStack);
		_decomposer.tolerance = o._decomposer.tolerance;
		_decomposer._x = o._decomposer._x;
		_decomposer._y = o._decomposer._y;
		_decomposer._sx = o._decomposer._sx;
		_decomposer._sy = o._decomposer._sy;
		_lutDirty = true;

		return *this;
	}

	// -------------------------------------------------------------------------
	// CurveDecomposer access
	// -------------------------------------------------------------------------

	CurveDecomposer& decomposer() { return _decomposer; }
	const CurveDecomposer& decomposer() const { return _decomposer; }

	// -------------------------------------------------------------------------
	// Path management
	// -------------------------------------------------------------------------

	// Reset all geometry state. CTM is intentionally NOT cleared - matches HTML Canvas
	// behavior where beginPath() does not affect the current transform.
	void clear() {
		_pendingCurves.clear();
		_activeCurves.clear();
		_decomposer._x = _decomposer._y = 0_cv;
		_decomposer._sx = _decomposer._sy = 0_cv;
		_penX = _penY = 0_cv;
		_lutDirty = true;
	}

	// Append all curves from @p other into this path's pending accumulator.
	// Does not affect @p other.
	void addPath(const Path& other) {
		for(const auto& c : other._pendingCurves) _pendingCurves.push_back(c);
		for(const auto& c : other._activeCurves) _pendingCurves.push_back(c);

		_lutDirty = true;
	}

	// Append curves from @p other with each control point transformed by @p transform.
	// Matches HTML Canvas Path2D.addPath(path, DOMMatrix) semantics.
	void addPath(const Path& other, const slughorn::Matrix& transform) {
		auto xform = [&](const slughorn::Atlas::Curve& c) {
			slughorn::Atlas::Curve out;
			transform.apply(c.x1, c.y1, out.x1, out.y1);
			transform.apply(c.x2, c.y2, out.x2, out.y2);
			transform.apply(c.x3, c.y3, out.x3, out.y3);
			return out;
		};

		for(const auto& c : other._pendingCurves) _pendingCurves.push_back(xform(c));
		for(const auto& c : other._activeCurves)  _pendingCurves.push_back(xform(c));

		_lutDirty = true;
	}

	// -------------------------------------------------------------------------
	// Transform stack
	//
	// CTM is applied to every path command (moveTo / lineTo / etc.) before
	// coordinates reach the decomposer. Geometry is baked in its transformed
	// state; the CTM does not affect already-accumulated curves.
	// -------------------------------------------------------------------------

	void save() { _ctmStack.push_back(_ctm); }

	void restore() {
		if(!_ctmStack.empty()) {
			_ctm = _ctmStack.back();
			_ctmStack.pop_back();
		}
	}

	void resetTransform() { _ctm = Matrix::identity(); }
	void setTransform(const Matrix& m) { _ctm = m; }
	void transform(const Matrix& m) { _ctm = _ctm * m; }

	void translate(slug_t tx, slug_t ty) {
		Matrix m;

		m.dx = tx; m.dy = ty;

		_ctm = _ctm * m;
	}

	void rotate(slug_t angle) {
		const slug_t c = std::cos(angle), s = std::sin(angle);

		Matrix m;

		m.xx = c; m.xy = -s;
		m.yx = s; m.yy = c;

		_ctm = _ctm * m;
	}

	void scale(slug_t sx, slug_t sy) {
		Matrix m;

		m.xx = sx; m.yy = sy;

		_ctm = _ctm * m;
	}

	// -------------------------------------------------------------------------
	// Path commands
	// -------------------------------------------------------------------------

	void moveTo(slug_t x, slug_t y) {
		_penX = x;
		_penY = y;

		slug_t tx, ty;

		_ctm.apply(x, y, tx, ty);

		_decomposer.moveTo(tx, ty);

		_lutDirty = true;
	}

	void lineTo(slug_t x, slug_t y) {
		_penX = x;
		_penY = y;

		slug_t tx, ty;

		_ctm.apply(x, y, tx, ty);
		_decomposer.lineTo(tx, ty);

		_lutDirty = true;
	}

	void quadTo(slug_t cx, slug_t cy, slug_t x, slug_t y) {
		_penX = x;
		_penY = y;

		slug_t tcx, tcy, tx, ty;

		_ctm.apply(cx, cy, tcx, tcy);
		_ctm.apply(x, y, tx, ty);

		_decomposer.quadTo(tcx, tcy, tx, ty);

		_lutDirty = true;
	}

	void bezierTo(slug_t c1x, slug_t c1y, slug_t c2x, slug_t c2y, slug_t x, slug_t y) {
		_penX = x;
		_penY = y;

		slug_t tc1x, tc1y, tc2x, tc2y, tx, ty;

		_ctm.apply(c1x, c1y, tc1x, tc1y);
		_ctm.apply(c2x, c2y, tc2x, tc2y);
		_ctm.apply(x, y, tx, ty);

		_decomposer.cubicTo(tc1x, tc1y, tc2x, tc2y, tx, ty);

		_lutDirty = true;
	}

	void closePath() {
		_decomposer.close();

		for(const auto& c : _activeCurves) _pendingCurves.push_back(c);

		_activeCurves.clear();

		_lutDirty = true;
	}

	// -------------------------------------------------------------------------
	// Convenience shape helpers
	//
	// These do NOT reset path state. Call clear() / beginPath() first if you
	// want a fresh path. Multiple helpers can be chained after a single clear()
	// to build compound shapes (e.g. a rect with a circular hole).
	// -------------------------------------------------------------------------

	void rect(slug_t x, slug_t y, slug_t w, slug_t h) {
		moveTo(x, y);
		lineTo(x + w, y);
		lineTo(x + w, y + h);
		lineTo(x, y + h);
		closePath();
	}

	void roundedRect(slug_t x, slug_t y, slug_t w, slug_t h, slug_t r) {
		r = std::min(r, std::min(w, h) * 0.5_cv);

		if(r <= 0_cv) { rect(x, y, w, h); return; }

		const slug_t k = 0.5522847498_cv;
		const slug_t kr = k * r;

		moveTo(x + r, y);
		lineTo(x + w - r, y);
		bezierTo(x + w - r + kr, y, x + w, y + r - kr, x + w, y + r);
		lineTo(x + w, y + h - r);
		bezierTo(x + w, y + h - r + kr, x + w - r + kr, y + h, x + w - r, y + h);
		lineTo(x + r, y + h);
		bezierTo(x + r - kr, y + h, x, y + h - r + kr, x, y + h - r);
		lineTo(x, y + r);
		bezierTo(x, y + r - kr, x + r - kr, y, x + r, y);
		closePath();
	}

	void circle(slug_t cx, slug_t cy, slug_t r) { ellipse(cx, cy, r, r); }

	void ellipse(slug_t cx, slug_t cy, slug_t rx, slug_t ry) {
		const slug_t k = 0.5522847498_cv;
		const slug_t kx = k * rx;
		const slug_t ky = k * ry;

		moveTo(cx + rx, cy);
		bezierTo(cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
		bezierTo(cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
		bezierTo(cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
		bezierTo(cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
		closePath();
	}

	// arc() does NOT call clear(). It appends to the current path, enabling composition
	// with lineTo() for pie slices, stadium shapes, etc.
	void arc(slug_t cx, slug_t cy, slug_t r, slug_t startAngle, slug_t endAngle, bool ccw=false) {
		slug_t sweep;

		if(ccw) {
			sweep = startAngle - endAngle;

			if(sweep < 0_cv) sweep += 2_cv * PI_CV;

			sweep = -sweep;
		}

		else {
			sweep = endAngle - startAngle;

			if(sweep < 0_cv) sweep += 2_cv * PI_CV;
		}

		_arcSegments(cx, cy, r, startAngle, sweep);
	}

	void arcTo(slug_t x1, slug_t y1, slug_t x2, slug_t y2, slug_t r) {
		if(r <= 0_cv) { lineTo(x1, y1); return; }

		const slug_t p0x = _penX, p0y = _penY;
		const slug_t d0x = p0x - x1, d0y = p0y - y1;
		const slug_t d1x = x2 - x1, d1y = y2 - y1;
		const slug_t len0 = std::sqrt(d0x*d0x + d0y*d0y);
		const slug_t len1 = std::sqrt(d1x*d1x + d1y*d1y);

		if(len0 < 1e-6_cv || len1 < 1e-6_cv) { lineTo(x1, y1); return; }

		const slug_t u0x = d0x / len0, u0y = d0y / len0;
		const slug_t u1x = d1x / len1, u1y = d1y / len1;
		const slug_t cross = u0x*u1y - u0y*u1x;

		if(std::abs(cross) < 1e-6_cv) { lineTo(x1, y1); return; }

		const slug_t dot = u0x*u1x + u0y*u1y;
		const slug_t tanHalf = std::abs(cross) / (1_cv + dot);

		if(tanHalf < 1e-6_cv) { lineTo(x1, y1); return; }

		const slug_t tangentDist = r / tanHalf;
		const slug_t t0x = x1 + u0x * tangentDist, t0y = y1 + u0y * tangentDist;
		const slug_t t1x = x1 + u1x * tangentDist, t1y = y1 + u1y * tangentDist;

		const slug_t sign = (cross > 0_cv) ? 1_cv : -1_cv;
		const slug_t cenX = t0x - sign * u0y * r;
		const slug_t cenY = t0y + sign * u0x * r;

		const slug_t a0 = std::atan2(t0y - cenY, t0x - cenX);
		const slug_t a1 = std::atan2(t1y - cenY, t1x - cenX);

		lineTo(t0x, t0y);
		arc(cenX, cenY, r, a0, a1, cross > 0_cv);
	}

	// -------------------------------------------------------------------------
	// Stroke expansion
	// -------------------------------------------------------------------------

	// @p cw - if false (default) the outline is appended CCW (filled area). If true the outline
	// is appended CW (subtracts coverage), enabling punch-outs when combined with a CCW outline
	// in the same beginPath() session.
	bool strokePath(slug_t width, bool cw=false) {
		Atlas::Curves centerline;

		if(!_activeCurves.empty()) centerline = std::move(_activeCurves);
		else if(!_pendingCurves.empty()) centerline = std::move(_pendingCurves);
		else return false;

		// Scale width by the CTM's pen scale so canvas.scale() affects stroke width.
		const slug_t xScale = std::sqrt(_ctm.xx * _ctm.xx + _ctm.yx * _ctm.yx);
		const slug_t yScale = std::sqrt(_ctm.xy * _ctm.xy + _ctm.yy * _ctm.yy);
		const slug_t penScale = (xScale > 0_cv && yScale > 0_cv) ? std::sqrt(xScale * yScale) : 1_cv;
		const slug_t h = width * penScale * 0.5_cv;
		const slug_t tol = _decomposer.tolerance < TOLERANCE_EXACT
			? _decomposer.tolerance
			: std::max(TOLERANCE_BALANCED, h * 0.1_cv)
		;

		// Pass 1: flatten centerline to polyline.
		std::vector<std::pair<slug_t, slug_t>> pts;

		pts.push_back({centerline.front().x1, centerline.front().y1});

		for(const auto& c : centerline) detail::flattenCurve(
			c.x1, c.y1,
			c.x2, c.y2,
			c.x3, c.y3,
			tol,
			0,
			pts
		);

		if(pts.size() < 2) return false;

		const size_t numSegs = pts.size() - 1;

		// Pass 2a: per-segment perpendicular normals.
		std::vector<std::pair<slug_t, slug_t>> segN(numSegs);

		for(size_t i = 0; i < numSegs; i++) {
			const slug_t dx = pts[i + 1].first - pts[i].first;
			const slug_t dy = pts[i + 1].second - pts[i].second;
			const slug_t len = std::sqrt(dx * dx + dy * dy);

			segN[i] = len > 1e-9_cv
				? std::pair{-dy / len, dx / len}
				: std::pair{0_cv, 1_cv}
			;
		}

		// Pass 2b: per-point miter-corrected normals.
		static constexpr slug_t MITER_LIMIT = 4_cv;

		struct PN { slug_t nx, ny; };

		const bool isClosed =
			std::abs(pts.back().first - pts.front().first) < 1e-6_cv &&
			std::abs(pts.back().second - pts.front().second) < 1e-6_cv;

		auto calcMiter = [&](size_t prev, size_t cur) -> PN {
			slug_t nx = segN[prev].first + segN[cur].first;
			slug_t ny = segN[prev].second + segN[cur].second;

			const slug_t len = std::sqrt(nx*nx + ny*ny);

			if(len > 1e-6_cv) {
				nx /= len; ny /= len;

				const slug_t d = nx * segN[cur].first + ny * segN[cur].second;

				if(d > 1e-3_cv) {
					const slug_t m = 1_cv / d;

					if(m <= MITER_LIMIT) { nx *= m; ny *= m; }

					else { nx *= MITER_LIMIT; ny *= MITER_LIMIT; }
				}
			}

			else {
				nx = segN[cur].first;
				ny = segN[cur].second;
			}

			return {nx, ny};
		};

		std::vector<PN> pn(pts.size());

		for(size_t i = 0; i < pts.size(); i++) {
			if(!i) pn[i] = isClosed ? calcMiter(numSegs-1, 0) : PN{segN[0].first, segN[0].second};

			else if(i==numSegs) pn[i] = isClosed ?
				pn[0] :
				PN{segN[numSegs-1].first, segN[numSegs-1].second}
			;

			else pn[i] = calcMiter(i-1, i);
		}

		// Pass 3: build lwall / rwall, then close.
		Atlas::Curves lwall, rwall;

		for(size_t i = 0; i < numSegs; i++) {
			const slug_t p0x = pts[i].first, p0y = pts[i].second;
			const slug_t p2x = pts[i+1].first, p2y = pts[i+1].second;
			const slug_t l0x = p0x + h*pn[i].nx, l0y = p0y + h*pn[i].ny;
			const slug_t l2x = p2x + h*pn[i+1].nx, l2y = p2y + h*pn[i+1].ny;
			const slug_t r0x = p0x - h*pn[i].nx, r0y = p0y - h*pn[i].ny;
			const slug_t r2x = p2x - h*pn[i+1].nx, r2y = p2y - h*pn[i+1].ny;

			lwall.push_back({l0x, l0y, (l0x+l2x)*0.5_cv, (l0y+l2y)*0.5_cv, l2x, l2y});
			rwall.push_back({r0x, r0y, (r0x+r2x)*0.5_cv, (r0y+r2y)*0.5_cv, r2x, r2y});
		}

		if(lwall.empty()) return false;

		Atlas::Curves outline;

		for(const auto& r : rwall) outline.push_back(r);

		{
			const slug_t ax = rwall.back().x3, ay = rwall.back().y3;
			const slug_t bx = lwall.back().x3, by = lwall.back().y3;

			outline.push_back({ax, ay, (ax+bx)*0.5_cv, (ay+by)*0.5_cv, bx, by});
		}

		for(size_t i = lwall.size(); i-- > 0;) {
			const auto& l = lwall[i];

			outline.push_back({l.x3, l.y3, l.x2, l.y2, l.x1, l.y1});
		}

		{
			const slug_t ax = lwall.front().x1, ay = lwall.front().y1;
			const slug_t bx = rwall.front().x1, by = rwall.front().y1;

			outline.push_back({ax, ay, (ax+bx)*0.5_cv, (ay+by)*0.5_cv, bx, by});
		}

		if(!cw) for(const auto& c : outline) _pendingCurves.push_back(c);

		else for(size_t i = outline.size(); i-- > 0;) {
			const auto& c = outline[i];

			_pendingCurves.push_back({c.x3, c.y3, c.x2, c.y2, c.x1, c.y1});
		}

		_lutDirty = true;

		return true;
	}

	// -------------------------------------------------------------------------
	// Accessors
	// -------------------------------------------------------------------------

	bool hasPendingPath() const { return !_pendingCurves.empty() || !_activeCurves.empty(); }

	// -------------------------------------------------------------------------
	// Arc-length sampling
	//
	// sample(t) returns the position and tangent direction at normalized arc-length
	// t in the range [0, 1]. The LUT is rebuilt lazily whenever the path geometry changes.
	// -------------------------------------------------------------------------

	slug_t arcLength() const {
		if(_lutDirty) _rebuildLUT();

		return _totalLength;
	}

	Sample sample(slug_t t) const {
		if(_lutDirty) _rebuildLUT();

		if(_pts.size() < 2) return {};

		const slug_t s = std::max(0_cv, std::min(1_cv, t)) * _totalLength;

		auto it = std::lower_bound(_lut.begin(), _lut.end(), s);
		size_t i = static_cast<size_t>(std::distance(_lut.begin(), it));

		i = std::min(i, _pts.size() - 1);

		if(!i) i = 1;

		const slug_t segLen = _lut[i] - _lut[i-1];
		const slug_t frac = segLen > 1e-12_cv ? (s - _lut[i-1]) / segLen : 0_cv;

		const slug_t x = _pts[i-1].first + frac * (_pts[i].first - _pts[i-1].first);
		const slug_t y = _pts[i-1].second + frac * (_pts[i].second - _pts[i-1].second);

		const slug_t angle = std::atan2(
			_pts[i].second - _pts[i-1].second,
			_pts[i].first - _pts[i-1].first
		);

		return { x, y, angle };
	}

private:
	friend class Canvas;

	void _arcSegments(slug_t cx, slug_t cy, slug_t r, slug_t startAngle, slug_t sweep) {
		if(r <= 0_cv || sweep == 0_cv) return;

		const slug_t absSweep = std::abs(sweep);
		const size_t nSegs = std::max(size_t{1}, static_cast<size_t>(std::ceil(absSweep / PI_2_CV)));
		const slug_t segSweep = sweep / cv(nSegs);

		slug_t angle = startAngle;

		for(size_t i = 0; i < nSegs; i++) {
			const slug_t a0 = angle, a1 = angle + segSweep;
			const slug_t k = (4_cv / 3_cv) * std::tan(segSweep * 0.25_cv);
			const slug_t cos0 = std::cos(a0), sin0 = std::sin(a0);
			const slug_t cos1 = std::cos(a1), sin1 = std::sin(a1);

			const slug_t p0x = cx + r*cos0, p0y = cy + r*sin0;
			const slug_t p3x = cx + r*cos1, p3y = cy + r*sin1;
			const slug_t p1x = p0x - k*r*sin0, p1y = p0y + k*r*cos0;
			const slug_t p2x = p3x + k*r*sin1, p2y = p3y - k*r*cos1;

			if(!i && _activeCurves.empty() && _pendingCurves.empty()) moveTo(p0x, p0y);

			else if(!i) {
				const slug_t dx = p0x - _penX, dy = p0y - _penY;

				if(dx*dx + dy*dy > 1e-10_cv) lineTo(p0x, p0y);
			}

			bezierTo(p1x, p1y, p2x, p2y, p3x, p3y);

			angle = a1;
		}
	}

	void _rebuildLUT() const {
		_pts.clear();
		_lut.clear();

		_totalLength = 0_cv;

		Atlas::Curves all = _pendingCurves;

		for(const auto& c : _activeCurves) all.push_back(c);

		if(!all.empty()) {
			_pts.push_back({all.front().x1, all.front().y1});

			for(const auto& c : all) detail::flattenCurve(
				c.x1, c.y1,
				c.x2, c.y2,
				c.x3, c.y3,
				TOLERANCE_BALANCED,
				0,
				_pts
			);

			_lut.resize(_pts.size(), 0_cv);

			for(size_t i = 1; i < _pts.size(); i++) {
				const slug_t dx = _pts[i].first - _pts[i-1].first;
				const slug_t dy = _pts[i].second - _pts[i-1].second;

				_lut[i] = _lut[i-1] + std::sqrt(dx*dx + dy*dy);
			}

			_totalLength = _lut.back();
		}

		_lutDirty = false;
	}

	// _activeCurves MUST be declared before _decomposer (reference binding order).
	Atlas::Curves _activeCurves;
	Atlas::Curves _pendingCurves;
	slug_t _penX = 0_cv, _penY = 0_cv;
	Matrix _ctm = Matrix::identity();
	std::vector<Matrix> _ctmStack;
	CurveDecomposer _decomposer;

	mutable std::vector<std::pair<slug_t, slug_t>> _pts;
	mutable std::vector<slug_t> _lut;
	mutable slug_t _totalLength = 0_cv;
	mutable bool _lutDirty = true;
};

// ================================================================================================
// Text layout enums
// ================================================================================================

enum class TextAnchorY {
	Baseline, // y is the text baseline (default)
	CapCenter, // y is the vertical center of the cap-height band
	CapTop, // y is the top of capital letters
	XCenter, // y is the vertical center of the x-height band
};

enum class TextAlignX {
	Left, // x is the left edge of the first glyph (default, single-pass)
	Center, // x is the horizontal center of the run (two-pass)
	Right, // x is the right edge of the last glyph (two-pass)
};

// ================================================================================================
// Canvas
// ================================================================================================

class Canvas {
public:
	friend std::ostream& operator<<(std::ostream& os, const Canvas& c);

	Canvas(Atlas& atlas, KeyIterator key=KeyIterator()):
	_atlas(atlas),
	_key(key) {}

	// -------------------------------------------------------------------------
	// CurveDecomposer access (forwarded to internal Path)
	// -------------------------------------------------------------------------

	CurveDecomposer& decomposer() { return _path.decomposer(); }
	const CurveDecomposer& decomposer() const { return _path.decomposer(); }

	// -------------------------------------------------------------------------
	// Path snapshot
	//
	// Returns a copy of the internal path. The copy can be sampled, modified
	// independently, or passed back to explicit-path commit verbs. The internal
	// path is left completely intact.
	// -------------------------------------------------------------------------

	Path path() const { return _path; }

	// -------------------------------------------------------------------------
	// Transform stack
	//
	// Two parallel CTMs are maintained and kept in sync:
	//   _path._ctm  - bakes transforms into internal path geometry at moveTo/lineTo time
	//   _ctm        - applied as a placement transform for external-Path commit verbs
	//
	// Internal path commits (fill(Color), stroke(width, Color), etc.) rely on geometry
	// already baked by _path._ctm, so they do not additionally compose _ctm.
	// External path commits (fill(Path, Color), etc.) compose _ctm into the layer
	// transform so that translate/rotate/scale correctly position pre-built paths.
	// Text always composes _ctm since glyph positions are computed in canvas space.
	// save() / restore() checkpoint both CTMs together.
	// -------------------------------------------------------------------------

	void save() {
		_path.save();
		_ctmStack.push_back(_ctm);
	}

	void restore() {
		_path.restore();

		if(!_ctmStack.empty()) {
			_ctm = _ctmStack.back();

			_ctmStack.pop_back();
		}
	}

	const Matrix& getTransform() const { return _ctm; }
	void resetTransform() { _path.resetTransform(); _ctm = Matrix::identity(); }
	void setTransform(const Matrix& m) { _path.setTransform(m); _ctm = m; }
	void transform(const Matrix& m) { _path.transform(m); _ctm = _ctm * m; }

	void translate(slug_t tx, slug_t ty) {
		_path.translate(tx, ty);

		Matrix m;

		m.dx = tx; m.dy = ty;

		_ctm = _ctm * m;
	}

	void rotate(slug_t angle) {
		_path.rotate(angle);

		const slug_t c = std::cos(angle), s = std::sin(angle);

		Matrix m;

		m.xx = c; m.xy = -s;
		m.yx = s; m.yy = c;

		_ctm = _ctm * m;
	}

	void scale(slug_t sx, slug_t sy) {
		_path.scale(sx, sy);

		Matrix m;

		m.xx = sx; m.yy = sy;

		_ctm = _ctm * m;
	}

	// -------------------------------------------------------------------------
	// Path commands (forwarded to internal Path)
	// -------------------------------------------------------------------------

	void beginPath() { _path.clear(); }
	void addPath(const Path& other) { _path.addPath(other); }
	void addPath(const Path& other, const Matrix& transform) { _path.addPath(other, transform); }
	void moveTo(slug_t x, slug_t y) { _path.moveTo(x, y); }
	void lineTo(slug_t x, slug_t y) { _path.lineTo(x, y); }
	void quadTo(slug_t cx, slug_t cy, slug_t x, slug_t y) { _path.quadTo(cx, cy, x, y); }

	void bezierTo(slug_t c1x, slug_t c1y, slug_t c2x, slug_t c2y, slug_t x, slug_t y) {
		_path.bezierTo(c1x, c1y, c2x, c2y, x, y);
	}

	void closePath() { _path.closePath(); }
	bool strokePath(slug_t width, bool cw=false) { return _path.strokePath(width, cw); }
	bool hasPendingPath() const { return _path.hasPendingPath(); }

	// -------------------------------------------------------------------------
	// Shape helpers (forwarded to internal Path)
	// -------------------------------------------------------------------------

	void rect(slug_t x, slug_t y, slug_t w, slug_t h) { _path.rect(x, y, w, h); }
	void roundedRect(slug_t x, slug_t y, slug_t w, slug_t h, slug_t r) { _path.roundedRect(x, y, w, h, r); }
	void circle(slug_t cx, slug_t cy, slug_t r) { _path.circle(cx, cy, r); }
	void ellipse(slug_t cx, slug_t cy, slug_t rx, slug_t ry) { _path.ellipse(cx, cy, rx, ry); }

	void arc(slug_t cx, slug_t cy, slug_t r, slug_t startAngle, slug_t endAngle, bool ccw=false) {
		_path.arc(cx, cy, r, startAngle, endAngle, ccw);
	}

	void arcTo(slug_t x1, slug_t y1, slug_t x2, slug_t y2, slug_t r) {
		_path.arcTo(x1, y1, x2, y2, r);
	}

	// -------------------------------------------------------------------------
	// Gradient factory
	// -------------------------------------------------------------------------

	struct GradientHandle {
		GradientInfo::Type type = GradientInfo::Type::Linear;

		slug_t x0 = 0_cv, y0 = 0_cv, x1 = 1_cv, y1 = 0_cv;

		std::vector<GradientStop> stops;
	};

	GradientHandle createLinearGradient(
		slug_t x0, slug_t y0,
		slug_t x1, slug_t y1,
		std::vector<GradientStop> stops
	) {
		return { GradientInfo::Type::Linear, x0, y0, x1, y1, std::move(stops) };
	}

	GradientHandle createRadialGradient(
		slug_t cx, slug_t cy,
		slug_t r0, slug_t r1,
		std::vector<GradientStop> stops
	) {
		return { GradientInfo::Type::Radial, cx, cy, r0, r1, std::move(stops) };
	}

	GradientHandle createSweepGradient(
		slug_t cx, slug_t cy,
		slug_t startAngle, slug_t endAngle,
		std::vector<GradientStop> stops
	) {
		return { GradientInfo::Type::Sweep, cx, cy, startAngle, endAngle, std::move(stops) };
	}

	// -------------------------------------------------------------------------
	// Band split state (persists like fillStyle - applies to subsequent commits)
	//
	// setSplits() - explicit normalized [0,1] fractions; clears any strategy
	// setSplitStrategy() - callable computed from curves at commit time; clears explicit splits
	// clearSplits() - revert to default auto-band behavior
	// -------------------------------------------------------------------------

	void setSplits(std::vector<slug_t> splitsX, std::vector<slug_t> splitsY) {
		_splitsX = std::move(splitsX);
		_splitsY = std::move(splitsY);
		_splitStrategy = {};
	}

	void setSplitStrategy(Atlas::SplitStrategy strategy) {
		_splitStrategy = std::move(strategy);

		_splitsX.clear();
		_splitsY.clear();
	}

	void clearSplits() {
		_splitsX.clear();
		_splitsY.clear();

		_splitStrategy = {};
	}

	// -------------------------------------------------------------------------
	// Commit verbs - implicit path
	//
	// Operate on the Canvas's internal path. After fill() / stroke(), the
	// accumulated curves remain in the path until the next beginPath() call.
	// -------------------------------------------------------------------------

	Key fill(Color color, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		_consolidate();

		if(_path._pendingCurves.empty()) return Key(0u);

		return _commitFill(_path._pendingCurves, color, scale, _key.next(), origin);
	}

	Key fill(Color color, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		_consolidate();

		if(_path._pendingCurves.empty()) return Key(0u);

		return _commitFill(_path._pendingCurves, color, scale, key, origin);
	}

	bool defineShape(Key key, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		_consolidate();

		return _commitShape(_path._pendingCurves, key, scale, origin);
	}

	Key stroke(slug_t width, Color color, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		if(!_path.strokePath(width)) return Key(0u);

		return _commitFill(_path._pendingCurves, color, scale, _key.next(), origin);
	}

	Key stroke(slug_t width, Color color, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		if(!_path.strokePath(width)) return Key(0u);

		return _commitFill(_path._pendingCurves, color, scale, key, origin);
	}

	Key fillGradient(const GradientHandle& handle, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		_consolidate();

		if(_path._pendingCurves.empty()) return Key(0u);

		Key k = _commitGradient(_path._pendingCurves, handle, scale, _key.next(), origin);

		_path._pendingCurves.clear();

		return k;
	}

	Key fillGradient(const GradientHandle& handle, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		_consolidate();

		if(_path._pendingCurves.empty()) return Key(0u);

		Key k = _commitGradient(_path._pendingCurves, handle, scale, key, origin);

		_path._pendingCurves.clear();

		return k;
	}

	Key strokeGradient(slug_t width, const GradientHandle& handle, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		if(!_path.strokePath(width)) return Key(0u);

		Key k = _commitGradient(_path._pendingCurves, handle, scale, _key.next(), origin);

		_path._pendingCurves.clear();

		return k;
	}

	Key strokeGradient(slug_t width, const GradientHandle& handle, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		if(!_path.strokePath(width)) return Key(0u);

		Key k = _commitGradient(_path._pendingCurves, handle, scale, key, origin);

		_path._pendingCurves.clear();

		return k;
	}

	// -------------------------------------------------------------------------
	// Commit verbs - explicit Path
	//
	// Accept a standalone Path. The path is not modified by any of these calls.
	// stroke() makes an internal copy to expand the stroke without altering @p p.
	// -------------------------------------------------------------------------

	Key fill(const Path& p, Color color, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		if(!p.hasPendingPath()) return Key(0u);

		return _commitFill(_merged(p), color, scale, _key.next(), origin, _ctm);
	}

	Key fill(const Path& p, Color color, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		if(!p.hasPendingPath()) return Key(0u);

		return _commitFill(_merged(p), color, scale, key, origin, _ctm);
	}

	bool defineShape(const Path& p, Key key, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		if(!p.hasPendingPath()) return false;

		return _commitShape(_merged(p), key, scale, origin);
	}

	Key stroke(const Path& p, slug_t width, Color color, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		Path copy = p;

		if(!copy.strokePath(width)) return Key(0u);

		return _commitFill(_merged(copy), color, scale, _key.next(), origin, _ctm);
	}

	Key stroke(const Path& p, slug_t width, Color color, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		Path copy = p;

		if(!copy.strokePath(width)) return Key(0u);

		return _commitFill(_merged(copy), color, scale, key, origin, _ctm);
	}

	Key fillGradient(const Path& p, const GradientHandle& handle, slug_t scale=1_cv, Atlas::ShapeInfo::Origin origin={}) {
		if(!p.hasPendingPath()) return Key(0u);

		return _commitGradient(_merged(p), handle, scale, _key.next(), origin, _ctm);
	}

	Key fillGradient(const Path& p, const GradientHandle& handle, slug_t scale, Key key, Atlas::ShapeInfo::Origin origin={}) {
		if(!p.hasPendingPath()) return Key(0u);

		return _commitGradient(_merged(p), handle, scale, key, origin, _ctm);
	}

	// -------------------------------------------------------------------------
	// Text layout
	//
	// Places glyphs from the atlas into the current composite. The atlas must
	// already contain the requested codepoints (loaded via a font backend before
	// atlas->build()). Handles em-space conversion, vertical anchoring, and
	// optional horizontal alignment internally.
	// -------------------------------------------------------------------------

	void text(
		std::string_view str,
		slug_t fontSize,
		slug_t x,
		slug_t y,
		Color color,
		const FontMetrics& metrics,
		TextAnchorY anchorY=TextAnchorY::Baseline,
		TextAlignX alignX=TextAlignX::Left
	) {
		if(str.empty() || fontSize == 0_cv) return;

		// Apply CTM so text() uses the same coordinate space as path operations.
		slug_t tx, ty;
		_ctm.apply(x, y, tx, ty);

		// Baseline dy in em-space, adjusted for vertical anchor
		slug_t dy = ty / fontSize;

		switch(anchorY) {
			case TextAnchorY::Baseline: break;
			case TextAnchorY::CapCenter: dy -= metrics.capHeightRatio * 0.5_cv; break;
			case TextAnchorY::CapTop: dy -= metrics.capHeightRatio; break;
			case TextAnchorY::XCenter: dy -= metrics.xHeightRatio * 0.5_cv; break;
		}

		// Starting dx in em-space; two-pass only for Center/Right
		slug_t dx = tx / fontSize;

		if(alignX != TextAlignX::Left) {
			slug_t totalAdvance = 0_cv;

			for(char c : str) {
				const auto* shape = _atlas.getShape(Key(static_cast<uint32_t>(static_cast<unsigned char>(c))));

				totalAdvance += shape ? shape->advance : 0.6_cv;
			}

			if(alignX == TextAlignX::Center) dx -= totalAdvance * 0.5_cv;

			else dx -= totalAdvance;
		}

		for(char c : str) {
			const uint32_t cp = static_cast<uint32_t>(static_cast<unsigned char>(c));
			const auto* shape = _atlas.getShape(Key(cp));

			Layer layer;

			layer.key = Key(cp);
			layer.color = color;
			layer.transform = {.x = dx, .y = dy};
			layer.scale = fontSize;

			_composite.layers.push_back(layer);

			dx += shape ? shape->advance : 0.6_cv;
		}
	}

	// -------------------------------------------------------------------------
	// CompositeShape management
	// -------------------------------------------------------------------------

	void beginComposite() {
		_composite = CompositeShape{};

		beginPath();
	}

	void setAdvance(slug_t advance) { _composite.advance = advance; }

	CompositeShape finalize() {
		CompositeShape result = std::move(_composite);

		beginComposite();

		return result;
	}

	void finalize(Key key) { _atlas.addCompositeShape(key, finalize()); }

	size_t layerCount() const { return _composite.layers.size(); }

private:
	// Merge _activeCurves into _pendingCurves on the internal path.
	void _consolidate() {
		for(const auto& c : _path._activeCurves) _path._pendingCurves.push_back(c);

		_path._activeCurves.clear();
	}

	// Return a merged (active + pending) copy of @p p's curves without consuming them.
	static Atlas::Curves _merged(const Path& p) {
		Atlas::Curves out = p._pendingCurves;

		for(const auto& c : p._activeCurves) out.push_back(c);

		return out;
	}

	// Core fill commit: scale, apply origin, register in atlas, push Layer.
	Key _commitFill(
		const Atlas::Curves& curves,
		Color color,
		slug_t scale,
		Key key,
		Atlas::ShapeInfo::Origin origin,
		const Matrix& placement=Matrix::identity()
	) {
		if(curves.empty()) return Key(0u);

		Atlas::Curves scaled = _scaleCurves(curves, scale);

		Atlas::ShapeInfo::Origin infoOrigin = origin;

		if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot && !scaled.empty()) {
			slug_t minX_em = std::numeric_limits<slug_t>::max();
			slug_t minY_em = std::numeric_limits<slug_t>::max();

			for(const auto& c : scaled) {
				minX_em = std::min({minX_em, c.x1, c.x2, c.x3});
				minY_em = std::min({minY_em, c.y1, c.y2, c.y3});
			}

			infoOrigin.x = origin.x * scale - minX_em;
			infoOrigin.y = origin.y * scale - minY_em;
		}

		else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
			infoOrigin.x = origin.x * scale;
			infoOrigin.y = origin.y * scale;
		}

		auto [local, transform] = _toLocalOrigin(scaled, origin, scale);

		if(local.empty()) return Key(0u);

		Atlas::ShapeInfo info;

		info.curves = std::move(local);
		info.origin = infoOrigin;

		_applySplits(info);
		_atlas.addShape(key, info);

		Layer layer;

		layer.key = key;
		layer.color = color;
		placement.apply(transform.dx, transform.dy, layer.transform.x, layer.transform.y);

		_composite.layers.push_back(layer);

		return key;
	}

	// Shape-only commit: no Layer pushed, no color.
	bool _commitShape(
		const Atlas::Curves& curves,
		Key key,
		slug_t scale,
		Atlas::ShapeInfo::Origin origin
	) {
		if(curves.empty()) return false;

		Atlas::Curves scaled = _scaleCurves(curves, scale);

		Atlas::ShapeInfo::Origin infoOrigin = origin;

		if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot && !scaled.empty()) {
			slug_t minX_em = std::numeric_limits<slug_t>::max();
			slug_t minY_em = std::numeric_limits<slug_t>::max();

			for(const auto& c : scaled) {
				minX_em = std::min({minX_em, c.x1, c.x2, c.x3});
				minY_em = std::min({minY_em, c.y1, c.y2, c.y3});
			}

			infoOrigin.x = origin.x * scale - minX_em;
			infoOrigin.y = origin.y * scale - minY_em;
		}

		else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
			infoOrigin.x = origin.x * scale;
			infoOrigin.y = origin.y * scale;
		}

		Atlas::Curves local = _toLocalOrigin(scaled).first;

		if(local.empty()) return false;

		Atlas::ShapeInfo info;

		info.curves = std::move(local);
		info.origin = infoOrigin;

		_applySplits(info);
		_atlas.addShape(key, info);

		return true;
	}

	// Gradient fill commit.
	Key _commitGradient(
		const Atlas::Curves& curves,
		const GradientHandle& handle,
		slug_t scale,
		Key key,
		Atlas::ShapeInfo::Origin origin,
		const Matrix& placement = Matrix::identity()
	) {
		if(curves.empty()) return Key(0u);

		Atlas::Curves scaled = _scaleCurves(curves, scale);

		slug_t minX = std::numeric_limits<slug_t>::max();
		slug_t minY = std::numeric_limits<slug_t>::max();

		for(const auto& c : scaled) {
			minX = std::min({minX, c.x1, c.x2, c.x3});
			minY = std::min({minY, c.y1, c.y2, c.y3});
		}

		Atlas::ShapeInfo::Origin infoOrigin = origin;

		if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
			infoOrigin.x = origin.x * scale - minX;
			infoOrigin.y = origin.y * scale - minY;
		}

		else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
			infoOrigin.x = origin.x * scale;
			infoOrigin.y = origin.y * scale;
		}

		auto [local, transform] = _toLocalOrigin(scaled, origin, scale);

		if(local.empty()) return Key(0u);

		GradientInfo ginfo;

		ginfo.type = handle.type;
		ginfo.stops = handle.stops;

		if(handle.type == GradientInfo::Type::Radial) {
			const slug_t cx = handle.x0 * scale - minX;
			const slug_t cy = handle.y0 * scale - minY;
			const slug_t r0 = handle.x1 * scale;
			const slug_t r1 = handle.y1 * scale;

			ginfo.innerRadius = r0;
			ginfo.transform = buildRadialGradientMatrix(cx, cy, r1);
		}

		else if(handle.type == GradientInfo::Type::Sweep) {
			const slug_t cx = handle.x0 * scale - minX;
			const slug_t cy = handle.y0 * scale - minY;

			ginfo.transform = buildSweepGradientMatrix(cx, cy, handle.x1, handle.y1 - handle.x1);
		}

		else {
			ginfo.transform = buildLinearGradientMatrix(
				handle.x0 * scale - minX, handle.y0 * scale - minY,
				handle.x1 * scale - minX, handle.y1 * scale - minY
			);
		}

		const uint32_t gid = _atlas.addGradient(ginfo);

		Atlas::ShapeInfo info;

		info.curves = std::move(local);
		info.origin = infoOrigin;

		_applySplits(info);
		_atlas.addShape(key, info);

		Layer layer;

		layer.key = key;
		layer.color = {};
		placement.apply(transform.dx, transform.dy, layer.transform.x, layer.transform.y);
		layer.gradientId = gid;

		_composite.layers.push_back(layer);

		return key;
	}

	void _applySplits(Atlas::ShapeInfo& info) const {
		if(_splitStrategy) {
			auto [sx, sy] = _splitStrategy(info.curves);
			info.splitsX = std::move(sx);
			info.splitsY = std::move(sy);
		}
		else {
			info.splitsX = _splitsX;
			info.splitsY = _splitsY;
		}
	}

	static Atlas::Curves _scaleCurves(const Atlas::Curves& src, slug_t scale) {
		if(scale == 1_cv) return src;

		Atlas::Curves out;

		out.reserve(src.size());

		for(const auto& c : src) out.push_back({
			c.x1 * scale, c.y1 * scale,
			c.x2 * scale, c.y2 * scale,
			c.x3 * scale, c.y3 * scale
		});

		return out;
	}

	static std::pair<Atlas::Curves, Matrix> _toLocalOrigin(
		const Atlas::Curves& src,
		Atlas::ShapeInfo::Origin origin={},
		slug_t scale=1_cv
	) {
		if(src.empty()) return { {}, Matrix::identity() };

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

		if(maxX <= minX || maxY <= minY) return { {}, Matrix::identity() };

		Matrix transform = Matrix::identity();

		if(origin.type == Atlas::ShapeInfo::Origin::Type::Centered) {
			transform.dx = (minX + maxX) * 0.5_cv;
			transform.dy = (minY + maxY) * 0.5_cv;
		}

		else if(origin.type == Atlas::ShapeInfo::Origin::Type::Pivot) {
			transform.dx = origin.x * scale;
			transform.dy = origin.y * scale;
		}

		else if(origin.type == Atlas::ShapeInfo::Origin::Type::Custom) {
			// computeQuad subtracts originX/Y from transform, so pre-add it here so the
			// subtraction cancels out and the quad lands at the bbox corner (same as Default).
			// originX/Y still holds the raw user value for the shader.
			transform.dx = minX + origin.x * scale;
			transform.dy = minY + origin.y * scale;
		}

		else {
			transform.dx = minX;
			transform.dy = minY;
		}

		Atlas::Curves out;

		out.reserve(src.size());

		for(const auto& c : src) out.push_back({
			c.x1 - minX, c.y1 - minY,
			c.x2 - minX, c.y2 - minY,
			c.x3 - minX, c.y3 - minY
		});

		return { out, transform };
	}

	Atlas& _atlas;
	KeyIterator _key;
	Path _path;
	CompositeShape _composite;
	Matrix _ctm = Matrix::identity();
	std::vector<Matrix> _ctmStack;
	std::vector<slug_t> _splitsX;
	std::vector<slug_t> _splitsY;
	Atlas::SplitStrategy _splitStrategy;
};

// ================================================================================================
// Debugging Helpers
// ================================================================================================

inline std::ostream& operator<<(std::ostream& os, const Path::Sample& s) {
	return os << "Sample(x=" << s.x << " y=" << s.y << " angle=" << s.angle << ")";
}

inline std::ostream& operator<<(std::ostream& os, const Path& p) {
	return os
		<< "Path(activeCurves=" << p._activeCurves.size()
		<< " pendingCurves=" << p._pendingCurves.size()
		<< " pen=(" << p._penX << "," << p._penY << ")"
		<< " ctm=" << p._ctm
		<< " ctmStack=" << p._ctmStack.size()
		<< " lutDirty=" << p._lutDirty
		<< " totalLength=" << p._totalLength << ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Canvas::GradientHandle& g) {
	return os
		<< "GradientHandle(type=" << g.type
		<< " p0=(" << g.x0 << "," << g.y0 << ")"
		<< " p1=(" << g.x1 << "," << g.y1 << ")"
		<< " stops=" << g.stops.size() << ")"
	;
}

inline std::ostream& operator<<(std::ostream& os, const Canvas& c) {
	return os
		<< "Canvas(key=" << c._key
		<< " path=" << c._path
		<< " composite=" << c._composite << ")"
	;
}

}
}
