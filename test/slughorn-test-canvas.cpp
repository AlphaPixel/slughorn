//vimrun! ./slughorn-test-canvas
//
// Canvas API demonstration: all commit patterns.
//
// There are three path-commit verbs:
//
// fill(color, scale, key?) - commit as colored Layer
// stroke(width, color, scale, key?) - expand + commit as colored Layer
// defineShape(key, scale) - commit as geometry only (no Layer, no color)
//
// And one composite-commit verb:
//
// finalize() - return in-progress CompositeShape, reset state
// finalize(key) - register CompositeShape in Atlas + reset
//
// strokePath(width) is the in-place path transformer for the rare case
// where you need the raw outline before deciding how to commit it.
//
// +-------------------------+---------------------------------+------------------------------+
// |           Key           |              Type               |           Pattern            |
// +-------------------------+---------------------------------+------------------------------+
// | s_0                     | shape (auto-key)                | 1 - fill auto-key            |
// +-------------------------+---------------------------------+------------------------------+
// | tri_composite           | composite [s_0]                 | 1                            |
// +-------------------------+---------------------------------+------------------------------+
// | circle_shape            | shape (named)                   | 2 - fill named               |
// +-------------------------+---------------------------------+------------------------------+
// | circle_composite        | composite [circle_shape]        | 2                            |
// +-------------------------+---------------------------------+------------------------------+
// | s_1..s_3                | shapes (auto-key)               | 3 - multi-layer auto         |
// +-------------------------+---------------------------------+------------------------------+
// | three_layer             | composite [s_1, s_2, s_3]       | 3                            |
// +-------------------------+---------------------------------+------------------------------+
// | badge_bg, badge_bar     | shapes (named)                  | 4 - multi-layer named        |
// +-------------------------+---------------------------------+------------------------------+
// | badge_composite         | composite [badge_bg, badge_bar] | 4                            |
// +-------------------------+---------------------------------+------------------------------+
// | rrect_geom              | shape (geometry-only)           | 5 - defineShape              |
// +-------------------------+---------------------------------+------------------------------+
// | s_4                     | shape (auto-key stroke)         | 6 - stroke commit            |
// +-------------------------+---------------------------------+------------------------------+
// | scurve_stroke_composite | composite [s_4]                 | 6                            |
// +-------------------------+---------------------------------+------------------------------+
// | zigzag_stroke           | shape (named stroke)            | 7 - stroke named             |
// +-------------------------+---------------------------------+------------------------------+
// | scurve_outline_geom     | shape (geometry-only)           | 8 - strokePath + defineShape |
// +-------------------------+---------------------------------+------------------------------+
// | stadium_arcto           | shape (named)                   | 9 - arcTo                    |
// +-------------------------+---------------------------------+------------------------------+
// | stadium_composite       | composite [stadium_arcto]       | 9                            |
// +-------------------------+---------------------------------+------------------------------+

#include "slughorn/canvas.hpp"

#ifndef SLUGHORN_HAS_SERIAL
#  error "This test requires SLUGHORN_SERIAL=ON"
#endif

#include "slughorn/serial.hpp"

#include <fstream>
#include <iostream>

using namespace slughorn::literals;
using slughorn::Color;
using slughorn::Key;
using slughorn::slug_t;

// All shapes authored in [0, 1] em-space; scale = 1.0 throughout.
static const Color RED = {1_cv, 0_cv, 0_cv, 1_cv};
static const Color GREEN = {0_cv, 0.6_cv, 0_cv, 1_cv};
static const Color BLUE = {0_cv, 0_cv, 1_cv, 1_cv};
static const Color WHITE = {1_cv, 1_cv, 1_cv, 1_cv};
static const Color CYAN = {0_cv, 0.8_cv, 0.8_cv, 1_cv};
static const Color GOLD = {1_cv, 0.75_cv, 0_cv, 1_cv};

