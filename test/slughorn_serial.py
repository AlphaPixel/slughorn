#!/usr/bin/env python3
# slughorn_serial.py
#
# Pure-Python reader/writer for the slughorn .slug (JSON) and .slugb (binary)
# atlas serialization formats.
#
# No compiled slughorn module required for reading — the duck-typed
# SlugAtlasData class is accepted anywhere a slughorn.Atlas is expected,
# as long as the consumer only calls:
#   .get_shape(key)       → Shape-like object or None
#   .get_composite(key)   → CompositeShape-like object or None
#   .curve_texture        → object with .width, .height, .bytes (memoryview/bytes)
#   .band_texture         → object with .width, .height, .bytes (memoryview/bytes)
#
# That covers AtlasView in slughorn_render.py and all the diagnostic tooling.
#
# Writing requires a real slughorn.Atlas (needs .getShapes() etc. from the
# C++ bindings) OR a SlugAtlasData instance (round-trips cleanly).
#
# Formats
# -------
#   .slug   JSON + base64 texture blobs.  Human-readable.
#   .slugb  Binary container (12-byte header + JSON chunk + BIN chunk).
#           Layout mirrors glTF's .glb — see slughorn-serial.hpp for the spec.

import base64
import json
import struct
import types
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Tuple

# =============================================================================
# Constants — must match slughorn-serial.hpp
# =============================================================================

SLUG_MAGIC          = b"SLUG"
SLUG_VERSION        = 1
CHUNK_TYPE_JSON     = 0x4E4F534A  # "JSON"
CHUNK_TYPE_BIN      = 0x004E4942  # "BIN\0"
CURRENT_FORMAT_VER  = "1.0"

# =============================================================================
# Lightweight data classes (duck-typed Atlas substitute)
# =============================================================================

@dataclass
class SlugShape:
    """Mirrors slughorn::Atlas::Shape.  All fields read-only by convention."""
    band_tex_x:    int   = 0
    band_tex_y:    int   = 0
    band_max_x:    int   = 0
    band_max_y:    int   = 0
    band_scale_x:  float = 0.0
    band_scale_y:  float = 0.0
    band_offset_x: float = 0.0
    band_offset_y: float = 0.0
    bearing_x:     float = 0.0
    bearing_y:     float = 0.0
    width:         float = 0.0
    height:        float = 0.0
    advance:       float = 0.0

    # Convenience — mirrors Shape.em_to_uv() from the C++ bindings
    def em_to_uv(self, em_x: float, em_y: float) -> Tuple[float, float]:
        ox = (-self.band_offset_x / self.band_scale_x) if self.band_scale_x else 0.0
        oy = (-self.band_offset_y / self.band_scale_y) if self.band_scale_y else 0.0
        sx = (float(self.band_max_x + 1) / self.band_scale_x) if self.band_scale_x else 1.0
        sy = (float(self.band_max_y + 1) / self.band_scale_y) if self.band_scale_y else 1.0
        return ((em_x - ox) / sx, (em_y - oy) / sy)

    def __repr__(self):
        return (f"SlugShape(size={self.width:.3f}x{self.height:.3f} "
                f"bands={self.band_max_x+1}x{self.band_max_y+1} "
                f"advance={self.advance:.3f})")


@dataclass
class SlugColor:
    r: float = 0.0
    g: float = 0.0
    b: float = 0.0
    a: float = 1.0

    def to_tuple(self):
        return (self.r, self.g, self.b, self.a)


@dataclass
class SlugMatrix:
    """Mirrors slughorn::Matrix — column-major affine [xx, yx, xy, yy, dx, dy]."""
    xx: float = 1.0
    yx: float = 0.0
    xy: float = 0.0
    yy: float = 1.0
    dx: float = 0.0
    dy: float = 0.0

    def is_identity(self) -> bool:
        eps = 1e-6
        return (abs(self.xx - 1) < eps and abs(self.yy - 1) < eps and
                abs(self.yx) < eps and abs(self.xy) < eps and
                abs(self.dx) < eps and abs(self.dy) < eps)

    def apply(self, x: float, y: float) -> Tuple[float, float]:
        return (self.xx * x + self.xy * y + self.dx,
                self.yx * x + self.yy * y + self.dy)


