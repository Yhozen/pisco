// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL 1.1 for Windows desktop apps (opengl32.dll, with GL_ARB_multitexture, GL_EXT_compiled_vertex_array,
// GL_EXT_texture_env_add and WGL_EXT_swap_control), on the GPU ring's explicit records (rt/d3d12_cmd.h), as
// Direct3D 9 is (d3d9_gpu.c): no OpenGL is underneath. One context draws into the window it was made current
// with; SwapBuffers presents it. The fixed-function stages are WGSL from gl_ffp.c.
//
// What a draw becomes: its vertices (from the client arrays between the least and greatest index, or from the
// glBegin / glEnd calls) are packed into one interleaved stream (GlVertex), strips, fans, quads and polygons into
// lists of triangles (WebGPU has neither fans nor quads), then a pipeline made of the state, the textures and
// samplers of the units that are on, the uniform block (gl_ffp.h), one GPU_R_DRAW.
//
// ---- where OpenGL and WebGPU differ, each handled once ----
//  Window y       OpenGL counts from the bottom, WebGPU from the top: viewport, scissor, raster positions and
//                 glReadPixels rows are turned over here (set_view, draw_pixels, read_pixels).
//  Clip z         -w..w against 0..w: the vertex stage maps it (gl_ffp.c); depth ranges are WebGPU's as they are.
//  Winding        a triangle's turn is the one it has on the screen, in OpenGL, Direct3D and WebGPU alike:
//                 GL_CCW is the ring's "counter-clockwise" (Direct3D 9's default front face is the clockwise one).
//  Textures       every one is RGBA8 on the GPU; its base format (GL_RGB, GL_RGBA, GL_ALPHA, ...) is kept for the
//                 texture environment, which reads only the components the format has.
//  Clears         a glClear the scissor rectangle cuts is drawn as a rectangle; a whole one begins a pass.
//  Not covered (said once, and the draw goes on without it, or is left out): lighting, texture coordinate
//  generation, stencil, display lists, the accumulation buffer, polygon modes other than GL_FILL,
//  glCopyTexSubImage2D, textures of types other than GL_UNSIGNED_BYTE.
#include <math.h>
#include "win32.h"
#include "com.h"
#include "gl_ffp.h"
#include "../gpu_ring.h"

#define UNSURE_ONCE(...) do { static int told_; if (!told_++) com_unsure(__VA_ARGS__); } while (0)
static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static u32 bits_of(float f) { u32 v; memcpy(&v, &f, 4); return v; }
// stdcall arguments: a float takes one 4-byte slot, a double two
static float argf(Cpu *c, int slot) { return f32_of((u32)ARG(slot)); }
static double argd(Cpu *c, int slot) { u64 v = (u64)(u32)ARG(slot) | (u64)(u32)ARG(slot + 1) << 32; double d; memcpy(&d, &v, 8); return d; }
static int gl_stats;
static void gl_session_shot(const char *name, u32 frame);

// ---- OpenGL's numbers ----
enum {
  GL_POINTS = 0, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON,
  GL_NEVER = 0x200, GL_FRONT = 0x404, GL_BACK = 0x405, GL_FRONT_AND_BACK = 0x408, GL_CW = 0x900, GL_CCW = 0x901,
  GL_CULL_FACE = 0x0B44, GL_LIGHTING = 0x0B50, GL_FOG = 0x0B60, GL_DEPTH_TEST = 0x0B71, GL_STENCIL_TEST = 0x0B90, GL_ALPHA_TEST = 0x0BC0, GL_BLEND = 0x0BE2,
  GL_SCISSOR_TEST = 0x0C11, GL_TEXTURE_GEN_S = 0x0C60, GL_TEXTURE_GEN_T = 0x0C61, GL_TEXTURE_2D = 0x0DE1, GL_POLYGON_OFFSET_FILL = 0x8037, GL_CLIP_PLANE0 = 0x3000,
  GL_VERTEX_ARRAY = 0x8074, GL_NORMAL_ARRAY = 0x8075, GL_COLOR_ARRAY = 0x8076, GL_TEXTURE_COORD_ARRAY = 0x8078,
  GL_BYTE = 0x1400, GL_UNSIGNED_BYTE, GL_SHORT, GL_UNSIGNED_SHORT, GL_INT, GL_UNSIGNED_INT, GL_FLOAT, GL_DOUBLE = 0x140A,
  GL_MODELVIEW = 0x1700, GL_PROJECTION, GL_TEXTURE,
  GL_ALPHA = 0x1906, GL_RGB, GL_RGBA, GL_LUMINANCE, GL_LUMINANCE_ALPHA, GL_BGR = 0x80E0, GL_BGRA = 0x80E1,
  GL_TEXTURE0 = 0x84C0, GL_COLOR_BUFFER_BIT = 0x4000, GL_DEPTH_BUFFER_BIT = 0x100, GL_STENCIL_BUFFER_BIT = 0x400,
};

// ---- the GPU side: ids and records ----
static u32 next_id = 0x40000000u;   // apart from the other front ends' ids
static void rec(u32 op, const u32 *w, u32 n) { u8 *p = rt_gpu_rec_begin(op, 4 * n); memcpy(p, w, 4 * n); rt_gpu_rec_end_quiet(); }
static u32 resource(u32 dim, u32 format, u32 width, u32 height, u32 mips, u32 flags) {
  u32 w[14] = {next_id++, dim, format, width, 0, height, 1, mips, 1, flags, 1, 0, 0, 0};
  rec(GPU_R_RESOURCE, w, 14); return w[0];
}
static void destroy(u32 id) { if (id) rec(GPU_R_DESTROY, &id, 1); }
static void write_buffer(u32 id, u32 offset, const void *src, u32 bytes, u32 mode) {
  u32 padded = (bytes + 3) & ~3u;
  u8 *p = rt_gpu_rec_begin(GPU_R_WRITE_BUFFER, 16 + padded); u32 *w = (u32 *)p;
  w[0] = id; w[1] = offset; w[2] = padded; w[3] = mode; memcpy(p + 16, src, bytes); rt_gpu_rec_end_quiet();
}
static void write_texture(u32 id, u32 mip, u32 x, u32 y, u32 width, u32 height, const u8 *rgba) {
  u32 pitch = 4 * width, rows = (4u << 20) / (pitch ? pitch : 1); if (!rows) rows = 1;
  for (u32 r = 0; r < height; r += rows) {
    u32 n = height - r < rows ? height - r : rows;
    u8 *p = rt_gpu_rec_begin(GPU_R_WRITE_TEXTURE, 36 + n * pitch); u32 *w = (u32 *)p;
    w[0] = id; w[1] = mip; w[2] = x; w[3] = y + r; w[4] = 0; w[5] = width; w[6] = n; w[7] = 1; w[8] = pitch;
    memcpy(p + 36, rgba + (size_t)r * pitch, (size_t)n * pitch); rt_gpu_rec_end_quiet();
  }
}
enum { DXGI_RGBA8 = 28, DXGI_BGRA8 = 87, DXGI_D24S8 = 45, DXGI_R32G32B32A32_FLOAT = 2, DXGI_R8G8B8A8_UNORM = 28, DXGI_R32G32_FLOAT = 16 };
static struct { u64 draws, skipped, vertices, frames, texture_bytes, clears_drawn; } stat;

// ---- textures ----
typedef struct {
  u32 gpu, w, h, levels, defined;   // the GPU texture (all levels of a w x h level 0); which levels were given
  u8 base;                          // GL_BASE_*
  u32 min, mag, wrap_s, wrap_t;
} Tex;
static Tex **texs; static u32 tex_cap;   // by name
static Tex *tex_of(u32 name, int make) {
  if (name >= tex_cap) {
    if (!make || name >= (1u << 22)) return 0;
    u32 n = tex_cap ? tex_cap : 1024; while (n <= name) n *= 2;
    texs = realloc(texs, n * sizeof *texs); memset(texs + tex_cap, 0, (n - tex_cap) * sizeof *texs); tex_cap = n;
  }
  if (!texs[name] && make) { Tex *t = calloc(1, sizeof *t); t->min = 0x2702; t->mag = 0x2601; t->wrap_s = t->wrap_t = 0x2901; texs[name] = t; }
  return texs[name];
}

// ---- the context's state ----
typedef struct { int on; u32 size, type, stride, ptr; } Array;
typedef struct { float m[32][16]; int depth; } Stack;
static struct {
  int current;                        // a context is current
  u32 hdc, hwnd, width, height;       // the window drawn into, its client area's size
  u32 color_tex, depth_tex;           // the window's colour and depth textures
  int pass_open;
  u32 frames;
  // enables
  int tex2d[GL_FFP_UNITS], blend, depth_test, cull, alpha_test, fog, scissor, offset_fill, clip0, stencil, lighting, texgen;
  u32 active, client_active;          // texture units, 0 or 1
  u32 bound[GL_FFP_UNITS];            // texture names
  u32 env[GL_FFP_UNITS]; float env_color[GL_FFP_UNITS][4];
  u32 src, dst, depth_func, cull_face, front_face, alpha_func; float alpha_ref;
  int depth_mask; u8 color_mask;
  float offset_factor, offset_units;
  s32 vp[4], sc[4]; double depth_near, depth_far;
  u32 fog_mode; float fog_start, fog_end, fog_density, fog_color[4];
  float clear_color[4]; double clear_depth; u32 clear_stencil;
  double clip_plane[4];               // in eye coordinates
  u32 matrix_mode; Stack modelview, projection, texture[GL_FFP_UNITS];
  float color[4], tex[GL_FFP_UNITS][4], normal[3];   // the current values
  Array vertex, color_array, normal_array, tex_array[GL_FFP_UNITS];
  u32 unpack_alignment, pack_alignment, unpack_row_length;
  float raster[4]; int raster_valid; float zoom_x, zoom_y;
  u32 error;
  // glBegin / glEnd
  int in_begin; u32 begin_mode; GlVertex *imm; u32 imm_count, imm_cap;
} gl;

static void identity(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
static void reset_state(void) {
  for (int i = 0; i < GL_FFP_UNITS; i++) { gl.env[i] = 0x2100; identity(gl.texture[i].m[0]); gl.texture[i].depth = 0; gl.tex[i][3] = 1.0f; }
  identity(gl.modelview.m[0]); identity(gl.projection.m[0]); gl.modelview.depth = gl.projection.depth = 0;
  gl.src = 1; gl.dst = 0; gl.depth_func = 0x201; gl.cull_face = GL_BACK; gl.front_face = GL_CCW; gl.alpha_func = 0x207;
  gl.depth_mask = 1; gl.color_mask = 15; gl.depth_near = 0; gl.depth_far = 1;
  gl.fog_mode = 0x0800; gl.fog_end = 1; gl.fog_density = 1;
  gl.clear_depth = 1; gl.matrix_mode = GL_MODELVIEW;
  gl.color[0] = gl.color[1] = gl.color[2] = gl.color[3] = 1.0f; gl.normal[2] = 1.0f;
  gl.unpack_alignment = gl.pack_alignment = 4; gl.zoom_x = gl.zoom_y = 1.0f; gl.raster[3] = 1.0f; gl.raster_valid = 1;
}
static Stack *stack(void) { return gl.matrix_mode == GL_PROJECTION ? &gl.projection : gl.matrix_mode == GL_TEXTURE ? &gl.texture[gl.active] : &gl.modelview; }
static float *top(Stack *s) { return s->m[s->depth]; }
static void mul(float *r, const float *a, const float *b) {   // r = a b, column-major
  float t[16];
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a[k * 4 + j] * b[i * 4 + k]; t[i * 4 + j] = v; }
  memcpy(r, t, 64);
}
static void mult(const float *m) { float *t = top(stack()); mul(t, t, m); }
static int inverse(double *inv, const float *m) {   // a 4x4 inverse by cofactors; 0 if singular
  double a[16]; for (int i = 0; i < 16; i++) a[i] = m[i];
  inv[0] = a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
  inv[4] = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
  inv[8] = a[4]*a[9]*a[15] - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
  inv[12] = -a[4]*a[9]*a[14] + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
  inv[1] = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
  inv[5] = a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
  inv[9] = -a[0]*a[9]*a[15] + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
  inv[13] = a[0]*a[9]*a[14] - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
  inv[2] = a[1]*a[6]*a[15] - a[1]*a[7]*a[14] - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7] - a[13]*a[3]*a[6];
  inv[6] = -a[0]*a[6]*a[15] + a[0]*a[7]*a[14] + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7] + a[12]*a[3]*a[6];
  inv[10] = a[0]*a[5]*a[15] - a[0]*a[7]*a[13] - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7] - a[12]*a[3]*a[5];
  inv[14] = -a[0]*a[5]*a[14] + a[0]*a[6]*a[13] + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6] + a[12]*a[2]*a[5];
  inv[3] = -a[1]*a[6]*a[11] + a[1]*a[7]*a[10] + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7] + a[9]*a[3]*a[6];
  inv[7] = a[0]*a[6]*a[11] - a[0]*a[7]*a[10] - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7] - a[8]*a[3]*a[6];
  inv[11] = -a[0]*a[5]*a[11] + a[0]*a[7]*a[9] + a[4]*a[1]*a[11] - a[4]*a[3]*a[9] - a[8]*a[1]*a[7] + a[8]*a[3]*a[5];
  inv[15] = a[0]*a[5]*a[10] - a[0]*a[6]*a[9] - a[4]*a[1]*a[10] + a[4]*a[2]*a[9] + a[8]*a[1]*a[6] - a[8]*a[2]*a[5];
  double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
  if (det == 0) return 0;
  for (int i = 0; i < 16; i++) inv[i] /= det;
  return 1;
}

