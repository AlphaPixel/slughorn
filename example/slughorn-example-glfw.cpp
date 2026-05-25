// XXX: This example was created entirely by AI! osgSlug is the REAL "testbed" (for now)...
//
// Minimal single-file GLFW + GLAD demo for the slughorn Atlas pipeline.
//
// Builds an Atlas via slughorn::canvas::Canvas (same API as osgslug-shape-canvas),
// uploads the two Slug textures, assembles a VAO from Layer data, and renders
// with a stripped-down Slug fragment shader.
//
// Interaction:
// - Left drag: orbit camera
// - Right drag: pan
// - Mouse wheel: zoom
// - P: toggle perspective / orthographic projection
// - R: reset view
// - Esc: quit
//
// No OSG, no GLM, no external math. View/projection math is computed inline.
//
// Build requirements:
//
// - slughorn (slughorn.hpp / slughorn.cpp)
// - slughorn-canvas.hpp (SLUGHORN_CANVAS=ON or include path)
// - GLFW 3.x
// - GLAD (GL 3.3 core, generated for your platform)
//
// See CMakeLists.txt (slughorn-test-glfw) for a minimal build target.
//
// Shader uniforms (intentionally renamed away from osgSlug names):
//
// u_mvp          mat4       view-projection transform
// u_time         float      seconds since start
// u_curveTexture sampler2D  (unit 0) RGBA32F curve data
// u_bandTexture  usampler2D (unit 1) RGBA16UI band data

#include "slughorn/canvas.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>
#include <string>

using namespace slughorn::literals;
using slughorn::slug_t;

// ============================================================================
// Inline shaders
// ============================================================================

static const char* k_VertSrc = R"(
#version 330 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec4 a_emCoord;
layout(location = 3) in vec4 a_bandXform; // bandScaleX/Y, bandOffsetX/Y
layout(location = 4) in vec4 a_shapeData; // bandTexX/Y, bandMaxX/Y
layout(location = 5) in float a_effectId; // unused in this demo, carried through

uniform mat4 u_mvp;
uniform float u_time;

out vec2 v_emCoord;
out vec2 v_uv;
out vec4 v_color;

flat out vec4 v_bandXform;
flat out vec4 v_shapeData;

void main() {
	v_emCoord = a_emCoord.xy;
	v_uv = a_emCoord.zw;
	v_color = a_color;
	v_bandXform = a_bandXform;
	v_shapeData = a_shapeData;

	gl_Position = u_mvp * vec4(a_position, 1.0);
}
)";

// Fragment shader: Slug core only. No debug modes, no effect branches — just
// the coverage solve and a single standard fill. Add effects / debug modes
// from osgSlug's fragment shader when needed.
static const char* k_FragSrc = R"(
#version 330 core

in vec2 v_emCoord;
in vec2 v_uv;
in vec4 v_color;

flat in vec4 v_bandXform; // bandScaleX/Y, bandOffsetX/Y
flat in vec4 v_shapeData; // bandTexX/Y, bandMaxX/Y

uniform sampler2D u_curveTexture;
uniform usampler2D u_bandTexture;

out vec4 fragColor;

// Hardcoded to log2(512) = 9, matching the default Atlas() constructor.
// If using Atlas(uint32_t texWidth), replace with: int(log2(atlas.getTextureWidth()))
// and pass as a uniform rather than a define.
#define TEX_WIDTH 9

// Must match slughorn::Atlas::INDIRECTION_SIZE
#define SLUG_INDIRECTION_SIZE 32

// ---------------------------------------------------------------------------
// Slug core (Lengyel 2017); identical to osgSlug's implementation.
// ---------------------------------------------------------------------------

uint slug_CalcRootCode(float y1, float y2, float y3) {
	uint i1 = floatBitsToUint(y1) >> 31u;
	uint i2 = floatBitsToUint(y2) >> 30u;
	uint i3 = floatBitsToUint(y3) >> 29u;

	uint shift = (i2 & 2u) | (i1 & ~2u);
	shift = (i3 & 4u) | (shift & ~4u);

	return ((0x2E74u >> shift) & 0x0101u);
}

