// XXX: This example was created entirely by AI! osgSlug is the REAL "testbed" (for now)...
//
// Minimal single-file GLFW + GLAD demo for the slughorn Atlas pipeline.
//
// Builds an Atlas via slughorn::canvas::Canvas (same API as osgslug-shape-canvas),
// uploads the two Slug textures, assembles a VAO from Layer data, and renders
// with a stripped-down Slug fragment shader.
//
// No OSG, no GLM, no external math. Orthographic MVP computed inline.
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
// u_mvp          mat4       orthographic projection
// u_time         float      seconds since start
// u_curveTexture sampler2D  (unit 0) RGBA32F curve data
// u_bandTexture  usampler2D (unit 1) RGBA16UI band data

#include "slughorn.hpp"
#include "slughorn-canvas.hpp"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

// ============================================================================
// Inline shaders
// ============================================================================

static const char* k_VertSrc = R"(
#version 330 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 2) in vec2 a_emCoord;
layout(location = 3) in vec4 a_bandXform; // bandScaleX/Y, bandOffsetX/Y
layout(location = 4) in vec4 a_shapeData; // bandTexX/Y, bandMaxX/Y
layout(location = 5) in float a_effectId; // unused in this demo, carried through

uniform mat4 u_mvp;
uniform float u_time;

out vec2 v_emCoord;
out vec4 v_color;

flat out vec4 v_bandXform;
flat out vec4 v_shapeData;

void main() {
	v_emCoord = a_emCoord;
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
in vec4 v_color;

flat in vec4 v_bandXform; // bandScaleX/Y, bandOffsetX/Y
flat in vec4 v_shapeData; // bandTexX/Y, bandMaxX/Y

uniform sampler2D u_curveTexture;
uniform usampler2D u_bandTexture;

out vec4 fragColor;

// Must match slughorn::Atlas::TEX_WIDTH == 512 == 1<<9
#define TEX_WIDTH 9

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

	ivec2 bandIndex = clamp(
		ivec2(renderCoord * bandTransform.xy + bandTransform.zw),
		ivec2(0, 0),
		ivec2(bandMax.x, bandMax.y)
	);

	// Horizontal bands
	float xcov = 0.0, xwgt = 0.0;
	uvec2 hbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + bandIndex.y, glyphLoc.y), 0).xy;
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

	// Vertical bands
	float ycov = 0.0, ywgt = 0.0;
	uvec2 vbandData = texelFetch(u_bandTexture, ivec2(glyphLoc.x + bandMax.y + 1 + bandIndex.x, glyphLoc.y), 0).xy;
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

// ============================================================================
// Vertex layout (mirrors ShapeDrawable::compile() exactly)
//
// location 0: position vec3
// location 1: color vec4
// location 2: emCoord vec2
// location 3: bandXform vec4 (bandScaleX/Y, bandOffsetX/Y)
// location 4: shapeData vec4 (bandTexX/Y, bandMaxX/Y)
// location 5: effectId float (unused here, carried for forward-compat)
// ============================================================================
struct Vertex {
	float position[3];
	float color[4];
	float emCoord[2];
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
		const float emCoords[4][2] = {
			{emX0, emY0},
			{emX1, emY0},
			{emX1, emY1},
			{emX0, emY1}
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
	if(key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
		glfwSetWindowShouldClose(win, GLFW_TRUE);
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
	canvas.lineTo(0.5_cv, 1.0_cv);
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

	glfwSetKeyCallback(window, onKey);
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

	// location 2: emCoord (vec2)
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
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

		// Orthographic projection: show em-space coordinates directly.
		// The triangle lives in [0,1] x [0,1] em-space; we add a small
		// margin and correct for aspect ratio so it's centred and fills
		// most of the window without distortion.
		const float aspect = (fbH > 0) ? (float)fbW / (float)fbH : 1.0f;
		const float margin = 0.15f;
		const float halfW = (0.5f + margin) * aspect;
		const float halfH = 0.5f + margin;
		const float cx = 0.5f; // centre of the [0,1] em square
		const float cy = 0.5f;

		float mvp[16];
		orthoMatrix(
			cx - halfW, cx + halfW,
			cy - halfH, cy + halfH,
			-1.0f, 1.0f,
			mvp
		);

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
