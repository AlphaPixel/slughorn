#pragma once

// =============================================================================
// slughorn-serial.hpp - Atlas serialization (.slug JSON / .slugb binary)
//
// Stb-style single-header companion to slughorn.hpp.
// Define SLUGHORN_SERIAL_IMPLEMENTATION in exactly one .cpp before including.
//
// Depends on:
//   slughorn.hpp
//   nlohmann/json.hpp (expected at $git/ext/json)
//
// Formats
// -------
//   .slug JSON + base64-encoded texture blobs.  Human-readable, single file.
//         Analogous to glTF's .gltf format.
//
//   .slugb Binary container: 12-byte file header, JSON chunk, BIN chunk.
//          Compact, single file, fast to load; analogous to glTF's .glb format.
//
// Container layout (.slugb)
// -------------------------
//   [magic:   4 bytes  "SLUG"     ]
//   [version: uint32_t 1          ]
//   [length:  uint32_t total bytes]
//
//   Chunk 0 - JSON
//     [chunk_length: uint32_t          ]  byte count of JSON data (before padding)
//     [chunk_type:   uint32_t 0x4E4F534A "JSON"]
//     [data:         chunk_length bytes, UTF-8 ]
//     [padding:      0-3 bytes 0x20 (space) to reach 4-byte alignment]
//
//   Chunk 1 - BIN
//     [chunk_length: uint32_t          ]  byte count of binary data (before padding)
//     [chunk_type:   uint32_t 0x004E4942 "BIN\0"]
//     [data:         chunk_length bytes ]
//     [padding:      0-3 bytes 0x00 to reach 4-byte alignment]
//
// JSON schema
// -----------
//   {
//     "asset": { "version": "1.0", "generator": "slughorn" },
//     "tex_width": 512,
//     "bufferViews": [
//       { "byteOffset": 0,    "byteLength": N, "format": "RGBA32F",  "width": 512, "height": H },
//       { "byteOffset": N,    "byteLength": M, "format": "RGBA16UI", "width": 512, "height": H }
//     ],
//     "curve_texture": 0,
//     "band_texture":  1,
//     "shapes": [
//       {
//         "key": { "type": "codepoint", "value": 70 },
//         "band_tex_x": 0, "band_tex_y": 0,
//         "band_max_x": 1, "band_max_y": 1,
//         "band_scale_x": 2.0, "band_scale_y": 2.0,
//         "band_offset_x": 0.0, "band_offset_y": 0.0,
//         "bearing_x": 0.0, "bearing_y": 0.7,
//         "width": 1.0, "height": 0.7, "advance": 1.0
//       }
//     ],
//     "composites": [
//       {
//         "key": { "type": "name", "value": "PIXEL" },
//         "advance": 5.2,
//         "layers": [
//           {
//             "key": { "type": "codepoint", "value": 80 },
//             "color": [1.0, 0.0, 0.0, 1.0],
//             "transform": [1.0, 0.0, 0.0, 1.0, 0.0, 0.0],
//             "effect_id": 0
//           }
//         ]
//       }
//     ]
//   }
//
//   In .slug: each bufferView gains "data": "<base64>" and byteOffset is 0.
//   In .slugb: byteOffset is a real byte offset into the BIN chunk; no "data".
//
// Usage
// -----
//   // Write
//   slughorn::serial::write(atlas, "logo.slug"); // JSON
//   slughorn::serial::write(atlas, "logo.slugb"); // binary
//
//   // Read (format auto-detected)
//   slughorn::Atlas atlas = slughorn::serial::read("logo.slug");
//   slughorn::Atlas atlas = slughorn::serial::read("logo.slugb");
// =============================================================================

#include "slughorn.hpp"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace slughorn::serial {

// =============================================================================
// Public API
// =============================================================================

// Write as JSON + base64 (.slug).
// pretty=true produces human-readable indented output (recommended default).
void writeJSON(const Atlas& atlas, std::ostream& out, bool pretty=true);

// Write as binary container (.slugb).
void writeBinary(const Atlas& atlas, std::ostream& out);

// Convenience: write to file path.
// Extension determines format: .slug -> JSON, .slugb -> binary.
// Throws std::runtime_error if the file cannot be opened.
void write(const Atlas& atlas, const std::string& path);

// Read either format - auto-detected ('{' -> JSON, 'S' -> binary).
// Returns a fully-built Atlas (is_built() == true).
// Throws std::runtime_error on parse errors or unknown format.
Atlas read(std::istream& in);

