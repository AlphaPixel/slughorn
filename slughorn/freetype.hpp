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
// #include <slughorn/freetype.hpp>
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
#include <optional>
#include <utility>

// FreeType headers are needed wherever you pass FT_Face / FT_Color* directly.
// Include them before this header in those translation units.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_COLOR_H
#include FT_TRUETYPE_TABLES_H

namespace slughorn {
namespace freetype {

// =============================================================================
// Configuration
// =============================================================================

using LogCallback = std::function<void(int level, const std::string& msg)>;

static constexpr int LOG_INFO = 0;
static constexpr int LOG_NOTICE = 1;
static constexpr int LOG_WARN = 2;

// Passed to all load functions. Default construction is silent, non-uniform, no custom splitting.
// Output fields are populated by the high-level load*Font wrappers (which manage their own
// FT_Face lifetime). The face-taking overloads (loadGlyph, loadGlyphs, etc.) do not populate them.
struct LoadConfig {
	// Input fields (set these BEFORE)...
	Atlas::SplitStrategy strategy = {};

	LogCallback log = {};

	bool uniform = false;

	// Output fields (conditionally set, when possible)...
	FontMetrics metrics = {};

	std::string familyName;
	std::string styleName;
};

// =============================================================================
// Font metrics
// =============================================================================

// Read metrics from an already-open FT_Face. Safe to call immediately after
// FT_New_Face / FT_Open_Face, before any glyph is loaded.
slughorn::FontMetrics readFontMetrics(FT_Face face);

// Open the font at fontPath, read its metrics, and close it. Returns nullopt
// if the font cannot be opened.
std::optional<slughorn::FontMetrics> loadFontMetrics(const std::string& fontPath);

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
// Uniform-bounding-box note: set config.uniform=true to force all glyphs in a batch to share
// the same em-space bounding box, required for setLayerShapeIndex. Auto-detects tabular advances
// (Option C) and falls back to union bbox + centering (Option A) when advances differ.
bool loadGlyph(
	FT_Face face,
	uint32_t codepoint,
	Atlas& atlas,
	LoadConfig* config=nullptr
);

// Convenience: load a contiguous range of codepoints [first, last]. Returns the number of glyphs
// successfully added.
size_t loadGlyphRange(
	FT_Face face,
	uint32_t first,
	uint32_t last,
	Atlas& atlas,
	LoadConfig* config=nullptr
);

// Convenience: load an explicit list of Unicode codepoints. Returns the number of glyphs
// successfully added.
size_t loadGlyphs(
	FT_Face face,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	LoadConfig* config=nullptr
);

// Convenience: iterate the face's full charmap and load every mapped codepoint. Returns the number
// of glyphs successfully added.
size_t loadAllGlyphs(
	FT_Face face,
	Atlas& atlas,
	LoadConfig* config=nullptr
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
	CompositeShape& out,
	LoadConfig* config=nullptr
);

// Convenience: load a list of emoji codepoints. Populates colorGlyphs (keyed by codepoint) for
// successfully loaded entries. Returns the number of codepoints successfully loaded.
size_t loadColorGlyphs(
	FT_Face face,
	const std::vector<uint32_t>& codepoints,
	FT_Color* palette,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs,
	LoadConfig* config=nullptr
);

// =============================================================================
// High-level helpers (manage their own FT_Library / FT_Face lifetime)
// =============================================================================

// Load printable ASCII (codepoints 32-126) from @p fontPath into @p atlas. Creates and destroys an
// FT_Library / FT_Face internally. Returns false if the font cannot be opened.
bool loadAsciiFont(
	const std::string& fontPath,
	Atlas& atlas,
	LoadConfig* config=nullptr);

// Load an explicit list of codepoints from @p fontPath into @p atlas. Creates and destroys an
// FT_Library / FT_Face internally. Returns the number of glyphs successfully added.
size_t loadFontGlyphs(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	LoadConfig* config=nullptr
);

// Load every mapped codepoint from @p fontPath into @p atlas. Creates and destroys an
// FT_Library / FT_Face internally. Returns the number of glyphs successfully added.
size_t loadAllFontGlyphs(
	const std::string& fontPath,
	Atlas& atlas,
	LoadConfig* config=nullptr
);

// Load COLR emoji from @p fontPath for the given codepoints. Creates and destroys an FT_Library /
// FT_Face internally. Returns false if the font cannot be opened.
bool loadEmojiFont(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs,
	LoadConfig* config=nullptr
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

static void doLog(const LogCallback& log, int level, const auto&... args) {
	if(!log) return;

	std::ostringstream oss;

	((oss << args), ...);

	log(level, oss.str());
}

// =============================================================================
// detail - internal helpers (not part of the public API)
// =============================================================================
namespace detail {

struct LibraryHandle {
	FT_Library value = nullptr;

	~LibraryHandle() {
		if(value) FT_Done_FreeType(value);
	}

	bool init(const char* caller, const LogCallback& log) {
		if(FT_Init_FreeType(&value)) {
			doLog(log, LOG_WARN, caller, ": failed to initialise FreeType");

			return false;
		}

		return true;
	}
};

struct FaceHandle {
	FT_Face value = nullptr;

	~FaceHandle() {
		if(value) FT_Done_Face(value);
	}

	bool open(FT_Library library, const std::string& fontPath, const char* caller, const LogCallback& log) {
		if(FT_New_Face(library, fontPath.c_str(), 0, &value)) {
			doLog(log, LOG_WARN, caller, ": failed to open font: ", fontPath);

			return false;
		}

		return true;
	}
};

template<typename Result, typename F>
static Result withFace(
	const std::string& fontPath,
	const char* caller,
	Result failureValue,
	LoadConfig* config,
	F&& fn
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	LibraryHandle library;

	if(!library.init(caller, cfg.log)) return failureValue;

	FaceHandle face;

	if(!face.open(library.value, fontPath, caller, cfg.log)) return failureValue;

	if(config) {
		config->metrics = readFontMetrics(face.value);

		if(face.value->family_name) config->familyName = face.value->family_name;
		if(face.value->style_name) config->styleName = face.value->style_name;
	}

	return std::forward<F>(fn)(face.value);
}

template<typename F>
static size_t countRange(uint32_t first, uint32_t last, F&& fn) {
	if(first > last) return 0;

	size_t count = 0;

	for(uint32_t cp = first; ; cp++) {
		if(std::forward<F>(fn)(cp)) count++;

		if(cp == last) break;
	}

	return count;
}

template<typename F>
static size_t countCodepoints(const std::vector<uint32_t>& codepoints, F&& fn) {
	size_t count = 0;

	for(uint32_t cp : codepoints) if(std::forward<F>(fn)(cp)) count++;

	return count;
}

template<typename F>
static size_t countCharmap(FT_Face face, F&& fn) {
	size_t count = 0;

	FT_UInt glyphIndex = 0;
	FT_ULong charCode = FT_Get_First_Char(face, &glyphIndex);

	while(glyphIndex != 0) {
		if(std::forward<F>(fn)(static_cast<uint32_t>(charCode))) count++;

		charCode = FT_Get_Next_Char(face, charCode, &glyphIndex);
	}

	return count;
}

// Returns glyph height / unitsPerEM for codepoint, or 0 if not found.
// Used as fallback when OS/2 sCapHeight / sxHeight fields are absent.
static slug_t measureGlyphHeight(FT_Face face, uint32_t codepoint) {
	const FT_UInt gi = FT_Get_Char_Index(face, codepoint);

	if(!gi) return 0_cv;
	if(FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE)) return 0_cv;

	return cv(face->glyph->metrics.height) / cv(face->units_per_EM);
}

[[maybe_unused]] static FT_Color* selectPalette(FT_Face face) {
	FT_Color* palette = nullptr;
	FT_Palette_Data paletteData = {};

	if(
		!FT_Palette_Data_Get(face, &paletteData) &&
		paletteData.num_palettes > 0
	) FT_Palette_Select(face, 0, &palette);

	return palette;
}

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
		cv(c.red) / 255_cv,
		cv(c.green) / 255_cv,
		cv(c.blue) / 255_cv,
		cv(c.alpha) / 255_cv
	};
}

// -------------------------------------------------------------------------
// COLRv0
// -------------------------------------------------------------------------

[[maybe_unused]] static void processColorGlyphV0(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out,
	const LoadConfig& config
) {
	const auto glyphIndex = FT_Get_Char_Index(face, codepoint);
	const slug_t emScale = 1_cv / cv(face->units_per_EM);
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

			if(config.strategy && !data.curves.empty()) {
				auto [sx, sy] = config.strategy(data.curves);
				data.splitsX = std::move(sx);
				data.splitsY = std::move(sy);
			}

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

// Carries both the resolved solid color and any gradient ID up the paint tree.
// gradientId == 0 means solid color; non-zero means the gradient was registered with the atlas.
struct PaintResult {
	Color color = {1_cv, 1_cv, 1_cv, 1_cv};
	uint32_t gradientId = 0;
};

// Extract all stops from a COLRv1 color line iterator. stop_offset is 16.16 fixed-point [0, 1].
// FT_ColorIndex.alpha is FT_F2Dot14 (0x4000 = 1.0) and multiplied into the palette alpha.
static std::vector<GradientStop> extractColorStops(
	FT_Face face,
	FT_ColorStopIterator iter,
	FT_Color* palette
) {
	std::vector<GradientStop> stops;
	FT_ColorStop stop;

	while(FT_Get_Colorline_Stops(face, &stop, &iter)) {
		Color c = resolveColor(palette, stop.color.palette_index);

		c.a *= cv(stop.color.alpha) / 16384_cv;

		stops.push_back({cv(stop.stop_offset) / 65536_cv, c});
	}

	return stops;
}

static PaintResult traversePaint(
	FT_Face face,
	const FT_OpaquePaint* opaquePaint,
	FT_Color* palette,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix& parentMatrix,
	uint32_t codepoint,
	uint8_t& layerIdx,
	Atlas& atlas,
	CompositeShape& out,
	const LoadConfig& config
);

// -------------------------------------------------------------------------
// COLRv1 - traversePaint
//
// Walks the FreeType paint graph recursively, building up an accumulated
// affine transform (parentMatrix) and emitting one atlas shape + Layer
// per PaintGlyph leaf.
//
// Return value: the PaintResult resolved at this node (color for PaintSolid,
// gradientId for gradient nodes; used by the PaintGlyph case).
// -------------------------------------------------------------------------
static PaintResult traversePaint(
	FT_Face face,
	const FT_OpaquePaint* opaquePaint,
	FT_Color* palette,
	slug_t emScale,
	slug_t advance,
	const slughorn::Matrix& parentMatrix,
	uint32_t codepoint,
	uint8_t& layerIdx,
	Atlas& atlas,
	CompositeShape& out,
	const LoadConfig& config
) {
	const auto white = Color{1_cv, 1_cv, 1_cv, 1_cv};

	FT_COLR_Paint paint;

	if(!FT_Get_Paint(face, *opaquePaint, &paint)) return {white};

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
					codepoint, layerIdx, atlas, out, config
				);
			}