// ---- the window's buffers and passes ----
static void make_buffers(void) {
  s32 w = 640, h = 480;
  if (!win32_client_size(gl.hwnd, &w, &h) || w <= 0 || h <= 0) { w = 640; h = 480; }
  if (gl.color_tex && (u32)w == gl.width && (u32)h == gl.height) return;
  destroy(gl.color_tex); destroy(gl.depth_tex);
  gl.width = (u32)w; gl.height = (u32)h;
  gl.color_tex = resource(2, DXGI_BGRA8, gl.width, gl.height, 1, 1);
  gl.depth_tex = resource(2, DXGI_D24S8, gl.width, gl.height, 1, 2);
  gl.pass_open = 0;
  gl.vp[2] = w; gl.vp[3] = h; gl.sc[2] = w; gl.sc[3] = h;
  if (rt_trace) rt_log("OpenGL: the window's buffers are %ux%u", gl.width, gl.height);
}
static void begin_pass(u32 flags) {
  u32 w[10] = {gl.color_tex, 0, gl.depth_tex, flags, bits_of(gl.clear_color[0]), bits_of(gl.clear_color[1]), bits_of(gl.clear_color[2]), bits_of(gl.clear_color[3]),
               bits_of((float)gl.clear_depth), gl.clear_stencil & 255};
  rec(GPU_R_PASS, w, 10); gl.pass_open = 1;
}
static struct { int sent; s32 v[4], s[4]; double n, f; float fix[4]; } view;
static void clampi(s32 *v, s32 lo, s32 hi) { if (*v < lo) *v = lo; if (*v > hi) *v = hi; }
// the viewport and the scissor rectangle, turned over (OpenGL's y counts from the bottom); `vp` and `sc` in OpenGL's
// terms. A viewport that reaches past the window is cut to it (WebGPU wants it inside), and view.fix scales clip space
// so that what is drawn lands where OpenGL's viewport puts it (outside the window, nothing is).
static void set_view_rect(const s32 *vp, const s32 *sc, double n, double f) {
  if (view.sent && !memcmp(view.v, vp, 16) && !memcmp(view.s, sc, 16) && view.n == n && view.f == f) return;
  view.sent = 1; memcpy(view.v, vp, 16); memcpy(view.s, sc, 16); view.n = n; view.f = f;
  s32 x0 = sc[0], y0 = sc[1], x1 = sc[0] + sc[2], y1 = sc[1] + sc[3];
  clampi(&x0, 0, (s32)gl.width); clampi(&x1, 0, (s32)gl.width); clampi(&y0, 0, (s32)gl.height); clampi(&y1, 0, (s32)gl.height);
  if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
  float lo = (float)(n < 0 ? 0 : n > 1 ? 1 : n), hi = (float)(f < 0 ? 0 : f > 1 ? 1 : f);
  // OpenGL's viewport in WebGPU's coordinates (y from the top), and its part inside the window
  float W = (float)gl.width, H = (float)gl.height, vw = (float)(vp[2] > 0 ? vp[2] : 1), vh = (float)(vp[3] > 0 ? vp[3] : 1), vx = (float)vp[0], vt = H - (float)vp[1] - vh;
  float cx0 = vx < 0 ? 0 : vx > W ? W : vx, cx1 = vx + vw > W ? W : vx + vw < 0 ? 0 : vx + vw, ct0 = vt < 0 ? 0 : vt > H ? H : vt, ct1 = vt + vh > H ? H : vt + vh < 0 ? 0 : vt + vh;
  float cw = cx1 - cx0, ch = ct1 - ct0;
  if (cw < 1 || ch < 1) { cx0 = 0; ct0 = 0; cw = W; ch = H; x0 = x1 = 0; y0 = y1 = 0; }   // nothing of it is in the window: an empty scissor draws nothing
  view.fix[0] = vw / cw; view.fix[1] = (2 * (vx - cx0) + vw) / cw - 1;
  view.fix[2] = vh / ch; view.fix[3] = 1 - (2 * (vt - ct0) + vh) / ch;
  u32 w[10] = {bits_of(cx0), bits_of(ct0), bits_of(cw), bits_of(ch), bits_of(lo), bits_of(hi), (u32)x0, (u32)((s32)gl.height - y1), (u32)(x1 - x0), (u32)(y1 - y0)};
  rec(GPU_R_VIEWPORT, w, 10);
}
static void set_view(void) {
  s32 full[4] = {0, 0, (s32)gl.width, (s32)gl.height};
  set_view_rect(gl.vp, gl.scissor ? gl.sc : full, gl.depth_near, gl.depth_far);
}

// ---- shaders, samplers, pipelines ----
static u32 shader_record(const char *text, size_t len) {
  u8 *p = rt_gpu_rec_begin(GPU_R_SHADER, 8 + (u32)len); u32 *w = (u32 *)p;
  w[0] = next_id++; w[1] = (u32)len; memcpy(p + 8, text, len); rt_gpu_rec_end_quiet();
  return w[0];
}
static char shader_text[16384];
static u32 vertex_shader(void) {
  static u32 id; if (id) return id;
  size_t n = gl_vertex_wgsl(shader_text, sizeof shader_text);
  if (!n) rt_fatal("OpenGL: the vertex stage: %s", shader_text);
  return id = shader_record(shader_text, n);
}
typedef struct { GlFragmentKey key; u32 id; } FragmentShader;
static FragmentShader *fragment_shaders; static u32 fragment_shader_count;
static u32 fragment_shader(const GlFragmentKey *k) {
  for (u32 i = 0; i < fragment_shader_count; i++) if (!memcmp(&fragment_shaders[i].key, k, sizeof *k)) return fragment_shaders[i].id;
  char hex[2 * sizeof *k + 1], what[200];
  for (size_t i = 0; i < sizeof *k; i++) snprintf(hex + 2 * i, 3, "%02x", ((const u8 *)k)[i]);
  gl_fragment_describe(k, what, sizeof what);
  size_t n = gl_fragment_wgsl(k, shader_text, sizeof shader_text);
  u32 id = n ? shader_record(shader_text, n) : 0;
  if (!n) com_unsure("OpenGL: no fragment stage for %s (%s): %s", hex, what, shader_text);
  else if (gl_stats) rt_log("OpenGL shader fs %s: %s", hex, what);
  fragment_shaders = realloc(fragment_shaders, (fragment_shader_count + 1) * sizeof *fragment_shaders);
  fragment_shaders[fragment_shader_count++] = (FragmentShader){*k, id};
  return id;
}
typedef struct { u8 u, v, mag, min, mip, mips; u16 pad; u32 id; } Sampler;
static Sampler *samplers; static u32 sampler_count;
static u32 sampler(const Tex *t) {
  Sampler k = {0};
  k.u = t->wrap_s == 0x2901 ? 1 : 3; k.v = t->wrap_t == 0x2901 ? 1 : 3;   // GL_REPEAT; GL_CLAMP and GL_CLAMP_TO_EDGE clamp
  k.mag = t->mag == 0x2601;
  k.min = t->min == 0x2601 || t->min == 0x2701 || t->min == 0x2703;
  k.mip = t->min == 0x2702 || t->min == 0x2703;
  k.mips = t->min >= 0x2700 && t->min <= 0x2703;
  for (u32 i = 0; i < sampler_count; i++) if (!memcmp(&samplers[i], &k, 8)) return samplers[i].id;
  k.id = next_id++;
  u32 w[9] = {k.id, k.u, k.v, 3, k.mag, k.min, k.mip, 1, bits_of(k.mips ? 32.0f : 0.0f)};
  rec(GPU_R_SAMPLER, w, 9);
  samplers = realloc(samplers, (sampler_count + 1) * sizeof *samplers); samplers[sampler_count++] = k;
  return k.id;
}
typedef struct {
  u32 fs; u8 topology, cull, front, write_mask, blend, src, dst, src_a, dst_a, depth_compare, depth_write, units; s32 bias; u32 slope;
} PipelineKey;
_Static_assert(sizeof(PipelineKey) == 24, "PipelineKey has no hidden padding");
typedef struct { PipelineKey key; u32 id; } Pipeline;
static Pipeline *pipelines; static u32 pipeline_count;
static u32 pipeline(const PipelineKey *k) {
  for (u32 i = 0; i < pipeline_count; i++) if (!memcmp(&pipelines[i].key, k, sizeof *k)) return pipelines[i].id;
  u32 w[GPU_PIPELINE_WORDS + 3 * 5 + 2 + 4 * 4], n = GPU_PIPELINE_WORDS, id = next_id++, units = __builtin_popcount(k->units);
  u32 head[GPU_PIPELINE_WORDS] = {id, vertex_shader(), k->fs, k->topology, k->cull, k->front, DXGI_BGRA8, k->write_mask, k->blend, k->src, k->dst, 1, k->src_a, k->dst_a, 1,
    DXGI_D24S8, k->depth_compare, k->depth_write, (u32)k->bias, k->slope, 1 + 2 * units, 1, 4, 0};
  memcpy(w, head, sizeof head);
  w[n++] = 0; w[n++] = GPU_BIND_UNIFORM; w[n++] = sizeof(GlUniforms);
  for (u32 i = 0; i < GL_FFP_UNITS; i++) if (k->units & (1u << i)) { w[n++] = 1 + 2 * i; w[n++] = GPU_BIND_TEXTURE_2D; w[n++] = 0; w[n++] = 2 + 2 * i; w[n++] = GPU_BIND_SAMPLER; w[n++] = 0; }
  w[n++] = sizeof(GlVertex); w[n++] = 0;
  u32 attrs[4][4] = {{GL_LOC_POSITION, 0, 0, DXGI_R32G32B32A32_FLOAT}, {GL_LOC_COLOR, 0, 16, DXGI_R8G8B8A8_UNORM}, {GL_LOC_TEX0, 0, 20, DXGI_R32G32_FLOAT}, {GL_LOC_TEX1, 0, 28, DXGI_R32G32_FLOAT}};
  memcpy(w + n, attrs, sizeof attrs); n += 16;
  rec(GPU_R_PIPELINE, w, n);
  pipelines = realloc(pipelines, (pipeline_count + 1) * sizeof *pipelines); pipelines[pipeline_count++] = (Pipeline){*k, id};
  return id;
}
// OpenGL's blend factors -> the ring's (Direct3D's numbers); for alpha, the colour factors read alpha
static u8 blend_factor(u32 f, int alpha) {
  switch (f) {
  case 0: return 1; case 1: return 2;
  case 0x300: return alpha ? 5 : 3; case 0x301: return alpha ? 6 : 4; case 0x302: return 5; case 0x303: return 6;
  case 0x304: return 7; case 0x305: return 8; case 0x306: return alpha ? 7 : 9; case 0x307: return alpha ? 8 : 10; case 0x308: return 11;
  default: UNSURE_ONCE("OpenGL: blend factor 0x%x", f); return 2;
  }
}

// ---- vertices from the client arrays ----
static float read_comp(u32 type, u64 p, int normalize) {
  switch (type) {
  case GL_FLOAT: return f32_of(M32(p));
  case GL_DOUBLE: { u64 v = M64(p); double d; memcpy(&d, &v, 8); return (float)d; }
  case GL_SHORT: return normalize ? (float)(s16)M16(p) / 32767.0f : (float)(s16)M16(p);
  case GL_INT: return normalize ? (float)(s32)M32(p) / 2147483647.0f : (float)(s32)M32(p);
  case GL_UNSIGNED_BYTE: return normalize ? (float)M8(p) / 255.0f : (float)M8(p);
  case GL_BYTE: return normalize ? (float)(s8)M8(p) / 127.0f : (float)(s8)M8(p);
  case GL_UNSIGNED_SHORT: return normalize ? (float)M16(p) / 65535.0f : (float)M16(p);
  case GL_UNSIGNED_INT: return normalize ? (float)((double)M32(p) / 4294967295.0) : (float)M32(p);
  default: UNSURE_ONCE("OpenGL: an array of type 0x%x", type); return 0;
  }
}
static u32 type_size(u32 type) { switch (type) { case GL_BYTE: case GL_UNSIGNED_BYTE: return 1; case GL_SHORT: case GL_UNSIGNED_SHORT: return 2; case GL_DOUBLE: return 8; default: return 4; } }
static u64 element(const Array *a, u32 i) { u32 stride = a->stride ? a->stride : a->size * type_size(a->type); return (u64)a->ptr + (u64)stride * i; }
static u8 to_byte(float v) { v = v < 0 ? 0 : v > 1 ? 1 : v; return (u8)(v * 255.0f + 0.5f); }
static void fetch(GlVertex *v, u32 i) {
  const Array *a = &gl.vertex;
  u64 p = element(a, i); u32 ts = type_size(a->type);
  v->pos[0] = v->pos[1] = v->pos[2] = 0; v->pos[3] = 1;
  for (u32 k = 0; k < a->size && k < 4; k++) v->pos[k] = read_comp(a->type, p + k * ts, 0);
  if (gl.color_array.on) {
    const Array *c = &gl.color_array; u64 q = element(c, i); u32 cs = type_size(c->type);
    if (c->type == GL_UNSIGNED_BYTE) { v->color[3] = 255; for (u32 k = 0; k < c->size && k < 4; k++) v->color[k] = M8(q + k); }
    else { v->color[3] = 255; for (u32 k = 0; k < c->size && k < 4; k++) v->color[k] = to_byte(read_comp(c->type, q + k * cs, 1)); }
  } else for (int k = 0; k < 4; k++) v->color[k] = to_byte(gl.color[k]);
  for (int u = 0; u < GL_FFP_UNITS; u++) {
    float *t = u ? v->tex1 : v->tex0;
    if (gl.tex_array[u].on) { const Array *ta = &gl.tex_array[u]; u64 q = element(ta, i); u32 s = type_size(ta->type); t[0] = read_comp(ta->type, q, 0); t[1] = ta->size > 1 ? read_comp(ta->type, q + s, 0) : 0; }
    else { t[0] = gl.tex[u][0]; t[1] = gl.tex[u][1]; }
  }
}

