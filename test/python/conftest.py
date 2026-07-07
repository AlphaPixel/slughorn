import math
import pytest
import slughorn

# ---------------------------------------------------------------------------
# Primitive fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def color_red():
	return slughorn.Color(1.0, 0.0, 0.0, 1.0)

@pytest.fixture
def color_blue():
	return slughorn.Color(0.0, 0.0, 1.0, 1.0)

@pytest.fixture
def gradient_stops(color_red, color_blue):
	return [
		slughorn.GradientStop(0.0, color_red),
		slughorn.GradientStop(1.0, color_blue),
	]

@pytest.fixture
def rect_curves():
	"""Unit square (0,0)→(1,1) as a flat Curves list."""
	d = slughorn.CurveDecomposer()
	d.move_to(0.0, 0.0)
	d.line_to(1.0, 0.0)
	d.line_to(1.0, 1.0)
	d.line_to(0.0, 1.0)
	d.close()
	return d.get_curves()

# ---------------------------------------------------------------------------
# Atlas / Canvas fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def atlas():
	"""Fresh, un-built Atlas."""
	return slughorn.Atlas()

@pytest.fixture
def built_atlas(atlas, rect_curves):
	"""Atlas with a single unit-square shape pre-built."""
	info = slughorn.ShapeInfo()
	info.curves = rect_curves
	atlas.add_shape(slughorn.Key("rect"), info)
	atlas.build()
	return atlas

@pytest.fixture
def canvas(atlas):
	"""Canvas wired to a fresh Atlas with an auto-incrementing KeyIterator."""
	return slughorn.canvas.Canvas(atlas, slughorn.KeyIterator("t"))

# ---------------------------------------------------------------------------
# Font / path constants (used by freetype tests)
# ---------------------------------------------------------------------------

MONO_FONT = "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"
PROP_FONT = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
DIGIT_CODEPOINTS = [ord(c) for c in "0123456789"]

# ---------------------------------------------------------------------------
# Skip helpers
# ---------------------------------------------------------------------------

def requires_nanosvg():
	return pytest.mark.skipif(
		not hasattr(slughorn, "nanosvg"),
		reason="slughorn.nanosvg not compiled in (SLUGHORN_NANOSVG)",
	)

def requires_freetype():
	return pytest.mark.skipif(
		not hasattr(slughorn, "freetype"),
		reason="slughorn.freetype not compiled in (SLUGHORN_HAS_FREETYPE)",
	)

def requires_tessellate():
	return pytest.mark.skipif(
		not hasattr(slughorn, "tessellate"),
		reason="slughorn.tessellate not compiled in (SLUGHORN_TESSELLATE)",
	)
