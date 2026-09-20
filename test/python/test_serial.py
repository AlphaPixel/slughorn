"""
Tests for slughorn/serial.hpp — slughorn.read / slughorn.write.

Serial I/O is only compiled in when SLUGHORN_SERIAL=ON; every test below
is skipped automatically when the binding is absent.  SDF-specific tests
also require SLUGHORN_SDF=ON.

Formats:
    .slug   JSON  + base64-encoded texture blobs (human-readable)
    .slugb  Binary container: 12-byte header + JSON chunk + BIN chunk

Roundtrip invariants verified:
    - is_built() is True immediately after read()
    - All shapes present with correct band/metric/origin fields
    - per-shape sdf tiles (x, y, w, h, range, ...) restored
    - MSDF texture data is non-empty and the correct size
    - packing_stats.msdf_tile_size matches what was registered
    - CompositeShape layers (key, color, effectId) preserved
    - Error on unbuilt atlas write; error on missing file read
"""

import io
import math
import os
import pytest
import slughorn

# ---------------------------------------------------------------------------
# Capability guards
# ---------------------------------------------------------------------------

HAS_SERIAL = hasattr(slughorn, "read")
HAS_SDF    = hasattr(slughorn.render, "field")

skip_serial = pytest.mark.skipif(not HAS_SERIAL, reason="built without SLUGHORN_SERIAL=ON")
skip_sdf    = pytest.mark.skipif(not HAS_SDF,    reason="built without SLUGHORN_SDF=ON")

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _unit_square_atlas():
    """Atlas with a single unit-square shape, fully built."""
    atlas = slughorn.Atlas()
    d = slughorn.CurveDecomposer()
    d.move_to(0.0, 0.0)
    d.line_to(1.0, 0.0)
    d.line_to(1.0, 1.0)
    d.line_to(0.0, 1.0)
    d.close()
    info = slughorn.ShapeInfo()
    info.curves = d.get_curves()
    atlas.add_shape(slughorn.Key("rect"), info)
    atlas.build()
    return atlas


def _atlas_with_composite():
    """Atlas with two shapes and a CompositeShape, fully built."""
    atlas = slughorn.Atlas()

    for name, pts in [("A", [(0,0),(1,0),(1,1),(0,1)]), ("B", [(0,0),(0.5,0),(0.5,0.5),(0,0.5)])]:
        d = slughorn.CurveDecomposer()
        d.move_to(*pts[0])
        for p in pts[1:]:
            d.line_to(*p)
        d.close()
        info = slughorn.ShapeInfo()
        info.curves = d.get_curves()
        atlas.add_shape(slughorn.Key(name), info)

    cs = slughorn.CompositeShape()
    cs.layers.append(slughorn.Layer(slughorn.Key("A"), slughorn.Color(1.0, 0.0, 0.0, 1.0), effectId=7))
    cs.layers.append(slughorn.Layer(slughorn.Key("B"), slughorn.Color(0.0, 0.0, 1.0, 0.5)))
    atlas.add_composite_shape(slughorn.Key("AB"), cs)
    atlas.build()

    return atlas


def _check_rect_shape(shape):
    """Assert the unit-square shape metrics survived a roundtrip."""
    assert shape.width  == pytest.approx(1.0, abs=1e-5)
    assert shape.height == pytest.approx(1.0, abs=1e-5)
    assert shape.band_max_x > 0
    assert shape.band_max_y > 0


# ---------------------------------------------------------------------------
# Availability
# ---------------------------------------------------------------------------

def test_serial_read_exists():
    assert HAS_SERIAL, "slughorn.read not found — is SLUGHORN_SERIAL=ON?"

@skip_serial
def test_serial_write_exists():
    assert hasattr(slughorn, "write")


# ---------------------------------------------------------------------------
# Error cases
# ---------------------------------------------------------------------------