// ---- a draw ----
// Ring buffers for vertices and indices: what a draw needs is appended; when the end is reached the buffer starts
// again with new storage (GPU_WRITE_DISCARD), and the draws before keep the old.
enum { VB_SIZE = 8u << 20, IB_SIZE = 4u << 20 };
static u32 vb, vb_at, ib, ib_at;
static u32 put_vertices(const GlVertex *v, u32 count) {
  u32 bytes = count * sizeof(GlVertex), mode = GPU_WRITE_APPEND;
  if (bytes > VB_SIZE) return ~0u;
  if (!vb) { vb = resource(0, 0, VB_SIZE, 1, 1, 0); vb_at = VB_SIZE; }
  if (vb_at + bytes > VB_SIZE) { vb_at = 0; mode = GPU_WRITE_DISCARD; }
  u32 at = vb_at; write_buffer(vb, at, v, bytes, mode); vb_at += (bytes + 15) & ~15u;
  return at;
}
static u32 put_indices(const u32 *ix, u32 count) {
  u32 bytes = 4 * count, mode = GPU_WRITE_APPEND;
  if (bytes > IB_SIZE) return ~0u;
  if (!ib) { ib = resource(0, 0, IB_SIZE, 1, 1, 0); ib_at = IB_SIZE; }
  if (ib_at + bytes > IB_SIZE) { ib_at = 0; mode = GPU_WRITE_DISCARD; }
  u32 at = ib_at; write_buffer(ib, at, ix, bytes, mode); ib_at += (bytes + 15) & ~15u;
  return at;
}
// a primitive's indices as a list: `in` are positions in the vertex list; the topology the list is drawn with
static u32 *expand(u32 mode, const u32 *in, u32 n, u32 *out_n, u8 *topology) {
  u32 cap = 3 * n + 6, k = 0; u32 *o = malloc(4 * cap);
  switch (mode) {
  case GL_POINTS: *topology = 1; for (u32 i = 0; i < n; i++) o[k++] = in[i]; break;
  case GL_LINES: *topology = 2; for (u32 i = 0; i + 1 < n; i += 2) { o[k++] = in[i]; o[k++] = in[i + 1]; } break;
  case GL_LINE_STRIP: case GL_LINE_LOOP: *topology = 2;
    for (u32 i = 0; i + 1 < n; i++) { o[k++] = in[i]; o[k++] = in[i + 1]; }
    if (mode == GL_LINE_LOOP && n > 2) { o[k++] = in[n - 1]; o[k++] = in[0]; }
    break;
  case GL_TRIANGLES: *topology = 4; for (u32 i = 0; i + 2 < n; i += 3) { o[k++] = in[i]; o[k++] = in[i + 1]; o[k++] = in[i + 2]; } break;
  case GL_TRIANGLE_STRIP: *topology = 4;   // every other triangle turned, so that all keep the first one's winding
    for (u32 i = 0; i + 2 < n; i++) { if (i & 1) { o[k++] = in[i + 1]; o[k++] = in[i]; } else { o[k++] = in[i]; o[k++] = in[i + 1]; } o[k++] = in[i + 2]; }
    break;
  case GL_TRIANGLE_FAN: case GL_POLYGON: *topology = 4; for (u32 i = 1; i + 1 < n; i++) { o[k++] = in[0]; o[k++] = in[i]; o[k++] = in[i + 1]; } break;
  case GL_QUADS: *topology = 4;
    for (u32 i = 0; i + 3 < n; i += 4) { o[k++] = in[i]; o[k++] = in[i + 1]; o[k++] = in[i + 2]; o[k++] = in[i]; o[k++] = in[i + 2]; o[k++] = in[i + 3]; }
    break;
  case GL_QUAD_STRIP: *topology = 4;
    for (u32 i = 0; i + 3 < n; i += 2) { o[k++] = in[i]; o[k++] = in[i + 1]; o[k++] = in[i + 3]; o[k++] = in[i]; o[k++] = in[i + 3]; o[k++] = in[i + 2]; }
    break;
  default: UNSURE_ONCE("OpenGL: primitive mode %u", mode); *topology = 4;
  }
  *out_n = k; return o;
}
static void uniforms(GlUniforms *u) {
  memset(u, 0, sizeof *u);
  mul(u->mvp, top(&gl.projection), top(&gl.modelview));
  memcpy(u->modelview, top(&gl.modelview), 64);
  for (int i = 0; i < GL_FFP_UNITS; i++) { memcpy(u->tex[i], top(&gl.texture[i]), 64); memcpy(u->env_color[i], gl.env_color[i], 16); }
  memcpy(u->fog_color, gl.fog_color, 16);
  u->fog[0] = gl.fog_start; u->fog[1] = gl.fog_end; u->fog[2] = gl.fog_end != gl.fog_start ? 1.0f / (gl.fog_end - gl.fog_start) : 0.0f; u->fog[3] = gl.fog_density;
  u->misc[0] = roundf((gl.alpha_ref < 0 ? 0 : gl.alpha_ref > 1 ? 1 : gl.alpha_ref) * 255.0f) / 255.0f;
  for (int i = 0; i < 4; i++) u->clip[i] = (float)gl.clip_plane[i];
}
static int complete(const Tex *t) {   // what OpenGL would sample: level 0, and every level when a mipmap filter is on
  if (!t || !t->gpu || !(t->defined & 1)) return 0;
  u32 all = (1u << t->levels) - 1;   // (levels are at most 12: textures are at most 2048 wide)
  if (t->min >= 0x2700 && t->min <= 0x2703 && (t->defined & all) != all) return 0;
  return 1;
}
// Draws `n` indices (into `verts`, `nv` of them) with the state as it is. `fixed`: a draw of this file's own (a
// clear, glDrawPixels) gives its pipeline key, uniforms and texture instead.
typedef struct { PipelineKey pk; GlUniforms u; u32 texture, samp; u8 base; } Own;
static void draw(u32 mode, const GlVertex *verts, u32 nv, const u32 *idx, u32 n, const Own *own) {
  if (!gl.current || !nv || !n) return;
  if (gl.lighting) UNSURE_ONCE("OpenGL: lighting is not drawn (the vertex colours are used)");
  if (gl.texgen) UNSURE_ONCE("OpenGL: texture coordinate generation is not done");
  if (gl.stencil && !own) UNSURE_ONCE("OpenGL: the stencil test is not done");
  u32 nl; u8 topology; u32 *list = expand(mode, idx, n, &nl, &topology);
  if (!nl) { free(list); return; }
  if (!gl.pass_open) begin_pass(0);
  if (!own) set_view();
  PipelineKey pk; GlUniforms u; u32 bind[2 * GL_FFP_UNITS], nb = 0;
  if (own) { pk = own->pk; u = own->u; if (pk.units) { bind[nb++] = own->texture; bind[nb++] = own->samp; } }
  else {
    GlFragmentKey fk = {0};
    memset(&pk, 0, sizeof pk);
    for (int i = 0; i < GL_FFP_UNITS; i++) {
      if (!gl.tex2d[i]) continue;
      Tex *t = tex_of(gl.bound[i], 0);
      if (!complete(t)) {   // OpenGL: an incomplete texture is as if texturing were off for the unit
        if (gl_stats > 1 && t && !(t->defined & 0x80000000u)) { t->defined |= 0x80000000u; rt_log("OpenGL: texture %u (%ux%u, %u levels, levels given 0x%x, min 0x%x) is incomplete", gl.bound[i], t->w, t->h, t->levels, t->defined & 0x7FFFFFFF, t->min); }
        continue;
      }
      fk.unit_on[i] = 1; fk.base[i] = t->base;
      fk.env[i] = gl.env[i] == 0x1E01 ? GL_ENV_REPLACE : gl.env[i] == 0x2101 ? GL_ENV_DECAL : gl.env[i] == 0x0BE2 ? GL_ENV_BLEND : gl.env[i] == 0x0104 ? GL_ENV_ADD : GL_ENV_MODULATE;
      pk.units |= (u8)(1u << i); bind[nb++] = t->gpu; bind[nb++] = sampler(t);
    }
    if (gl.alpha_test) fk.alpha_test = (u8)(gl.alpha_func - GL_NEVER + 1);
    if (gl.fog) fk.fog = gl.fog_mode == 0x2601 ? GL_FOG_LINEAR : gl.fog_mode == 0x0801 ? GL_FOG_EXP2 : GL_FOG_EXP;
    fk.clip = (u8)gl.clip0;
    pk.fs = fragment_shader(&fk);
    if (!pk.fs) { free(list); stat.skipped++; return; }
    pk.topology = topology;
    if (gl.cull && topology == 4) {
      if (gl.cull_face == GL_FRONT_AND_BACK) { free(list); return; }
      pk.cull = gl.cull_face == GL_FRONT ? 1 : 2;
    }
    pk.front = gl.front_face == GL_CW;
    pk.write_mask = gl.color_mask;
    if (gl.blend) { pk.blend = 1; pk.src = blend_factor(gl.src, 0); pk.dst = blend_factor(gl.dst, 0); pk.src_a = blend_factor(gl.src, 1); pk.dst_a = blend_factor(gl.dst, 1); }
    pk.depth_compare = gl.depth_test ? (u8)(gl.depth_func - GL_NEVER + 1) : 8;
    pk.depth_write = gl.depth_test && gl.depth_mask;
    if (gl.offset_fill && topology == 4) { pk.bias = (s32)gl.offset_units; pk.slope = bits_of(gl.offset_factor); }
    uniforms(&u);
  }
  pk.topology = topology;
  memcpy(u.view, view.fix, 16);
  u32 at = put_vertices(verts, nv), iat = put_indices(list, nl);
  free(list);
  if (at == ~0u || iat == ~0u) { UNSURE_ONCE("OpenGL: a draw of %u vertices is too large", nv); stat.skipped++; return; }
  u32 pid = pipeline(&pk);
  u32 words = GPU_DRAW_WORDS + 2 + nb + 1;
  u8 *p = rt_gpu_rec_begin(GPU_R_DRAW, 4 * words + sizeof u); u32 *w = (u32 *)p;
  u32 head[GPU_DRAW_WORDS] = {pid, GPU_DRAW_INDEXED | GPU_DRAW_INDEX32, nl, 1, 0, 0, 0, ib, iat, 1, 1 + nb, sizeof u};
  memcpy(w, head, sizeof head);
  u32 k = GPU_DRAW_WORDS;
  w[k++] = vb; w[k++] = at;
  w[k++] = 0; for (u32 i = 0; i < nb; i++) w[k++] = bind[i];
  memcpy(p + 4 * k, &u, sizeof u);
  rt_gpu_rec_end_quiet();
  stat.draws++; stat.vertices += nv;
}

// glDrawArrays / glDrawElements: the vertices between the least and greatest index, packed
static void draw_arrays_range(u32 mode, u32 lo, u32 hi, const u32 *idx, u32 n) {
  if (!gl.vertex.on || !gl.vertex.ptr) { UNSURE_ONCE("OpenGL: a draw without a vertex array"); return; }
  u32 nv = hi - lo + 1;
  GlVertex *v = malloc(nv * sizeof *v);
  for (u32 i = 0; i < nv; i++) fetch(&v[i], lo + i);
  u32 *rel = malloc(4 * n); for (u32 i = 0; i < n; i++) rel[i] = idx[i] - lo;
  draw(mode, v, nv, rel, n, 0);
  free(v); free(rel);
}
IMP(opengl32, glDrawArrays) { // (mode, first, count)
  u32 first = (u32)ARG(1), n = (u32)ARG(2); if (!n) return;
  u32 *ix = malloc(4 * n); for (u32 i = 0; i < n; i++) ix[i] = first + i;
  draw_arrays_range((u32)ARG(0), first, first + n - 1, ix, n); free(ix);
}
IMP(opengl32, glDrawElements) { // (mode, count, type, indices)
  u32 n = (u32)ARG(1), type = (u32)ARG(2); u64 p = ARG(3); if (!n) return;
  u32 *ix = malloc(4 * n), lo = ~0u, hi = 0;
  for (u32 i = 0; i < n; i++) {
    u32 v = type == GL_UNSIGNED_INT ? M32(p + 4ull * i) : type == GL_UNSIGNED_SHORT ? M16(p + 2ull * i) : M8(p + i);
    ix[i] = v; if (v < lo) lo = v; if (v > hi) hi = v;
  }
  draw_arrays_range((u32)ARG(0), lo, hi, ix, n); free(ix);
}
IMP(opengl32, glArrayElement) { // inside glBegin: one vertex from the arrays
  if (!gl.in_begin || !gl.vertex.on) return;
  if (gl.imm_count == gl.imm_cap) { gl.imm_cap = gl.imm_cap ? 2 * gl.imm_cap : 1024; gl.imm = realloc(gl.imm, gl.imm_cap * sizeof *gl.imm); }
  fetch(&gl.imm[gl.imm_count++], (u32)ARG(0));
}
IMP(opengl32, glLockArraysEXT) {}
IMP(opengl32, glUnlockArraysEXT) {}

