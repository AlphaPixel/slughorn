#pragma once

// =============================================================================
// Decomposes TrueType/OpenType outlines and COLRv0/v1 emoji into slughorn Atlas shapes. No OSG,
// VSG, or other graphics library dependency.
//
// USAGE
// -----
// In exactly one .cpp file, before including this header:
//
// #define SLUGHORN_FREETYPE_IMPLEMENTATION
// #include "slughorn-freetype.hpp"
//
// All other translation units include it without the define.
//
// The FreeType headers must be on your include path. Link against freetype2.
//
// TODO: POTENTIALLY INCLUDE THIS IN OTHER BACKENDS!
// OPTIONAL: inject a log callback before calling any function
//
// slughorn::freetype::setLogCallback([](int level, const std::string& msg) {
//     if(level >= slughorn::freetype::LOG_WARN) std::cerr << msg << "\n";
// });
//
// Log levels: LOG_INFO = 0, LOG_NOTICE = 1, LOG_WARN = 2
// The default callback prints WARN and above to stderr.
// =============================================================================

#include "slughorn.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>
#include <cstdint>
#include <iostream>

// FreeType headers are needed wherever you pass FT_Face / FT_Color* directly.
// Include them before this header in those translation units.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_COLOR_H

namespace slughorn {
namespace freetype {

// =============================================================================
// Logging
// =============================================================================

static constexpr int LOG_INFO = 0;
static constexpr int LOG_NOTICE = 1;
static constexpr int LOG_WARN = 2;

using LogCallback = std::function<void(int level, const std::string& msg)>;

// Set the log callback used by all ft2 functions in this translation unit. Thread-safety: set once
// before any calls; not synchronized.
void setLogCallback(LogCallback cb);

// =============================================================================
// Core decomposition
// =============================================================================

// Decompose a single glyph outline from an already-loaded FT_Face into a ShapeInfo. The caller must
// have called FT_Load_Glyph with FT_LOAD_NO_SCALE before this function.
//
// emScale = 1.0f / face->units_per_EM
//
// If @p matrix is non-null the transform is baked into every curve coordinate. Pass nullptr (or a
// Matrix::identity()) for no transform.
//
// Returns true if curves were produced; false if the outline was empty.
bool decomposeGlyph(
	FT_Face face,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix* matrix, // nullptr = identity
	Atlas::ShapeInfo& out
);

// =============================================================================
// Regular (monochrome) glyph loading
// =============================================================================

// Decompose a single Unicode codepoint from @p face and add it to @p atlas. Skips codepoints
// already present in the atlas. Returns true on success, false if the glyph is missing or
// decomposition fails.
bool loadGlyph(FT_Face face, uint32_t codepoint, Atlas& atlas);

// Convenience: load a contiguous range of codepoints [first, last]. Returns the number of glyphs
// successfully added.
size_t loadGlyphRange(
	FT_Face face,
	uint32_t first,
	uint32_t last,
	Atlas& atlas
);

// =============================================================================
// COLR emoji loading
// =============================================================================

// Load a single COLR emoji codepoint from @p face into @p atlas.
// Automatically detects COLRv1 (FreeType >= 2.11) and falls back to COLRv0.
//
// On success, populates @p out with the layer stack and returns true.
// Returns false if the codepoint has no COLR data or decomposition fails.
//
// @p palette may be nullptr - resolveColor() will return opaque white.
// Obtain it via:
//
// FT_Color* palette = nullptr;
// FT_Palette_Data pd = {};
// if(!FT_Palette_Data_Get(face, &pd) && pd.num_palettes > 0) FT_Palette_Select(face, 0, &palette);
bool loadColorGlyph(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out
);

// Convenience: load a list of emoji codepoints. Populates colorGlyphs (keyed by codepoint) for
// successfully loaded entries. Returns the number of codepoints successfully loaded.
size_t loadColorGlyphs(
	FT_Face face,
	const std::vector<uint32_t>& codepoints,
	FT_Color* palette,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs
);

// =============================================================================
// High-level helpers (manage their own FT_Library / FT_Face lifetime)
// =============================================================================

// Load printable ASCII (codepoints 32-126) from @p fontPath into @p atlas. Creates and destroys an
// FT_Library / FT_Face internally. Returns false if the font cannot be opened.
bool loadAsciiFont(const std::string& fontPath, Atlas& atlas);

// Load COLR emoji from @p fontPath for the given codepoints. Creates and destroys an FT_Library /
// FT_Face internally. Returns false if the font cannot be opened.
bool loadEmojiFont(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs
);

}
}