@skip_serial
def test_write_unbuilt_atlas_raises(tmp_path):
    atlas = slughorn.Atlas()
    with pytest.raises(RuntimeError):
        slughorn.write(atlas, str(tmp_path / "out.slug"))

@skip_serial
def test_read_missing_file_raises(tmp_path):
    with pytest.raises(RuntimeError):
        slughorn.read(str(tmp_path / "does_not_exist.slug"))


# ---------------------------------------------------------------------------
# JSON (.slug) roundtrip
# ---------------------------------------------------------------------------

@skip_serial
def test_json_roundtrip_is_built(tmp_path):
    path = str(tmp_path / "atlas.slug")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    assert back.is_built

@skip_serial
def test_json_roundtrip_shape_present(tmp_path):
    path = str(tmp_path / "atlas.slug")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    assert back.has_key(slughorn.Key("rect"))

@skip_serial
def test_json_roundtrip_shape_metrics(tmp_path):
    path = str(tmp_path / "atlas.slug")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    _check_rect_shape(back.get_shape(slughorn.Key("rect")))

@skip_serial
def test_json_roundtrip_composite(tmp_path):
    atlas = _atlas_with_composite()
    path  = str(tmp_path / "composite.slug")
    slughorn.write(atlas, path)
    back = slughorn.read(path)

    cs = back.get_composite_shape(slughorn.Key("AB"))
    assert cs is not None
    assert len(cs.layers) == 2
    assert cs.layers[0].key == slughorn.Key("A")
    assert cs.layers[0].color.r == pytest.approx(1.0, abs=1e-5)
    assert cs.layers[0].effectId == 7
    assert cs.layers[1].key == slughorn.Key("B")
    assert cs.layers[1].color.a == pytest.approx(0.5, abs=1e-5)


# ---------------------------------------------------------------------------
# Binary (.slugb) roundtrip
# ---------------------------------------------------------------------------

@skip_serial
def test_binary_roundtrip_is_built(tmp_path):
    path = str(tmp_path / "atlas.slugb")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    assert back.is_built

@skip_serial
def test_binary_roundtrip_shape_present(tmp_path):
    path = str(tmp_path / "atlas.slugb")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    assert back.has_key(slughorn.Key("rect"))

@skip_serial
def test_binary_roundtrip_shape_metrics(tmp_path):
    path = str(tmp_path / "atlas.slugb")
    slughorn.write(_unit_square_atlas(), path)
    back = slughorn.read(path)
    _check_rect_shape(back.get_shape(slughorn.Key("rect")))

@skip_serial
def test_binary_roundtrip_composite(tmp_path):
    atlas = _atlas_with_composite()
    path  = str(tmp_path / "composite.slugb")
    slughorn.write(atlas, path)
    back = slughorn.read(path)

    cs = back.get_composite_shape(slughorn.Key("AB"))
    assert cs is not None
    assert len(cs.layers) == 2
    assert cs.layers[0].key == slughorn.Key("A")
    assert cs.layers[1].key == slughorn.Key("B")


# ---------------------------------------------------------------------------
# In-memory JSON string (read_string / write_string) — no disk I/O
# ---------------------------------------------------------------------------

@skip_serial
def test_serial_read_string_exists():
    assert hasattr(slughorn, "read_string")

@skip_serial
def test_serial_write_string_exists():
    assert hasattr(slughorn, "write_string")

@skip_serial
def test_write_string_unbuilt_atlas_raises():
    atlas = slughorn.Atlas()
    with pytest.raises(RuntimeError):
        slughorn.write_string(atlas)

@skip_serial
def test_read_string_invalid_json_raises():
    with pytest.raises(RuntimeError):
        slughorn.read_string("not json")

@skip_serial
def test_write_string_returns_str():
    assert isinstance(slughorn.write_string(_unit_square_atlas()), str)

@skip_serial
def test_string_roundtrip_is_built():
    back = slughorn.read_string(slughorn.write_string(_unit_square_atlas()))
    assert back.is_built