			return {white};
		}

		// -----------------------------------------------------------------
		// Glyph outline - recurse into child paint for color, then decompose
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_GLYPH: {
			const FT_UInt gi = paint.u.glyph.glyphID;

			// Get color/gradient from child paint BEFORE loading the glyph --
			// loading a new glyph clobbers face->glyph.
			PaintResult result = traversePaint(
				face, &paint.u.glyph.paint, palette,
				emScale, advance, parentMatrix,
				codepoint, layerIdx, atlas, out, config
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

			if(config.strategy) {
				auto [sx, sy] = config.strategy(data.curves);
				data.splitsX = std::move(sx);
				data.splitsY = std::move(sy);
			}

			atlas.addShape(layerKey, data);

			Layer layer;

			layer.key = layerKey;
			layer.color = result.color;
			layer.gradientId = result.gradientId;

			out.layers.push_back(layer);

			layerIdx++;

			return result;
		}

		// -----------------------------------------------------------------
		// Flat color
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_SOLID: {
			return {resolveColor(palette, paint.u.solid.color.palette_index)};
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

			if(!a.xx && !a.xy && !a.yx && !a.yy) {
				// Zero/broken transform (FreeType root-transform quirk) - skip and recurse with parent matrix.
				return traversePaint(
					face, &paint.u.transform.paint, palette,
					emScale, advance, parentMatrix,
					codepoint, layerIdx, atlas, out, config
				);
			}

			const slughorn::Matrix m{
				.xx = cv(a.xx) / 65536_cv,
				.yx = cv(a.yx) / 65536_cv,
				.xy = cv(a.xy) / 65536_cv,
				.yy = cv(a.yy) / 65536_cv,
				.dx = cv(a.dx) / 65536_cv * emScale,
				.dy = cv(a.dy) / 65536_cv * emScale,
			};

			// combined = m * parentMatrix (parentMatrix applied first)
			const slughorn::Matrix combined = m * parentMatrix;

			return traversePaint(
				face, &paint.u.transform.paint, palette,
				emScale, advance, combined,
				codepoint, layerIdx, atlas, out, config
			);
		}

		// -----------------------------------------------------------------
		// Translation - accumulate and recurse.
		// dx/dy are in font units - scale by emScale.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_TRANSLATE: {
			const auto combined = slughorn::Matrix::translate(
				cv(paint.u.translate.dx) / 65536_cv * emScale,
				cv(paint.u.translate.dy) / 65536_cv * emScale
			) * parentMatrix;

			return traversePaint(
				face, &paint.u.translate.paint, palette,
				emScale, advance, combined,
				codepoint, layerIdx, atlas, out, config
			);
		}

		// -----------------------------------------------------------------
		// Scale - scale_x/y are dimensionless 16.16; center is font units.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_SCALE: {
			const auto& s = paint.u.scale;
			const slug_t sx = cv(s.scale_x) / 65536_cv;
			const slug_t sy = cv(s.scale_y) / 65536_cv;
			const slug_t cx = cv(s.center_x) / 65536_cv * emScale;
			const slug_t cy = cv(s.center_y) / 65536_cv * emScale;

			const slughorn::Matrix m{
				.xx = sx,
				.yy = sy,
				.dx = cx * (1_cv - sx),
				.dy = cy * (1_cv - sy),
			};

			return traversePaint(
				face, &s.paint, palette,
				emScale, advance, m * parentMatrix,
				codepoint, layerIdx, atlas, out, config
			);
		}

		// -----------------------------------------------------------------
		// Rotate - angle is degrees/180 in 16.16 (x angle = radians);
		// center is in font units.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_ROTATE: {
			const auto& r = paint.u.rotate;
			const slug_t theta = cv(r.angle) / 65536_cv * PI_CV;
			const slug_t c = cv(std::cos(static_cast<double>(theta)));
			const slug_t s = cv(std::sin(static_cast<double>(theta)));
			const slug_t cx = cv(r.center_x) / 65536_cv * emScale;
			const slug_t cy = cv(r.center_y) / 65536_cv * emScale;

			const slughorn::Matrix m{
				.xx = c,
				.yx = s,
				.xy = -s,
				.yy = c,
				.dx = cx * (1_cv - c) + s * cy,
				.dy = cy * (1_cv - c) - s * cx,
			};

			return traversePaint(
				face, &r.paint, palette,
				emScale, advance, m * parentMatrix,
				codepoint, layerIdx, atlas, out, config
			);
		}

		// -----------------------------------------------------------------
		// Skew - angles are degrees/180 in 16.16; center is in font units.
		// x' = x + tan(ax) * y, y' = y + tan(ay) * x
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_SKEW: {
			const auto& sk = paint.u.skew;
			const slug_t tax = cv(std::tan(static_cast<double>(cv(sk.x_skew_angle) / 65536_cv * PI_CV)));
			const slug_t tay = cv(std::tan(static_cast<double>(cv(sk.y_skew_angle) / 65536_cv * PI_CV)));
			const slug_t cx = cv(sk.center_x) / 65536_cv * emScale;
			const slug_t cy = cv(sk.center_y) / 65536_cv * emScale;

			const slughorn::Matrix m{
				.xx = 1_cv,
				.yx = tay,
				.xy = tax,
				.yy = 1_cv,
				.dx = -tax * cy,
				.dy = -tay * cx,
			};

			return traversePaint(
				face, &sk.paint, palette,
				emScale, advance, m * parentMatrix,
				codepoint, layerIdx, atlas, out, config
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
				codepoint, layerIdx, atlas, out, config
			);

			return traversePaint(
				face, &paint.u.composite.source_paint, palette,
				emScale, advance, parentMatrix,
				codepoint, layerIdx, atlas, out, config
			);
		}

		// -----------------------------------------------------------------
		// Linear gradient
		// p0/p1 endpoints and p2 (rotation pivot) are 16.16 font units.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_LINEAR_GRADIENT: {
			const auto& lg = paint.u.linear_gradient;
			auto stops = extractColorStops(face, lg.colorline.color_stop_iterator, palette);

			if(stops.empty()) return {white};

			slug_t x0 = cv(lg.p0.x) / 65536_cv * emScale;
			slug_t y0 = cv(lg.p0.y) / 65536_cv * emScale;
			slug_t x1 = cv(lg.p1.x) / 65536_cv * emScale;
			slug_t y1 = cv(lg.p1.y) / 65536_cv * emScale;

			parentMatrix.apply(x0, y0, x0, y0);
			parentMatrix.apply(x1, y1, x1, y1);

			const GradientInfo info{
				.type = GradientInfo::Type::Linear,
				.stops = std::move(stops),
				.transform = buildLinearGradientMatrix(x0, y0, x1, y1),
			};

			return {white, atlas.addGradient(info)};
		}

		// -----------------------------------------------------------------
		// Radial gradient
		// c0/c1 are 16.16 font units; r0/r1 are 16.16 distances.
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_RADIAL_GRADIENT: {
			const auto& rg = paint.u.radial_gradient;
			auto stops = extractColorStops(face, rg.colorline.color_stop_iterator, palette);

			if(stops.empty()) return {white};

			// Use the outer circle (c1, r1) as the gradient center/radius.
			slug_t cx = cv(rg.c1.x) / 65536_cv * emScale;
			slug_t cy = cv(rg.c1.y) / 65536_cv * emScale;
			slug_t r1 = cv(rg.r1) / 65536_cv * emScale;
			slug_t r0 = cv(rg.r0) / 65536_cv * emScale;

			parentMatrix.apply(cx, cy, cx, cy);

			const GradientInfo info{
				.type = GradientInfo::Type::Radial,
				.stops = std::move(stops),
				.transform = buildRadialGradientMatrix(cx, cy, r1),
				.innerRadius = r0,
			};

			return {white, atlas.addGradient(info)};
		}

		// -----------------------------------------------------------------
		// Sweep gradient
		// center is 16.16 font units; angles are 16.16 fractions of a full
		// turn (multiply by 2 for radians).
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_SWEEP_GRADIENT: {
			const auto& sg = paint.u.sweep_gradient;
			auto stops = extractColorStops(face, sg.colorline.color_stop_iterator, palette);

			if(stops.empty()) return {white};

			slug_t cx = cv(sg.center.x) / 65536_cv * emScale;
			slug_t cy = cv(sg.center.y) / 65536_cv * emScale;

			parentMatrix.apply(cx, cy, cx, cy);

			const slug_t tau = 2_cv * PI_CV;
			const slug_t startAngle = cv(sg.start_angle) / 65536_cv * tau;
			const slug_t arcSpan = cv(sg.end_angle) / 65536_cv * tau - startAngle;

			const GradientInfo info{
				.type = GradientInfo::Type::Sweep,
				.stops = std::move(stops),
				.transform = buildSweepGradientMatrix(cx, cy, startAngle, arcSpan),
			};

			return {white, atlas.addGradient(info)};
		}

		// -----------------------------------------------------------------
		// ColrGlyph reference - TODO
		// -----------------------------------------------------------------
		case FT_COLR_PAINTFORMAT_COLR_GLYPH: {
			doLog(config.log, LOG_INFO, "COLRv1 PaintColrGlyph - not yet implemented");

			break;
		}

		// -----------------------------------------------------------------
		default: {
			doLog(
				config.log, LOG_INFO,
				"COLRv1 unhandled paint format ", paint.format, " - skipped"
			);

			break;
		}
	}

	return {white};
}