// ---- glBegin / glEnd ----
IMP(opengl32, glBegin) { gl.in_begin = 1; gl.begin_mode = (u32)ARG(0); gl.imm_count = 0; }
static void vertex4(float x, float y, float z, float w) {
  if (!gl.in_begin) return;
  if (gl.imm_count == gl.imm_cap) { gl.imm_cap = gl.imm_cap ? 2 * gl.imm_cap : 1024; gl.imm = realloc(gl.imm, gl.imm_cap * sizeof *gl.imm); }
  GlVertex *v = &gl.imm[gl.imm_count++];
  v->pos[0] = x; v->pos[1] = y; v->pos[2] = z; v->pos[3] = w;
  for (int k = 0; k < 4; k++) v->color[k] = to_byte(gl.color[k]);
  v->tex0[0] = gl.tex[0][0]; v->tex0[1] = gl.tex[0][1]; v->tex1[0] = gl.tex[1][0]; v->tex1[1] = gl.tex[1][1];
}
IMP(opengl32, glEnd) {
  gl.in_begin = 0; if (!gl.imm_count) return;
  u32 *ix = malloc(4 * gl.imm_count); for (u32 i = 0; i < gl.imm_count; i++) ix[i] = i;
  draw(gl.begin_mode, gl.imm, gl.imm_count, ix, gl.imm_count, 0); free(ix);
}
IMP(opengl32, glVertex2f) { vertex4(argf(c, 0), argf(c, 1), 0, 1); }
IMP(opengl32, glVertex2i) { vertex4((float)(s32)ARG(0), (float)(s32)ARG(1), 0, 1); }
IMP(opengl32, glVertex2s) { vertex4((float)(s16)ARG(0), (float)(s16)ARG(1), 0, 1); }
IMP(opengl32, glVertex2d) { vertex4((float)argd(c, 0), (float)argd(c, 2), 0, 1); }
IMP(opengl32, glVertex2fv) { u64 p = ARG(0); vertex4(f32_of(M32(p)), f32_of(M32(p + 4)), 0, 1); }
IMP(opengl32, glVertex2iv) { u64 p = ARG(0); vertex4((float)(s32)M32(p), (float)(s32)M32(p + 4), 0, 1); }
IMP(opengl32, glVertex3f) { vertex4(argf(c, 0), argf(c, 1), argf(c, 2), 1); }
IMP(opengl32, glVertex3i) { vertex4((float)(s32)ARG(0), (float)(s32)ARG(1), (float)(s32)ARG(2), 1); }
IMP(opengl32, glVertex3d) { vertex4((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4), 1); }
IMP(opengl32, glVertex3fv) { u64 p = ARG(0); vertex4(f32_of(M32(p)), f32_of(M32(p + 4)), f32_of(M32(p + 8)), 1); }
IMP(opengl32, glVertex3dv) { u64 p = ARG(0); double d[3]; memcpy(d, (void *)GP(p), 24); vertex4((float)d[0], (float)d[1], (float)d[2], 1); }
IMP(opengl32, glVertex4f) { vertex4(argf(c, 0), argf(c, 1), argf(c, 2), argf(c, 3)); }
IMP(opengl32, glVertex4fv) { u64 p = ARG(0); vertex4(f32_of(M32(p)), f32_of(M32(p + 4)), f32_of(M32(p + 8)), f32_of(M32(p + 12))); }
static void color4(float r, float g, float b, float a) { gl.color[0] = r; gl.color[1] = g; gl.color[2] = b; gl.color[3] = a; }
IMP(opengl32, glColor3f) { color4(argf(c, 0), argf(c, 1), argf(c, 2), 1); }
IMP(opengl32, glColor4f) { color4(argf(c, 0), argf(c, 1), argf(c, 2), argf(c, 3)); }
IMP(opengl32, glColor3fv) { u64 p = ARG(0); color4(f32_of(M32(p)), f32_of(M32(p + 4)), f32_of(M32(p + 8)), 1); }
IMP(opengl32, glColor4fv) { u64 p = ARG(0); color4(f32_of(M32(p)), f32_of(M32(p + 4)), f32_of(M32(p + 8)), f32_of(M32(p + 12))); }
IMP(opengl32, glColor3ub) { color4((u8)ARG(0) / 255.0f, (u8)ARG(1) / 255.0f, (u8)ARG(2) / 255.0f, 1); }
IMP(opengl32, glColor4ub) { color4((u8)ARG(0) / 255.0f, (u8)ARG(1) / 255.0f, (u8)ARG(2) / 255.0f, (u8)ARG(3) / 255.0f); }
IMP(opengl32, glColor3ubv) { u64 p = ARG(0); color4(M8(p) / 255.0f, M8(p + 1) / 255.0f, M8(p + 2) / 255.0f, 1); }
IMP(opengl32, glColor4ubv) { u64 p = ARG(0); color4(M8(p) / 255.0f, M8(p + 1) / 255.0f, M8(p + 2) / 255.0f, M8(p + 3) / 255.0f); }
IMP(opengl32, glColor3d) { color4((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4), 1); }
IMP(opengl32, glColor4d) { color4((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4), (float)argd(c, 6)); }
static void texcoord(int unit, float s, float t) { gl.tex[unit][0] = s; gl.tex[unit][1] = t; }
IMP(opengl32, glTexCoord2f) { texcoord(0, argf(c, 0), argf(c, 1)); }
IMP(opengl32, glTexCoord2d) { texcoord(0, (float)argd(c, 0), (float)argd(c, 2)); }
IMP(opengl32, glTexCoord2fv) { u64 p = ARG(0); texcoord(0, f32_of(M32(p)), f32_of(M32(p + 4))); }
IMP(opengl32, glTexCoord2i) { texcoord(0, (float)(s32)ARG(0), (float)(s32)ARG(1)); }
IMP(opengl32, glMultiTexCoord2fARB) { u32 t = (u32)ARG(0) - GL_TEXTURE0; if (t < GL_FFP_UNITS) texcoord((int)t, argf(c, 1), argf(c, 2)); }
IMP(opengl32, glMultiTexCoord2fvARB) { u32 t = (u32)ARG(0) - GL_TEXTURE0; u64 p = ARG(1); if (t < GL_FFP_UNITS) texcoord((int)t, f32_of(M32(p)), f32_of(M32(p + 4))); }
IMP(opengl32, glNormal3f) { gl.normal[0] = argf(c, 0); gl.normal[1] = argf(c, 1); gl.normal[2] = argf(c, 2); }
IMP(opengl32, glNormal3fv) { u64 p = ARG(0); gl.normal[0] = f32_of(M32(p)); gl.normal[1] = f32_of(M32(p + 4)); gl.normal[2] = f32_of(M32(p + 8)); }

// ---- arrays ----
static void set_array(Array *a, u32 size, u32 type, u32 stride, u32 ptr) { a->size = size; a->type = type; a->stride = stride; a->ptr = ptr; }
IMP(opengl32, glVertexPointer) { set_array(&gl.vertex, (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3)); }
IMP(opengl32, glColorPointer) { set_array(&gl.color_array, (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3)); }
IMP(opengl32, glNormalPointer) { set_array(&gl.normal_array, 3, (u32)ARG(0), (u32)ARG(1), (u32)ARG(2)); }
IMP(opengl32, glTexCoordPointer) { set_array(&gl.tex_array[gl.client_active], (u32)ARG(0), (u32)ARG(1), (u32)ARG(2), (u32)ARG(3)); }
static Array *client_array(u32 cap) {
  switch (cap) { case GL_VERTEX_ARRAY: return &gl.vertex; case GL_COLOR_ARRAY: return &gl.color_array; case GL_NORMAL_ARRAY: return &gl.normal_array;
                 case GL_TEXTURE_COORD_ARRAY: return &gl.tex_array[gl.client_active]; default: return 0; }
}
IMP(opengl32, glEnableClientState) { Array *a = client_array((u32)ARG(0)); if (a) a->on = 1; else UNSURE_ONCE("OpenGL: client state 0x%x", (u32)ARG(0)); }
IMP(opengl32, glDisableClientState) { Array *a = client_array((u32)ARG(0)); if (a) a->on = 0; }
IMP(opengl32, glClientActiveTextureARB) { u32 t = (u32)ARG(0) - GL_TEXTURE0; if (t < GL_FFP_UNITS) gl.client_active = t; else UNSURE_ONCE("OpenGL: texture unit %u", t); }
IMP(opengl32, glActiveTextureARB) { u32 t = (u32)ARG(0) - GL_TEXTURE0; if (t < GL_FFP_UNITS) gl.active = t; else UNSURE_ONCE("OpenGL: texture unit %u", t); }

// ---- enables ----
static int *flag(u32 cap) {
  switch (cap) {
  case GL_TEXTURE_2D: return &gl.tex2d[gl.active]; case GL_BLEND: return &gl.blend; case GL_DEPTH_TEST: return &gl.depth_test; case GL_CULL_FACE: return &gl.cull;
  case GL_ALPHA_TEST: return &gl.alpha_test; case GL_FOG: return &gl.fog; case GL_SCISSOR_TEST: return &gl.scissor; case GL_POLYGON_OFFSET_FILL: return &gl.offset_fill;
  case GL_CLIP_PLANE0: return &gl.clip0; case GL_STENCIL_TEST: return &gl.stencil; case GL_LIGHTING: return &gl.lighting;
  case GL_TEXTURE_GEN_S: case GL_TEXTURE_GEN_T: return &gl.texgen;
  default: return 0;
  }
}
static void enable(u32 cap, int on) {
  int *f = flag(cap);
  if (f) { *f = on; return; }
  // dithering, smooth lines and points, normalizing, colour material, lights...: of no consequence while lighting is off
  if (cap == 0x0BD0 || cap == 0x0B20 || cap == 0x0B10 || cap == 0x0BA1 || cap == 0x0B57 || (cap >= 0x4000 && cap < 0x4008) || cap == 0x0DE0 || cap == 0x8037 + 1 || cap == 0x2A02 || cap == 0x2A01) return;
  if (on) UNSURE_ONCE("OpenGL: glEnable 0x%x", cap);
}
IMP(opengl32, glEnable) { enable((u32)ARG(0), 1); }
IMP(opengl32, glDisable) { enable((u32)ARG(0), 0); }
IMP(opengl32, glIsEnabled) { int *f = flag((u32)ARG(0)); Array *a = client_array((u32)ARG(0)); RET(f ? *f != 0 : a ? a->on != 0 : 0); }

// ---- per-fragment state ----
IMP(opengl32, glBlendFunc) { gl.src = (u32)ARG(0); gl.dst = (u32)ARG(1); }
IMP(opengl32, glDepthFunc) { gl.depth_func = (u32)ARG(0); }
IMP(opengl32, glDepthMask) { gl.depth_mask = (u8)ARG(0) != 0; }
IMP(opengl32, glColorMask) { gl.color_mask = (u8)(((u8)ARG(0) ? 1 : 0) | ((u8)ARG(1) ? 2 : 0) | ((u8)ARG(2) ? 4 : 0) | ((u8)ARG(3) ? 8 : 0)); }
IMP(opengl32, glAlphaFunc) { gl.alpha_func = (u32)ARG(0); gl.alpha_ref = argf(c, 1); }
IMP(opengl32, glCullFace) { gl.cull_face = (u32)ARG(0); }
IMP(opengl32, glFrontFace) { gl.front_face = (u32)ARG(0); }
IMP(opengl32, glPolygonOffset) { gl.offset_factor = argf(c, 0); gl.offset_units = argf(c, 1); }
IMP(opengl32, glDepthRange) { gl.depth_near = argd(c, 0); gl.depth_far = argd(c, 2); }
IMP(opengl32, glViewport) { gl.vp[0] = (s32)ARG(0); gl.vp[1] = (s32)ARG(1); gl.vp[2] = (s32)ARG(2); gl.vp[3] = (s32)ARG(3); }
IMP(opengl32, glScissor) { gl.sc[0] = (s32)ARG(0); gl.sc[1] = (s32)ARG(1); gl.sc[2] = (s32)ARG(2); gl.sc[3] = (s32)ARG(3); }
IMP(opengl32, glShadeModel) { if ((u32)ARG(0) != 0x1D01) UNSURE_ONCE("OpenGL: flat shading (the colours are interpolated)"); }
IMP(opengl32, glPolygonMode) { if ((u32)ARG(1) != 0x1B02) UNSURE_ONCE("OpenGL: polygon mode 0x%x (drawn filled)", (u32)ARG(1)); }
IMP(opengl32, glLineWidth) {}
IMP(opengl32, glPointSize) {}
IMP(opengl32, glHint) {}
IMP(opengl32, glDrawBuffer) {}   // the back buffer and the front one are the same image here: what is drawn is presented
IMP(opengl32, glReadBuffer) {}
IMP(opengl32, glStencilFunc) {}
IMP(opengl32, glStencilOp) {}
IMP(opengl32, glStencilMask) {}
IMP(opengl32, glClearStencil) { gl.clear_stencil = (u32)ARG(0); }
static void fog_param(u32 p, const float *v) {
  switch (p) {
  case 0x0B65: gl.fog_mode = (u32)v[0]; break;
  case 0x0B62: gl.fog_density = v[0]; break;
  case 0x0B63: gl.fog_start = v[0]; break;
  case 0x0B64: gl.fog_end = v[0]; break;
  case 0x0B66: memcpy(gl.fog_color, v, 16); break;
  case 0x855A: break;   // GL_FOG_DISTANCE_MODE_NV
  default: UNSURE_ONCE("OpenGL: fog parameter 0x%x", p);
  }
}
IMP(opengl32, glFogf) { float v = argf(c, 1); fog_param((u32)ARG(0), &v); }
IMP(opengl32, glFogi) { float v = (float)(s32)ARG(1); fog_param((u32)ARG(0), &v); }
IMP(opengl32, glFogfv) { float v[4]; memcpy(v, (void *)GP(ARG(1)), (u32)ARG(0) == 0x0B66 ? 16 : 4); fog_param((u32)ARG(0), v); }
IMP(opengl32, glFogiv) { float v[4] = {(float)(s32)M32(ARG(1))}; if ((u32)ARG(0) == 0x0B66) for (int i = 0; i < 4; i++) v[i] = (float)((double)(s32)M32(ARG(1) + 4 * i) / 2147483647.0); fog_param((u32)ARG(0), v); }
IMP(opengl32, glClipPlane) { // (plane, double *equation): kept in eye coordinates, through the inverse of the modelview matrix
  if ((u32)ARG(0) != GL_CLIP_PLANE0) { UNSURE_ONCE("OpenGL: clip plane %u", (u32)ARG(0) - GL_CLIP_PLANE0); return; }
  double e[4], inv[16]; memcpy(e, (void *)GP(ARG(1)), 32);
  if (!inverse(inv, top(&gl.modelview))) return;
  for (int j = 0; j < 4; j++) gl.clip_plane[j] = e[0] * inv[4 * j] + e[1] * inv[4 * j + 1] + e[2] * inv[4 * j + 2] + e[3] * inv[4 * j + 3];
}