vec2 slug_SolveHorizPoly(vec4 p12, vec2 p3) {
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.y;
	float rb = 0.5 / b.y;
	float d = sqrt(max(b.y * b.y - a.y * p12.y, 0.0));
	float t1 = (b.y - d) * ra;
	float t2 = (b.y + d) * ra;
	if(abs(a.y) < 1.0 / 65536.0) { t1 = p12.y * rb; t2 = t1; }
	return vec2(
		(a.x * t1 - b.x * 2.0) * t1 + p12.x,
		(a.x * t2 - b.x * 2.0) * t2 + p12.x
	);
}

vec2 slug_SolveVertPoly(vec4 p12, vec2 p3) {
	vec2 a = p12.xy - p12.zw * 2.0 + p3;
	vec2 b = p12.xy - p12.zw;
	float ra = 1.0 / a.x;
	float rb = 0.5 / b.x;
	float d = sqrt(max(b.x * b.x - a.x * p12.x, 0.0));
	float t1 = (b.x - d) * ra;
	float t2 = (b.x + d) * ra;
	if(abs(a.x) < 1.0 / 65536.0) { t1 = p12.x * rb; t2 = t1; }
	return vec2(
		(a.y * t1 - b.y * 2.0) * t1 + p12.y,
		(a.y * t2 - b.y * 2.0) * t2 + p12.y
	);
}

ivec2 slug_CalcBandLoc(ivec2 glyphLoc, uint offset) {
	ivec2 bandLoc = ivec2(glyphLoc.x + int(offset), glyphLoc.y);
	bandLoc.y += bandLoc.x >> TEX_WIDTH;
	bandLoc.x &= (1 << TEX_WIDTH) - 1;
	return bandLoc;
}

float slug_CalcCoverage(float xcov, float ycov, float xwgt, float ywgt) {
	float coverage = max(
		abs(xcov * xwgt + ycov * ywgt) / max(xwgt + ywgt, 1.0 / 65536.0),
		min(abs(xcov), abs(ycov))
	);
	return clamp(coverage, 0.0, 1.0);
}