int main(int argc, char** argv) {
	slughorn::Atlas atlas;

	// KeyIterator prefix "s" produces auto-keys "s_0", "s_1", ... when fill()
	// or stroke() is called without an explicit key.
	slughorn::canvas::Canvas canvas(atlas, slughorn::KeyIterator("s"));

	canvas.decomposer().tolerance = slughorn::TOLERANCE_BALANCED;

	// ============================================================================================
	// Pattern 1: fill(color) auto-key shape -> finalize(key) names composite.
	//
	// The shape gets an auto-key ("s_0"), the composite gets the named key.
	// Most convenient for single-use geometry you never need to look up directly.
	//
	// CLI: `slughorn render atlas.slug tri_composite` resolves via composite
	//      fallback to the single layer's shape.
	// ============================================================================================

	canvas.beginPath();
	canvas.moveTo(0.5_cv, 0.9_cv);
	canvas.lineTo(0.9_cv, 0.1_cv);
	canvas.lineTo(0.1_cv, 0.1_cv);
	canvas.closePath();
	canvas.fill(RED);

	// canvas.finalize(Key("tri_composite"));
	canvas.finalize("tri_composite");

	// ============================================================================================
	// Pattern 2: fill(color, scale, key) named shape -> finalize(key).
	//
	// Shape and composite both get explicit names. Use when you need the shape directly addressable
	// (e.g. CLI `render`, Python atlas.get_shape(), or sharing the geometry with another Layer
	// later).
	// ============================================================================================

	canvas.circle(0.5_cv, 0.5_cv, 0.4_cv);
	canvas.fill(BLUE, 1.0_cv, Key("circle_shape"));

	canvas.finalize(Key("circle_composite"));

	// ============================================================================================
	// Pattern 3: Multi-layer composite with auto-key shapes.
	//
	// Each fill() generates a new auto-key shape and appends a Layer. All three accumulate before
	// finalize(key) registers the composite. The standard pattern for building a scene with
	// multiple colored regions.
	// ============================================================================================

	canvas.rect(0.05_cv, 0.05_cv, 0.9_cv, 0.9_cv);
	canvas.fill(RED); // "s_2"

	canvas.circle(0.5_cv, 0.5_cv, 0.35_cv);
	canvas.fill(BLUE); // "s_3"

	canvas.roundedRect(0.25_cv, 0.25_cv, 0.5_cv, 0.5_cv, 0.08_cv);
	canvas.fill(GREEN); // "s_4"

	canvas.finalize(Key("three_layer"));

	// ============================================================================================
	// Pattern 4: Multi-layer composite with named shapes.
	//
	// Each layer's shape is independently addressable AND part of the composite. Use when the
	// caller needs both fine-grained shape access and the composite as a unit (e.g. one layer gets
	// a hover highlight, the others do not).
	// ============================================================================================

	canvas.ellipse(0.5_cv, 0.5_cv, 0.45_cv, 0.28_cv);
	canvas.fill(CYAN, 1.0_cv, Key("badge_bg"));

	canvas.roundedRect(0.15_cv, 0.35_cv, 0.7_cv, 0.3_cv, 0.12_cv);
	canvas.fill(GOLD, 1.0_cv, Key("badge_bar"));

	canvas.finalize(Key("badge_composite"));

	// ============================================================================================
	// Pattern 5: defineShape(key) geometry only, no color, no Layer.
	//
	// Registers the shape in the Atlas but does NOT add a Layer to the in-progress composite. Use
	// when you want to reuse the same outline with different colors or transforms, managed by the
	// caller.
	//
	// finalize() is not needed here: defineShape() commits directly and the composite accumulator
	// is still empty after this call.
	// ============================================================================================

	canvas.roundedRect(0.1_cv, 0.1_cv, 0.8_cv, 0.8_cv, 0.15_cv);
	canvas.defineShape(Key("rrect_geom"));

	// ============================================================================================
	// Pattern 6: stroke(width, color) stroke as commit verb, auto-key.
	//
	// Expands the path to a constant-width outline AND commits it as a colored Layer in one call.
	// Matches HTML Canvas / Cairo / NanoVG semantics. The path transformer (strokePath) is called
	// internally.
	// ============================================================================================

	canvas.beginPath();
	canvas.moveTo(0.1_cv, 0.5_cv);
	canvas.quadTo(0.25_cv, 0.05_cv, 0.5_cv, 0.5_cv);
	canvas.quadTo(0.75_cv, 0.95_cv, 0.9_cv, 0.5_cv);
	canvas.stroke(0.06_cv, WHITE); // auto-key "s_5"

	canvas.finalize(Key("scurve_stroke_composite"));

	// ============================================================================================
	// Pattern 7: stroke(width, color, scale, key) named stroke commit.
	//
	// Same as pattern 6 but the shape is registered under an explicit key. CLI `render atlas.slug
	// zigzag_stroke` works without composite fallback. Also demonstrates miter joins from the
	// Phase 2 stroke implementation.
	// ============================================================================================

	canvas.beginPath();
	canvas.moveTo(0.0_cv, 0.0_cv);
	canvas.lineTo(0.1_cv, 0.5_cv);
	canvas.lineTo(0.2_cv, 0.0_cv);
	canvas.lineTo(0.3_cv, 0.5_cv);
	canvas.lineTo(0.4_cv, 0.0_cv);
	canvas.lineTo(0.5_cv, 0.5_cv);
	canvas.lineTo(0.6_cv, 0.0_cv);
	canvas.lineTo(0.7_cv, 0.5_cv);
	canvas.lineTo(0.8_cv, 0.0_cv);
	canvas.lineTo(0.9_cv, 0.5_cv);
	canvas.lineTo(1.0_cv, 0.0_cv);
	canvas.stroke(0.04_cv, CYAN, 1.0_cv, Key("zigzag_stroke"));
	// stroke() with an explicit key registers the shape AND queues a Layer in the
	// composite accumulator, just like fill(). If you only want the named shape
	// and not the composite wrapper, clear the accumulator explicitly.
	canvas.beginComposite();

	// ============================================================================================
	// Pattern 8: strokePath(width) + defineShape(key) geometry-only stroke.
	//
	// The escape hatch: strokePath() transforms the path in place (centerline -> outline), then
	// defineShape() commits the resulting outline as raw geometry with no color. Use when you need
	// the outline curves for something the commit verbs can't express directly.
	// ============================================================================================

	canvas.beginPath();
	canvas.moveTo(0.2_cv, 0.85_cv);
	canvas.quadTo(0.1_cv, 0.5_cv, 0.5_cv, 0.5_cv);
	canvas.quadTo(0.9_cv, 0.5_cv, 0.8_cv, 0.15_cv);
	canvas.strokePath(0.08_cv);
	canvas.defineShape(Key("scurve_outline_geom"));

	// ============================================================================================
	// Pattern 9: arcTo() rounded corners, stadium shape.
	//
	// moveTo() must start at a tangent point on the shape boundary, not at a bounding-box corner.
	// closePath() then returns to that same point with a zero-length segment. For axis-aligned
	// rounded rects prefer roundedRect() (Patterns 3/4); arcTo() shines for irregular corners.
	// ============================================================================================

	canvas.beginPath();
	canvas.moveTo(0.3_cv, 0.3_cv);
	canvas.arcTo(0.8_cv, 0.3_cv, 0.8_cv, 0.7_cv, 0.1_cv);
	canvas.arcTo(0.8_cv, 0.7_cv, 0.2_cv, 0.7_cv, 0.1_cv);
	canvas.arcTo(0.2_cv, 0.7_cv, 0.2_cv, 0.3_cv, 0.1_cv);
	canvas.arcTo(0.2_cv, 0.3_cv, 0.8_cv, 0.3_cv, 0.1_cv);
	canvas.closePath();
	canvas.fill(GREEN, 1.0_cv, Key("stadium_arcto"));

	canvas.finalize(Key("stadium_composite"));

	canvas.beginPath();
	canvas.moveTo(0_cv, 0_cv);
	canvas.lineTo(5_cv, 5_cv);
	canvas.lineTo(10_cv, 0_cv);
	canvas.lineTo(15_cv, 0_cv);
	canvas.lineTo(20_cv, 10_cv);
	canvas.lineTo(25_cv, 10_cv);
	canvas.lineTo(30_cv, 15_cv);
	canvas.lineTo(35_cv, 0_cv);
	canvas.lineTo(40_cv, 5_cv);
	canvas.lineTo(45_cv, 0_cv);
	canvas.lineTo(50_cv, 0_cv);
	canvas.strokePath(2_cv);
	canvas.defineShape(slughorn::Key("stroke_test"), 1_cv / 50_cv);

	// ============================================================================================
	// Pattern 10: Linear gradient fill.
	//
	// createLinearGradient() produces a GradientHandle from two authoring-space endpoints and a
	// stop list. fillGradient() commits the path the same way fill() does - geometry goes into
	// the atlas, a Layer is pushed onto the composite - but layer.gradientId is set to the 1-based
	// gradient index instead of a flat color. The gradient atlas texture is rasterized during
	// atlas.build().
	// ============================================================================================

	// Simple left-to-right red -> blue gradient over a unit square.
	{
		auto grad = canvas.createLinearGradient(
			0_cv, 0_cv, 1_cv, 0_cv,
			{
				{0_cv, {1_cv, 0_cv, 0_cv, 1_cv}}, // red at t=0
				{1_cv, {0_cv, 0_cv, 1_cv, 1_cv}} // blue at t=1
			}
		);

		canvas.beginPath();
		canvas.rect(0.1_cv, 0.1_cv, 0.8_cv, 0.8_cv);
		canvas.fillGradient(grad, 1_cv, Key("grad_rect_shape"));

		canvas.finalize(Key("grad_rect_composite"));
	}

	// Diagonal yellow -> transparent gradient over a triangle.
	{
		auto grad = canvas.createLinearGradient(
			0_cv, 0_cv, 1_cv, 1_cv,
			{
				{0_cv, {1_cv, 0.8_cv, 0_cv, 1_cv}}, // gold at t=0
				{1_cv, {1_cv, 0.8_cv, 0_cv, 0_cv}} // transparent gold at t=1
			}
		);

		canvas.beginPath();
		canvas.moveTo(0.5_cv, 0.9_cv);
		canvas.lineTo(0.1_cv, 0.1_cv);
		canvas.lineTo(0.9_cv, 0.1_cv);
		canvas.closePath();
		canvas.fillGradient(grad, 1_cv, Key("grad_tri_shape"));

		canvas.finalize(Key("grad_tri_composite"));
	}

	// ============================================================================================
	// Pattern 11: Radial gradient fill.
	//
	// createRadialGradient() takes a center (cx, cy), inner radius r0, and outer radius r1.
	// r0 = 0 produces the common point-centre radial. t = 0 at the inner edge, t = 1 at the
	// outer edge. Points beyond the outer radius clamp to the last stop color.
	// ============================================================================================

	// Point-centre radial: red at centre, blue at the rim, over a circle.
	{
		auto grad = canvas.createRadialGradient(
			0.5_cv, 0.5_cv, // center
			0_cv, // inner radius (point centre)
			0.5_cv, // outer radius (reaches the circle edge)
			{
				{0_cv, {1_cv, 0_cv, 0_cv, 1_cv}}, // red at t=0 (centre)
				{1_cv, {0_cv, 0_cv, 1_cv, 1_cv}} // blue at t=1 (rim)
			}
		);

		canvas.circle(0.5_cv, 0.5_cv, 0.45_cv);
		canvas.fillGradient(grad, 1_cv, Key("grad_radial_circle_shape"));

		canvas.finalize(Key("grad_radial_circle_composite"));
	}

	// Annular radial: green ring (inner radius 0.2, outer 0.45) over a circle.
	{
		auto grad = canvas.createRadialGradient(
			0.5_cv, 0.5_cv,
			0.2_cv, // inner radius
			0.45_cv, // outer radius
			{
				{0_cv, {0_cv, 0.8_cv, 0_cv, 1_cv}}, // bright green at inner edge
				{1_cv, {0_cv, 0.2_cv, 0_cv, 1_cv}} // dark green at outer edge
			}
		);

		canvas.circle(0.5_cv, 0.5_cv, 0.45_cv);
		canvas.fillGradient(grad, 1_cv, Key("grad_radial_ring_shape"));

		canvas.finalize(Key("grad_radial_ring_composite"));
	}

	// ============================================================================================
	// Pattern 12: Sweep (conic) gradient fill.
	//
	// createSweepGradient() takes a center, startAngle and endAngle (radians, same convention as
	// arc()). t=0 at startAngle, t=1 at endAngle. Using -a to +a gives a seam-free full circle
	// because atan2's output range exactly matches, so the first and last stops meet cleanly.
	// ============================================================================================

	// Full-circle colour wheel: red -> yellow -> green -> cyan -> blue -> magenta -> red.
	{
		const auto PI = cv(M_PI);

		auto grad = canvas.createSweepGradient(
			0.5_cv, 0.5_cv, // center
			-PI, PI, // full circle, seam at -a/+a (left edge)
			{
				{0.000_cv, {1_cv, 0_cv, 0_cv, 1_cv}}, // red
				{0.167_cv, {1_cv, 1_cv, 0_cv, 1_cv}}, // yellow
				{0.333_cv, {0_cv, 1_cv, 0_cv, 1_cv}}, // green
				{0.500_cv, {0_cv, 1_cv, 1_cv, 1_cv}}, // cyan
				{0.667_cv, {0_cv, 0_cv, 1_cv, 1_cv}}, // blue
				{0.833_cv, {1_cv, 0_cv, 1_cv, 1_cv}}, // magenta
				{1.000_cv, {1_cv, 0_cv, 0_cv, 1_cv}} // red (closes seam-free)
			}
		);

		canvas.circle(0.5_cv, 0.5_cv, 0.45_cv);
		canvas.fillGradient(grad, 1_cv, Key("grad_sweep_wheel_shape"));

		canvas.finalize(Key("grad_sweep_wheel_composite"));
	}

	// 270-degree progress gauge: green (start) -> yellow (mid) -> red (end).
	// Sweep from -135 degrees to +135 degrees (bottom-left to bottom-right, leaving a gap at the
	// bottom).
	{
		const auto PI = cv(M_PI);

		auto grad = canvas.createSweepGradient(
			0.5_cv, 0.5_cv,
			-PI * 0.75_cv, PI * 0.75_cv,
			{
				{0.0_cv, {0_cv, 1_cv, 0_cv, 1_cv}}, // green
				{0.5_cv, {1_cv, 1_cv, 0_cv, 1_cv}}, // yellow
				{1.0_cv, {1_cv, 0_cv, 0_cv, 1_cv}} // red
			}
		);

		canvas.circle(0.5_cv, 0.5_cv, 0.45_cv);
		canvas.fillGradient(grad, 1_cv, Key("grad_sweep_gauge_shape"));

		canvas.finalize(Key("grad_sweep_gauge_composite"));
	}

	// ============================================================================================
	// Pattern 13: Transform stack - clock face with baked tick marks.
	//
	// Demonstrates save()/restore(), translate(), and rotate(). The outer circle arc and all
	// 12 tick stroke outlines are accumulated as sub-paths and committed in a SINGLE fill()
	// call, producing one atlas Shape. This works because closePath() and strokePath() both
	// append to _pendingCurves without committing; fill() commits everything at once.
	//
	// The non-zero winding rule gives each sub-path its own filled region, so the ticks render
	// as solid rectangles inset from the clock rim, all baked into a single atlas entry.
	// ============================================================================================

	{
		const auto PI = cv(M_PI);

		const slug_t CX = 0.5_cv, CY = 0.5_cv;
		const slug_t FACE_R = 0.45_cv;
		const slug_t TICK_OUTER = 0.43_cv;
		const slug_t TICK_INNER = 0.36_cv;
		const slug_t TICK_WIDTH = 0.025_cv;

		const Color FACE_COLOR = {0.95_cv, 0.92_cv, 0.82_cv, 1_cv}; // parchment
		const Color TICK_COLOR = {0.15_cv, 0.15_cv, 0.15_cv, 1_cv}; // near-black

		using Origin = slughorn::Atlas::ShapeInfo::Origin;

		// -- Clock face: filled circle (one Shape, one Layer) --------------------------------

		canvas.circle(CX, CY, FACE_R);
		canvas.fill(FACE_COLOR, 1_cv, Key("clock_face_shape"), Origin(Origin::Type::Centered));

		canvas.finalize(Key("clock_face_composite"));

		// -- Tick marks: 12 stroked lines, all baked into ONE Shape -------------------------
		//
		// save()/restore() isolates each rotation so subsequent save()s start from the
		// base CTM. The ticks are drawn along the +Y axis in local space; translate() moves
		// the origin to the clock centre first.

		canvas.beginPath();

		for(int i = 0; i < 12; ++i) {
			canvas.save();
			canvas.translate(CX, CY);
			canvas.rotate(cv(i * 2.0 * M_PI / 12.0));

			canvas.moveTo(0_cv, TICK_INNER);
			canvas.lineTo(0_cv, TICK_OUTER);
			canvas.strokePath(TICK_WIDTH);

			canvas.restore();
		}

		canvas.fill(TICK_COLOR, 1_cv, Key("clock_ticks_shape"), Origin(Origin::Type::Centered));

		canvas.finalize(Key("clock_ticks_composite"));
	}

	// ============================================================================================
	// Pattern 14: Origin(px, py) - explicit pivot for GPU-side rotation.
	//
	// A clock hand (a stroke from the pivot point to the tip) authored with Origin(cx, cy)
	// so that Layer::transform.dx/dy == (cx, cy) in em-space. The GPU consumer can then rotate
	// the shape around that exact point without any translate-rotate-translate gymnastics.
	//
	// Compare the three modes on an asymmetric shape like this hand:
	//
	// Origin{} - transform.dx/dy = bbox corner (wrong pivot)
	// Origin(Centered) - transform.dx/dy = bbox center (wrong pivot, hand is asymmetric)
	// Origin(cx, cy) - transform.dx/dy = hand base (correct pivot = clock centre)
	// ============================================================================================

	{
		const slug_t CX = 0.5_cv, CY = 0.25_cv; // pivot = clock centre / hand base
		const slug_t HAND_LENGTH = 0.45_cv;
		const slug_t HAND_WIDTH = 0.03_cv;

		const Color HAND_COLOR = {0.12_cv, 0.12_cv, 0.18_cv, 1.0_cv};

		canvas.moveTo(CX, CY);
		canvas.lineTo(CX, CY + HAND_LENGTH);

		const Key handKey = canvas.stroke(
			HAND_WIDTH,
			HAND_COLOR,
			1.0_cv,
			slughorn::Atlas::ShapeInfo::Origin(CX, CY)
		);

		// Verify: the layer's transform.dx/dy should equal (CX, CY) - the caller-supplied pivot.
		const auto hand = canvas.finalize();

		if(!hand.layers.empty()) {
			const auto& t = hand.layers.front().transform;
			const bool pivotOk = std::abs(t.dx - CX) < 1e-5_cv && std::abs(t.dy - CY) < 1e-5_cv;

			std::cerr
				<< "Pattern 14: hand pivot = (" << t.dx << ", " << t.dy << ")"
				<< " expected (" << CX << ", " << CY << ")"
				<< (pivotOk ? " OK" : " MISMATCH") << "\n"
			;
		}

		atlas.addCompositeShape(Key("clock_hand_composite"), hand);
	}

	// ============================================================================================
	// Pattern 15: Same geometry, two different origins - for SVG debug visualization.
	//
	// The rectangle spans [0.1, 0.9] x [0.15, 0.85] in authoring space.
	//
	// pivot_centered_shape: Origin::Centered -> dot at geometric center (0.5, 0.5)
	// pivot_custom_shape: Origin(0.2, 0.72) -> dot clearly off-center; obviously intentional
	//
	// CLI: `slughorn svg atlas.slug pivot_centered_shape`
	//      `slughorn svg atlas.slug pivot_custom_shape`
	//  The yellow dot should jump between the two positions.
	// ============================================================================================

	{
		using Origin = slughorn::Atlas::ShapeInfo::Origin;

		const Color PURPLE = {0.6_cv, 0_cv, 0.8_cv, 1_cv};

		canvas.rect(0.1_cv, 0.15_cv, 0.8_cv, 0.7_cv);
		canvas.fill(PURPLE, 1_cv, Key("pivot_centered_shape"), Origin(Origin::Type::Centered));

		canvas.finalize(Key("pivot_centered_composite"));

		canvas.rect(0.1_cv, 0.15_cv, 0.8_cv, 0.7_cv);
		canvas.fill(PURPLE, 1_cv, Key("pivot_custom_shape"), Origin(0.2_cv, 0.72_cv));

		canvas.finalize(Key("pivot_custom_composite"));
	}

	// ============================================================================================

	// ============================================================================================
	// Pattern 16: Standalone Path - build, sample, and commit independently of Canvas.
	//
	// NOTE: Patterns 17 and 18 below exercise Canvas::text(). No font is loaded in this
	// test, so atlas.getShape() returns nullptr for every codepoint and the fallback
	// advance (0.6 em) is used for spacing. The tests verify layer accumulation and that
	// both the single-pass (Left) and two-pass (Center) alignment paths execute cleanly.
	//
	// The path is constructed without any canvas involvement. sample() works directly on
	// the standalone Path. canvas.stroke(p, ...) commits p's geometry without consuming it;
	// p could be reused, transformed, or sampled again after the call.
	// ============================================================================================
	{
		slughorn::canvas::Path p;

		p.moveTo(0_cv, 0_cv);
		p.lineTo(0.3_cv, 0_cv);
		p.quadTo(0.5_cv, 1_cv, 0.7_cv, 0_cv);
		p.lineTo(1_cv, 0_cv);

		// for(slug_t s = 0_cv; s <= 1_cv; s += 0.1_cv) std::cout
		for(size_t i = 0; i < 11; i++) {
			const auto s = cv(i / 10_cv);

			std::cout << "sample @ " << s << " = " << p.sample(s) << std::endl;
		}

		// Commit via explicit-path overload - p is unchanged after this call.
		canvas.stroke(p, 0.06_cv, WHITE);
		canvas.finalize(Key("sample_path"));

		// Verify: canvas.path() still returns the implicit path (unaffected by p).
		std::cerr
			<< "Pattern 16: internal path hasPendingPath="
			<< canvas.hasPendingPath() << " (expected 0)\n"
		;
	}

	// ============================================================================================
	// Pattern 17: Canvas::text() - left-aligned baseline placement (single-pass).
	//
	// FontMetrics is a plain slug_t struct in slughorn.hpp; no FreeType dependency here.
	// canvas.text() pushes one Layer per glyph into _composite, same as fill()/stroke().
	// With no font loaded, atlas.getShape() returns nullptr and the 0.6-em fallback
	// advance is used; the layer count and key values are still correct.
	// ============================================================================================
	{
		slughorn::FontMetrics m;

		// Approximate Latin metrics for a typical serif face.
		m.unitsPerEM = 1000_cv;
		m.capHeightRatio = 0.72_cv;
		m.xHeightRatio = 0.53_cv;
		m.ascenderRatio = 0.80_cv;
		m.descenderRatio = 0.20_cv;
		m.lineGapRatio = 0.00_cv;

		canvas.text("AXO", 70_cv, 20_cv, 55_cv, WHITE, m);
		canvas.finalize(Key("text_left"));

		std::cerr
			<< "Pattern 17: text_left layers="
			<< canvas.layerCount() << " (expected 0 after finalize)\n"
		;
	}

	// ============================================================================================
	// Pattern 18: Canvas::text() - centered + CapCenter anchor (two-pass alignment).
	//
	// TextAlignX::Center triggers a measure pass before placing glyphs.
	// TextAnchorY::CapCenter shifts the baseline so that capital-letter tops align to y.
	// ============================================================================================
	{
		slughorn::FontMetrics m;

		m.unitsPerEM = 1000_cv;
		m.capHeightRatio = 0.72_cv;
		m.xHeightRatio = 0.53_cv;
		m.ascenderRatio = 0.80_cv;
		m.descenderRatio = 0.20_cv;
		m.lineGapRatio = 0.00_cv;

		const size_t before = canvas.layerCount();

		canvas.text(
			"AXO", 70_cv,
			180_cv, 260_cv,
			WHITE, m,
			slughorn::canvas::TextAnchorY::CapCenter,
			slughorn::canvas::TextAlignX::Center
		);

		std::cerr
			<< "Pattern 18: text_centered layers added="
			<< (canvas.layerCount() - before) << " (expected 3)\n"
		;

		canvas.finalize(Key("text_centered"));
	}

	// ============================================================================================

	atlas.build();

	std::cerr << "PackingStats: " << atlas.getPackingStats() << std::endl;

	if(argc > 1) {
		std::ofstream f(argv[1]);

		slughorn::serial::writeJSON(atlas, f);
	}

	else slughorn::serial::writeJSON(atlas, std::cout);

	return 0;
}