// ---- matrices ----
IMP(opengl32, glMatrixMode) { gl.matrix_mode = (u32)ARG(0); }
IMP(opengl32, glLoadIdentity) { identity(top(stack())); }
IMP(opengl32, glLoadMatrixf) { memcpy(top(stack()), (void *)GP(ARG(0)), 64); }
IMP(opengl32, glLoadMatrixd) { double d[16]; memcpy(d, (void *)GP(ARG(0)), 128); float *t = top(stack()); for (int i = 0; i < 16; i++) t[i] = (float)d[i]; }
IMP(opengl32, glMultMatrixf) { float m[16]; memcpy(m, (void *)GP(ARG(0)), 64); mult(m); }
IMP(opengl32, glMultMatrixd) { double d[16]; float m[16]; memcpy(d, (void *)GP(ARG(0)), 128); for (int i = 0; i < 16; i++) m[i] = (float)d[i]; mult(m); }
IMP(opengl32, glPushMatrix) { Stack *s = stack(); if (s->depth == 31) { gl.error = 0x503; return; } memcpy(s->m[s->depth + 1], s->m[s->depth], 64); s->depth++; }
IMP(opengl32, glPopMatrix) { Stack *s = stack(); if (!s->depth) { gl.error = 0x504; return; } s->depth--; }
static void ortho(double l, double r, double b, double t, double n, double f) {
  float m[16] = {0}; m[0] = (float)(2 / (r - l)); m[5] = (float)(2 / (t - b)); m[10] = (float)(-2 / (f - n));
  m[12] = (float)(-(r + l) / (r - l)); m[13] = (float)(-(t + b) / (t - b)); m[14] = (float)(-(f + n) / (f - n)); m[15] = 1; mult(m);
}
IMP(opengl32, glOrtho) { ortho(argd(c, 0), argd(c, 2), argd(c, 4), argd(c, 6), argd(c, 8), argd(c, 10)); }
IMP(opengl32, glFrustum) {
  double l = argd(c, 0), r = argd(c, 2), b = argd(c, 4), t = argd(c, 6), n = argd(c, 8), f = argd(c, 10);
  float m[16] = {0}; m[0] = (float)(2 * n / (r - l)); m[5] = (float)(2 * n / (t - b)); m[8] = (float)((r + l) / (r - l)); m[9] = (float)((t + b) / (t - b));
  m[10] = (float)(-(f + n) / (f - n)); m[11] = -1; m[14] = (float)(-2 * f * n / (f - n)); mult(m);
}
static void translate(float x, float y, float z) { float m[16]; identity(m); m[12] = x; m[13] = y; m[14] = z; mult(m); }
static void scale(float x, float y, float z) { float m[16]; identity(m); m[0] = x; m[5] = y; m[10] = z; mult(m); }
static void rotate(float a, float x, float y, float z) {
  float l = sqrtf(x * x + y * y + z * z); if (l == 0) return; x /= l; y /= l; z /= l;
  float r = a * 3.14159265358979f / 180.0f, cs = cosf(r), sn = sinf(r), t = 1 - cs;
  float m[16] = {x * x * t + cs, y * x * t + z * sn, x * z * t - y * sn, 0, x * y * t - z * sn, y * y * t + cs, y * z * t + x * sn, 0,
                 x * z * t + y * sn, y * z * t - x * sn, z * z * t + cs, 0, 0, 0, 0, 1};
  mult(m);
}
IMP(opengl32, glTranslatef) { translate(argf(c, 0), argf(c, 1), argf(c, 2)); }
IMP(opengl32, glTranslated) { translate((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4)); }
IMP(opengl32, glScalef) { scale(argf(c, 0), argf(c, 1), argf(c, 2)); }
IMP(opengl32, glScaled) { scale((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4)); }
IMP(opengl32, glRotatef) { rotate(argf(c, 0), argf(c, 1), argf(c, 2), argf(c, 3)); }
IMP(opengl32, glRotated) { rotate((float)argd(c, 0), (float)argd(c, 2), (float)argd(c, 4), (float)argd(c, 6)); }

// ---- textures ----
IMP(opengl32, glGenTextures) { // names past every one in use
  static u32 next = 1; u32 n = (u32)ARG(0); u64 out = ARG(1);
  for (u32 i = 0; i < n; i++) { while (tex_of(next, 0)) next++; tex_of(next, 1); M32(out + 4ull * i) = next++; }
}
IMP(opengl32, glDeleteTextures) {
  u32 n = (u32)ARG(0); u64 p = ARG(1);
  for (u32 i = 0; i < n; i++) { u32 name = M32(p + 4ull * i); Tex *t = tex_of(name, 0); if (!t) continue; destroy(t->gpu); free(t); texs[name] = 0;
    for (int u = 0; u < GL_FFP_UNITS; u++) if (gl.bound[u] == name) gl.bound[u] = 0; }
}
IMP(opengl32, glBindTexture) { if ((u32)ARG(0) != GL_TEXTURE_2D) { UNSURE_ONCE("OpenGL: texture target 0x%x", (u32)ARG(0)); return; } gl.bound[gl.active] = (u32)ARG(1); if (ARG(1)) tex_of((u32)ARG(1), 1); }
IMP(opengl32, glIsTexture) { RET(ARG(0) && tex_of((u32)ARG(0), 0) != 0); }
IMP(opengl32, glPrioritizeTextures) {}
IMP(opengl32, glAreTexturesResident) { u32 n = (u32)ARG(0); for (u32 i = 0; i < n; i++) M8(ARG(2) + i) = 1; RET(1); }
static void tex_param(u32 p, float v) {
  Tex *t = tex_of(gl.bound[gl.active], 1); if (!t) return;
  switch (p) {
  case 0x2801: t->min = (u32)v; break; case 0x2800: t->mag = (u32)v; break;
  case 0x2802: t->wrap_s = (u32)v; break; case 0x2803: t->wrap_t = (u32)v; break;
  case 0x84FE: case 0x813C: case 0x813D: case 0x8191: break;   // anisotropy, base and greatest level, generate mipmap: not used
  default: UNSURE_ONCE("OpenGL: texture parameter 0x%x", p);
  }
}
IMP(opengl32, glTexParameterf) { tex_param((u32)ARG(1), argf(c, 2)); }
IMP(opengl32, glTexParameteri) { tex_param((u32)ARG(1), (float)(s32)ARG(2)); }
IMP(opengl32, glTexParameterfv) { tex_param((u32)ARG(1), f32_of(M32(ARG(2)))); }
IMP(opengl32, glTexParameteriv) { tex_param((u32)ARG(1), (float)(s32)M32(ARG(2))); }
static void tex_env(u32 p, const float *v) {
  if (p == 0x2200) gl.env[gl.active] = (u32)v[0];
  else if (p == 0x2201) memcpy(gl.env_color[gl.active], v, 16);
  else UNSURE_ONCE("OpenGL: texture environment parameter 0x%x", p);
}
IMP(opengl32, glTexEnvf) { float v[4] = {argf(c, 2)}; tex_env((u32)ARG(1), v); }
IMP(opengl32, glTexEnvi) { float v[4] = {(float)(s32)ARG(2)}; tex_env((u32)ARG(1), v); }
IMP(opengl32, glTexEnvfv) { float v[4]; memcpy(v, (void *)GP(ARG(2)), (u32)ARG(1) == 0x2201 ? 16 : 4); tex_env((u32)ARG(1), v); }
IMP(opengl32, glTexEnviv) { float v[4] = {(float)(s32)M32(ARG(2))}; tex_env((u32)ARG(1), v); }
IMP(opengl32, glPixelStorei) {
  switch ((u32)ARG(0)) { case 0x0CF5: gl.unpack_alignment = (u32)ARG(1); break; case 0x0D05: gl.pack_alignment = (u32)ARG(1); break; case 0x0CF2: gl.unpack_row_length = (u32)ARG(1); break;
                         default: if (ARG(1)) UNSURE_ONCE("OpenGL: pixel store 0x%x = %u", (u32)ARG(0), (u32)ARG(1)); }
}
IMP(opengl32, glPixelStoref) { u32 v = (u32)argf(c, 1); if ((u32)ARG(0) == 0x0CF5) gl.unpack_alignment = v; else if ((u32)ARG(0) == 0x0D05) gl.pack_alignment = v; }
// the base format an internal format names
static int base_of(u32 internal) {
  switch (internal) {
  case 3: case GL_RGB: case 0x804F: case 0x8050: case 0x8051: case 0x8052: case 0x8053: case 0x8054: case 0x8055: case 0x2A10: case GL_LUMINANCE: case 1:
  case 0x803F: case 0x8040: case 0x8041: case 0x8042: case 0x83F0: case 0x84ED: return GL_BASE_RGB;   // RGB, R3_G3_B2, RGB4..16, LUMINANCE4..16, S3TC DXT1, COMPRESSED_RGB
  case 4: case GL_RGBA: case 0x8055 + 1: case 0x8057: case 0x8058: case 0x8059: case 0x805A: case 0x805B: case 2: case GL_LUMINANCE_ALPHA:
  case 0x8043: case 0x8044: case 0x8045: case 0x8046: case 0x8047: case 0x8048: case 0x8049: case 0x804A: case 0x804B: case 0x804C: case 0x804D: case 0x804E:
  case 0x83F1: case 0x83F2: case 0x83F3: case 0x84EE: case GL_BGRA: return GL_BASE_RGBA;   // RGBA2..16, LUMINANCE_ALPHA*, INTENSITY*, S3TC, COMPRESSED_RGBA
  case GL_ALPHA: case 0x803B: case 0x803C: case 0x803D: case 0x803E: return GL_BASE_ALPHA;
  default: return -1;
  }
}
// `w` x `h` pixels in format / type at `p` (with the unpack alignment) as RGBA8: what each base format keeps
static u8 *to_rgba(u32 format, u32 type, u64 p, u32 w, u32 h, int base) {
  if (type != GL_UNSIGNED_BYTE) { UNSURE_ONCE("OpenGL: pixels of type 0x%x", type); return 0; }
  u32 n = format == GL_RGBA || format == GL_BGRA ? 4 : format == GL_RGB || format == GL_BGR ? 3 : format == GL_LUMINANCE_ALPHA ? 2 : 1;
  u32 row = (gl.unpack_row_length ? gl.unpack_row_length : w) * n, a = gl.unpack_alignment ? gl.unpack_alignment : 1; row = (row + a - 1) / a * a;
  u8 *o = malloc((size_t)w * h * 4 + 4);
  for (u32 y = 0; y < h; y++) {
    const u8 *s = (const u8 *)GP(p + (u64)y * row); u8 *d = o + (size_t)y * w * 4;
    for (u32 x = 0; x < w; x++, s += n, d += 4) {
      switch (format) {
      case GL_RGBA: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; break;
      case GL_BGRA: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; break;
      case GL_RGB: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
      case GL_BGR: d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; break;
      case GL_LUMINANCE: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
      case GL_LUMINANCE_ALPHA: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
      case GL_ALPHA: d[0] = d[1] = d[2] = 0; d[3] = s[0]; break;
      default: UNSURE_ONCE("OpenGL: pixels of format 0x%x", format); free(o); return 0;
      }
      if (base == GL_BASE_RGB) d[3] = 255;
    }
  }
  return o;
}
static u32 levels_of(u32 w, u32 h) { u32 m = w > h ? w : h, n = 1; while (m > 1) { m >>= 1; n++; } return n; }
IMP(opengl32, glTexImage2D) { // (target, level, internal format, width, height, border, format, type, pixels)
  u32 level = (u32)ARG(1), w = (u32)ARG(3), h = (u32)ARG(4);
  int base = base_of((u32)ARG(2));
  if ((u32)ARG(0) != GL_TEXTURE_2D) { UNSURE_ONCE("OpenGL: glTexImage2D of target 0x%x", (u32)ARG(0)); return; }
  if (base < 0) { UNSURE_ONCE("OpenGL: internal format 0x%x", (u32)ARG(2)); return; }
  if (ARG(5)) UNSURE_ONCE("OpenGL: a texture with a border");
  Tex *t = tex_of(gl.bound[gl.active], 1); if (!t || !w || !h) return;
  if (level == 0 && (!t->gpu || t->w != w || t->h != h || t->base != (u8)base)) {
    destroy(t->gpu); t->w = w; t->h = h; t->levels = levels_of(w, h); t->base = (u8)base; t->defined = 0;
    t->gpu = resource(2, DXGI_RGBA8, w, h, t->levels, 0);
  }
  if (!t->gpu || level >= t->levels || (w != (t->w >> level ? t->w >> level : 1)) || (h != (t->h >> level ? t->h >> level : 1))) { UNSURE_ONCE("OpenGL: level %u of %ux%u does not fit a %ux%u texture", level, w, h, t->w, t->h); return; }
  t->defined |= 1u << level;
  if (gl_stats > 1 && level == 0) rt_log("OpenGL: glTexImage2D %u: %ux%u internal 0x%x format 0x%x", gl.bound[gl.active], w, h, (u32)ARG(2), (u32)ARG(6));
  if (!ARG(8)) return;
  u8 *rgba = to_rgba((u32)ARG(6), (u32)ARG(7), ARG(8), w, h, base);
  if (rgba) { write_texture(t->gpu, level, 0, 0, w, h, rgba); stat.texture_bytes += 4ull * w * h; free(rgba); }
}
IMP(opengl32, glTexSubImage2D) { // (target, level, x, y, width, height, format, type, pixels)
  Tex *t = tex_of(gl.bound[gl.active], 0); u32 level = (u32)ARG(1), x = (u32)ARG(2), y = (u32)ARG(3), w = (u32)ARG(4), h = (u32)ARG(5);
  if (!t || !t->gpu || level >= t->levels || !w || !h) return;
  u8 *rgba = to_rgba((u32)ARG(6), (u32)ARG(7), ARG(8), w, h, t->base);
  if (rgba) { write_texture(t->gpu, level, x, y, w, h, rgba); stat.texture_bytes += 4ull * w * h; free(rgba); }
}
IMP(opengl32, glCopyTexSubImage2D) { UNSURE_ONCE("OpenGL: glCopyTexSubImage2D is not done"); }
IMP(opengl32, glCopyTexImage2D) { UNSURE_ONCE("OpenGL: glCopyTexImage2D is not done"); }

// ---- clears ----
IMP(opengl32, glClearColor) { gl.clear_color[0] = argf(c, 0); gl.clear_color[1] = argf(c, 1); gl.clear_color[2] = argf(c, 2); gl.clear_color[3] = argf(c, 3); }
IMP(opengl32, glClearDepth) { gl.clear_depth = argd(c, 0); }
// A rectangle drawn in window coordinates (OpenGL's: y from the bottom) with this file's own pipeline
static void own_quad(Own *o, float x0, float y0, float x1, float y1, float z, const float *color) {
  GlVertex v[4]; float W = (float)gl.width, H = (float)gl.height;
  float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1}, ss[4] = {0, 1, 1, 0}, ts[4] = {0, 0, 1, 1};
  for (int i = 0; i < 4; i++) {
    v[i].pos[0] = 2 * xs[i] / W - 1; v[i].pos[1] = 2 * ys[i] / H - 1; v[i].pos[2] = 2 * z - 1; v[i].pos[3] = 1;
    for (int k = 0; k < 4; k++) v[i].color[k] = to_byte(color[k]);
    v[i].tex0[0] = ss[i]; v[i].tex0[1] = ts[i]; v[i].tex1[0] = v[i].tex1[1] = 0;
  }
  identity(o->u.mvp); identity(o->u.modelview); identity(o->u.tex[0]); identity(o->u.tex[1]);
  s32 full[4] = {0, 0, (s32)gl.width, (s32)gl.height};
  set_view_rect(full, gl.scissor ? gl.sc : full, 0, 1);
  u32 ix[4] = {0, 1, 2, 3};
  draw(GL_QUADS, v, 4, ix, 4, o);
}
IMP(opengl32, glClear) {
  u32 mask = (u32)ARG(0);
  if (!gl.current) return;
  int whole = !gl.scissor || (gl.sc[0] <= 0 && gl.sc[1] <= 0 && gl.sc[0] + gl.sc[2] >= (s32)gl.width && gl.sc[1] + gl.sc[3] >= (s32)gl.height);
  int masked = ((mask & GL_COLOR_BUFFER_BIT) && gl.color_mask != 15) || ((mask & GL_DEPTH_BUFFER_BIT) && !gl.depth_mask);
  if (whole && !masked) {
    begin_pass(((mask & GL_COLOR_BUFFER_BIT) ? GPU_PASS_CLEAR_COLOR : 0) | ((mask & GL_DEPTH_BUFFER_BIT) ? GPU_PASS_CLEAR_DEPTH : 0) | ((mask & GL_STENCIL_BUFFER_BIT) ? GPU_PASS_CLEAR_STENCIL : 0));
    return;
  }
  if (mask & GL_STENCIL_BUFFER_BIT) UNSURE_ONCE("OpenGL: a stencil clear cut by the scissor");
  // a part: a rectangle over the scissor's with the clear values, no test, writing what the mask says
  Own o; memset(&o, 0, sizeof o);
  static u32 fs; if (!fs) { GlFragmentKey k = {0}; fs = fragment_shader(&k); }
  o.pk.fs = fs; o.pk.topology = 4; o.pk.front = 1; o.pk.depth_compare = 8;
  o.pk.write_mask = (mask & GL_COLOR_BUFFER_BIT) ? gl.color_mask : 0; o.pk.depth_write = (mask & GL_DEPTH_BUFFER_BIT) && gl.depth_mask;
  if (!o.pk.write_mask && !o.pk.depth_write) return;
  float x0 = (float)gl.sc[0], y0 = (float)gl.sc[1];
  own_quad(&o, x0, y0, x0 + (float)gl.sc[2], y0 + (float)gl.sc[3], (float)gl.clear_depth, gl.clear_color);
  stat.clears_drawn++;
}

