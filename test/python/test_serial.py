"""
Tests for slughorn/serial.hpp — slughorn.read / slughorn.write.

Serial I/O is only compiled in when SLUGHORN_SERIAL=ON; every test below
is skipped automatically when the binding is absent.  MSDF-specific tests
also require SLUGHORN_MSDF=ON.

Formats:
    .slug   JSON  + base64-encoded texture blobs (human-readable)
    .slugb  Binary container: 12-byte header + JSON chunk + BIN chunk

Roundtrip invariants verified:
    - is_built() is True immediately after read()
    - All shapes present with correct band/metric/origin fields
    - msdf_layer and msdf_range restored per shape
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
HAS_MSDF   = hasattr(slughorn.Atlas, "request_msdf")

skip_serial = pytest.mark.skipif(not HAS_SERIAL, reason="built without SLUGHORN_SERIAL=ON")
skip_msdf   = pytest.mark.skipif(not HAS_MSDF,   reason="built without SLUGHORN_MSDF=ON")

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
# MSDF roundtrip (requires SLUGHORN_MSDF=ON)
# ---------------------------------------------------------------------------

def _msdf_atlas(tile_size=64, range_=0.1):
    """Built atlas with one MSDF-registered shape."""
    atlas = _unit_square_atlas()
    atlas.msdf_tile_size = tile_size
    atlas.request_msdf(slughorn.Key("rect"), range_)
    return atlas


@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_layer(tmp_path):
    orig  = _msdf_atlas()
    path  = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back  = slughorn.read(path)
    shape = back.get_shape(slughorn.Key("rect"))
    assert shape.msdf_layer == 0

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_range(tmp_path):
    orig  = _msdf_atlas(range_=0.15)
    path  = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back  = slughorn.read(path)
    shape = back.get_shape(slughorn.Key("rect"))
    assert shape.msdf_range == pytest.approx(0.15, abs=1e-4)

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_get_msdf_layer(tmp_path):
    orig = _msdf_atlas()
    path = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    assert back.get_msdf_layer(slughorn.Key("rect")) == 0

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_texture_non_empty(tmp_path):
    orig = _msdf_atlas(tile_size=32)
    path = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    td = back.get_msdf_texture_data()
    assert td is not None
    mv = memoryview(td)
    assert len(mv) > 0

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_texture_size(tmp_path):
    tile_size = 32
    orig = _msdf_atlas(tile_size=tile_size)
    path = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    td = back.get_msdf_texture_data()
    mv = memoryview(td)
    # RGB32F: tile_size * tile_size * 3 channels * 4 bytes per float, 1 layer
    expected = tile_size * tile_size * 3 * 4
    assert len(mv) == expected

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_packing_stats(tmp_path):
    tile_size = 48
    orig = _msdf_atlas(tile_size=tile_size)
    path = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    assert back.packing_stats.msdf_tile_size == tile_size
    assert back.packing_stats.msdf_layer_count == 1

@skip_serial
@skip_msdf
def test_msdf_binary_roundtrip_layer(tmp_path):
    orig = _msdf_atlas()
    path = str(tmp_path / "msdf.slugb")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    assert back.get_shape(slughorn.Key("rect")).msdf_layer == 0

@skip_serial
@skip_msdf
def test_msdf_binary_roundtrip_range(tmp_path):
    orig = _msdf_atlas(range_=0.2)
    path = str(tmp_path / "msdf.slugb")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    assert back.get_shape(slughorn.Key("rect")).msdf_range == pytest.approx(0.2, abs=1e-4)

@skip_serial
@skip_msdf
def test_msdf_binary_roundtrip_texture_non_empty(tmp_path):
    orig = _msdf_atlas(tile_size=32)
    path = str(tmp_path / "msdf.slugb")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    td = back.get_msdf_texture_data()
    assert td is not None
    assert len(memoryview(td)) > 0

@skip_serial
@skip_msdf
def test_msdf_binary_roundtrip_texture_size(tmp_path):
    tile_size = 32
    orig = _msdf_atlas(tile_size=tile_size)
    path = str(tmp_path / "msdf.slugb")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    td = back.get_msdf_texture_data()
    expected = tile_size * tile_size * 3 * 4
    assert len(memoryview(td)) == expected

@skip_serial
@skip_msdf
def test_msdf_json_roundtrip_texture_bytes_identical(tmp_path):
    """Raw RGB32F bytes must survive the .slug base64 roundtrip unchanged."""
    orig = _msdf_atlas(tile_size=32)
    # Force packing before write; get_msdf_texture_data() triggers it on first call.
    orig_bytes = bytes(memoryview(orig.get_msdf_texture_data()))
    path = str(tmp_path / "msdf.slug")
    slughorn.write(orig, path)
    back = slughorn.read(path)  # named variable keeps Atlas alive during memoryview read
    back_bytes = bytes(memoryview(back.get_msdf_texture_data()))
    assert orig_bytes == back_bytes

@skip_serial
@skip_msdf
def test_msdf_binary_roundtrip_texture_bytes_identical(tmp_path):
    """Raw RGB32F bytes must survive the .slugb binary roundtrip unchanged."""
    orig = _msdf_atlas(tile_size=32)
    orig_bytes = bytes(memoryview(orig.get_msdf_texture_data()))
    path = str(tmp_path / "msdf.slugb")
    slughorn.write(orig, path)
    back = slughorn.read(path)
    back_bytes = bytes(memoryview(back.get_msdf_texture_data()))
    assert orig_bytes == back_bytes

@skip_serial
@skip_msdf
def test_msdf_shapes_without_msdf_have_negative_layer(tmp_path):
    """Shapes that weren't registered should still have msdf_layer == -1 after roundtrip."""
    atlas = _atlas_with_composite()
    # Register MSDF only for "A", not "B"
    atlas.msdf_tile_size = 32
    atlas.request_msdf(slughorn.Key("A"), 0.1)
    path = str(tmp_path / "partial.slug")
    slughorn.write(atlas, path)
    back = slughorn.read(path)
    assert back.get_shape(slughorn.Key("A")).msdf_layer == 0
    assert back.get_shape(slughorn.Key("B")).msdf_layer == -1