@dataclass
class SlugKey:
    """Mirrors slughorn::Key."""
    type:  str              # "codepoint" or "name"
    value: Any              # int (codepoint) or str (name)

    def __hash__(self):
        return hash((self.type, self.value))

    def __eq__(self, other):
        if isinstance(other, SlugKey):
            return self.type == other.type and self.value == other.value
        # Allow comparison with plain int (codepoint shorthand)
        if isinstance(other, int):
            return self.type == "codepoint" and self.value == other
        return NotImplemented

    def __repr__(self):
        if self.type == "codepoint":
            return f"SlugKey(0x{self.value:X})"
        return f'SlugKey("{self.value}")'


@dataclass
class SlugLayer:
    """Mirrors slughorn::Layer."""
    key:       SlugKey   = field(default_factory=lambda: SlugKey("codepoint", 0))
    color:     SlugColor = field(default_factory=SlugColor)
    transform: SlugMatrix= field(default_factory=SlugMatrix)
    effect_id: int       = 0


@dataclass
class SlugCompositeShape:
    """Mirrors slughorn::CompositeShape."""
    layers:  List[SlugLayer] = field(default_factory=list)
    advance: float           = 0.0

    def __len__(self):
        return len(self.layers)


@dataclass
class SlugTextureData:
    """Mirrors slughorn::Atlas::TextureData — holds raw pixel bytes."""
    width:  int   = 0
    height: int   = 0
    format: str   = "RGBA32F"   # "RGBA32F" or "RGBA16UI"
    bytes:  bytes = field(default_factory=bytes)


class SlugAtlasData:
    """
    Duck-typed Atlas substitute.  Accepted anywhere a slughorn.Atlas is
    expected, as long as the consumer only uses get_shape(), get_composite(),
    curve_texture, and band_texture.

    Constructed by load_slug() — do not instantiate directly.
    """

    def __init__(
        self,
        shapes:          Dict[SlugKey, SlugShape],
        composites:      Dict[SlugKey, SlugCompositeShape],
        curve_texture:   SlugTextureData,
        band_texture:    SlugTextureData,
    ):
        self._shapes     = shapes
        self._composites = composites
        self.curve_texture = curve_texture
        self.band_texture  = band_texture

    def get_shape(self, key) -> Optional[SlugShape]:
        """
        Accept SlugKey, int (codepoint), or str (name).
        Mirrors slughorn.Atlas.get_shape().
        """
        k = _normalize_key(key)
        return self._shapes.get(k)

    def get_composite(self, key) -> Optional[SlugCompositeShape]:
        k = _normalize_key(key)
        return self._composites.get(k)

    def is_built(self) -> bool:
        return True

    def __repr__(self):
        return (f"SlugAtlasData("
                f"{len(self._shapes)} shapes, "
                f"{len(self._composites)} composites, "
                f"curve={self.curve_texture.width}x{self.curve_texture.height}, "
                f"band={self.band_texture.width}x{self.band_texture.height})")


# =============================================================================
# Internal helpers
# =============================================================================

def _normalize_key(key) -> SlugKey:
    """Coerce int / str / SlugKey to a canonical SlugKey for dict lookup."""
    if isinstance(key, SlugKey):
        return key
    if isinstance(key, int):
        return SlugKey("codepoint", key)
    if isinstance(key, str):
        return SlugKey("name", key)
    raise TypeError(f"Cannot convert {type(key)} to SlugKey")


def _key_to_dict(key: SlugKey) -> dict:
    return {"type": key.type, "value": key.value}


def _key_from_dict(d: dict) -> SlugKey:
    t = d["type"]
    v = d["value"]

    if t == "codepoint":
        return SlugKey("codepoint", int(v))
    if t == "name":
        return SlugKey("name", str(v))

    raise ValueError(f"Unknown key type '{t}'")