// ---- pixels: glRasterPos, glDrawPixels, glReadPixels ----
static void raster(float x, float y, float z, float w) {
  float p[4] = {x, y, z, w}, e[4], cl[4]; const float *mv = top(&gl.modelview), *pr = top(&gl.projection);
  for (int i = 0; i < 4; i++) e[i] = mv[i] * p[0] + mv[4 + i] * p[1] + mv[8 + i] * p[2] + mv[12 + i] * p[3];
  for (int i = 0; i < 4; i++) cl[i] = pr[i] * e[0] + pr[4 + i] * e[1] + pr[8 + i] * e[2] + pr[12 + i] * e[3];
  if (cl[3] == 0) { gl.raster_valid = 0; return; }
  gl.raster_valid = 1;
  gl.raster[0] = (float)gl.vp[0] + (cl[0] / cl[3] + 1) * 0.5f * (float)gl.vp[2];
  gl.raster[1] = (float)gl.vp[1] + (cl[1] / cl[3] + 1) * 0.5f * (float)gl.vp[3];
  gl.raster[2] = (float)(gl.depth_near + (cl[2] / cl[3] + 1) * 0.5 * (gl.depth_far - gl.depth_near));
}
IMP(opengl32, glRasterPos2i) { raster((float)(s32)ARG(0), (float)(s32)ARG(1), 0, 1); }
IMP(opengl32, glRasterPos2f) { raster(argf(c, 0), argf(c, 1), 0, 1); }
IMP(opengl32, glRasterPos2d) { raster((float)argd(c, 0), (float)argd(c, 2), 0, 1); }
IMP(opengl32, glRasterPos3f) { raster(argf(c, 0), argf(c, 1), argf(c, 2), 1); }
IMP(opengl32, glPixelZoom) { gl.zoom_x = argf(c, 0); gl.zoom_y = argf(c, 1); }
IMP(opengl32, glPixelTransferf) { if ((u32)ARG(0) != 0x0D1C && argf(c, 1) != ((u32)ARG(0) >= 0x0D14 && (u32)ARG(0) <= 0x0D1B && ((u32)ARG(0) & 1) == 0 ? 1.0f : 0.0f)) UNSURE_ONCE("OpenGL: pixel transfer 0x%x", (u32)ARG(0)); }
IMP(opengl32, glPixelTransferi) { if ((u32)ARG(0) <= 0x0D11 && ARG(1)) UNSURE_ONCE("OpenGL: pixel transfer 0x%x = %u", (u32)ARG(0), (u32)ARG(1)); }
// glDrawPixels: the pixels as a texture, drawn as a rectangle from the raster position, the zoom its size; through
// the per-fragment operations of the state (alpha test and blending off here: what apps draw a background with)
static u32 pix_tex, pix_w, pix_h;
IMP(opengl32, glDrawPixels) { // (width, height, format, type, pixels)
  u32 w = (u32)ARG(0), h = (u32)ARG(1);
  if (!gl.current || !w || !h || !gl.raster_valid) return;
  if (gl_stats > 1) rt_log("OpenGL: glDrawPixels %ux%u format 0x%x at %.1f,%.1f,%.3f zoom %.3f,%.3f (frame %u)", w, h, (u32)ARG(2), gl.raster[0], gl.raster[1], gl.raster[2], gl.zoom_x, gl.zoom_y, gl.frames);
  u8 *rgba = to_rgba((u32)ARG(2), (u32)ARG(3), ARG(4), w, h, GL_BASE_RGBA);
  if (!rgba) return;
  if (!pix_tex || pix_w != w || pix_h != h) { destroy(pix_tex); pix_tex = resource(2, DXGI_RGBA8, w, h, 1, 0); pix_w = w; pix_h = h; }
  write_texture(pix_tex, 0, 0, 0, w, h, rgba); free(rgba);
  Own o; memset(&o, 0, sizeof o);
  static u32 fs; if (!fs) { GlFragmentKey k = {{1, 0}, {GL_ENV_REPLACE, 0}, {GL_BASE_RGBA, 0}, 0, 0, 0, {0}}; fs = fragment_shader(&k); }
  Tex t = {0}; t.min = t.mag = 0x2600; t.wrap_s = t.wrap_t = 0x812F;
  o.texture = pix_tex; o.samp = sampler(&t);
  o.pk.fs = fs; o.pk.topology = 4; o.pk.front = 1; o.pk.units = 1; o.pk.write_mask = gl.color_mask;
  o.pk.depth_compare = gl.depth_test ? (u8)(gl.depth_func - GL_NEVER + 1) : 8; o.pk.depth_write = gl.depth_test && gl.depth_mask;
  if (gl.blend) { o.pk.blend = 1; o.pk.src = blend_factor(gl.src, 0); o.pk.dst = blend_factor(gl.dst, 0); o.pk.src_a = blend_factor(gl.src, 1); o.pk.dst_a = blend_factor(gl.dst, 1); }
  float white[4] = {1, 1, 1, 1};
  own_quad(&o, gl.raster[0], gl.raster[1], gl.raster[0] + (float)w * gl.zoom_x, gl.raster[1] + (float)h * gl.zoom_y, gl.raster[2], white);
}
// glReadPixels: the window's pixels (BGRA on the GPU, rows from the top) as OpenGL gives them (rows from the bottom)
IMP(opengl32, glReadPixels) { // (x, y, width, height, format, type, pixels)
  s32 x = (s32)ARG(0), y = (s32)ARG(1); u32 w = (u32)ARG(2), h = (u32)ARG(3), format = (u32)ARG(4); u64 out = ARG(6);
  if (!gl.current || (u32)ARG(5) != GL_UNSIGNED_BYTE || (format != GL_RGB && format != GL_RGBA && format != GL_BGR && format != GL_BGRA)) { UNSURE_ONCE("OpenGL: glReadPixels of format 0x%x type 0x%x", format, (u32)ARG(5)); return; }
  u8 *all = calloc((size_t)gl.width * gl.height, 4);
  u32 wds[7] = {gl.color_tex, 0, 0, gl.width, gl.height, 4 * gl.width, (u32)(uptr)all};
  rec(GPU_R_READ_TEXTURE, wds, 7); rt_gpu_drain();
  u32 n = format == GL_RGB || format == GL_BGR ? 3 : 4, a = gl.pack_alignment ? gl.pack_alignment : 1, row = (w * n + a - 1) / a * a;
  for (u32 j = 0; j < h; j++) for (u32 i = 0; i < w; i++) {
    s32 sx = x + (s32)i, sy = (s32)gl.height - 1 - (y + (s32)j);
    u8 px[4] = {0, 0, 0, 255};
    if (sx >= 0 && sy >= 0 && sx < (s32)gl.width && sy < (s32)gl.height) { const u8 *s = all + 4 * ((size_t)sy * gl.width + (size_t)sx); px[0] = s[2]; px[1] = s[1]; px[2] = s[0]; px[3] = s[3]; }
    u8 *d = (u8 *)GP(out + (u64)j * row + (u64)i * n);
    if (format == GL_BGR || format == GL_BGRA) { d[0] = px[2]; d[1] = px[1]; d[2] = px[0]; } else { d[0] = px[0]; d[1] = px[1]; d[2] = px[2]; }
    if (n == 4) d[3] = px[3];
  }
  free(all);
}

// ---- queries ----
IMP(opengl32, glGetError) { RET(gl.error); gl.error = 0; }
static const char extensions[] = "GL_ARB_multitexture GL_EXT_compiled_vertex_array GL_EXT_texture_env_add GL_ARB_texture_env_add GL_EXT_bgra WGL_EXT_swap_control";
IMP(opengl32, glGetString) {
  static u64 s[4];
  u32 n = (u32)ARG(0), k = n >= 0x1F00 && n <= 0x1F03 ? n - 0x1F00 : 4;
  if (k == 4) { gl.error = 0x500; RET(0); return; }
  if (!s[k]) { const char *t[4] = {"PISCO", "WebGPU (PISCO OpenGL 1.1)", "1.1.0 PISCO", extensions}; s[k] = (uptr)strdup(t[k]); }
  RET(s[k]);
}
static int get_ints(u32 p, s32 *v) {   // the number of values, or 0 for one not answered
  switch (p) {
  case 0x0D33: v[0] = 2048; return 1;                          // GL_MAX_TEXTURE_SIZE
  case 0x84E2: v[0] = GL_FFP_UNITS; return 1;                  // GL_MAX_TEXTURE_UNITS_ARB
  case 0x0BA2: memcpy(v, gl.vp, 16); return 4;                 // GL_VIEWPORT
  case 0x0C10: memcpy(v, gl.sc, 16); return 4;                 // GL_SCISSOR_BOX
  case 0x0D52: case 0x0D53: case 0x0D54: case 0x0D55: v[0] = 8; return 1;   // red / green / blue / alpha bits
  case 0x0D56: v[0] = 24; return 1; case 0x0D57: v[0] = 8; return 1;        // depth, stencil bits
  case 0x0D36: v[0] = 32; return 1; case 0x0D38: v[0] = 4; return 1;        // modelview / projection stack depth
  case 0x0D32: v[0] = 1; return 1;                             // GL_MAX_CLIP_PLANES (one is drawn)
  case 0x0D31: v[0] = 0; return 1;                             // GL_MAX_LIGHTS: none are lit
  case 0x84E1: v[0] = (s32)(GL_TEXTURE0 + gl.active); return 1; // GL_ACTIVE_TEXTURE_ARB
  case 0x0BA0: v[0] = (s32)gl.matrix_mode; return 1;
  default: return 0;
  }
}
IMP(opengl32, glGetIntegerv) {
  u32 p = (u32)ARG(0); s32 v[16];
  if (p == 0x0BA6 || p == 0x0BA7) { const float *m = top(p == 0x0BA6 ? &gl.modelview : &gl.projection); for (int i = 0; i < 16; i++) M32(ARG(1) + 4 * i) = (u32)(s32)m[i]; return; }
  int n = get_ints(p, v); if (!n) { UNSURE_ONCE("OpenGL: glGetIntegerv 0x%x", p); M32(ARG(1)) = 0; return; }
  for (int i = 0; i < n; i++) M32(ARG(1) + 4 * i) = (u32)v[i];
}
IMP(opengl32, glGetFloatv) {
  u32 p = (u32)ARG(0); s32 v[16];
  if (p == 0x0BA6 || p == 0x0BA7 || p == 0x0BA8) { const float *m = top(p == 0x0BA6 ? &gl.modelview : p == 0x0BA7 ? &gl.projection : &gl.texture[gl.active]); memcpy((void *)GP(ARG(1)), m, 64); return; }
  if (p == 0x0B21) { M32(ARG(1)) = bits_of(1.0f); return; }    // GL_LINE_WIDTH
  int n = get_ints(p, v); if (!n) { UNSURE_ONCE("OpenGL: glGetFloatv 0x%x", p); M32(ARG(1)) = 0; return; }
  for (int i = 0; i < n; i++) M32(ARG(1) + 4 * i) = bits_of((float)v[i]);
}
IMP(opengl32, glGetBooleanv) { s32 v[16]; int n = get_ints((u32)ARG(0), v); if (!n) { UNSURE_ONCE("OpenGL: glGetBooleanv 0x%x", (u32)ARG(0)); M8(ARG(1)) = 0; return; } for (int i = 0; i < n; i++) M8(ARG(1) + i) = v[i] != 0; }
IMP(opengl32, glGetDoublev) {
  u32 p = (u32)ARG(0);
  if (p == 0x0BA6 || p == 0x0BA7) { const float *m = top(p == 0x0BA6 ? &gl.modelview : &gl.projection); for (int i = 0; i < 16; i++) { double d = m[i]; memcpy((void *)GP(ARG(1) + 8 * i), &d, 8); } return; }
  UNSURE_ONCE("OpenGL: glGetDoublev 0x%x", p);
}
IMP(opengl32, glFinish) {}
IMP(opengl32, glFlush) {}