// Convenience: read from file path.
// Throws std::runtime_error if the file cannot be opened.
Atlas read(const std::string& path);

// =============================================================================
// Implementation
// =============================================================================

#ifdef SLUGHORN_SERIAL_IMPLEMENTATION

namespace {

using json = nlohmann::json;

// -----------------------------------------------------------------------------
// Base64
// -----------------------------------------------------------------------------

static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const std::vector<uint8_t>& data) {
	std::string out;
	out.reserve(((data.size() + 2) / 3) * 4);

	size_t i = 0;
	const size_t n = data.size();

	while(i < n) {
		const uint32_t a = data[i++];
		const uint32_t b = (i < n) ? data[i++] : 0u;
		const uint32_t c = (i < n) ? data[i++] : 0u;
		const uint32_t triple = (a << 16) | (b << 8) | c;

		out += B64[(triple >> 18) & 0x3F];
		out += B64[(triple >> 12) & 0x3F];
		out += B64[(triple >> 6) & 0x3F];
		out += B64[(triple >> 0) & 0x3F];
	}

	// Pad
	const size_t mod = data.size() % 3;

	if(mod >= 1) out[out.size() - 1] = '=';
	if(mod == 1) out[out.size() - 2] = '=';

	return out;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
	// Build reverse lookup table
	static uint8_t lut[256] = {};
	static bool lutReady = false;

	if(!lutReady) {
		for(int i = 0; i < 64; i++) lut[(uint8_t)B64[i]] = (uint8_t)i;

		lutReady = true;
	}

	const size_t len = s.size();

	if(len % 4 != 0) throw std::runtime_error("slughorn-serial: base64 length not a multiple of 4");

	size_t padding = 0;

	if(len >= 1 && s[len - 1] == '=') padding++;
	if(len >= 2 && s[len - 2] == '=') padding++;

	std::vector<uint8_t> out;

	out.reserve((len / 4) * 3 - padding);

	for(size_t i = 0; i < len; i += 4) {
		const uint32_t triple =
			// TODO: Good grief this is ... ugly.
			((uint32_t)lut[(uint8_t)s[i]] << 18) |
			((uint32_t)lut[(uint8_t)s[i + 1]] << 12) |
			((uint32_t)lut[(uint8_t)s[i + 2]] << 6) |
			((uint32_t)lut[(uint8_t)s[i + 3]])
		;

		out.push_back((triple >> 16) & 0xFF);

		if(s[i + 2] != '=') out.push_back((triple >> 8) & 0xFF);
		if(s[i + 3] != '=') out.push_back((triple) & 0xFF);
	}

	return out;
}

// -----------------------------------------------------------------------------
// Key serialization helpers
// -----------------------------------------------------------------------------

json keyToJson(const Key& k) {
	if(k.type() == Key::Type::Codepoint) return {
		{"type", "codepoint"}, {"value", k.codepoint()}
	};

	return { {"type", "name"}, {"value", k.name()} };
}

Key keyFromJson(const json& j) {
	const std::string type = j.at("type");

	if(type == "codepoint") return Key::fromCodepoint(j.at("value").get<uint32_t>());
	if(type == "name") return Key::fromString(j.at("value").get<std::string>());

	throw std::runtime_error("slughorn-serial: unknown key type '" + type + "'");
}

// -----------------------------------------------------------------------------
// Shared JSON builder (used by both writeJSON and writeBinary)
//
// embedBase64=true -> .slug: bufferViews contain "data" field, byteOffset=0
// embedBase64=false -> .slugb: bufferViews contain byteOffset into BIN chunk
// -----------------------------------------------------------------------------