@skip_serial
def test_string_roundtrip_shape_present():
    back = slughorn.read_string(slughorn.write_string(_unit_square_atlas()))
    assert back.has_key(slughorn.Key("rect"))

@skip_serial
def test_string_roundtrip_shape_metrics():
    back = slughorn.read_string(slughorn.write_string(_unit_square_atlas()))
    _check_rect_shape(back.get_shape(slughorn.Key("rect")))

@skip_serial
def test_string_roundtrip_composite():
    back = slughorn.read_string(slughorn.write_string(_atlas_with_composite()))
    cs = back.get_composite_shape(slughorn.Key("AB"))
    assert cs is not None
    assert len(cs.layers) == 2
    assert cs.layers[0].key == slughorn.Key("A")
    assert cs.layers[1].key == slughorn.Key("B")

@skip_serial
def test_string_roundtrip_matches_file_roundtrip(tmp_path):
    """write_string()'s JSON must decode to the same shape as write()'s .slug file."""
    atlas = _unit_square_atlas()
    path = str(tmp_path / "a.slug")
    slughorn.write(atlas, path)
    from_file = slughorn.read(path).get_shape(slughorn.Key("rect"))
    from_string = slughorn.read_string(slughorn.write_string(atlas)).get_shape(slughorn.Key("rect"))
    assert from_file.width      == pytest.approx(from_string.width,      abs=1e-5)
    assert from_file.height     == pytest.approx(from_string.height,     abs=1e-5)
    assert from_file.band_max_x == from_string.band_max_x
    assert from_file.band_max_y == from_string.band_max_y

@skip_serial
def test_write_string_compact_default():
    """Default pretty=True should differ from the explicit compact form."""
    atlas = _unit_square_atlas()
    pretty = slughorn.write_string(atlas)
    compact = slughorn.write_string(atlas, False)
    assert len(compact) < len(pretty)


# ---------------------------------------------------------------------------
# JSON vs binary consistency
# ---------------------------------------------------------------------------

@skip_serial
def test_json_and_binary_shape_metrics_agree(tmp_path):
    atlas = _unit_square_atlas()
    slughorn.write(atlas, str(tmp_path / "a.slug"))
    slughorn.write(atlas, str(tmp_path / "a.slugb"))
    j = slughorn.read(str(tmp_path / "a.slug")).get_shape(slughorn.Key("rect"))
    b = slughorn.read(str(tmp_path / "a.slugb")).get_shape(slughorn.Key("rect"))
    assert j.width       == pytest.approx(b.width,       abs=1e-5)
    assert j.height      == pytest.approx(b.height,      abs=1e-5)
    assert j.band_max_x  == b.band_max_x
    assert j.band_max_y  == b.band_max_y
    assert j.band_tex_x  == b.band_tex_x
    assert j.band_tex_y  == b.band_tex_y


# ---------------------------------------------------------------------------
# SDF roundtrip (requires SLUGHORN_SDF=ON)
# ---------------------------------------------------------------------------

def _sdf_atlas(type=slughorn.SDF.Type.MSDF, tile_size=64, range_=0.1, only=None):
    """Built atlas with SDF tiles: every shape by default, or just the keys in `only`."""
    atlas = slughorn.Atlas()
    atlas.set_sdf(slughorn.SDF.Config(type=type, tile_size=tile_size, range=range_))
    for name, pts in [("A", [(0,0),(1,0),(1,1),(0,1)]), ("B", [(0,0),(0.5,0),(0.5,0.5),(0,0.5)])]:
        d = slughorn.CurveDecomposer()
        d.move_to(*pts[0])
        for p in pts[1:]:
            d.line_to(*p)
        d.close()
        info = slughorn.ShapeInfo()
        info.curves = d.get_curves()
        atlas.add_shape(slughorn.Key(name), info)
    atlas.request_sdf([slughorn.Key(k) for k in (only or ("A", "B"))])
    atlas.build()
    return atlas