// =============================================================================
// IMPLEMENTATION
// =============================================================================
#ifdef SLUGHORN_FREETYPE_IMPLEMENTATION

#include <cmath>
#include <cstring>
#include <sstream>

namespace slughorn {
namespace freetype {

// =============================================================================
// Logging
//
// TODO: Either REMOVE THIS or add it to the other backends; one or the other. It's only here
// because of how important the messages can be during development (honestly, this was easily the
// most difficult backend yet).
//
// TODO: If we DO decide to globalize logging, consider using `spdlog`.
// =============================================================================

static LogCallback& logCallbackRef() {
	static LogCallback cb = [](int level, const std::string& msg) {
		if(level >= LOG_WARN) {
			const char* prefix =
				level >= LOG_WARN ? "[slughorn-freetype WARN] " :
				level >= LOG_NOTICE ? "[slughorn-freetype] " :
				"[slughorn-freetype info] "
			;

			// fputs(prefix, stderr);
			// fputs(msg.c_str(), stderr);
			// fputc('\n', stderr);

			std::cerr << prefix << msg << std::endl;
		}
	};

	return cb;
}

void setLogCallback(LogCallback cb) {
	logCallbackRef() = std::move(cb);
}

static void log(int level, const auto&... args) {
	std::ostringstream oss;

	((oss << args), ...) << std::endl;

	if(logCallbackRef()) logCallbackRef()(level, oss.str());
}

// =============================================================================
// detail - internal helpers (not part of the public API)
// =============================================================================
namespace detail {

// -------------------------------------------------------------------------
// FT_Outline_Decompose callbacks + outline decomposition
// -------------------------------------------------------------------------

struct OutlineContext {
	CurveDecomposer decomposer;

	slug_t scale;