def _shape_to_dict(key: SlugKey, shape: SlugShape) -> dict:
    return {
        "key":          _key_to_dict(key),
        "band_tex_x":   shape.band_tex_x,
        "band_tex_y":   shape.band_tex_y,
        "band_max_x":   shape.band_max_x,
        "band_max_y":   shape.band_max_y,
        "band_scale_x": shape.band_scale_x,
        "band_scale_y": shape.band_scale_y,
        "band_offset_x":shape.band_offset_x,
        "band_offset_y":shape.band_offset_y,
        "bearing_x":    shape.bearing_x,
        "bearing_y":    shape.bearing_y,
        "width":        shape.width,
        "height":       shape.height,
        "advance":      shape.advance,
    }


def _shape_from_dict(d: dict) -> Tuple[SlugKey, SlugShape]:
    key = _key_from_dict(d["key"])
    shape = SlugShape(
        band_tex_x   = d["band_tex_x"],
        band_tex_y   = d["band_tex_y"],
        band_max_x   = d["band_max_x"],
        band_max_y   = d["band_max_y"],
        band_scale_x = d["band_scale_x"],
        band_scale_y = d["band_scale_y"],
        band_offset_x= d["band_offset_x"],
        band_offset_y= d["band_offset_y"],
        bearing_x    = d["bearing_x"],
        bearing_y    = d["bearing_y"],
        width        = d["width"],
        height       = d["height"],
        advance      = d["advance"],
    )
    return key, shape


def _composite_to_dict(key: SlugKey, composite: SlugCompositeShape) -> dict:
    layers = []

    for layer in composite.layers:
        layers.append({
            "key":      _key_to_dict(layer.key),
            "color":    [layer.color.r, layer.color.g, layer.color.b, layer.color.a],
            "transform":[
                layer.transform.xx, layer.transform.yx,
                layer.transform.xy, layer.transform.yy,
                layer.transform.dx, layer.transform.dy,
            ],
            "effect_id": layer.effect_id,
        })

    return {
        "key":     _key_to_dict(key),
        "advance": composite.advance,
        "layers":  layers,
    }


def _composite_from_dict(d: dict) -> Tuple[SlugKey, SlugCompositeShape]:
    key = _key_from_dict(d["key"])
    composite = SlugCompositeShape(advance=d["advance"])

    for jl in d["layers"]:
        c = jl["color"]
        t = jl["transform"]

        layer = SlugLayer(
            key       = _key_from_dict(jl["key"]),
            color     = SlugColor(c[0], c[1], c[2], c[3]),
            transform = SlugMatrix(t[0], t[1], t[2], t[3], t[4], t[5]),
            effect_id = jl["effect_id"],
        )

        composite.layers.append(layer)

    return key, composite


def _atlas_to_shapes_composites(atlas) -> Tuple[
    Dict[SlugKey, SlugShape],
    Dict[SlugKey, SlugCompositeShape]
]:
    """
    Extract shapes and composites from either a SlugAtlasData or a real
    slughorn.Atlas (C++ binding).
    """
    shapes     = {}
    composites = {}

    if isinstance(atlas, SlugAtlasData):
        return dict(atlas._shapes), dict(atlas._composites)

    # Real slughorn.Atlas — use C++ accessors
    for cpp_key, cpp_shape in atlas.get_shapes().items():
        k = SlugKey("codepoint", cpp_key.codepoint()) \
            if cpp_key.type == 0 \
            else SlugKey("name", cpp_key.name())

        shapes[k] = SlugShape(
            band_tex_x   = cpp_shape.band_tex_x,
            band_tex_y   = cpp_shape.band_tex_y,
            band_max_x   = cpp_shape.band_max_x,
            band_max_y   = cpp_shape.band_max_y,
            band_scale_x = cpp_shape.band_scale_x,
            band_scale_y = cpp_shape.band_scale_y,
            band_offset_x= cpp_shape.band_offset_x,
            band_offset_y= cpp_shape.band_offset_y,
            bearing_x    = cpp_shape.bearing_x,
            bearing_y    = cpp_shape.bearing_y,
            width        = cpp_shape.width,
            height       = cpp_shape.height,
            advance      = cpp_shape.advance,
        )

    for cpp_key, cpp_composite in atlas.get_composite_shapes().items():
        k = SlugKey("codepoint", cpp_key.codepoint()) \
            if cpp_key.type == 0 \
            else SlugKey("name", cpp_key.name())

        composite = SlugCompositeShape(advance=cpp_composite.advance)

        for cpp_layer in cpp_composite.layers:
            lk = SlugKey("codepoint", cpp_layer.key.codepoint()) \
                 if cpp_layer.key.type == 0 \
                 else SlugKey("name", cpp_layer.key.name())

            layer = SlugLayer(
                key       = lk,
                color     = SlugColor(*cpp_layer.color.to_tuple()),
                transform = SlugMatrix(
                    cpp_layer.transform.xx, cpp_layer.transform.yx,
                    cpp_layer.transform.xy, cpp_layer.transform.yy,
                    cpp_layer.transform.dx, cpp_layer.transform.dy,
                ),
                effect_id = cpp_layer.effect_id,
            )

            composite.layers.append(layer)

        composites[k] = composite

    return shapes, composites