float slug_Render(vec2 renderCoord, vec4 bandTransform, ivec2 glyphLoc, ivec2 bandMax) {
	vec2 emsPerPixel = fwidth(renderCoord);
	vec2 pixelsPerEm = 1.0 / emsPerPixel;

	// O(1) band index via indirection tables (2 fetches per axis).
	int qY = clamp(int(renderCoord.y * bandTransform.y + bandTransform.w), 0, SLUG_INDIRECTION_SIZE - 1);
	int qX = clamp(int(renderCoord.x * bandTransform.x + bandTransform.z), 0, SLUG_INDIRECTION_SIZE - 1);
	int bandY = int(texelFetch(u_bandTexture, ivec2(glyphLoc.x + qY, glyphLoc.y), 0).r);
	int bandX = int(texelFetch(u_bandTexture, ivec2(glyphLoc.x + SLUG_INDIRECTION_SIZE + qX, glyphLoc.y), 0).r);

	// Horizontal bands — headers at glyphLoc + 2*IS + bandY
	float xcov = 0.0, xwgt = 0.0;
	uvec2 hbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + 2 * SLUG_INDIRECTION_SIZE + bandY, glyphLoc.y), 0).xy;
	ivec2 hbandLoc = slug_CalcBandLoc(glyphLoc, hbandData.y);

	for(int ci = 0; ci < int(hbandData.x); ci++) {
		ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, ivec2(hbandLoc.x + ci, hbandLoc.y), 0).xy);
		vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
		vec2 p3 = texelFetch(u_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

		if(max(max(p12.x, p12.z), p3.x) * pixelsPerEm.x < -0.5) break;

		uint code = slug_CalcRootCode(p12.y, p12.w, p3.y);
		if(code != 0u) {
			vec2 r = slug_SolveHorizPoly(p12, p3) * pixelsPerEm.x;
			if((code & 1u) != 0u) { xcov += clamp(r.x + 0.5, 0.0, 1.0); xwgt = max(xwgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0)); }
			if(code > 1u) { xcov -= clamp(r.y + 0.5, 0.0, 1.0); xwgt = max(xwgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
		}
	}

	// Vertical bands — headers at glyphLoc + 2*IS + numHBands + bandX
	float ycov = 0.0, ywgt = 0.0;
	uvec2 vbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + 2 * SLUG_INDIRECTION_SIZE + bandMax.y + 1 + bandX, glyphLoc.y), 0).xy;
	ivec2 vbandLoc = slug_CalcBandLoc(glyphLoc, vbandData.y);

	for(int ci = 0; ci < int(vbandData.x); ci++) {
		ivec2 curveLoc = ivec2(texelFetch(u_bandTexture, ivec2(vbandLoc.x + ci, vbandLoc.y), 0).xy);
		vec4 p12 = texelFetch(u_curveTexture, curveLoc, 0) - vec4(renderCoord, renderCoord);
		vec2 p3 = texelFetch(u_curveTexture, ivec2(curveLoc.x + 1, curveLoc.y), 0).xy - renderCoord;

		if(max(max(p12.y, p12.w), p3.y) * pixelsPerEm.y < -0.5) break;

		uint code = slug_CalcRootCode(p12.x, p12.z, p3.x);
		if(code != 0u) {
			vec2 r = slug_SolveVertPoly(p12, p3) * pixelsPerEm.y;
			if((code & 1u) != 0u) { ycov -= clamp(r.x + 0.5, 0.0, 1.0); ywgt = max(ywgt, clamp(1.0 - abs(r.x) * 2.0, 0.0, 1.0)); }
			if(code > 1u) { ycov += clamp(r.y + 0.5, 0.0, 1.0); ywgt = max(ywgt, clamp(1.0 - abs(r.y) * 2.0, 0.0, 1.0)); }
		}
	}

	return slug_CalcCoverage(xcov, ycov, xwgt, ywgt);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
void main() {
	ivec2 glyphLoc = ivec2(v_shapeData.xy);
	ivec2 bandMax = ivec2(v_shapeData.zw);

	float fill = slug_Render(v_emCoord, v_bandXform, glyphLoc, bandMax);

	if(fill < 0.001) discard;

	fragColor = vec4(v_color.rgb, fill * v_color.a);
}
)";

// ============================================================================
// GL helpers
// ============================================================================

static GLuint compileShader(GLenum type, const char* src) {
	GLuint s = glCreateShader(type);
	glShaderSource(s, 1, &src, nullptr);
	glCompileShader(s);

	GLint ok = 0;
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);

	if(!ok) {
		char log[2048];

		glGetShaderInfoLog(s, sizeof(log), nullptr, log);

		std::fprintf(stderr, "Shader compile error:\n%s\n", log);
		std::exit(1);
	}

	return s;
}

static GLuint linkProgram(GLuint vert, GLuint frag) {
	GLuint p = glCreateProgram();
	glAttachShader(p, vert);
	glAttachShader(p, frag);
	glLinkProgram(p);

	GLint ok = 0;
	glGetProgramiv(p, GL_LINK_STATUS, &ok);

	if(!ok) {
		char log[2048];
		glGetProgramInfoLog(p, sizeof(log), nullptr, log);
		std::fprintf(stderr, "Program link error:\n%s\n", log);
		std::exit(1);
	}

	return p;
}