	OutlineContext(Atlas::Curves& curves, slug_t s):
	decomposer(curves),
	scale(s) {
	}
};

static int ftMoveTo(const FT_Vector* to, void* user) {
	auto* ctx = static_cast<OutlineContext*>(user);

	ctx->decomposer.moveTo(cv(to->x) * ctx->scale, cv(to->y) * ctx->scale);

	return 0;
}

static int ftLineTo(const FT_Vector* to, void* user) {
	auto* ctx = static_cast<OutlineContext*>(user);

	ctx->decomposer.lineTo(cv(to->x) * ctx->scale, cv(to->y) * ctx->scale);

	return 0;
}

static int ftConicTo(const FT_Vector* control, const FT_Vector* to, void* user) {
	auto* ctx = static_cast<OutlineContext*>(user);

	ctx->decomposer.quadTo(
		cv(control->x) * ctx->scale, cv(control->y) * ctx->scale,
		cv(to->x) * ctx->scale, cv(to->y) * ctx->scale
	);

	return 0;
}

static int ftCubicTo(
	const FT_Vector* c1,
	const FT_Vector* c2,
	const FT_Vector* to,
	void* user
) {
	auto* ctx = static_cast<OutlineContext*>(user);

	ctx->decomposer.cubicTo(
		cv(c1->x) * ctx->scale, cv(c1->y) * ctx->scale,
		cv(c2->x) * ctx->scale, cv(c2->y) * ctx->scale,
		cv(to->x) * ctx->scale, cv(to->y) * ctx->scale
	);

	return 0;
}

// Decompose a raw FT_Outline, applying emScale. Used by both regular-glyph and COLRv0/v1 paths.
//
// TODO: Instead of accepting `curves`, return it (just like the Cairo and NanoVG backends do)!
static void decomposeOutline(FT_Outline& outline, Atlas::Curves& curves, slug_t emScale) {
	if(outline.n_points <= 0) return;

	OutlineContext ctx(curves, emScale);

	FT_Outline_Funcs funcs{
		ftMoveTo,
		ftLineTo,
		ftConicTo,
		ftCubicTo,
		0,
		0
	};

	FT_Outline_Decompose(&outline, &funcs, &ctx);
}

// -------------------------------------------------------------------------
// Color resolution
// -------------------------------------------------------------------------

static Color resolveColor(FT_Color* palette, uint32_t colorIndex) {
	// 0xFFFF is FreeType's "foreground color" sentinel - return opaque white.
	if(!palette || colorIndex == 0xFFFF) return Color{1_cv, 1_cv, 1_cv, 1_cv};

	const auto& c = palette[colorIndex];

	return Color{
		cv(c.red) / 255.0_cv,
		cv(c.green) / 255.0_cv,
		cv(c.blue) / 255.0_cv,
		cv(c.alpha) / 255.0_cv
	};
}

// -------------------------------------------------------------------------
// COLRv0
// -------------------------------------------------------------------------

static void processColorGlyphV0(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out
) {
	const auto glyphIndex = FT_Get_Char_Index(face, codepoint);
	const slug_t emScale = 1.0_cv / cv(face->units_per_EM);
	const slug_t advance = out.advance;

	FT_LayerIterator iterator = {};
	FT_UInt layerGlyphIndex = 0;
	FT_UInt layerColorIndex = 0;

	bool hasLayer = FT_Get_Color_Glyph_Layer(
		face,
		glyphIndex,
		&layerGlyphIndex,
		&layerColorIndex,
		&iterator
	);

	uint8_t layerIdx = 0;

	while(hasLayer) {
		if(!FT_Load_Glyph(face, layerGlyphIndex, FT_LOAD_NO_SCALE)) {
			const uint32_t layerKey = (codepoint << 8) | layerIdx;
			const auto color = resolveColor(palette, layerColorIndex);

			Atlas::ShapeInfo data;

			data.autoMetrics = false;
			data.bearingX = cv(face->glyph->metrics.horiBearingX) * emScale;
			data.bearingY = cv(face->glyph->metrics.horiBearingY) * emScale;
			data.width = cv(face->glyph->metrics.width) * emScale;
			data.height = cv(face->glyph->metrics.height) * emScale;
			data.advance = advance;

			decomposeOutline(face->glyph->outline, data.curves, emScale);

			atlas.addShape(layerKey, data);
			out.layers.push_back({layerKey, color});
		}

		hasLayer = FT_Get_Color_Glyph_Layer(
			face,
			glyphIndex,
			&layerGlyphIndex,
			&layerColorIndex,
			&iterator
		);

		layerIdx++;
	}
}

// -------------------------------------------------------------------------
// COLRv1 - forward declaration (traversePaint is mutually recursive)
// -------------------------------------------------------------------------

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)

static Color traversePaint(
	FT_Face face,
	FT_OpaquePaint* opaquePaint,
	FT_Color* palette,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix& parentMatrix,
	uint32_t codepoint,
	uint8_t& layerIdx,
	Atlas& atlas,
	CompositeShape& out
);

// -------------------------------------------------------------------------
// COLRv1 - traversePaint
//
// Walks the FreeType paint graph recursively, building up an accumulated
// affine transform (parentMatrix) and emitting one atlas shape + Layer
// per PaintGlyph leaf.
//
// Return value: the Color resolved at this node (meaningful for PaintSolid
// and gradient stubs; used by the PaintGlyph case to colour its layer).
// -------------------------------------------------------------------------
static Color traversePaint(
	FT_Face face,
	FT_OpaquePaint* opaquePaint,
	FT_Color* palette,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix& parentMatrix,
	uint32_t codepoint,
	uint8_t& layerIdx,
	Atlas& atlas,
	CompositeShape& out
) {
	// TODO: Probably safe to just return `{}`, since `Color` defaults to that.
	const auto white = Color{1_cv, 1_cv, 1_cv, 1_cv};

	FT_COLR_Paint paint;

	if(!FT_Get_Paint(face, *opaquePaint, &paint)) return white;

	switch(paint.format) {
		// -----------------------------------------------------------------
		// Container - iterate child layers in bottom-to-top order
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_COLR_LAYERS: {
			FT_OpaquePaint childPaint = {};

			while(FT_Get_Paint_Layers(
				face,
				&paint.u.colr_layers.layer_iterator,
				&childPaint
			)) {
				traversePaint(
					face, &childPaint, palette,
					emScale, advance, parentMatrix,
					codepoint, layerIdx, atlas, out
				);
			}

			return white;
		}

		// -----------------------------------------------------------------
		// Glyph outline - recurse into child paint for color, then decompose
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_GLYPH: {
			const FT_UInt gi = paint.u.glyph.glyphID;

			// Get color from child paint BEFORE loading the glyph --
			// loading a new glyph clobbers face->glyph.
			Color color = traversePaint(
				face, &paint.u.glyph.paint, palette,
				emScale, advance, parentMatrix,
				codepoint, layerIdx, atlas, out
			);

			if(FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE)) break;

			const uint32_t layerKey = (codepoint << 8) | layerIdx;

			Atlas::ShapeInfo data;

			// data.autoMetrics = true;
			data.advance = advance;

			if(!parentMatrix.isIdentity()) {
				// Bake the accumulated transform into the curve coordinates.
				Atlas::Curves rawCurves;

				decomposeOutline(face->glyph->outline, rawCurves, emScale);

				for(auto& c : rawCurves) {
					parentMatrix.apply(c.x1, c.y1, c.x1, c.y1);
					parentMatrix.apply(c.x2, c.y2, c.x2, c.y2);
					parentMatrix.apply(c.x3, c.y3, c.x3, c.y3);

					data.curves.push_back(c);
				}
			}

			else decomposeOutline(face->glyph->outline, data.curves, emScale);

			if(data.curves.empty()) break;

			atlas.addShape(layerKey, data);

			out.layers.push_back({layerKey, color});

			layerIdx++;

			return color;
		}

		// -----------------------------------------------------------------
		// Flat color
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_SOLID: {
			return resolveColor(palette, paint.u.solid.color.palette_index);
		}

		// -----------------------------------------------------------------
		// Affine transform - accumulate into matrix and recurse.
		//
		// The root transform node emitted by FT_COLOR_INCLUDE_ROOT_TRANSFORM
		// is sometimes a synthetic zero matrix (known FreeType quirk for
		// certain fonts). When both xx and yy are zero the matrix would
		// zero-out all curve coordinates, so we skip it and pass the parent
		// matrix unchanged.
		//
		// xx/yx/xy/yy are dimensionless ratios (divide by 65536).
		// dx/dy are in font units and must be multiplied by emScale.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_TRANSFORM: {
			const FT_Affine23& a = paint.u.transform.affine;

			if(!a.xx && !a.yy) {
				// Zero/broken transform - skip and recurse with parent matrix.
				return traversePaint(
					face, &paint.u.transform.paint, palette,
					emScale, advance, parentMatrix,
					codepoint, layerIdx, atlas, out
				);
			}

			slughorn::Matrix m;

			m.xx = cv(a.xx) / 65536.0_cv;
			m.yx = cv(a.yx) / 65536.0_cv;
			m.xy = cv(a.xy) / 65536.0_cv;
			m.yy = cv(a.yy) / 65536.0_cv;
			m.dx = cv(a.dx) / 65536.0_cv * emScale;
			m.dy = cv(a.dy) / 65536.0_cv * emScale;

			// combined = m * parentMatrix (parentMatrix applied first)
			const slughorn::Matrix combined = m * parentMatrix;

			return traversePaint(
				face, &paint.u.transform.paint, palette,
				emScale, advance, combined,
				codepoint, layerIdx, atlas, out
			);
		}