// ---- wgl: one context, made current with a window's DC; SwapBuffers presents ----
IMP(opengl32, wglCreateContext) { RET(ARG(0) ? 0x00010001u : 0); }
IMP(opengl32, wglDeleteContext) { if ((u32)ARG(0) == 0x00010001u) gl.current = 0; RET(1); }
IMP(opengl32, wglMakeCurrent) {
  if (!ARG(1)) { gl.current = 0; RET(1); return; }
  static int first = 1;
  if (first) { first = 0; reset_state(); const char *e = getenv("SW_GL_STATS"); gl_stats = e ? atoi(e) : 0; rt_gpu_open(); rt_session_shot_hook = gl_session_shot; }
  gl.hdc = (u32)ARG(0); gl.hwnd = gl.hdc & 0xFFFFFF; gl.current = 1;
  make_buffers();
  RET(1);
}
IMP(opengl32, wglGetCurrentContext) { RET(gl.current ? 0x00010001u : 0); }
IMP(opengl32, wglGetCurrentDC) { RET(gl.current ? gl.hdc : 0); }
IMP(opengl32, wglShareLists) { RET(1); }
IMP(opengl32, wglSwapIntervalEXT) { RET(1); }
IMP(opengl32, wglGetSwapIntervalEXT) { RET(1); }
IMP(opengl32, wglGetExtensionsStringARB) { static u64 s; if (!s) s = (uptr)strdup("WGL_EXT_swap_control"); RET(s); }
IMP(opengl32, wglGetExtensionsStringEXT) { static u64 s; if (!s) s = (uptr)strdup("WGL_EXT_swap_control"); RET(s); }
IMP(opengl32, wglGetProcAddress) { RET(rt_dynamic_import("opengl32", GSTR(ARG(0)))); }
// the end of a frame: what was drawn is presented; the clock and the session move on
void gl_swap_buffers(u32 hdc) {
  (void)hdc;
  if (!gl.current) return;
  if (!gl.pass_open) begin_pass(0);
  gl.frames++; stat.frames++;
  rt_gpu_present(gl.color_tex, gl.frames, 1);
  gl.pass_open = 0; view.sent = 0;
  if (gl_stats && gl.frames % 100 == 0) rt_log("OpenGL frame %u: %llu draws (%llu left out), %llu vertices, %u pipelines, %u fragment stages, %llu texture bytes, %llu clears drawn",
                                              gl.frames, stat.draws, stat.skipped, stat.vertices, pipeline_count, fragment_shader_count, stat.texture_bytes, stat.clears_drawn);
  rt_clock_advance_frame();
  session_frame(gl.frames);
}
IMP(opengl32, wglSwapBuffers) { gl_swap_buffers((u32)ARG(0)); RET(1); }
// a session's `shot` step (rt/input.c's hook, set when the context is first made current): the checkpoint line,
// and the frame just presented saved by the host
static void gl_session_shot(const char *name, u32 frame) {
  rt_log("checkpoint %s frame %u: %llu draws, %u pipelines", name, frame, stat.draws, pipeline_count);
  if (!rt_gpu_ring) return;
  char text[64]; int n = snprintf(text, sizeof text, "shot %s", name);
  u8 *p = rt_gpu_rec_begin(GPU_R_MARK, (u32)n); memcpy(p, text, (size_t)n); rt_gpu_rec_end();
}

