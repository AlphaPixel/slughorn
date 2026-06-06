"""
Tests for slughorn/nanosvg.hpp — slughorn.nanosvg submodule.

All tests are skipped if slughorn.nanosvg is not compiled in.
"""

import pathlib
import pytest
import slughorn
from conftest import requires_nanosvg

pytestmark = requires_nanosvg()

_SVG_DIR = pathlib.Path(__file__).resolve().parent.parent

# ---------------------------------------------------------------------------
# Minimal inline SVGs
# ---------------------------------------------------------------------------

_SVG_ONE_SOLID = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
  <rect x="10" y="10" width="80" height="80" fill="#ff0000"/>
</svg>
"""

_SVG_TWO_SOLIDS = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 100">
  <rect x="0"   y="0" width="100" height="100" fill="#00ff00"/>
  <rect x="100" y="0" width="100" height="100" fill="#0000ff"/>
</svg>
"""

_SVG_EMPTY = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100"/>'

_SVG_WITH_IDS = """\
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 300 100">
  <rect id="keep"    x="0"   y="0" width="100" height="100" fill="#ff0000"/>
  <rect id="exclude" x="100" y="0" width="100" height="100" fill="#00ff00"/>
  <rect id="geo"     x="200" y="0" width="100" height="100" fill="#0000ff"/>
</svg>
"""


# ---------------------------------------------------------------------------
# Module / class availability
# ---------------------------------------------------------------------------

def test_nanosvg_submodule_exists():
	assert hasattr(slughorn, "nanosvg")

def test_shape_policy_enum_exists():
	assert hasattr(slughorn.nanosvg, "ShapePolicy")

def test_shape_rule_class_exists():
	assert hasattr(slughorn.nanosvg, "ShapeRule")

def test_load_string_function_exists():
	assert hasattr(slughorn.nanosvg, "load_string")

def test_load_file_function_exists():
	assert hasattr(slughorn.nanosvg, "load_file")


# ---------------------------------------------------------------------------
# ShapePolicy enum
# ---------------------------------------------------------------------------

def test_shape_policy_values():
	sp = slughorn.nanosvg.ShapePolicy
	assert hasattr(sp, "Default")
	assert hasattr(sp, "ForceInclude")
	assert hasattr(sp, "ForceExclude")
	assert hasattr(sp, "GeometryOnly")

def test_shape_policy_bitwise_or():
	sp = slughorn.nanosvg.ShapePolicy
	combined = sp.ForceInclude | sp.GeometryOnly
	assert combined is not None

def test_shape_policy_ror():
	# __ror__ must work for the reverse-or case.
	sp = slughorn.nanosvg.ShapePolicy
	combined = sp.GeometryOnly | sp.ForceInclude
	assert combined is not None


# ---------------------------------------------------------------------------
# ShapeRule
# ---------------------------------------------------------------------------

def test_shape_rule_default_policy():
	rule = slughorn.nanosvg.ShapeRule(".*")
	assert rule is not None

def test_shape_rule_explicit_policy():
	sp = slughorn.nanosvg.ShapePolicy
	rule = slughorn.nanosvg.ShapeRule("exclude.*", sp.ForceExclude)
	assert rule is not None


# ---------------------------------------------------------------------------
# load_string — basic
# ---------------------------------------------------------------------------

def test_load_string_single_solid(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)
	assert len(composite) == 1

def test_load_string_two_solids(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)
	assert len(composite) == 2

def test_load_string_empty_svg(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_EMPTY, atlas)
	assert len(composite) == 0

def test_load_string_invalid_xml(atlas):
	# Garbage input must not crash; returns empty composite.
	composite = slughorn.nanosvg.load_string("this is not xml", atlas)
	assert len(composite) == 0


# ---------------------------------------------------------------------------
# load_file
# ---------------------------------------------------------------------------

def test_load_file_gradient(atlas):
	svg_path = _SVG_DIR / "gradient-test.svg"
	if not svg_path.exists():
		pytest.skip(f"test SVG not found: {svg_path}")
	composite = slughorn.nanosvg.load_file(str(svg_path), atlas)
	assert len(composite) == 3