		// -----------------------------------------------------------------
		// Translation - accumulate and recurse.
		// dx/dy are in font units - scale by emScale.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_TRANSLATE: {
			slughorn::Matrix m;

			m.dx = cv(paint.u.translate.dx) / 65536.0_cv * emScale;
			m.dy = cv(paint.u.translate.dy) / 65536.0_cv * emScale;
			// xx, yy default to 1; yx, xy default to 0 - identity rotation

			// const slughorn::Matrix combined = m * parentMatrix;
			const auto combined = m * parentMatrix;

			return traversePaint(
				face, &paint.u.translate.paint, palette,
				emScale, advance, combined,
				codepoint, layerIdx, atlas, out
			);
		}

		// -----------------------------------------------------------------
		// Composite - render both paints, blend mode ignored for now
		//
		// TODO: honour CompositeMode
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_COMPOSITE: {
			traversePaint(
				face, &paint.u.composite.backdrop_paint, palette,
				emScale, advance, parentMatrix,
				codepoint, layerIdx, atlas, out
			);

			return traversePaint(
				face, &paint.u.composite.source_paint, palette,
				emScale, advance, parentMatrix,
				codepoint, layerIdx, atlas, out
			);
		}

		// -----------------------------------------------------------------
		// Gradients - use first color stop as flat approximation
		// TODO: bake gradient into texture and sample via UV attribute
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT:
		case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT:
		case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
			const FT_ColorStopIterator& csi =
				(paint.format == FT_COLR_PAINTFORMAT_LINEAR_GRADIENT)
					? paint.u.linear_gradient.colorline.color_stop_iterator
				: (paint.format == FT_COLR_PAINTFORMAT_RADIAL_GRADIENT)
					? paint.u.radial_gradient.colorline.color_stop_iterator
					: paint.u.sweep_gradient.colorline.color_stop_iterator
			;