def _texture_from_atlas(atlas, which: str) -> SlugTextureData:
    """Extract a TextureData from either a SlugAtlasData or a real slughorn.Atlas."""
    td = atlas.curve_texture if which == "curve" else atlas.band_texture

    if isinstance(td, SlugTextureData):
        return td

    # Real slughorn.TextureData — copy bytes out of the memoryview
    return SlugTextureData(
        width  = td.width,
        height = td.height,
        format = td.format,
        bytes  = bytes(td.bytes),
    )


# =============================================================================
# JSON builder (shared by write_json and write_binary)
# =============================================================================

def _build_json(
    atlas,
    embed_base64:      bool,
    curve_byte_offset: int = 0,
    band_byte_offset:  int = 0,
) -> dict:
    curve_td = _texture_from_atlas(atlas, "curve")
    band_td  = _texture_from_atlas(atlas, "band")

    bv0 = {
        "byteLength": len(curve_td.bytes),
        "format":     curve_td.format,
        "width":      curve_td.width,
        "height":     curve_td.height,
    }

    bv1 = {
        "byteLength": len(band_td.bytes),
        "format":     band_td.format,
        "width":      band_td.width,
        "height":     band_td.height,
    }

    if embed_base64:
        bv0["byteOffset"] = 0
        bv0["data"]       = base64.b64encode(curve_td.bytes).decode("ascii")
        bv1["byteOffset"] = 0
        bv1["data"]       = base64.b64encode(band_td.bytes).decode("ascii")
    else:
        bv0["byteOffset"] = curve_byte_offset
        bv1["byteOffset"] = band_byte_offset

    shapes, composites = _atlas_to_shapes_composites(atlas)

    return {
        "asset": {
            "version":   CURRENT_FORMAT_VER,
            "generator": "slughorn_serial.py",
        },
        "tex_width":     512,
        "bufferViews":   [bv0, bv1],
        "curve_texture": 0,
        "band_texture":  1,
        "shapes":        [_shape_to_dict(k, v)     for k, v in shapes.items()],
        "composites":    [_composite_to_dict(k, v) for k, v in composites.items()],
    }


# =============================================================================
# JSON → SlugAtlasData reconstruction
# =============================================================================

def _atlas_from_json(
    j:         dict,
    bin_chunk: Optional[bytes] = None,  # None → use base64 "data" fields
) -> SlugAtlasData:
    buffer_views = j["bufferViews"]

    def load_texture(index: int) -> SlugTextureData:
        bv  = buffer_views[index]
        fmt = bv["format"]

        if bin_chunk is not None:
            offset = bv["byteOffset"]
            length = bv["byteLength"]
            data   = bin_chunk[offset : offset + length]
        else:
            data = base64.b64decode(bv["data"])

        return SlugTextureData(
            width  = bv["width"],
            height = bv["height"],
            format = fmt,
            bytes  = data,
        )

    curve_texture = load_texture(j["curve_texture"])
    band_texture  = load_texture(j["band_texture"])

    shapes     = {}
    composites = {}

    for jshape in j.get("shapes", []):
        k, v = _shape_from_dict(jshape)
        shapes[k] = v

    for jcomp in j.get("composites", []):
        k, v = _composite_from_dict(jcomp)
        composites[k] = v

    return SlugAtlasData(shapes, composites, curve_texture, band_texture)


# =============================================================================
# Binary container helpers
# =============================================================================