json buildJson(
	const Atlas& atlas,
	bool embedBase64,
	uint32_t curveByteOffset=0,
	uint32_t bandByteOffset=0
) {
	const Atlas::TextureData& curve = atlas.getCurveTextureData();
	const Atlas::TextureData& band = atlas.getBandTextureData();

	json j;

	// Asset block
	j["asset"] = {
		{"version", "1.0"},
		{"generator", "slughorn"}
	};

	j["tex_width"] = 512;

	// Buffer views
	json bv0 = {
		{"byteLength", curve.bytes.size()},
		{"format", "RGBA32F"},
		{"width", curve.width},
		{"height", curve.height}
	};

	json bv1 = {
		{"byteLength", band.bytes.size()},
		{"format", "RGBA16UI"},
		{"width", band.width},
		{"height", band.height}
	};

	if(embedBase64) {
		bv0["byteOffset"] = 0;
		bv0["data"] = base64Encode(curve.bytes);
		bv1["byteOffset"] = 0;
		bv1["data"] = base64Encode(band.bytes);
	}

	else {
		bv0["byteOffset"] = curveByteOffset;
		bv1["byteOffset"] = bandByteOffset;
	}

	j["bufferViews"] = json::array({ bv0, bv1 });
	j["curve_texture"] = 0;
	j["band_texture"] = 1;

	// Shapes
	json shapes = json::array();

	for(const auto& [key, shape] : atlas.getShapes()) {
		shapes.push_back({
			{"key", keyToJson(key)},
			{"band_tex_x", shape.bandTexX},
			{"band_tex_y", shape.bandTexY},
			{"band_max_x", shape.bandMaxX},
			{"band_max_y", shape.bandMaxY},
			{"band_scale_x", shape.bandScaleX},
			{"band_scale_y", shape.bandScaleY},
			{"band_offset_x",shape.bandOffsetX},
			{"band_offset_y",shape.bandOffsetY},
			{"bearing_x", shape.bearingX},
			{"bearing_y", shape.bearingY},
			{"width", shape.width},
			{"height", shape.height},
			{"advance", shape.advance}
		});
	}

	j["shapes"] = shapes;

	// Composites
	json composites = json::array();

	for(const auto& [key, composite] : atlas.getCompositeShapes()) {
		json layers = json::array();

		for(const auto& layer : composite.layers) {
			layers.push_back({
				{"key", keyToJson(layer.key)},
				{"color", {layer.color.r, layer.color.g, layer.color.b, layer.color.a}},
				{"transform", {
					layer.transform.xx, layer.transform.yx,
					layer.transform.xy, layer.transform.yy,
					layer.transform.dx, layer.transform.dy
				}},
				{"effect_id", layer.effectId}
			});
		}

		composites.push_back({
			{"key", keyToJson(key)},
			{"advance", composite.advance},
			{"layers", layers}
		});
	}

	j["composites"] = composites;

	return j;
}

// -----------------------------------------------------------------------------
// JSON - Atlas reconstruction (shared by read paths)
// -----------------------------------------------------------------------------

Atlas atlasFromJson(
	const json& j,
	const std::vector<uint8_t>* binChunk=nullptr // nullptr -> use base64 "data" fields
) {
	Atlas atlas;

	// Textures
	const json& bufferViews = j.at("bufferViews");

	auto loadTexture = [&](size_t index) -> Atlas::TextureData {
		const json& bv = bufferViews.at(index);

		Atlas::TextureData td;

		td.width = bv.at("width");
		td.height = bv.at("height");

		const std::string fmt = bv.at("format");

		if (fmt == "RGBA32F") td.format = Atlas::TextureData::Format::RGBA32F;
		else if(fmt == "RGBA16UI") td.format = Atlas::TextureData::Format::RGBA16UI;
		else throw std::runtime_error("slughorn-serial: unknown texture format '" + fmt + "'");

		if(binChunk) {
			const uint32_t offset = bv.at("byteOffset");
			const uint32_t length = bv.at("byteLength");

			if(offset + length > binChunk->size()) throw
				std::runtime_error("slughorn-serial: bufferView out of range")
			;

			td.bytes.assign(
				binChunk->begin() + offset,
				binChunk->begin() + offset + length
			);
		}

		else td.bytes = base64Decode(bv.at("data").get<std::string>());

		return td;
	};

	// We need to inject pre-built texture data and the shape map into the
	// Atlas without re-running build(). We do this by calling the internal
	// reconstruction path exposed via the serial friend.
	Atlas::SerialData sd;

	sd.curveData = loadTexture(j.at("curve_texture"));
	sd.bandData = loadTexture(j.at("band_texture"));

	// Shapes
	for(const json& js : j.at("shapes")) {
		Key key = keyFromJson(js.at("key"));

		Atlas::Shape shape;

		shape.bandTexX = js.at("band_tex_x");
		shape.bandTexY = js.at("band_tex_y");
		shape.bandMaxX = js.at("band_max_x");
		shape.bandMaxY = js.at("band_max_y");
		shape.bandScaleX = js.at("band_scale_x");
		shape.bandScaleY = js.at("band_scale_y");
		shape.bandOffsetX= js.at("band_offset_x");
		shape.bandOffsetY= js.at("band_offset_y");
		shape.bearingX = js.at("bearing_x");
		shape.bearingY = js.at("bearing_y");
		shape.width = js.at("width");
		shape.height = js.at("height");
		shape.advance = js.at("advance");

		sd.shapes[key] = shape;
	}

	// Composites
	for(const json& jc : j.at("composites")) {
		Key key = keyFromJson(jc.at("key"));

		CompositeShape composite;
		composite.advance = jc.at("advance");

		for(const json& jl : jc.at("layers")) {
			Layer layer;
			layer.key = keyFromJson(jl.at("key"));
			layer.effectId = jl.at("effect_id");

			const auto& jcolor = jl.at("color");
			layer.color = { jcolor[0], jcolor[1], jcolor[2], jcolor[3] };

			const auto& jxform = jl.at("transform");
			layer.transform = {
				jxform[0], jxform[1], // xx, yx
				jxform[2], jxform[3], // xy, yy
				jxform[4], jxform[5] // dx, dy
			};

			composite.layers.push_back(layer);
		}

		sd.composites[key] = std::move(composite);
	}

	atlas.loadFromSerial(std::move(sd));

	return atlas;
}