@skip_serial
@skip_sdf
@pytest.mark.parametrize("ext", ["slug", "slugb"])
@pytest.mark.parametrize("type", [slughorn.SDF.Type.SDF, slughorn.SDF.Type.MSDF])
def test_sdf_roundtrip_tiles(tmp_path, ext, type):
    orig = _sdf_atlas(type=type, range_=0.15)
    path = str(tmp_path / f"sdf.{ext}")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    for name in ("A", "B"):
        a = orig.get_shape(slughorn.Key(name)).sdf
        b = back.get_shape(slughorn.Key(name)).sdf
        assert b is not None
        assert (b.x, b.y, b.w, b.h) == (a.x, a.y, a.w, a.h)
        assert b.range == pytest.approx(a.range, abs=1e-5)
        assert b.texels_per_em == pytest.approx(a.texels_per_em, rel=1e-5)
        assert b.em_origin == pytest.approx(a.em_origin, abs=1e-5)

@skip_serial
@skip_sdf
@pytest.mark.parametrize("ext", ["slug", "slugb"])
@pytest.mark.parametrize("type", [slughorn.SDF.Type.SDF, slughorn.SDF.Type.MSDF])
def test_sdf_roundtrip_config_and_format(tmp_path, ext, type):
    orig = _sdf_atlas(type=type, tile_size=48)
    path = str(tmp_path / f"sdf.{ext}")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    assert back.sdf.config.type == type
    assert back.sdf.config.tile_size == 48
    assert back.sdf.texture.format == orig.sdf.texture.format
    assert (back.sdf.texture.width, back.sdf.texture.height) == (orig.sdf.texture.width, orig.sdf.texture.height)

@skip_serial
@skip_sdf
@pytest.mark.parametrize("ext", ["slug", "slugb"])
@pytest.mark.parametrize("type", [slughorn.SDF.Type.SDF, slughorn.SDF.Type.MSDF])
def test_sdf_roundtrip_texture_bytes_identical(tmp_path, ext, type):
    """Raw float texels must survive both containers unchanged."""
    orig = _sdf_atlas(type=type, tile_size=32)
    orig_bytes = bytes(memoryview(orig.sdf.texture.bytes))
    path = str(tmp_path / f"sdf.{ext}")
    slughorn.write(orig, path)
    back = slughorn.read(path)  # named variable keeps the Atlas alive during the memoryview read
    assert bytes(memoryview(back.sdf.texture.bytes)) == orig_bytes

@skip_serial
@skip_sdf
@pytest.mark.parametrize("ext", ["slug", "slugb"])
def test_sdf_roundtrip_packing_stats(tmp_path, ext):
    orig = _sdf_atlas(tile_size=48)
    path = str(tmp_path / f"sdf.{ext}")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    a, b = orig.packing_stats.sdf, back.packing_stats.sdf
    assert b.tile_count == a.tile_count == 2
    assert b.texels_total == a.texels_total
    assert b.texels_used == a.texels_used
    assert b.format == a.format

@skip_serial
@skip_sdf
@pytest.mark.parametrize("ext", ["slug", "slugb"])
def test_sdf_shapes_without_a_tile_stay_none_after_roundtrip(tmp_path, ext):
    """Only "A" was requested, so "B" must still have no tile after a roundtrip."""
    atlas = _sdf_atlas(tile_size=32, only=("A",))
    path = str(tmp_path / f"partial.{ext}")
    slughorn.write(atlas, path)
    back = slughorn.read(path)
    assert back.get_shape(slughorn.Key("A")).sdf is not None
    assert back.get_shape(slughorn.Key("B")).sdf is None

@skip_serial
def test_atlas_without_sdf_roundtrips_empty(tmp_path):
    atlas = _unit_square_atlas()
    path = str(tmp_path / "plain.slug")
    slughorn.write(atlas, path)
    back = slughorn.read(path)
    assert back.sdf.empty
    assert back.get_shape(slughorn.Key("rect")).sdf is None
