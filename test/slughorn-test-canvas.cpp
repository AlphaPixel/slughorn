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

	// canvas.finalize(Key::fromString("tri_composite"));
	canvas.finalize("tri_composite");

	// ============================================================================================
	// Pattern 2: fill(color, scale, key) named shape -> finalize(key).
	//
	// Shape and composite both get explicit names. Use when you need the shape directly addressable
	// (e.g. CLI `render`, Python atlas.get_shape(), or sharing the geometry with another Layer
	// later).
	// ============================================================================================

	canvas.circle(0.5_cv, 0.5_cv, 0.4_cv);
	canvas.fill(BLUE, 1.0_cv, Key::fromString("circle_shape"));

	canvas.finalize(Key::fromString("circle_composite"));

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

	canvas.finalize(Key::fromString("three_layer"));

	// ============================================================================================
	// Pattern 4: Multi-layer composite with named shapes.
	//
	// Each layer's shape is independently addressable AND part of the composite. Use when the
	// caller needs both fine-grained shape access and the composite as a unit (e.g. one layer gets
	// a hover highlight, the others do not).
	// ============================================================================================

	canvas.ellipse(0.5_cv, 0.5_cv, 0.45_cv, 0.28_cv);
	canvas.fill(CYAN, 1.0_cv, Key::fromString("badge_bg"));

	canvas.roundedRect(0.15_cv, 0.35_cv, 0.7_cv, 0.3_cv, 0.12_cv);
	canvas.fill(GOLD, 1.0_cv, Key::fromString("badge_bar"));

	canvas.finalize(Key::fromString("badge_composite"));

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
	canvas.defineShape(Key::fromString("rrect_geom"));

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

	canvas.finalize(Key::fromString("scurve_stroke_composite"));

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
	canvas.stroke(0.04_cv, CYAN, 1.0_cv, Key::fromString("zigzag_stroke"));
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
	canvas.defineShape(Key::fromString("scurve_outline_geom"));

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
	canvas.fill(GREEN, 1.0_cv, Key::fromString("stadium_arcto"));

	canvas.finalize(Key::fromString("stadium_composite"));

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
	canvas.defineShape(slughorn::Key::fromString("stroke_test"), 1_cv / 50_cv);

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
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_rect_shape"));

		canvas.finalize(Key::fromString("grad_rect_composite"));
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
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_tri_shape"));

		canvas.finalize(Key::fromString("grad_tri_composite"));
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
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_radial_circle_shape"));

		canvas.finalize(Key::fromString("grad_radial_circle_composite"));
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
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_radial_ring_shape"));

		canvas.finalize(Key::fromString("grad_radial_ring_composite"));
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
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_sweep_wheel_shape"));

		canvas.finalize(Key::fromString("grad_sweep_wheel_composite"));
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
				{0.0_cv, {0_cv, 1_cv, 0_cv, 1_cv}},   // green
				{0.5_cv, {1_cv, 1_cv, 0_cv, 1_cv}},   // yellow
				{1.0_cv, {1_cv, 0_cv, 0_cv, 1_cv}}    // red
			}
		);

		canvas.circle(0.5_cv, 0.5_cv, 0.45_cv);
		canvas.fillGradient(grad, 1_cv, Key::fromString("grad_sweep_gauge_shape"));

		canvas.finalize(Key::fromString("grad_sweep_gauge_composite"));
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

		const Color FACE_COLOR  = {0.95_cv, 0.92_cv, 0.82_cv, 1_cv};  // parchment
		const Color TICK_COLOR  = {0.15_cv, 0.15_cv, 0.15_cv, 1_cv};  // near-black

		// -- Clock face: filled circle (one Shape, one Layer) --------------------------------

		canvas.circle(CX, CY, FACE_R);
		canvas.fill(FACE_COLOR, 1_cv, Key::fromString("clock_face_shape"));

		canvas.finalize(Key::fromString("clock_face_composite"));

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

		canvas.fill(TICK_COLOR, 1_cv, Key::fromString("clock_ticks_shape"));

		canvas.finalize(Key::fromString("clock_ticks_composite"));
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