// Upload a slughorn::Atlas::TextureData buffer to a new GL texture.
// Returns the GL texture object name.
static GLuint uploadSlugTexture(const slughorn::Atlas::TextureData& td) {
	GLuint tex = 0;
	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);

	// Slug textures are fetched with texelFetch — no filtering needed.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	if(td.format == slughorn::Atlas::TextureData::Format::RGBA32F) {
		glTexImage2D(
			GL_TEXTURE_2D, 0,
			GL_RGBA32F,
			(GLsizei)td.width, (GLsizei)td.height, 0,
			GL_RGBA, GL_FLOAT,
			td.bytes.data()
		);
	} else {
		// RGBA16UI — band texture
		glTexImage2D(
			GL_TEXTURE_2D, 0,
			GL_RGBA16UI,
			(GLsizei)td.width, (GLsizei)td.height, 0,
			GL_RGBA_INTEGER, GL_UNSIGNED_SHORT,
			td.bytes.data()
		);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

// ============================================================================
// Tiny orthographic matrix (column-major, matching GLSL mat4 layout)
//
// Maps [left,right] x [bottom,top] x [near,far] -> NDC.
// No GLM required.
// ============================================================================
static void orthoMatrix(
	float left, float right,
	float bottom, float top,
	float near_, float far_,
	float out[16]
) {
	std::memset(out, 0, 64);

	out[0] = 2.0f / (right - left);
	out[5] = 2.0f / (top - bottom);
	out[10] = -2.0f / (far_ - near_);
	out[12] = -(right + left) / (right - left);
	out[13] = -(top + bottom) / (top - bottom);
	out[14] = -(far_ + near_) / (far_ - near_);
	out[15] = 1.0f;
}

struct Vec3 {
	float x, y, z;
};

static Vec3 operator+(const Vec3& a, const Vec3& b) {
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static Vec3 operator-(const Vec3& a, const Vec3& b) {
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 operator*(const Vec3& v, float s) {
	return {v.x * s, v.y * s, v.z * s};
}

static float dot(const Vec3& a, const Vec3& b) {
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static Vec3 cross(const Vec3& a, const Vec3& b) {
	return {
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	};
}

static Vec3 normalize(const Vec3& v) {
	const float len = std::sqrt(dot(v, v));

	if(len <= 1e-6f) return {0.0f, 0.0f, 0.0f};

	return {v.x / len, v.y / len, v.z / len};
}

static void perspectiveMatrix(float fovyRadians, float aspect, float near_, float far_, float out[16]) {
	std::memset(out, 0, 64);

	const float f = 1.0f / std::tan(fovyRadians * 0.5f);

	out[0] = f / aspect;
	out[5] = f;
	out[10] = (far_ + near_) / (near_ - far_);
	out[11] = -1.0f;
	out[14] = (2.0f * far_ * near_) / (near_ - far_);
}

static void lookAtMatrix(const Vec3& eye, const Vec3& center, const Vec3& up, float out[16]) {
	const Vec3 f = normalize(center - eye);
	const Vec3 s = normalize(cross(f, up));
	const Vec3 u = cross(s, f);

	std::memset(out, 0, 64);

	out[0] = s.x;
	out[1] = u.x;
	out[2] = -f.x;

	out[4] = s.y;
	out[5] = u.y;
	out[6] = -f.y;

	out[8] = s.z;
	out[9] = u.z;
	out[10] = -f.z;

	out[12] = -dot(s, eye);
	out[13] = -dot(u, eye);
	out[14] = dot(f, eye);
	out[15] = 1.0f;
}

static void multiplyMatrix4(const float a[16], const float b[16], float out[16]) {
	float tmp[16];

	for(int c = 0; c < 4; c++) {
		for(int r = 0; r < 4; r++) {
			tmp[c * 4 + r] =
				a[0 * 4 + r] * b[c * 4 + 0] +
				a[1 * 4 + r] * b[c * 4 + 1] +
				a[2 * 4 + r] * b[c * 4 + 2] +
				a[3 * 4 + r] * b[c * 4 + 3];
		}
	}

	std::memcpy(out, tmp, sizeof(tmp));
}

struct ViewState {
	Vec3 target = {0.5f, 0.5f, 0.0f};
	float yaw = 0.55f;
	float pitch = 0.45f;
	float distance = 2.4f;
	bool usePerspective = true;

	bool leftDragging = false;
	bool rightDragging = false;
	double lastCursorX = 0.0;
	double lastCursorY = 0.0;

	void reset() {
		target = {0.5f, 0.5f, 0.0f};
		yaw = 0.55f;
		pitch = 0.45f;
		distance = 2.4f;
		usePerspective = true;
		leftDragging = false;
		rightDragging = false;
		lastCursorX = 0.0;
		lastCursorY = 0.0;
	}
};

static Vec3 orbitCameraPosition(const ViewState& view) {
	const float cp = std::cos(view.pitch);
	const float sp = std::sin(view.pitch);
	const float sy = std::sin(view.yaw);
	const float cy = std::cos(view.yaw);

	return {
		view.target.x + view.distance * cp * sy,
		view.target.y + view.distance * sp,
		view.target.z + view.distance * cp * cy
	};
}

// ============================================================================
// Vertex layout (mirrors ShapeDrawable::compile() exactly)
//
// location 0: position vec3
// location 1: color vec4
// location 2: emCoord vec4 (.xy = em-space, .zw = true [0,1] UV)
// location 3: bandXform vec4 (bandScaleX/Y, bandOffsetX/Y)
// location 4: shapeData vec4 (bandTexX/Y, bandMaxX/Y)
// location 5: effectId float (unused here, carried for forward-compat)
// ============================================================================
struct Vertex {
	float position[3];
	float color[4];
	float emCoord[4];
	float bandXform[4];
	float shapeData[4];
	float effectId;
};

// Build the interleaved vertex + index arrays from a set of slughorn Layers.
// This is a direct port of ShapeDrawable::compile() with OSG types replaced
// by plain std::vectors.
static void buildMesh(
	const slughorn::Atlas& atlas,
	const std::vector<slughorn::Layer>& layers,
	std::vector<Vertex>& outVerts,
	std::vector<unsigned short>& outIndices
) {
	static constexpr float SLUG_EXPAND = 0.01f;

	unsigned short base = 0;

	for(const auto& layer : layers) {
		const slughorn::Atlas::Shape* shape = atlas.getShape(layer.key);

		if(!shape) continue;

		const slughorn::Quad q = shape->computeQuad(layer.transform, layer.scale, cv(SLUG_EXPAND));

		const float x0 = (float)q.x0, y0 = (float)q.y0;
		const float x1 = (float)q.x1, y1 = (float)q.y1;

		// em-space corners (same expand as quad, same pattern as ShapeDrawable)
		const float emX0 = (float)shape->bearingX - SLUG_EXPAND;
		const float emY0 = (float)(shape->bearingY - shape->height) - SLUG_EXPAND;
		const float emX1 = (float)(shape->bearingX + shape->width) + SLUG_EXPAND;
		const float emY1 = (float)shape->bearingY + SLUG_EXPAND;

		const float bx[4] = {
			(float)shape->bandScaleX, (float)shape->bandScaleY,
			(float)shape->bandOffsetX, (float)shape->bandOffsetY
		};

		const float sd[4] = {
			(float)shape->bandTexX, (float)shape->bandTexY,
			(float)shape->bandMaxX, (float)shape->bandMaxY
		};
		const float col[4] = {
			(float)layer.color.r, (float)layer.color.g,
			(float)layer.color.b, (float)layer.color.a
		};
		const float eid = (float)layer.effectId;

		// Four corners: BL, BR, TR, TL (same winding as ShapeDrawable)
		const float positions[4][3] = {
			{x0, y0, 0.0f},
			{x1, y0, 0.0f},
			{x1, y1, 0.0f},
			{x0, y1, 0.0f}
		};
		const float emCoords[4][4] = {
			{emX0, emY0, 0.0f, 0.0f},
			{emX1, emY0, 1.0f, 0.0f},
			{emX1, emY1, 1.0f, 1.0f},
			{emX0, emY1, 0.0f, 1.0f}
		};

		for(int i = 0; i < 4; i++) {
			Vertex v;
			std::memcpy(v.position, positions[i], sizeof(v.position));
			std::memcpy(v.color, col, sizeof(v.color));
			std::memcpy(v.emCoord, emCoords[i], sizeof(v.emCoord));
			std::memcpy(v.bandXform, bx, sizeof(v.bandXform));
			std::memcpy(v.shapeData, sd, sizeof(v.shapeData));
			v.effectId = eid;
			outVerts.push_back(v);
		}

		outIndices.push_back(base + 0);
		outIndices.push_back(base + 1);
		outIndices.push_back(base + 2);
		outIndices.push_back(base + 0);
		outIndices.push_back(base + 2);
		outIndices.push_back(base + 3);

		base += 4;
	}
}

// ============================================================================
// GLFW callbacks
// ============================================================================

static void onGlfwError(int code, const char* msg) {
	std::fprintf(stderr, "GLFW error %d: %s\n", code, msg);
}

static void onKey(GLFWwindow* win, int key, int /*scancode*/, int action, int /*mods*/) {
	ViewState* view = static_cast<ViewState*>(glfwGetWindowUserPointer(win));

	if(key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		glfwSetWindowShouldClose(win, GLFW_TRUE);

	if(!view || action != GLFW_PRESS) return;

	if(key == GLFW_KEY_P) view->usePerspective = !view->usePerspective;
	if(key == GLFW_KEY_R) view->reset();
}

static void onMouseButton(GLFWwindow* win, int button, int action, int /*mods*/) {
	ViewState* view = static_cast<ViewState*>(glfwGetWindowUserPointer(win));

	if(!view) return;

	if(button == GLFW_MOUSE_BUTTON_LEFT) view->leftDragging = (action == GLFW_PRESS);
	if(button == GLFW_MOUSE_BUTTON_RIGHT) view->rightDragging = (action == GLFW_PRESS);

	double x = 0.0, y = 0.0;
	glfwGetCursorPos(win, &x, &y);
	view->lastCursorX = x;
	view->lastCursorY = y;
}

static void onCursorPos(GLFWwindow* win, double x, double y) {
	ViewState* view = static_cast<ViewState*>(glfwGetWindowUserPointer(win));

	if(!view) return;

	const float dx = static_cast<float>(x - view->lastCursorX);
	const float dy = static_cast<float>(y - view->lastCursorY);

	view->lastCursorX = x;
	view->lastCursorY = y;

	if(view->leftDragging) {
		view->yaw -= dx * 0.01f;
		view->pitch -= dy * 0.01f;

		const float limit = 1.45f;
		if(view->pitch > limit) view->pitch = limit;
		if(view->pitch < -limit) view->pitch = -limit;
	}

	if(view->rightDragging) {
		int fbW = 1, fbH = 1;
		glfwGetFramebufferSize(win, &fbW, &fbH);

		const Vec3 eye = orbitCameraPosition(*view);
		const Vec3 forward = normalize(view->target - eye);
		Vec3 right = normalize(cross(forward, {0.0f, 1.0f, 0.0f}));
		Vec3 up = normalize(cross(right, forward));

		if(dot(right, right) <= 1e-6f) right = {1.0f, 0.0f, 0.0f};
		if(dot(up, up) <= 1e-6f) up = {0.0f, 1.0f, 0.0f};

		float worldPerPixel = 0.0f;
		if(view->usePerspective) {
			const float fovy = 45.0f * 3.14159265f / 180.0f;
			worldPerPixel = 2.0f * view->distance * std::tan(fovy * 0.5f) / float(std::max(fbH, 1));
		} else {
			worldPerPixel = 2.0f * (0.5f / view->distance + 0.15f) / float(std::max(fbH, 1));
		}

		view->target = view->target - right * (dx * worldPerPixel) + up * (dy * worldPerPixel);
	}
}

static void onScroll(GLFWwindow* win, double /*xoffset*/, double yoffset) {
	ViewState* view = static_cast<ViewState*>(glfwGetWindowUserPointer(win));

	if(!view) return;

	const float zoom = std::exp(-static_cast<float>(yoffset) * 0.12f);
	view->distance *= zoom;

	if(view->distance < 0.35f) view->distance = 0.35f;
	if(view->distance > 20.0f) view->distance = 20.0f;
}

// ============================================================================
// main
// ============================================================================

int main(int /*argc*/, char** /*argv*/) {
	// ------------------------------------------------------------------------
	// 1. Build Atlas via slughorn::canvas::Canvas
	// (same shape as osgslug-shape-canvas — swap in anything else here)
	// ------------------------------------------------------------------------
	slughorn::Atlas atlas;
	uint32_t keyBase = 0xE0000;

	slughorn::canvas::Canvas canvas(atlas, keyBase);

	canvas.beginPath();
	canvas.moveTo(0.0_cv, 0.0_cv);
	canvas.lineTo(1.0_cv, 0.0_cv);
	// canvas.lineTo(0.5_cv, 1.0_cv);
	canvas.quadTo(0.25_cv, 0.5_cv, 0.5_cv, 1.0_cv);
	canvas.closePath();

	slughorn::Key shapeKey = canvas.fill({1_cv, 0_cv, 0_cv, 1_cv});

	atlas.build();

	// ------------------------------------------------------------------------
	// 2. Assemble layers (colour can differ from the fill colour above)
	// ------------------------------------------------------------------------
	std::vector<slughorn::Layer> layers;

	slughorn::Layer layer;
	layer.key = shapeKey;
	layer.color = {0.2f, 0.8f, 0.4f, 1.0f};
	layer.scale = 1.0f;
	layer.transform = slughorn::Matrix::identity();
	layers.push_back(layer);

	// ------------------------------------------------------------------------
	// 3. Build interleaved mesh
	// ------------------------------------------------------------------------
	std::vector<Vertex> verts;
	std::vector<unsigned short> indices;

	buildMesh(atlas, layers, verts, indices);

	if(verts.empty()) {
		std::fprintf(stderr, "No geometry produced — check Atlas build.\n");
		return 1;
	}

	// ------------------------------------------------------------------------
	// 4. GLFW + GL context
	// ------------------------------------------------------------------------
	glfwSetErrorCallback(onGlfwError);

	if(!glfwInit()) {
		std::fprintf(stderr, "glfwInit failed\n");

		return 1;
	}

	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

#ifdef __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

	GLFWwindow* window = glfwCreateWindow(800, 600, "slughorn GLFW demo", nullptr, nullptr);

	if(!window) {
		std::fprintf(stderr, "glfwCreateWindow failed\n");

		glfwTerminate();

		return 1;
	}

	ViewState view;

	std::printf("Controls:\n");
	std::printf("  Left drag   orbit\n");
	std::printf("  Right drag  pan\n");
	std::printf("  Wheel       zoom\n");
	std::printf("  P           toggle perspective/ortho\n");
	std::printf("  R           reset view\n");
	std::printf("  Esc         quit\n");

	glfwSetWindowUserPointer(window, &view);
	glfwSetKeyCallback(window, onKey);
	glfwSetMouseButtonCallback(window, onMouseButton);
	glfwSetCursorPosCallback(window, onCursorPos);
	glfwSetScrollCallback(window, onScroll);
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1); // vsync

	if(!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
		std::fprintf(stderr, "gladLoadGLLoader failed\n");

		return 1;
	}

	std::printf("OpenGL %s\n", glGetString(GL_VERSION));

	// ------------------------------------------------------------------------
	// 5. Upload Slug textures
	// ------------------------------------------------------------------------
	GLuint texCurve = uploadSlugTexture(atlas.getCurveTextureData());
	GLuint texBand = uploadSlugTexture(atlas.getBandTextureData());

	// ------------------------------------------------------------------------
	// 6. Compile shaders / link program
	// ------------------------------------------------------------------------
	GLuint vert = compileShader(GL_VERTEX_SHADER, k_VertSrc);
	GLuint frag = compileShader(GL_FRAGMENT_SHADER, k_FragSrc);
	GLuint program = linkProgram(vert, frag);

	glDeleteShader(vert);
	glDeleteShader(frag);

	// Uniform locations
	const GLint u_mvp = glGetUniformLocation(program, "u_mvp");
	const GLint u_time = glGetUniformLocation(program, "u_time");
	const GLint u_curveTexture = glGetUniformLocation(program, "u_curveTexture");
	const GLint u_bandTexture = glGetUniformLocation(program, "u_bandTexture");

	glUseProgram(program);
	glUniform1i(u_curveTexture, 0); // texture unit 0
	glUniform1i(u_bandTexture, 1); // texture unit 1
	glUseProgram(0);

	// ------------------------------------------------------------------------
	// 7. Upload geometry to GPU (VAO / VBO / EBO)
	// ------------------------------------------------------------------------
	GLuint vao, vbo, ebo;
	glGenVertexArrays(1, &vao);
	glGenBuffers(1, &vbo);
	glGenBuffers(1, &ebo);

	glBindVertexArray(vao);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	glBufferData(GL_ARRAY_BUFFER,
		(GLsizeiptr)(verts.size() * sizeof(Vertex)),
		verts.data(),
		GL_STATIC_DRAW);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER,
		(GLsizeiptr)(indices.size() * sizeof(unsigned short)),
		indices.data(),
		GL_STATIC_DRAW);

	const GLsizei stride = (GLsizei)sizeof(Vertex);

	// location 0: position (vec3)
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, position));

	// location 1: color (vec4)
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, color));

	// location 2: emCoord (vec4: .xy = em-space, .zw = true [0,1] UV)
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, emCoord));

	// location 3: bandXform (vec4)
	glEnableVertexAttribArray(3);
	glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, bandXform));

	// location 4: shapeData (vec4)
	glEnableVertexAttribArray(4);
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, shapeData));

	// location 5: effectId (float)
	glEnableVertexAttribArray(5);
	glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, stride,
		(void*)offsetof(Vertex, effectId));

	glBindVertexArray(0);

	// ------------------------------------------------------------------------
	// 8. GL state
	// ------------------------------------------------------------------------
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glClearColor(0.15f, 0.15f, 0.15f, 1.0f);

	// ------------------------------------------------------------------------
	// 9. Render loop
	// ------------------------------------------------------------------------
	while(!glfwWindowShouldClose(window)) {
		glfwPollEvents();

		int fbW, fbH;
		glfwGetFramebufferSize(window, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClear(GL_COLOR_BUFFER_BIT);

		const float aspect = (fbH > 0) ? (float)fbW / (float)fbH : 1.0f;
		const Vec3 eye = orbitCameraPosition(view);
		const Vec3 up = std::abs(std::cos(view.pitch)) < 0.05f
			? Vec3{0.0f, 0.0f, 1.0f}
			: Vec3{0.0f, 1.0f, 0.0f};

		float proj[16];
		if(view.usePerspective) {
			perspectiveMatrix(45.0f * 3.14159265f / 180.0f, aspect, 0.01f, 100.0f, proj);
		} else {
			const float margin = 0.15f;
			const float halfH = 0.5f / view.distance + margin;
			const float halfW = halfH * aspect;
			orthoMatrix(-halfW, halfW, -halfH, halfH, -100.0f, 100.0f, proj);
		}

		float viewMatrix[16];
		lookAtMatrix(eye, view.target, up, viewMatrix);

		float mvp[16];
		multiplyMatrix4(proj, viewMatrix, mvp);

		const float t = (float)glfwGetTime();

		glUseProgram(program);
		glUniformMatrix4fv(u_mvp, 1, GL_FALSE, mvp);
		glUniform1f(u_time, t);

		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, texCurve);

		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, texBand);

		glBindVertexArray(vao);
		glDrawElements(GL_TRIANGLES, (GLsizei)indices.size(), GL_UNSIGNED_SHORT, nullptr);
		glBindVertexArray(0);

		glUseProgram(0);

		glfwSwapBuffers(window);
	}

	// ------------------------------------------------------------------------
	// 10. Cleanup
	// ------------------------------------------------------------------------
	glDeleteVertexArrays(1, &vao);
	glDeleteBuffers(1, &vbo);
	glDeleteBuffers(1, &ebo);
	glDeleteTextures(1, &texCurve);
	glDeleteTextures(1, &texBand);
	glDeleteProgram(program);

	glfwDestroyWindow(window);
	glfwTerminate();

	return 0;
}