[[maybe_unused]] static void processColorGlyphV1(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out,
	const LoadConfig& config
) {
	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);
	const slug_t emScale = 1_cv / cv(face->units_per_EM);

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
		codepoint, layerIdx, atlas, out, config
	);
}

#endif

}

// =============================================================================
// Public API implementation
// =============================================================================

slughorn::FontMetrics readFontMetrics(FT_Face face) {
	slughorn::FontMetrics m;

	m.unitsPerEM = cv(face->units_per_EM);

	const slug_t upm = m.unitsPerEM;

	m.ascenderRatio = cv(face->ascender) / upm;
	m.descenderRatio = cv(std::abs(static_cast<long>(face->descender))) / upm;

	// face->height is the recommended line height; subtract the natural cap to get gap.
	const long lineGapRaw =
		static_cast<long>(face->height) -
		static_cast<long>(face->ascender) +
		static_cast<long>(face->descender) // descender is negative
	;

	m.lineGapRatio = lineGapRaw > 0 ? cv(lineGapRaw) / upm : 0_cv;

	TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));

	m.capHeightRatio = (os2 && os2->sCapHeight > 0)
		? cv(os2->sCapHeight) / upm
		: detail::measureGlyphHeight(face, 'H')
	;

	m.xHeightRatio = (os2 && os2->sxHeight > 0)
		? cv(os2->sxHeight) / upm
		: detail::measureGlyphHeight(face, 'x')
	;

	return m;
}