// -----------------------------------------------------------------------------
// Binary container helpers
// -----------------------------------------------------------------------------

static constexpr uint32_t SLUG_MAGIC = 0x47554C53; // "SLUG"
static constexpr uint32_t SLUG_VERISON = 1;
static constexpr uint32_t SLUG_CHUNK_TYPE_JSON = 0x4E4F534A; // "JSON"
static constexpr uint32_t SLUG_CHUNK_TYPE_BIN = 0x004E4942; // "BIN\0"

void writeU32LE(std::ostream& out, uint32_t v) {
	const uint8_t bytes[4] = {
		uint8_t(v),
		uint8_t(v >> 8),
		uint8_t(v >> 16),
		uint8_t(v >> 24)
	};

	out.write(reinterpret_cast<const char*>(bytes), 4);
}

uint32_t readU32LE(std::istream& in) {
	uint8_t bytes[4];

	in.read(reinterpret_cast<char*>(bytes), 4);

	return
		uint32_t(bytes[0]) |
		uint32_t(bytes[1]) << 8 |
		uint32_t(bytes[2]) << 16 |
		uint32_t(bytes[3]) << 24
	;
}

// Pad a buffer to a multiple of 4 bytes.
// JSON chunks pad with spaces (0x20); BIN chunks pad with zeros.
void padTo4(std::vector<uint8_t>& buf, uint8_t padByte) {
	while(buf.size() % 4 != 0) buf.push_back(padByte);
}

} // anonymous namespace

// =============================================================================
// writeJSON
// =============================================================================

void writeJSON(const Atlas& atlas, std::ostream& out, bool pretty) {
	if(!atlas.isBuilt()) throw
		std::runtime_error("slughorn-serial: Atlas must be built before writing")
	;

	const json j = buildJson(atlas, /*embedBase64=*/true);

	out << (pretty ? j.dump(2) : j.dump());
}

// =============================================================================
// writeBinary
// =============================================================================