// ---- the rest of OpenGL 1.1 and wgl: an app looks every function up; these say so once if called, and do nothing ----
// (each is the same: an app that calls one learns once, in the log, that it was not done)
static void stub(const char *name) { com_unsure("OpenGL: %s is not done", name); }
IMP(opengl32, glAccum) { static int t; if (!t++) stub("glAccum"); RET(0); }
IMP(opengl32, glBitmap) { static int t; if (!t++) stub("glBitmap"); RET(0); }
IMP(opengl32, glCallList) { static int t; if (!t++) stub("glCallList"); RET(0); }
IMP(opengl32, glCallLists) { static int t; if (!t++) stub("glCallLists"); RET(0); }
IMP(opengl32, glClearAccum) { static int t; if (!t++) stub("glClearAccum"); RET(0); }
IMP(opengl32, glClearIndex) { static int t; if (!t++) stub("glClearIndex"); RET(0); }
IMP(opengl32, glColor3b) { static int t; if (!t++) stub("glColor3b"); RET(0); }
IMP(opengl32, glColor3bv) { static int t; if (!t++) stub("glColor3bv"); RET(0); }
IMP(opengl32, glColor3dv) { static int t; if (!t++) stub("glColor3dv"); RET(0); }
IMP(opengl32, glColor3i) { static int t; if (!t++) stub("glColor3i"); RET(0); }
IMP(opengl32, glColor3iv) { static int t; if (!t++) stub("glColor3iv"); RET(0); }
IMP(opengl32, glColor3s) { static int t; if (!t++) stub("glColor3s"); RET(0); }
IMP(opengl32, glColor3sv) { static int t; if (!t++) stub("glColor3sv"); RET(0); }
IMP(opengl32, glColor3ui) { static int t; if (!t++) stub("glColor3ui"); RET(0); }
IMP(opengl32, glColor3uiv) { static int t; if (!t++) stub("glColor3uiv"); RET(0); }
IMP(opengl32, glColor3us) { static int t; if (!t++) stub("glColor3us"); RET(0); }
IMP(opengl32, glColor3usv) { static int t; if (!t++) stub("glColor3usv"); RET(0); }
IMP(opengl32, glColor4b) { static int t; if (!t++) stub("glColor4b"); RET(0); }
IMP(opengl32, glColor4bv) { static int t; if (!t++) stub("glColor4bv"); RET(0); }
IMP(opengl32, glColor4dv) { static int t; if (!t++) stub("glColor4dv"); RET(0); }
IMP(opengl32, glColor4i) { static int t; if (!t++) stub("glColor4i"); RET(0); }
IMP(opengl32, glColor4iv) { static int t; if (!t++) stub("glColor4iv"); RET(0); }
IMP(opengl32, glColor4s) { static int t; if (!t++) stub("glColor4s"); RET(0); }
IMP(opengl32, glColor4sv) { static int t; if (!t++) stub("glColor4sv"); RET(0); }
IMP(opengl32, glColor4ui) { static int t; if (!t++) stub("glColor4ui"); RET(0); }
IMP(opengl32, glColor4uiv) { static int t; if (!t++) stub("glColor4uiv"); RET(0); }
IMP(opengl32, glColor4us) { static int t; if (!t++) stub("glColor4us"); RET(0); }
IMP(opengl32, glColor4usv) { static int t; if (!t++) stub("glColor4usv"); RET(0); }
IMP(opengl32, glColorMaterial) { static int t; if (!t++) stub("glColorMaterial"); RET(0); }
IMP(opengl32, glCopyPixels) { static int t; if (!t++) stub("glCopyPixels"); RET(0); }
IMP(opengl32, glCopyTexImage1D) { static int t; if (!t++) stub("glCopyTexImage1D"); RET(0); }
IMP(opengl32, glCopyTexSubImage1D) { static int t; if (!t++) stub("glCopyTexSubImage1D"); RET(0); }
IMP(opengl32, glDeleteLists) { static int t; if (!t++) stub("glDeleteLists"); RET(0); }
IMP(opengl32, glEdgeFlag) { static int t; if (!t++) stub("glEdgeFlag"); RET(0); }
IMP(opengl32, glEdgeFlagPointer) { static int t; if (!t++) stub("glEdgeFlagPointer"); RET(0); }
IMP(opengl32, glEdgeFlagv) { static int t; if (!t++) stub("glEdgeFlagv"); RET(0); }
IMP(opengl32, glEndList) { static int t; if (!t++) stub("glEndList"); RET(0); }
IMP(opengl32, glEvalCoord1d) { static int t; if (!t++) stub("glEvalCoord1d"); RET(0); }
IMP(opengl32, glEvalCoord1dv) { static int t; if (!t++) stub("glEvalCoord1dv"); RET(0); }
IMP(opengl32, glEvalCoord1f) { static int t; if (!t++) stub("glEvalCoord1f"); RET(0); }
IMP(opengl32, glEvalCoord1fv) { static int t; if (!t++) stub("glEvalCoord1fv"); RET(0); }
IMP(opengl32, glEvalCoord2d) { static int t; if (!t++) stub("glEvalCoord2d"); RET(0); }
IMP(opengl32, glEvalCoord2dv) { static int t; if (!t++) stub("glEvalCoord2dv"); RET(0); }
IMP(opengl32, glEvalCoord2f) { static int t; if (!t++) stub("glEvalCoord2f"); RET(0); }
IMP(opengl32, glEvalCoord2fv) { static int t; if (!t++) stub("glEvalCoord2fv"); RET(0); }
IMP(opengl32, glEvalMesh1) { static int t; if (!t++) stub("glEvalMesh1"); RET(0); }
IMP(opengl32, glEvalMesh2) { static int t; if (!t++) stub("glEvalMesh2"); RET(0); }
IMP(opengl32, glEvalPoint1) { static int t; if (!t++) stub("glEvalPoint1"); RET(0); }
IMP(opengl32, glEvalPoint2) { static int t; if (!t++) stub("glEvalPoint2"); RET(0); }
IMP(opengl32, glFeedbackBuffer) { static int t; if (!t++) stub("glFeedbackBuffer"); RET(0); }
IMP(opengl32, glGenLists) { static int t; if (!t++) stub("glGenLists"); RET(0); }
IMP(opengl32, glGetClipPlane) { static int t; if (!t++) stub("glGetClipPlane"); RET(0); }
IMP(opengl32, glGetLightfv) { static int t; if (!t++) stub("glGetLightfv"); RET(0); }
IMP(opengl32, glGetLightiv) { static int t; if (!t++) stub("glGetLightiv"); RET(0); }
IMP(opengl32, glGetMapdv) { static int t; if (!t++) stub("glGetMapdv"); RET(0); }
IMP(opengl32, glGetMapfv) { static int t; if (!t++) stub("glGetMapfv"); RET(0); }
IMP(opengl32, glGetMapiv) { static int t; if (!t++) stub("glGetMapiv"); RET(0); }
IMP(opengl32, glGetMaterialfv) { static int t; if (!t++) stub("glGetMaterialfv"); RET(0); }
IMP(opengl32, glGetMaterialiv) { static int t; if (!t++) stub("glGetMaterialiv"); RET(0); }
IMP(opengl32, glGetPixelMapfv) { static int t; if (!t++) stub("glGetPixelMapfv"); RET(0); }
IMP(opengl32, glGetPixelMapuiv) { static int t; if (!t++) stub("glGetPixelMapuiv"); RET(0); }
IMP(opengl32, glGetPixelMapusv) { static int t; if (!t++) stub("glGetPixelMapusv"); RET(0); }
IMP(opengl32, glGetPointerv) { static int t; if (!t++) stub("glGetPointerv"); RET(0); }
IMP(opengl32, glGetPolygonStipple) { static int t; if (!t++) stub("glGetPolygonStipple"); RET(0); }
IMP(opengl32, glGetTexEnvfv) { static int t; if (!t++) stub("glGetTexEnvfv"); RET(0); }
IMP(opengl32, glGetTexEnviv) { static int t; if (!t++) stub("glGetTexEnviv"); RET(0); }
IMP(opengl32, glGetTexGendv) { static int t; if (!t++) stub("glGetTexGendv"); RET(0); }
IMP(opengl32, glGetTexGenfv) { static int t; if (!t++) stub("glGetTexGenfv"); RET(0); }
IMP(opengl32, glGetTexGeniv) { static int t; if (!t++) stub("glGetTexGeniv"); RET(0); }
IMP(opengl32, glGetTexImage) { static int t; if (!t++) stub("glGetTexImage"); RET(0); }
IMP(opengl32, glGetTexLevelParameterfv) { static int t; if (!t++) stub("glGetTexLevelParameterfv"); RET(0); }
IMP(opengl32, glGetTexLevelParameteriv) { static int t; if (!t++) stub("glGetTexLevelParameteriv"); RET(0); }
IMP(opengl32, glGetTexParameterfv) { static int t; if (!t++) stub("glGetTexParameterfv"); RET(0); }
IMP(opengl32, glGetTexParameteriv) { static int t; if (!t++) stub("glGetTexParameteriv"); RET(0); }
IMP(opengl32, glIndexMask) { static int t; if (!t++) stub("glIndexMask"); RET(0); }
IMP(opengl32, glIndexPointer) { static int t; if (!t++) stub("glIndexPointer"); RET(0); }
IMP(opengl32, glIndexd) { static int t; if (!t++) stub("glIndexd"); RET(0); }
IMP(opengl32, glIndexdv) { static int t; if (!t++) stub("glIndexdv"); RET(0); }
IMP(opengl32, glIndexf) { static int t; if (!t++) stub("glIndexf"); RET(0); }
IMP(opengl32, glIndexfv) { static int t; if (!t++) stub("glIndexfv"); RET(0); }
IMP(opengl32, glIndexi) { static int t; if (!t++) stub("glIndexi"); RET(0); }
IMP(opengl32, glIndexiv) { static int t; if (!t++) stub("glIndexiv"); RET(0); }
IMP(opengl32, glIndexs) { static int t; if (!t++) stub("glIndexs"); RET(0); }
IMP(opengl32, glIndexsv) { static int t; if (!t++) stub("glIndexsv"); RET(0); }
IMP(opengl32, glIndexub) { static int t; if (!t++) stub("glIndexub"); RET(0); }
IMP(opengl32, glIndexubv) { static int t; if (!t++) stub("glIndexubv"); RET(0); }
IMP(opengl32, glInitNames) { static int t; if (!t++) stub("glInitNames"); RET(0); }
IMP(opengl32, glInterleavedArrays) { static int t; if (!t++) stub("glInterleavedArrays"); RET(0); }
IMP(opengl32, glIsList) { static int t; if (!t++) stub("glIsList"); RET(0); }
IMP(opengl32, glLightModelf) { static int t; if (!t++) stub("glLightModelf"); RET(0); }
IMP(opengl32, glLightModelfv) { static int t; if (!t++) stub("glLightModelfv"); RET(0); }
IMP(opengl32, glLightModeli) { static int t; if (!t++) stub("glLightModeli"); RET(0); }
IMP(opengl32, glLightModeliv) { static int t; if (!t++) stub("glLightModeliv"); RET(0); }
IMP(opengl32, glLightf) { static int t; if (!t++) stub("glLightf"); RET(0); }
IMP(opengl32, glLightfv) { static int t; if (!t++) stub("glLightfv"); RET(0); }
IMP(opengl32, glLighti) { static int t; if (!t++) stub("glLighti"); RET(0); }
IMP(opengl32, glLightiv) { static int t; if (!t++) stub("glLightiv"); RET(0); }
IMP(opengl32, glLineStipple) { static int t; if (!t++) stub("glLineStipple"); RET(0); }
IMP(opengl32, glListBase) { static int t; if (!t++) stub("glListBase"); RET(0); }
IMP(opengl32, glLoadName) { static int t; if (!t++) stub("glLoadName"); RET(0); }
IMP(opengl32, glLogicOp) { static int t; if (!t++) stub("glLogicOp"); RET(0); }
IMP(opengl32, glMap1d) { static int t; if (!t++) stub("glMap1d"); RET(0); }
IMP(opengl32, glMap1f) { static int t; if (!t++) stub("glMap1f"); RET(0); }
IMP(opengl32, glMap2d) { static int t; if (!t++) stub("glMap2d"); RET(0); }
IMP(opengl32, glMap2f) { static int t; if (!t++) stub("glMap2f"); RET(0); }
IMP(opengl32, glMapGrid1d) { static int t; if (!t++) stub("glMapGrid1d"); RET(0); }
IMP(opengl32, glMapGrid1f) { static int t; if (!t++) stub("glMapGrid1f"); RET(0); }
IMP(opengl32, glMapGrid2d) { static int t; if (!t++) stub("glMapGrid2d"); RET(0); }
IMP(opengl32, glMapGrid2f) { static int t; if (!t++) stub("glMapGrid2f"); RET(0); }
IMP(opengl32, glMaterialf) { static int t; if (!t++) stub("glMaterialf"); RET(0); }
IMP(opengl32, glMaterialfv) { static int t; if (!t++) stub("glMaterialfv"); RET(0); }
IMP(opengl32, glMateriali) { static int t; if (!t++) stub("glMateriali"); RET(0); }
IMP(opengl32, glMaterialiv) { static int t; if (!t++) stub("glMaterialiv"); RET(0); }
IMP(opengl32, glNewList) { static int t; if (!t++) stub("glNewList"); RET(0); }
IMP(opengl32, glNormal3b) { static int t; if (!t++) stub("glNormal3b"); RET(0); }
IMP(opengl32, glNormal3bv) { static int t; if (!t++) stub("glNormal3bv"); RET(0); }
IMP(opengl32, glNormal3d) { static int t; if (!t++) stub("glNormal3d"); RET(0); }
IMP(opengl32, glNormal3dv) { static int t; if (!t++) stub("glNormal3dv"); RET(0); }
IMP(opengl32, glNormal3i) { static int t; if (!t++) stub("glNormal3i"); RET(0); }
IMP(opengl32, glNormal3iv) { static int t; if (!t++) stub("glNormal3iv"); RET(0); }
IMP(opengl32, glNormal3s) { static int t; if (!t++) stub("glNormal3s"); RET(0); }
IMP(opengl32, glNormal3sv) { static int t; if (!t++) stub("glNormal3sv"); RET(0); }
IMP(opengl32, glPassThrough) { static int t; if (!t++) stub("glPassThrough"); RET(0); }
IMP(opengl32, glPixelMapfv) { static int t; if (!t++) stub("glPixelMapfv"); RET(0); }
IMP(opengl32, glPixelMapuiv) { static int t; if (!t++) stub("glPixelMapuiv"); RET(0); }
IMP(opengl32, glPixelMapusv) { static int t; if (!t++) stub("glPixelMapusv"); RET(0); }
IMP(opengl32, glPolygonStipple) { static int t; if (!t++) stub("glPolygonStipple"); RET(0); }
IMP(opengl32, glPopAttrib) { static int t; if (!t++) stub("glPopAttrib"); RET(0); }
IMP(opengl32, glPopClientAttrib) { static int t; if (!t++) stub("glPopClientAttrib"); RET(0); }
IMP(opengl32, glPopName) { static int t; if (!t++) stub("glPopName"); RET(0); }
IMP(opengl32, glPushAttrib) { static int t; if (!t++) stub("glPushAttrib"); RET(0); }
IMP(opengl32, glPushClientAttrib) { static int t; if (!t++) stub("glPushClientAttrib"); RET(0); }
IMP(opengl32, glPushName) { static int t; if (!t++) stub("glPushName"); RET(0); }
IMP(opengl32, glRasterPos2dv) { static int t; if (!t++) stub("glRasterPos2dv"); RET(0); }
IMP(opengl32, glRasterPos2fv) { static int t; if (!t++) stub("glRasterPos2fv"); RET(0); }
IMP(opengl32, glRasterPos2iv) { static int t; if (!t++) stub("glRasterPos2iv"); RET(0); }
IMP(opengl32, glRasterPos2s) { static int t; if (!t++) stub("glRasterPos2s"); RET(0); }
IMP(opengl32, glRasterPos2sv) { static int t; if (!t++) stub("glRasterPos2sv"); RET(0); }
IMP(opengl32, glRasterPos3d) { static int t; if (!t++) stub("glRasterPos3d"); RET(0); }
IMP(opengl32, glRasterPos3dv) { static int t; if (!t++) stub("glRasterPos3dv"); RET(0); }
IMP(opengl32, glRasterPos3fv) { static int t; if (!t++) stub("glRasterPos3fv"); RET(0); }
IMP(opengl32, glRasterPos3i) { static int t; if (!t++) stub("glRasterPos3i"); RET(0); }
IMP(opengl32, glRasterPos3iv) { static int t; if (!t++) stub("glRasterPos3iv"); RET(0); }
IMP(opengl32, glRasterPos3s) { static int t; if (!t++) stub("glRasterPos3s"); RET(0); }
IMP(opengl32, glRasterPos3sv) { static int t; if (!t++) stub("glRasterPos3sv"); RET(0); }
IMP(opengl32, glRasterPos4d) { static int t; if (!t++) stub("glRasterPos4d"); RET(0); }
IMP(opengl32, glRasterPos4dv) { static int t; if (!t++) stub("glRasterPos4dv"); RET(0); }
IMP(opengl32, glRasterPos4f) { static int t; if (!t++) stub("glRasterPos4f"); RET(0); }
IMP(opengl32, glRasterPos4fv) { static int t; if (!t++) stub("glRasterPos4fv"); RET(0); }
IMP(opengl32, glRasterPos4i) { static int t; if (!t++) stub("glRasterPos4i"); RET(0); }
IMP(opengl32, glRasterPos4iv) { static int t; if (!t++) stub("glRasterPos4iv"); RET(0); }
IMP(opengl32, glRasterPos4s) { static int t; if (!t++) stub("glRasterPos4s"); RET(0); }
IMP(opengl32, glRasterPos4sv) { static int t; if (!t++) stub("glRasterPos4sv"); RET(0); }
IMP(opengl32, glRectd) { static int t; if (!t++) stub("glRectd"); RET(0); }
IMP(opengl32, glRectdv) { static int t; if (!t++) stub("glRectdv"); RET(0); }
IMP(opengl32, glRectf) { static int t; if (!t++) stub("glRectf"); RET(0); }
IMP(opengl32, glRectfv) { static int t; if (!t++) stub("glRectfv"); RET(0); }
IMP(opengl32, glRecti) { static int t; if (!t++) stub("glRecti"); RET(0); }
IMP(opengl32, glRectiv) { static int t; if (!t++) stub("glRectiv"); RET(0); }
IMP(opengl32, glRects) { static int t; if (!t++) stub("glRects"); RET(0); }
IMP(opengl32, glRectsv) { static int t; if (!t++) stub("glRectsv"); RET(0); }
IMP(opengl32, glRenderMode) { static int t; if (!t++) stub("glRenderMode"); RET(0); }
IMP(opengl32, glSelectBuffer) { static int t; if (!t++) stub("glSelectBuffer"); RET(0); }
IMP(opengl32, glTexCoord1d) { static int t; if (!t++) stub("glTexCoord1d"); RET(0); }
IMP(opengl32, glTexCoord1dv) { static int t; if (!t++) stub("glTexCoord1dv"); RET(0); }
IMP(opengl32, glTexCoord1f) { static int t; if (!t++) stub("glTexCoord1f"); RET(0); }
IMP(opengl32, glTexCoord1fv) { static int t; if (!t++) stub("glTexCoord1fv"); RET(0); }
IMP(opengl32, glTexCoord1i) { static int t; if (!t++) stub("glTexCoord1i"); RET(0); }
IMP(opengl32, glTexCoord1iv) { static int t; if (!t++) stub("glTexCoord1iv"); RET(0); }
IMP(opengl32, glTexCoord1s) { static int t; if (!t++) stub("glTexCoord1s"); RET(0); }
IMP(opengl32, glTexCoord1sv) { static int t; if (!t++) stub("glTexCoord1sv"); RET(0); }
IMP(opengl32, glTexCoord2dv) { static int t; if (!t++) stub("glTexCoord2dv"); RET(0); }
IMP(opengl32, glTexCoord2iv) { static int t; if (!t++) stub("glTexCoord2iv"); RET(0); }
IMP(opengl32, glTexCoord2s) { static int t; if (!t++) stub("glTexCoord2s"); RET(0); }
IMP(opengl32, glTexCoord2sv) { static int t; if (!t++) stub("glTexCoord2sv"); RET(0); }
IMP(opengl32, glTexCoord3d) { static int t; if (!t++) stub("glTexCoord3d"); RET(0); }
IMP(opengl32, glTexCoord3dv) { static int t; if (!t++) stub("glTexCoord3dv"); RET(0); }
IMP(opengl32, glTexCoord3f) { static int t; if (!t++) stub("glTexCoord3f"); RET(0); }
IMP(opengl32, glTexCoord3fv) { static int t; if (!t++) stub("glTexCoord3fv"); RET(0); }
IMP(opengl32, glTexCoord3i) { static int t; if (!t++) stub("glTexCoord3i"); RET(0); }
IMP(opengl32, glTexCoord3iv) { static int t; if (!t++) stub("glTexCoord3iv"); RET(0); }
IMP(opengl32, glTexCoord3s) { static int t; if (!t++) stub("glTexCoord3s"); RET(0); }
IMP(opengl32, glTexCoord3sv) { static int t; if (!t++) stub("glTexCoord3sv"); RET(0); }
IMP(opengl32, glTexCoord4d) { static int t; if (!t++) stub("glTexCoord4d"); RET(0); }
IMP(opengl32, glTexCoord4dv) { static int t; if (!t++) stub("glTexCoord4dv"); RET(0); }
IMP(opengl32, glTexCoord4f) { static int t; if (!t++) stub("glTexCoord4f"); RET(0); }
IMP(opengl32, glTexCoord4fv) { static int t; if (!t++) stub("glTexCoord4fv"); RET(0); }
IMP(opengl32, glTexCoord4i) { static int t; if (!t++) stub("glTexCoord4i"); RET(0); }
IMP(opengl32, glTexCoord4iv) { static int t; if (!t++) stub("glTexCoord4iv"); RET(0); }
IMP(opengl32, glTexCoord4s) { static int t; if (!t++) stub("glTexCoord4s"); RET(0); }
IMP(opengl32, glTexCoord4sv) { static int t; if (!t++) stub("glTexCoord4sv"); RET(0); }
IMP(opengl32, glTexGend) { static int t; if (!t++) stub("glTexGend"); RET(0); }
IMP(opengl32, glTexGendv) { static int t; if (!t++) stub("glTexGendv"); RET(0); }
IMP(opengl32, glTexGenf) { static int t; if (!t++) stub("glTexGenf"); RET(0); }
IMP(opengl32, glTexGenfv) { static int t; if (!t++) stub("glTexGenfv"); RET(0); }
IMP(opengl32, glTexGeni) { static int t; if (!t++) stub("glTexGeni"); RET(0); }
IMP(opengl32, glTexGeniv) { static int t; if (!t++) stub("glTexGeniv"); RET(0); }
IMP(opengl32, glTexImage1D) { static int t; if (!t++) stub("glTexImage1D"); RET(0); }
IMP(opengl32, glTexSubImage1D) { static int t; if (!t++) stub("glTexSubImage1D"); RET(0); }
IMP(opengl32, glVertex2dv) { static int t; if (!t++) stub("glVertex2dv"); RET(0); }
IMP(opengl32, glVertex2sv) { static int t; if (!t++) stub("glVertex2sv"); RET(0); }
IMP(opengl32, glVertex3iv) { static int t; if (!t++) stub("glVertex3iv"); RET(0); }
IMP(opengl32, glVertex3s) { static int t; if (!t++) stub("glVertex3s"); RET(0); }
IMP(opengl32, glVertex3sv) { static int t; if (!t++) stub("glVertex3sv"); RET(0); }
IMP(opengl32, glVertex4d) { static int t; if (!t++) stub("glVertex4d"); RET(0); }
IMP(opengl32, glVertex4dv) { static int t; if (!t++) stub("glVertex4dv"); RET(0); }
IMP(opengl32, glVertex4i) { static int t; if (!t++) stub("glVertex4i"); RET(0); }
IMP(opengl32, glVertex4iv) { static int t; if (!t++) stub("glVertex4iv"); RET(0); }
IMP(opengl32, glVertex4s) { static int t; if (!t++) stub("glVertex4s"); RET(0); }
IMP(opengl32, glVertex4sv) { static int t; if (!t++) stub("glVertex4sv"); RET(0); }
IMP(opengl32, wglChoosePixelFormat) { static int t; if (!t++) stub("wglChoosePixelFormat"); RET(0); }
IMP(opengl32, wglCopyContext) { static int t; if (!t++) stub("wglCopyContext"); RET(0); }
IMP(opengl32, wglCreateLayerContext) { static int t; if (!t++) stub("wglCreateLayerContext"); RET(0); }
IMP(opengl32, wglDescribeLayerPlane) { static int t; if (!t++) stub("wglDescribeLayerPlane"); RET(0); }
IMP(opengl32, wglDescribePixelFormat) { static int t; if (!t++) stub("wglDescribePixelFormat"); RET(0); }
IMP(opengl32, wglGetLayerPaletteEntries) { static int t; if (!t++) stub("wglGetLayerPaletteEntries"); RET(0); }
IMP(opengl32, wglGetPixelFormat) { static int t; if (!t++) stub("wglGetPixelFormat"); RET(0); }
IMP(opengl32, wglRealizeLayerPalette) { static int t; if (!t++) stub("wglRealizeLayerPalette"); RET(0); }
IMP(opengl32, wglSetLayerPaletteEntries) { static int t; if (!t++) stub("wglSetLayerPaletteEntries"); RET(0); }
IMP(opengl32, wglSetPixelFormat) { static int t; if (!t++) stub("wglSetPixelFormat"); RET(0); }
IMP(opengl32, wglSwapLayerBuffers) { static int t; if (!t++) stub("wglSwapLayerBuffers"); RET(0); }
IMP(opengl32, wglUseFontBitmapsA) { static int t; if (!t++) stub("wglUseFontBitmapsA"); RET(0); }
IMP(opengl32, wglUseFontOutlinesA) { static int t; if (!t++) stub("wglUseFontOutlinesA"); RET(0); }