std::optional<slughorn::FontMetrics> loadFontMetrics(const std::string& fontPath) {
	return detail::withFace(
		fontPath,
		"loadFontMetrics",
		std::optional<slughorn::FontMetrics>{},
		nullptr,
		[](FT_Face face) -> std::optional<slughorn::FontMetrics> {
			return readFontMetrics(face);
		}
	);
}

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

bool loadGlyph(
	FT_Face face,
	uint32_t codepoint,
	Atlas& atlas,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	if(atlas.hasKey(codepoint)) return true; // already present - not an error

	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);

	if(!glyphIndex && codepoint != 0) return false;
	if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE)) return false;

	const slug_t emScale = 1_cv / cv(face->units_per_EM);

	Atlas::ShapeInfo data;

	data.autoMetrics = false;
	data.bearingX = cv(face->glyph->metrics.horiBearingX) * emScale;
	data.bearingY = cv(face->glyph->metrics.horiBearingY) * emScale;
	data.width = cv(face->glyph->metrics.width) * emScale;
	data.height = cv(face->glyph->metrics.height) * emScale;
	data.advance = cv(face->glyph->metrics.horiAdvance) * emScale;

	detail::decomposeOutline(face->glyph->outline, data.curves, emScale);

	if(cfg.strategy && !data.curves.empty()) {
		auto [sx, sy] = cfg.strategy(data.curves);

		data.splitsX = std::move(sx);
		data.splitsY = std::move(sy);
	}

	atlas.addShape(codepoint, data);

	return true;
}