			FT_ColorStop stop;
			FT_ColorStopIterator iter = csi;

			if(FT_Get_Colorline_Stops(face, &stop, &iter)) return resolveColor(
				palette,
				stop.color.palette_index
			);

			return white;
		}

		// -----------------------------------------------------------------
		// ColrGlyph reference - TODO
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
			log(LOG_INFO, "COLRv1 PaintColrGlyph - not yet implemented");

			break;
		}

		// -----------------------------------------------------------------
		default: {
			log(LOG_INFO, "COLRv1 unhandled paint format ", paint.format, " - skipped");

			break;
		}
	}

	return white;
}

static void processColorGlyphV1(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out
) {
	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
	const slug_t emScale = 1.0_cv / cv(face->units_per_EM);

	FT_OpaquePaint rootPaint = {};

	if(!FT_Get_Color_Glyph_Paint(
		face, glyphIndex,
		FT_COLOR_INCLUDE_ROOT_TRANSFORM,
		&rootPaint
	)) return;

	uint8_t layerIdx = 0;

	traversePaint(
		face, &rootPaint, palette,
		emScale, out.advance, Matrix::identity(),
		codepoint, layerIdx, atlas, out
	);
}

#endif

}

// =============================================================================
// Public API implementation
// =============================================================================

bool decomposeGlyph(
	FT_Face face,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix* matrix,
	Atlas::ShapeInfo& out
) {
	if(face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

	if(matrix && !matrix->isIdentity()) {
		Atlas::Curves rawCurves;

		detail::decomposeOutline(face->glyph->outline, rawCurves, emScale);

		for(auto& c : rawCurves) {
			matrix->apply(c.x1, c.y1, c.x1, c.y1);
			matrix->apply(c.x2, c.y2, c.x2, c.y2);
			matrix->apply(c.x3, c.y3, c.x3, c.y3);
			out.curves.push_back(c);
		}
	}
	else detail::decomposeOutline(face->glyph->outline, out.curves, emScale);

	out.advance = advance;

	return !out.curves.empty();
}

bool loadGlyph(FT_Face face, uint32_t codepoint, Atlas& atlas) {
	if(atlas.hasKey(codepoint)) return true; // already present - not an error

	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);

	if(!glyphIndex && codepoint != 0) return false;
	if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE)) return false;

	const slug_t emScale = 1.0_cv / cv(face->units_per_EM);

	Atlas::ShapeInfo data;

	data.autoMetrics = false;
	data.bearingX = cv(face->glyph->metrics.horiBearingX) * emScale;
	data.bearingY = cv(face->glyph->metrics.horiBearingY) * emScale;
	data.width = cv(face->glyph->metrics.width) * emScale;
	data.height = cv(face->glyph->metrics.height) * emScale;
	data.advance = cv(face->glyph->metrics.horiAdvance) * emScale;

	detail::decomposeOutline(face->glyph->outline, data.curves, emScale);

	atlas.addShape(codepoint, data);

	return true;
}