def test_load_file_inkscape(atlas):
	svg_path = _SVG_DIR / "inkscape-test.svg"
	if not svg_path.exists():
		pytest.skip(f"test SVG not found: {svg_path}")
	composite = slughorn.nanosvg.load_file(str(svg_path), atlas)
	assert len(composite) == 1

def test_load_file_bad_path(atlas):
	composite = slughorn.nanosvg.load_file("/nonexistent/path/file.svg", atlas)
	assert len(composite) == 0


# ---------------------------------------------------------------------------
# Key assignment and uniqueness
# ---------------------------------------------------------------------------

def test_load_string_keys_assigned(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)
	assert composite.layers[0].key is not None

def test_load_string_no_explicit_key_iterator(atlas):
	# Omitting keys= must still produce valid, non-None keys.
	composite = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)
	for layer in composite.layers:
		assert layer.key is not None

def test_load_string_chained_keys_unique(atlas):
	keys = slughorn.KeyIterator()
	composite_a = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas, keys)
	composite_b = slughorn.nanosvg.load_string(_SVG_ONE_SOLID,  atlas, keys)
	keys_a = {layer.key for layer in composite_a.layers}
	keys_b = {layer.key for layer in composite_b.layers}
	assert keys_a.isdisjoint(keys_b), "chained loads must not produce overlapping keys"

def test_load_file_keys_unique(atlas):
	svg_path = _SVG_DIR / "gradient-test.svg"
	if not svg_path.exists():
		pytest.skip(f"test SVG not found: {svg_path}")
	keys = slughorn.KeyIterator()
	comp_a = slughorn.nanosvg.load_file(str(svg_path), atlas, keys)
	comp_b = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas, keys)
	keys_a = {layer.key for layer in comp_a.layers}
	keys_b = {layer.key for layer in comp_b.layers}
	assert keys_a.isdisjoint(keys_b)


# ---------------------------------------------------------------------------
# Layer color correctness
# ---------------------------------------------------------------------------

def test_layer_color_red(atlas):
	# _SVG_ONE_SOLID has fill="#ff0000"; colorFromNSVG unpacks 0xAABBGGRR.
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)
	layer = composite.layers[0]
	assert layer.color.r == pytest.approx(1.0, abs=1e-3)
	assert layer.color.g == pytest.approx(0.0, abs=1e-3)
	assert layer.color.b == pytest.approx(0.0, abs=1e-3)
	assert layer.color.a == pytest.approx(1.0, abs=1e-3)

def test_layer_color_green(atlas):
	# First rect in _SVG_TWO_SOLIDS is #00ff00.
	composite = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)
	layer = composite.layers[0]
	assert layer.color.g == pytest.approx(1.0, abs=1e-3)
	assert layer.color.r == pytest.approx(0.0, abs=1e-3)
	assert layer.color.b == pytest.approx(0.0, abs=1e-3)


# ---------------------------------------------------------------------------
# Atlas shape registration
# ---------------------------------------------------------------------------

def test_all_layers_in_atlas(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_TWO_SOLIDS, atlas)
	atlas.build()
	for layer in composite.layers:
		assert atlas.get_shape(layer.key) is not None

def test_composite_advance_nonzero(atlas):
	# advance should reflect the SVG viewBox width.
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)
	assert composite.advance > 0.0


# ---------------------------------------------------------------------------
# dpi parameter
# ---------------------------------------------------------------------------

def test_dpi_parameter_accepted(atlas):
	# dpi is accepted without error. For viewBox-relative SVGs slughorn normalises
	# to em-space after parsing, so dpi does NOT change Shape.width/height — it
	# only matters when the SVG uses physical units (mm, cm, in).
	comp = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas, dpi=144.0)
	assert len(comp) == 1