void writeBinary(const Atlas& atlas, std::ostream& out) {
	if(!atlas.isBuilt()) throw
		std::runtime_error("slughorn-serial: Atlas must be built before writing")
	;

	const Atlas::TextureData& curve = atlas.getCurveTextureData();
	const Atlas::TextureData& band = atlas.getBandTextureData();

	// Build BIN chunk: curve bytes then band bytes, each padded to 4 bytes
	std::vector<uint8_t> binData;

	binData.insert(binData.end(), curve.bytes.begin(), curve.bytes.end());

	const uint32_t curveByteOffset = 0;
	const uint32_t bandByteOffset = static_cast<uint32_t>(curve.bytes.size());

	// Note: curve texture size is always a multiple of 4 (4 floats * 4 bytes each)
	// and band texture is always a multiple of 4 (4 uint16s * 2 bytes each, * even row).
	// No padding needed between them in practice, but we pad the final buffer to be safe.
	binData.insert(binData.end(), band.bytes.begin(), band.bytes.end());

	padTo4(binData, 0x00);

	// Build JSON chunk
	const json j = buildJson(atlas, /*embedBase64=*/false, curveByteOffset, bandByteOffset);
	const std::string jsonStr = j.dump();

	std::vector<uint8_t> jsonData(jsonStr.begin(), jsonStr.end());

	padTo4(jsonData, 0x20); // pad with spaces

	// File header: magic + version + total_length
	// 12 bytes file header
	// + 8 bytes JSON chunk header + jsonData.size()
	// + 8 bytes BIN chunk header + binData.size()
	const uint32_t totalLength =
		12 +
		8 + static_cast<uint32_t>(jsonData.size()) +
		8 + static_cast<uint32_t>(binData.size())
	;

	// Write file header
	writeU32LE(out, SLUG_MAGIC);
	writeU32LE(out, SLUG_VERISON);
	writeU32LE(out, totalLength);

	// Write JSON chunk
	writeU32LE(out, static_cast<uint32_t>(jsonData.size()));
	writeU32LE(out, SLUG_CHUNK_TYPE_JSON);

	out.write(reinterpret_cast<const char*>(jsonData.data()), static_cast<std::streamsize>(jsonData.size()));

	// Write BIN chunk
	writeU32LE(out, static_cast<uint32_t>(binData.size()));
	writeU32LE(out, SLUG_CHUNK_TYPE_BIN);

	out.write(reinterpret_cast<const char*>(binData.data()), static_cast<std::streamsize>(binData.size()));
}

// =============================================================================
// write (path, format inferred from extension)
// =============================================================================

void write(const Atlas& atlas, const std::string& path) {
	const bool binary =
		path.size() >= 6 &&
		path.substr(path.size() - 6) == ".slugb"
	;

	std::ofstream f(path, binary ? (std::ios::out | std::ios::binary) : std::ios::out);

	if(!f) throw std::runtime_error("slughorn-serial: cannot open '" + path + "' for writing");

	if(binary) writeBinary(atlas, f);

	else writeJSON(atlas, f);
}

// =============================================================================
// read (stream)
// =============================================================================

Atlas read(std::istream& in) {
	// Peek at first byte to detect format
	const int first = in.peek();

	if(first == '{') {
		// .slug — JSON
		json j = json::parse(in);

		return atlasFromJson(j, nullptr);
	}

	if(first == 'S') {
		// .slugb - binary container
		const uint32_t magic = readU32LE(in);
		const uint32_t version = readU32LE(in);
		// TODO: This is related to the TODO below; why aren't we validating?
		[[maybe_unused]] const uint32_t length = readU32LE(in);

		if(magic != SLUG_MAGIC) throw
			std::runtime_error("slughorn-serial: not a .slugb file (bad magic)")
		;

		if(version != SLUG_VERISON) throw
			std::runtime_error("slughorn-serial: unsupported .slugb version " +
			std::to_string(version))
		;

		// Read JSON chunk
		const uint32_t jsonLength = readU32LE(in);
		const uint32_t jsonType = readU32LE(in);

		if(jsonType != SLUG_CHUNK_TYPE_JSON) throw
			std::runtime_error("slughorn-serial: expected JSON chunk first")
		;

		std::string jsonStr(jsonLength, '\0');

		in.read(jsonStr.data(), jsonLength);

		// Skip JSON padding (stream position must be 4-byte aligned)
		const uint32_t jsonPad = (4 - (jsonLength % 4)) % 4;

		in.seekg(jsonPad, std::ios::cur);

		// Read BIN chunk
		const uint32_t binLength = readU32LE(in);
		const uint32_t binType = readU32LE(in);

		if(binType != SLUG_CHUNK_TYPE_BIN) throw
			std::runtime_error("slughorn-serial: expected BIN chunk second")
		;

		std::vector<uint8_t> binData(binLength);

		in.read(reinterpret_cast<char*>(binData.data()), binLength);

		// TODO: Why?
		// (void)length; // total file length - used for validation if desired

		json j = json::parse(jsonStr);

		return atlasFromJson(j, &binData);
	}

	throw std::runtime_error(
		"slughorn-serial: unrecognised format (expected '{' for .slug or 'S' for .slugb)"
	);
}

// =============================================================================
// read (path)
// =============================================================================

Atlas read(const std::string& path) {
	const bool binary =
		path.size() >= 6 &&
		path.substr(path.size() - 6) == ".slugb"
	;

	std::ifstream f(path, binary ? (std::ios::in | std::ios::binary) : std::ios::in);

	if(!f) throw std::runtime_error("slughorn-serial: cannot open '" + path + "' for reading");

	return read(f);
}

#endif

}