size_t loadGlyphRange(
	FT_Face face,
	uint32_t first,
	uint32_t last,
	Atlas& atlas
) {
	size_t count = 0;

	for(uint32_t cp = first; cp <= last; cp++) {
		// TODO: Add log() calls here? Probably...
		if(loadGlyph(face, cp, atlas)) count++;
	}

	return count;
}

bool loadColorGlyph(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out
) {
	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);

	if(!glyphIndex) {
		log(LOG_WARN, "U+", std::hex, codepoint, " not found in font");

		return false;
	}

	if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE)) return false;

	const slug_t emScale = 1.0_cv / cv(face->units_per_EM);

	out.advance = cv(face->glyph->metrics.horiAdvance) * emScale;

	// -------------------------------------------------------------------------
	// Try COLRv1 first (FreeType >= 2.11)
	// -------------------------------------------------------------------------

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)

	{
		detail::processColorGlyphV1(face, codepoint, palette, atlas, out);

		if(!out.layers.empty()) {
			log(LOG_NOTICE,
				"loaded COLRv1 U+",
				std::hex, codepoint,
				std::dec, " (", out.layers.size(), " layers)"
			);

			return true;
		}
	}

#endif

	// -------------------------------------------------------------------------
	// Fall back to COLRv0
	// -------------------------------------------------------------------------
	detail::processColorGlyphV0(face, codepoint, palette, atlas, out);

	if(!out.layers.empty()) {
		log(LOG_NOTICE,
			"loaded COLRv0 U+",
			std::hex, codepoint,
			std::dec, " (", out.layers.size(), " layers)"
		);

		return true;
	}

	return false;
}

size_t loadColorGlyphs(
	FT_Face face,
	const std::vector<uint32_t>& codepoints,
	FT_Color* palette,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs
) {
	size_t count = 0;

	for(uint32_t cp : codepoints) {
		CompositeShape glyph;

		if(loadColorGlyph(face, cp, palette, atlas, glyph)) {
			colorGlyphs[cp] = std::move(glyph);

			count++;
		}
	}

	return count;
}

bool loadAsciiFont(const std::string& fontPath, Atlas& atlas) {
	FT_Library library;

	if(FT_Init_FreeType(&library)) {
		log(LOG_WARN, "loadAsciiFont: failed to initialise FreeType");

		return false;
	}

	FT_Face face;

	if(FT_New_Face(library, fontPath.c_str(), 0, &face)) {
		log(LOG_WARN, "loadAsciiFont: failed to open font: ", fontPath);

		FT_Done_FreeType(library);

		return false;
	}

	loadGlyphRange(face, 32, 126, atlas);

	FT_Done_Face(face);
	FT_Done_FreeType(library);

	return true;
}

bool loadEmojiFont(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs
) {
	FT_Library library;

	if(FT_Init_FreeType(&library)) {
		log(LOG_WARN, "loadEmojiFont: failed to initialise FreeType");

		return false;
	}

	FT_Face face;

	if(FT_New_Face(library, fontPath.c_str(), 0, &face)) {
		log(LOG_WARN, "loadEmojiFont: failed to open font: ", fontPath);

		FT_Done_FreeType(library);

		return false;
	}

	FT_Color* palette = nullptr;
	FT_Palette_Data paletteData = {};

	if(!FT_Palette_Data_Get(face, &paletteData) && paletteData.num_palettes > 0) {
		FT_Palette_Select(face, 0, &palette);
	}

	loadColorGlyphs(face, codepoints, palette, atlas, colorGlyphs);

	FT_Done_Face(face);
	FT_Done_FreeType(library);

	return true;
}

}
}

#endif