def _pad_to_4(data: bytes, pad_byte: int) -> bytes:
    rem = len(data) % 4
    if rem == 0:
        return data
    return data + bytes([pad_byte] * (4 - rem))


def _write_u32le(v: int) -> bytes:
    return struct.pack("<I", v)


def _read_u32le(data: bytes, offset: int) -> int:
    return struct.unpack_from("<I", data, offset)[0]


# =============================================================================
# Public API
# =============================================================================

def save_slug(atlas, path: str, pretty: bool = True) -> None:
    """
    Write an atlas to a .slug (JSON) or .slugb (binary) file.

    atlas  — slughorn.Atlas (C++ binding) or SlugAtlasData
    path   — destination path; extension determines format:
               .slug  → JSON + base64
               .slugb → binary container
    pretty — indent JSON output (only applies to .slug)
    """
    binary = path.endswith(".slugb")

    if binary:
        data = _build_binary(atlas)
        with open(path, "wb") as f:
            f.write(data)
    else:
        j = _build_json(atlas, embed_base64=True)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(j, f, indent=2 if pretty else None)

    print(f"Wrote {path}")


def load_slug(path: str) -> SlugAtlasData:
    """
    Load a .slug or .slugb file and return a SlugAtlasData instance.

    Format is auto-detected: files starting with '{' are JSON (.slug),
    files starting with 'S' are binary (.slugb).

    No compiled slughorn module required.
    """
    with open(path, "rb") as f:
        raw = f.read()

    return _parse_bytes(raw)


def parse_slug(data: bytes) -> SlugAtlasData:
    """Parse a .slug/.slugb from an in-memory bytes object."""
    return _parse_bytes(data)


# =============================================================================
# Internal write/parse implementations
# =============================================================================

def _build_binary(atlas) -> bytes:
    curve_td = _texture_from_atlas(atlas, "curve")
    band_td  = _texture_from_atlas(atlas, "band")

    # BIN chunk: curve bytes then band bytes
    curve_bytes       = bytes(curve_td.bytes)
    band_bytes        = bytes(band_td.bytes)
    curve_byte_offset = 0
    band_byte_offset  = len(curve_bytes)

    bin_data = _pad_to_4(curve_bytes + band_bytes, 0x00)

    # JSON chunk
    j        = _build_json(atlas, embed_base64=False,
                           curve_byte_offset=curve_byte_offset,
                           band_byte_offset=band_byte_offset)
    json_str = json.dumps(j, separators=(",", ":")).encode("utf-8")
    json_data = _pad_to_4(json_str, 0x20)

    # File header
    total_length = 12 + 8 + len(json_data) + 8 + len(bin_data)

    out  = SLUG_MAGIC
    out += _write_u32le(SLUG_VERSION)
    out += _write_u32le(total_length)

    # JSON chunk
    out += _write_u32le(len(json_data))
    out += _write_u32le(CHUNK_TYPE_JSON)
    out += json_data

    # BIN chunk
    out += _write_u32le(len(bin_data))
    out += _write_u32le(CHUNK_TYPE_BIN)
    out += bin_data

    return out


def _parse_bytes(raw: bytes) -> SlugAtlasData:
    if raw[0:1] == b"{":
        # .slug — JSON + base64
        j = json.loads(raw.decode("utf-8"))
        return _atlas_from_json(j, bin_chunk=None)

    if raw[0:4] == SLUG_MAGIC:
        # .slugb — binary container
        version      = _read_u32le(raw, 4)
        # total_length = _read_u32le(raw, 8)  # available for validation

        if version != SLUG_VERSION:
            raise ValueError(f"Unsupported .slugb version {version}")

        pos = 12

        # JSON chunk
        json_length = _read_u32le(raw, pos);     pos += 4
        json_type   = _read_u32le(raw, pos);     pos += 4

        if json_type != CHUNK_TYPE_JSON:
            raise ValueError("Expected JSON chunk first in .slugb")

        json_bytes = raw[pos : pos + json_length]
        pos += json_length
        pos += (4 - (json_length % 4)) % 4  # skip padding

        # BIN chunk
        bin_length = _read_u32le(raw, pos);      pos += 4
        bin_type   = _read_u32le(raw, pos);      pos += 4

        if bin_type != CHUNK_TYPE_BIN:
            raise ValueError("Expected BIN chunk second in .slugb")

        bin_chunk = raw[pos : pos + bin_length]

        j = json.loads(json_bytes.decode("utf-8"))
        return _atlas_from_json(j, bin_chunk=bin_chunk)

    raise ValueError(
        "Unrecognised format — expected '{' (.slug) or 'SLUG' magic (.slugb)"
    )