size_t loadGlyphRange(
	FT_Face face,
	uint32_t first,
	uint32_t last,
	Atlas& atlas,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	if(cfg.uniform) {
		std::vector<uint32_t> cps;

		for(uint32_t cp = first; cp <= last; cp++) cps.push_back(cp);

		return loadGlyphs(face, cps, atlas, config);
	}

	return detail::countRange(first, last, [&](uint32_t cp) {
		return loadGlyph(face, cp, atlas, config);
	});
}

size_t loadGlyphs(
	FT_Face face,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	if(!cfg.uniform) {
		return detail::countCodepoints(codepoints, [&](uint32_t cp) {
			return loadGlyph(face, cp, atlas, config);
		});
	}

	// uniform=true: two-pass load so all glyphs share the same em-space bounding box.
	// Auto-detects tabular advances (Option C); falls back to union bbox + centering (Option A).
	if(codepoints.empty()) return 0;

	const slug_t emScale = 1_cv / cv(face->units_per_EM);

	// Pass 1: collect per-glyph FT metrics.
	struct GlyphMeta {
		uint32_t codepoint;

		slug_t bearingX, bearingY, width, height, advance;

		bool valid = false;
	};

	std::vector<GlyphMeta> meta;

	meta.reserve(codepoints.size());

	for(uint32_t cp : codepoints) {
		GlyphMeta m;

		m.codepoint = cp;

		const FT_UInt gi = FT_Get_Char_Index(face, cp);

		if((!gi && cp != 0) || FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE)) {
			meta.push_back(m);

			continue;
		}

		m.bearingX = cv(face->glyph->metrics.horiBearingX) * emScale;
		m.bearingY = cv(face->glyph->metrics.horiBearingY) * emScale;
		m.width = cv(face->glyph->metrics.width) * emScale;
		m.height = cv(face->glyph->metrics.height) * emScale;
		m.advance = cv(face->glyph->metrics.horiAdvance) * emScale;
		m.valid = true;

		meta.push_back(m);
	}

	// Detect tabular: all valid glyphs share the same horiAdvance.
	slug_t refAdvance = -1_cv;
	bool tabular = true;

	for(const auto& m : meta) {
		if(!m.valid) continue;

		if(refAdvance < 0_cv) refAdvance = m.advance;

		else if(std::abs(m.advance - refAdvance) > 1e-6_cv) { tabular = false; break; }
	}

	// Compute uniform cell dimensions.
	slug_t cellBearingX, cellBearingY, cellWidth, cellHeight, cellAdvance;

	if(tabular) {
		// Font already handles side-bearings; cell = advance x max ascent+descent.
		cellBearingX = 0_cv;
		cellWidth = refAdvance;
		cellAdvance = refAdvance;
		cellBearingY = 0_cv;
		cellHeight = 0_cv;

		for(const auto& m : meta) {
			if(!m.valid) continue;
			if(m.bearingY > cellBearingY) cellBearingY = m.bearingY;
			if(m.height > cellHeight) cellHeight = m.height;
		}

		doLog(
			cfg.log, LOG_NOTICE,
			"loadGlyphsUniform: tabular advances detected, cell width=", refAdvance
		);
	}

	else {
		// Union bounding box; glyphs will be centered horizontally within the cell.
		slug_t minLeft = std::numeric_limits<slug_t>::max();
		slug_t maxRight = -std::numeric_limits<slug_t>::max();

		cellBearingY = 0_cv;
		cellHeight = 0_cv;
		cellAdvance = 0_cv;

		for(const auto& m : meta) {
			if(!m.valid) continue;

			if(m.bearingX < minLeft) minLeft = m.bearingX;
			if(m.bearingX + m.width > maxRight) maxRight = m.bearingX + m.width;
			if(m.bearingY > cellBearingY) cellBearingY = m.bearingY;
			if(m.height > cellHeight) cellHeight = m.height;
			if(m.advance > cellAdvance) cellAdvance = m.advance;
		}

		cellBearingX = minLeft;
		cellWidth = maxRight - minLeft;

		doLog(
			cfg.log, LOG_NOTICE,
			"loadGlyphsUniform: non-tabular, union cell width=", cellWidth,
			" (centering applied)"
		);
	}

	// Pass 2: decompose each glyph with uniform metrics.
	// Non-tabular path applies a horizontal centering translation via Matrix.
	size_t count = 0;

	for(const auto& m : meta) {
		if(!m.valid || atlas.hasKey(m.codepoint)) continue;

		const FT_UInt gi = FT_Get_Char_Index(face, m.codepoint);

		if(FT_Load_Glyph(face, gi, FT_LOAD_NO_SCALE)) continue;

		Atlas::ShapeInfo data;

		data.autoMetrics = false;
		data.bearingX = cellBearingX;
		data.bearingY = cellBearingY;
		data.width = cellWidth;
		data.height = cellHeight;
		data.advance = cellAdvance;

		const slughorn::Matrix* matrixPtr = nullptr;
		slughorn::Matrix centerMatrix;

		if(!tabular) {
			const slug_t cellCenter = cellBearingX + cellWidth / 2_cv;
			const slug_t glyphCenter = m.bearingX + m.width / 2_cv;

			centerMatrix.dx = cellCenter - glyphCenter;
			matrixPtr = &centerMatrix;
		}

		if(!decomposeGlyph(face, emScale, cellAdvance, matrixPtr, data)) continue;

		if(cfg.strategy && !data.curves.empty()) {
			auto [sx, sy] = cfg.strategy(data.curves);

			data.splitsX = std::move(sx);
			data.splitsY = std::move(sy);
		}

		atlas.addShape(m.codepoint, data);

		count++;
	}

	return count;
	// -- end loadGlyphsUniform (inlined) --
}