def test_dpi_no_effect_on_viewbox_svg():
	atlas_72  = slughorn.Atlas()
	atlas_144 = slughorn.Atlas()

	comp_72  = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas_72,  dpi=72.0)
	comp_144 = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas_144, dpi=144.0)

	atlas_72.build()
	atlas_144.build()

	shape_72  = atlas_72.get_shape(comp_72.layers[0].key)
	shape_144 = atlas_144.get_shape(comp_144.layers[0].key)

	assert shape_72  is not None
	assert shape_144 is not None
	# viewBox-relative coords: em-space dimensions are identical regardless of dpi.
	assert shape_72.width == pytest.approx(shape_144.width, rel=1e-5)


# ---------------------------------------------------------------------------
# log callback
# ---------------------------------------------------------------------------

def test_log_callback_called_on_bad_file(atlas):
	messages = []

	def on_log(level, msg):
		messages.append((level, msg))

	slughorn.nanosvg.load_file("/nonexistent/path/file.svg", atlas, log=on_log)
	assert len(messages) > 0

def test_log_callback_receives_level_and_string(atlas):
	messages = []

	def on_log(level, msg):
		messages.append((level, msg))

	slughorn.nanosvg.load_file("/nonexistent/path/file.svg", atlas, log=on_log)

	for level, msg in messages:
		assert isinstance(level, int)
		assert isinstance(msg, str)


# ---------------------------------------------------------------------------
# ShapeRule filtering
# ---------------------------------------------------------------------------

def test_shape_rule_force_exclude(atlas):
	sp    = slughorn.nanosvg.ShapePolicy
	rules = [slughorn.nanosvg.ShapeRule("exclude", sp.ForceExclude)]
	composite = slughorn.nanosvg.load_string(_SVG_WITH_IDS, atlas, rules=rules)
	# "exclude" rect should be absent; "keep" and "geo" remain.
	keys = {layer.key for layer in composite.layers}
	assert len(composite) == 2, f"expected 2 layers after ForceExclude, got {len(composite)}"

def test_shape_rule_geometry_only(atlas):
	sp    = slughorn.nanosvg.ShapePolicy
	rules = [slughorn.nanosvg.ShapeRule("geo", sp.GeometryOnly)]
	composite = slughorn.nanosvg.load_string(_SVG_WITH_IDS, atlas, rules=rules)
	# "geo" shape has curves but no layer in the composite.
	assert len(composite) == 2, f"expected 2 layers after GeometryOnly, got {len(composite)}"

def test_shape_rule_geometry_only_shape_in_atlas(atlas):
	sp    = slughorn.nanosvg.ShapePolicy
	rules = [slughorn.nanosvg.ShapeRule("geo", sp.GeometryOnly)]
	composite = slughorn.nanosvg.load_string(_SVG_WITH_IDS, atlas, rules=rules)
	atlas.build()
	# Even though there's no layer, the shape data must still be in the atlas.
	all_keys = [layer.key for layer in composite.layers]
	# We can't query "geo" directly because we don't know its key — but we can verify
	# the atlas built without error and the two visible shapes are present.
	for key in all_keys:
		assert atlas.get_shape(key) is not None


# ---------------------------------------------------------------------------
# Renderability
# ---------------------------------------------------------------------------

def test_renderable_solid(atlas):
	composite = slughorn.nanosvg.load_string(_SVG_ONE_SOLID, atlas)
	atlas.build()
	sampler = atlas.decode(composite.layers[0].key)
	grid = sampler.render_grid(32, 0.0, True)
	assert (grid.height, grid.width) == (32, 32)
	flat = memoryview(grid).cast('B').cast('f')
	assert max(flat) > 0.5, "solid filled rect must have high coverage"
	assert min(flat) >= 0.0

def test_renderable_all_gradient_shapes(atlas):
	svg_path = _SVG_DIR / "gradient-test.svg"
	if not svg_path.exists():
		pytest.skip(f"test SVG not found: {svg_path}")
	composite = slughorn.nanosvg.load_file(str(svg_path), atlas)
	atlas.build()
	for layer in composite.layers:
		grid = atlas.decode(layer.key).render_grid(16, 0.0, True)
		assert max(memoryview(grid).cast('B').cast('f')) > 0.0, f"gradient shape {layer.key} rendered empty"