# =============================================================================
# Self-test
# =============================================================================

if __name__ == "__main__":
    import io

    print("slughorn_serial.py self-test")
    print("=" * 48)

    # -------------------------------------------------------------------------
    # Build a SlugAtlasData manually (no C++ needed)
    # -------------------------------------------------------------------------
    import numpy as np

    # Minimal 1-curve atlas: one texel pair of curve data, one band row
    curve_bytes = np.array(
        [0.0, 0.0, 0.5, 0.35,   # texel 0: x1,y1,x2,y2
         1.0, 0.0, 0.0, 0.0],   # texel 1: x3,y3,_,_
        dtype=np.float32
    ).tobytes()

    # Pad curve texture to 512 wide (one row)
    curve_bytes += bytes(512 * 4 * 4 - len(curve_bytes))

    band_bytes = bytes(512 * 4 * 2)  # one empty band row

    curve_td = SlugTextureData(width=512, height=1, format="RGBA32F",  bytes=curve_bytes)
    band_td  = SlugTextureData(width=512, height=1, format="RGBA16UI", bytes=band_bytes)

    shapes = {
        SlugKey("codepoint", ord("F")): SlugShape(
            band_scale_x=2.0, band_scale_y=2.0,
            width=1.0, height=0.7, advance=1.0,
        )
    }

    composites = {
        SlugKey("name", "test_composite"): SlugCompositeShape(
            advance=1.0,
            layers=[
                SlugLayer(
                    key       = SlugKey("codepoint", ord("F")),
                    color     = SlugColor(1.0, 0.5, 0.0, 1.0),
                    transform = SlugMatrix(dx=0.1, dy=0.2),
                    effect_id = 0,
                )
            ]
        )
    }

    original = SlugAtlasData(shapes, composites, curve_td, band_td)

    # -------------------------------------------------------------------------
    # Round-trip: JSON (.slug)
    # -------------------------------------------------------------------------
    buf = io.BytesIO()
    j   = _build_json(original, embed_base64=True)
    buf.write(json.dumps(j, indent=2).encode("utf-8"))

    buf.seek(0)
    loaded_json = _parse_bytes(buf.read())

    assert loaded_json.get_shape(ord("F")) is not None, "JSON: shape not found"
    assert loaded_json.get_composite("test_composite") is not None, "JSON: composite not found"
    assert loaded_json.curve_texture.width == 512
    assert loaded_json.curve_texture.bytes == curve_bytes

    print("  .slug  round-trip: PASS")

    # -------------------------------------------------------------------------
    # Round-trip: binary (.slugb)
    # -------------------------------------------------------------------------
    bin_data    = _build_binary(original)
    loaded_bin  = _parse_bytes(bin_data)

    assert loaded_bin.get_shape(ord("F")) is not None, "Binary: shape not found"
    assert loaded_bin.get_composite("test_composite") is not None, "Binary: composite not found"
    assert loaded_bin.curve_texture.bytes == curve_bytes
    assert loaded_bin.band_texture.bytes  == band_bytes

    print("  .slugb round-trip: PASS")

    # -------------------------------------------------------------------------
    # Cross-format identity: JSON bytes == binary bytes after texture decode
    # -------------------------------------------------------------------------
    assert loaded_json.curve_texture.bytes == loaded_bin.curve_texture.bytes
    assert loaded_json.band_texture.bytes  == loaded_bin.band_texture.bytes

    shape_j = loaded_json.get_shape(ord("F"))
    shape_b = loaded_bin.get_shape(ord("F"))

    assert shape_j.band_scale_x == shape_b.band_scale_x
    assert shape_j.width        == shape_b.width

    print("  Cross-format identity: PASS")
    print()
    print("All tests passed.")