size_t loadAllGlyphs(FT_Face face, Atlas& atlas, LoadConfig* config) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;

	if(cfg.uniform) {
		std::vector<uint32_t> cps;

		detail::countCharmap(face, [&](uint32_t cp) { cps.push_back(cp); return true; });

		return loadGlyphs(face, cps, atlas, config);
	}

	return detail::countCharmap(face, [&](uint32_t cp) {
		return loadGlyph(face, cp, atlas, config);
	});
}

bool loadColorGlyph(
	FT_Face face,
	uint32_t codepoint,
	FT_Color* palette,
	Atlas& atlas,
	CompositeShape& out,
	LoadConfig* config
) {
	static const LoadConfig dflt{};
	const LoadConfig& cfg = config ? *config : dflt;
	const FT_UInt glyphIndex = FT_Get_Char_Index(face, codepoint);

	if(!glyphIndex) {
		doLog(cfg.log, LOG_WARN, "U+", std::hex, codepoint, " not found in font");

		return false;
	}

	if(FT_Load_Glyph(face, glyphIndex, FT_LOAD_NO_SCALE)) return false;

	const slug_t emScale = 1_cv / cv(face->units_per_EM);

	out.advance = cv(face->glyph->metrics.horiAdvance) * emScale;

	// -------------------------------------------------------------------------
	// Try COLRv1 first (FreeType >= 2.11)
	// -------------------------------------------------------------------------

#if FREETYPE_MAJOR > 2 || (FREETYPE_MAJOR == 2 && FREETYPE_MINOR >= 11)

	{
		detail::processColorGlyphV1(face, codepoint, palette, atlas, out, cfg);

		if(!out.layers.empty()) {
			doLog(
				cfg.log, LOG_NOTICE,
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
	detail::processColorGlyphV0(face, codepoint, palette, atlas, out, cfg);

	if(!out.layers.empty()) {
		doLog(
			cfg.log, LOG_NOTICE,
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
	std::map<uint32_t, CompositeShape>& colorGlyphs,
	LoadConfig* config
) {
	return detail::countCodepoints(codepoints, [&](uint32_t cp) {
		CompositeShape glyph;

		if(loadColorGlyph(face, cp, palette, atlas, glyph, config)) {
			colorGlyphs[cp] = std::move(glyph);

			return true;
		}

		return false;
	});
}

bool loadAsciiFont(
	const std::string& fontPath,
	Atlas& atlas,
	LoadConfig* config
) {
	return detail::withFace(fontPath, "loadAsciiFont", false, config, [&](FT_Face face) {
		loadGlyphRange(face, 32, 126, atlas, config);

		return true;
	});
}

size_t loadFontGlyphs(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	LoadConfig* config
) {
	return detail::withFace(fontPath, "loadFontGlyphs", size_t(0), config, [&](FT_Face face) {
		return loadGlyphs(face, codepoints, atlas, config);
	});
}

size_t loadAllFontGlyphs(
	const std::string& fontPath,
	Atlas& atlas,
	LoadConfig* config
) {
	return detail::withFace(fontPath, "loadAllFontGlyphs", size_t(0), config, [&](FT_Face face) {
		return loadAllGlyphs(face, atlas, config);
	});
}

bool loadEmojiFont(
	const std::string& fontPath,
	const std::vector<uint32_t>& codepoints,
	Atlas& atlas,
	std::map<uint32_t, CompositeShape>& colorGlyphs,
	LoadConfig* config
) {
	return detail::withFace(fontPath, "loadEmojiFont", false, config, [&](FT_Face face) {
		auto* palette = detail::selectPalette(face);

		loadColorGlyphs(face, codepoints, palette, atlas, colorGlyphs, config);

		return true;
	});
}

}
}

#endif
