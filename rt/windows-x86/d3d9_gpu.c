// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9 on the GPU ring. The device of d3d9*.c keeps every object and state; this file turns what a title
// asks to see (Clear, DrawPrimitive, DrawIndexedPrimitive, Present) into the explicit records of rt/d3d12_cmd.h,
// which say what WebGPU should do: shaders as WGSL text (written by d3d9_ffp.c from the fixed-function state),
// pipelines, samplers, buffer and texture bytes, passes, draws. Nothing of Direct3D 12 is involved. The same
// records go to the null back end in Node, which checks them and draws nothing.
//
// At each draw the device's state becomes: a vertex key and a fragment key (-> two shaders), a pipeline (the
// shaders, the vertex layout from the declaration, raster / depth / blend state, the target's formats), a
// uniform block (matrices, lights, material, fog, ...), the texture and sampler of stage 0, the vertex and index
// buffers. Buffers and textures are created on the GPU when first used and written when their `dirty` counters
// have moved (a buffer: the bytes its locks named, in the way the locks' flags allow). A pass begins when the
// target changes and at every Clear.
//
// ---- Direct3D 9's conventions where they differ from WebGPU's: each is handled once, here or in d3d9_ffp.c ----
//  D3DCOLOR        0xAARRGGBB, in memory B G R A. A8R8G8B8 / X8R8G8B8 textures and targets are WebGPU's
//                  bgra8unorm as they are (X8: the fragment key says alpha reads 1). Colours in render states and
//                  Clear are unpacked by color_rgba(). Vertex colours are swizzled in the vertex shader.
//  Pixel centres   Direct3D 9: whole numbers; WebGPU: halves. uniforms(): `half_pixel` moves every transformed
//                  position by (+0.5, +0.5) pixel, and XYZRHW positions are mapped by `screen_scale / _offset`
//                  with the same half pixel in it. The viewport and scissor rectangles are then the same numbers.
//  Clip space      the same in both (x, y in -w..w, y up; z in 0..w), so projection matrices pass unchanged and
//                  the viewport's MinZ / MaxZ are WebGPU's depth range. An XYZRHW vertex's z is the depth itself.
//  Matrices        row vectors (v x World x View x Projection), stored row by row: uploaded as they are, which
//                  WGSL reads as the transposes, so the shader multiplies from the left (d3d9_ffp.h).
//  Winding         D3DCULL_CCW (the default) culls counter-clockwise triangles: the front face is the clockwise
//                  one. pipeline(): front face clockwise always; CCW culls the back, CW culls the front.
//  Depth bias      D3DRS_DEPTHBIAS is a float in depth units, WebGPU's an integer count of the smallest depth
//                  step: times 2^24, for the 24-bit depth buffers titles ask for (pipeline()).
//  Triangle fans   WebGPU has none: fan_indices() draws them as indexed lists (0, i + 1, i + 2).
//  Points          one pixel in WebGPU: a point list is drawn as instanced four-vertex strips (d3d9_ffp.c sizes them).
//  Alpha test      not in WebGPU: the fragment shader discards.
//  A texture that is also the target: undefined in Direct3D 9, an error in WebGPU. The draw is left out.
#include <math.h>
#include "d3d9.h"
#include "d3d9_ffp.h"
#include "d3d9_sm.h"
#include "gpu_ring.h"

enum { RS_ZENABLE = 7, RS_FILLMODE = 8, RS_SHADEMODE = 9, RS_ZWRITEENABLE = 14, RS_ALPHATESTENABLE = 15, RS_SRCBLEND = 19, RS_DESTBLEND = 20, RS_CULLMODE = 22, RS_ZFUNC = 23,
  RS_ALPHAREF = 24, RS_ALPHAFUNC = 25, RS_ALPHABLENDENABLE = 27, RS_FOGENABLE = 28, RS_SPECULARENABLE = 29, RS_FOGCOLOR = 34, RS_FOGTABLEMODE = 35, RS_FOGSTART = 36, RS_FOGEND = 37,
  RS_FOGDENSITY = 38, RS_RANGEFOGENABLE = 48, RS_STENCILENABLE = 52, RS_TEXTUREFACTOR = 60, RS_LIGHTING = 137, RS_AMBIENT = 139, RS_FOGVERTEXMODE = 140, RS_COLORVERTEX = 141,
  RS_LOCALVIEWER = 142, RS_NORMALIZENORMALS = 143, RS_DIFFUSEMATERIALSOURCE = 145, RS_SPECULARMATERIALSOURCE = 146, RS_AMBIENTMATERIALSOURCE = 147, RS_EMISSIVEMATERIALSOURCE = 148,
  RS_VERTEXBLEND = 151, RS_CLIPPLANEENABLE = 152, RS_POINTSIZE = 154, RS_POINTSIZE_MIN = 155, RS_POINTSPRITEENABLE = 156, RS_POINTSCALEENABLE = 157, RS_POINTSCALE_A = 158,
  RS_POINTSCALE_B = 159, RS_POINTSCALE_C = 160, RS_POINTSIZE_MAX = 166, RS_INDEXEDVERTEXBLENDENABLE = 167, RS_COLORWRITEENABLE = 168, RS_BLENDOP = 171, RS_SCISSORTESTENABLE = 174,
  RS_SLOPESCALEDEPTHBIAS = 175, RS_DEPTHBIAS = 195, RS_SEPARATEALPHABLENDENABLE = 206, RS_SRCBLENDALPHA = 207, RS_DESTBLENDALPHA = 208, RS_BLENDOPALPHA = 209 };
enum { TSS_COLOROP = 1, TSS_COLORARG1, TSS_COLORARG2, TSS_ALPHAOP, TSS_ALPHAARG1, TSS_ALPHAARG2, TSS_TEXCOORDINDEX = 11, TSS_TEXTURETRANSFORMFLAGS = 24 };
enum { SAMP_ADDRESSU = 1, SAMP_ADDRESSV, SAMP_ADDRESSW, SAMP_BORDERCOLOR, SAMP_MAGFILTER, SAMP_MINFILTER, SAMP_MIPFILTER, SAMP_MIPMAPLODBIAS, SAMP_MAXMIPLEVEL, SAMP_MAXANISOTROPY };
enum { PT_POINTLIST = 1, PT_LINELIST, PT_LINESTRIP, PT_TRIANGLELIST, PT_TRIANGLESTRIP, PT_TRIANGLEFAN };
enum { TS_VIEW = 2, TS_PROJECTION = 3, TS_TEXTURE0 = 16, TS_WORLD = 256 };
enum { DXGI_R32G32B32A32_FLOAT = 2, DXGI_R32G32B32_FLOAT = 6, DXGI_R32G32_FLOAT = 16, DXGI_R8G8B8A8_UNORM = 28, DXGI_D32_FLOAT = 40, DXGI_R32_FLOAT = 41, DXGI_D24_UNORM_S8_UINT = 45,
       DXGI_D16_UNORM = 55, DXGI_B8G8R8A8_UNORM = 87, DXGI_B8G8R8X8_UNORM = 88,
       DXGI_R16G16B16A16_FLOAT = 10, DXGI_R16G16_FLOAT = 34, DXGI_R16_FLOAT = 54 };

// What is not handled is said once (and stops a strict run), then the draw goes on without it or is left out
#define UNSURE_ONCE(...) do { static int told_; if (!told_++) com_unsure(__VA_ARGS__); } while (0)
static float f32_of(u32 bits) { float f; memcpy(&f, &bits, 4); return f; }
static u32 bits_of(float f) { u32 v; memcpy(&v, &f, 4); return v; }
// D3DCOLOR 0xAARRGGBB -> r g b a
static void color_rgba(u32 c, float out[4]) { out[0] = (float)((c >> 16) & 255) / 255.0f; out[1] = (float)((c >> 8) & 255) / 255.0f; out[2] = (float)(c & 255) / 255.0f; out[3] = (float)(c >> 24) / 255.0f; }

static u32 next_id = 1;
static struct { u64 draws, skipped, passes, buffer_writes, buffer_bytes, texture_writes, texture_bytes, uniform_bytes; u32 buffers, textures; } stat;
static void rec(u32 op, const u32 *w, u32 n) { u8 *p = rt_gpu_rec_begin(op, 4 * n); memcpy(p, w, 4 * n); rt_gpu_rec_end_quiet(); }

// ---- resources ----
// D3DFORMAT -> the DXGI number the ring names formats by; 0: not a format this front end draws with
static u32 dxgi_of(u32 format) {
  switch (format) {
  case FMT_A8R8G8B8: return DXGI_B8G8R8A8_UNORM;
  case FMT_X8R8G8B8: return DXGI_B8G8R8X8_UNORM;
  case 111: return DXGI_R16_FLOAT;                                     // R16F
  case 112: return DXGI_R16G16_FLOAT;                                  // G16R16F
  case 113: return DXGI_R16G16B16A16_FLOAT;                            // A16B16G16R16F
  case 114: return DXGI_R32_FLOAT;                                     // R32F (Tag's paint maps: docs/tag.md)
  case 115: return DXGI_R32G32_FLOAT;                                  // G32R32F
  case 116: return DXGI_R32G32B32A32_FLOAT;                            // A32B32G32R32F
  case 75: case 77: case 79: case 83: return DXGI_D24_UNORM_S8_UINT;   // D24S8, D24X8, D24X4S4, D24FS8
  case 70: case 73: case 80: return DXGI_D16_UNORM;                    // D16_LOCKABLE, D15S1, D16
  case 71: case 82: return DXGI_D32_FLOAT;                             // D32, D32F_LOCKABLE
  default: return 0;
  }
}
// GPU_R_RESOURCE: dimension 0 a buffer, 2 a 2D texture, 3 a volume; flags 1 a render target, 2 a depth buffer
static u32 resource(u32 dim, u32 format, u32 width, u32 height, u32 depth, u32 mips, u32 flags) {
  u32 w[14] = {next_id++, dim, format, width, 0, height, depth, mips, 1, flags, 1, 0, 0, 0};
  rec(GPU_R_RESOURCE, w, 14);
  return w[0];
}
static void destroy(u32 id) { if (id) rec(GPU_R_DESTROY, &id, 1); }
// Bytes for a buffer, in pieces the ring takes; `bytes` are rounded up to words (the buffer on the GPU is)
static void write_buffer(u32 id, u32 offset, const u8 *src, u32 bytes, u32 mode) {
  for (u32 at = 0; at < bytes;) {
    u32 n = bytes - at; if (n > (4u << 20)) n = 4u << 20;
    u32 padded = (n + 3) & ~3u;
    u8 *p = rt_gpu_rec_begin(GPU_R_WRITE_BUFFER, 16 + padded); u32 *w = (u32 *)p;
    w[0] = id; w[1] = offset + at; w[2] = padded; w[3] = at ? GPU_WRITE_APPEND : mode;
    memcpy(p + 16, src + at, n); rt_gpu_rec_end_quiet();
    stat.buffer_writes++; stat.buffer_bytes += padded;
    at += n;
  }
}
// (`layer`: the first array layer written, a cube map's face; or the first slice of a volume)
static void write_texture_at(u32 id, u32 mip, u32 layer, u32 width, u32 height, u32 depth, const u8 *src, u32 pitch) {
  u32 rows = (4u << 20) / (pitch ? pitch : 1); if (!rows) rows = 1;
  for (u32 z = 0; z < depth; z++) for (u32 y = 0; y < height; y += rows) {
    u32 n = height - y < rows ? height - y : rows;
    u8 *p = rt_gpu_rec_begin(GPU_R_WRITE_TEXTURE, 36 + n * pitch); u32 *w = (u32 *)p;
    w[0] = id; w[1] = mip; w[2] = 0; w[3] = y; w[4] = layer + z; w[5] = width; w[6] = n; w[7] = 1; w[8] = pitch;
    memcpy(p + 36, src + (size_t)z * height * pitch + (size_t)y * pitch, (size_t)n * pitch); rt_gpu_rec_end_quiet();
    stat.texture_writes++; stat.texture_bytes += n * pitch;
  }
}
static void write_texture(u32 id, u32 mip, u32 width, u32 height, u32 depth, const u8 *src, u32 pitch) { write_texture_at(id, mip, 0, width, height, depth, src, pitch); }

// A lock for writing: which bytes may change, and what the lock promised about the ones draws have read.
// DISCARD lets the back end hand out new storage, so it is honoured only where nothing outside the lock can be
// missed: a dynamic buffer, or a lock of the whole buffer.
void d3d9_gpu_buffer_locked(Buffer9 *b, u32 offset, u32 size, u32 flags) {
  u32 end = size ? offset + size : b->size, mode = GPU_WRITE_ORDERED;
  if ((flags & LOCK_DISCARD) && ((b->usage & USAGE_DYNAMIC) || (!offset && end == b->size))) mode = GPU_WRITE_DISCARD;
  else if (flags & LOCK_NOOVERWRITE) mode = GPU_WRITE_APPEND;
  if (mode == GPU_WRITE_DISCARD || !b->gpu_pending) { b->gpu_mode = (u8)mode; b->gpu_lo = offset; b->gpu_hi = end; }
  else {
    if (mode == GPU_WRITE_ORDERED && b->gpu_mode == GPU_WRITE_APPEND) b->gpu_mode = GPU_WRITE_ORDERED;
    if (offset < b->gpu_lo) b->gpu_lo = offset; if (end > b->gpu_hi) b->gpu_hi = end;
  }
  b->gpu_pending = 1;
}
static u32 buffer(Buffer9 *b) {
  if (!b->gpu_id) { b->gpu_id = resource(0, 0, b->size, 1, 1, 1, 0); stat.buffers++; }
  if (b->dirty != b->gpu_dirty) {   // its bytes changed: the locked range, or all of it if nothing said which
    u32 lo = b->gpu_pending ? b->gpu_lo & ~3u : 0, hi = b->gpu_pending ? (b->gpu_hi + 3) & ~3u : b->size;   // whole words, of the buffer's own bytes
    if (hi > b->size || hi < lo) hi = b->size;
    if (hi > lo) write_buffer(b->gpu_id, lo, b->bits + lo, hi - lo, b->gpu_pending ? b->gpu_mode : GPU_WRITE_ORDERED);
    b->gpu_dirty = b->dirty; b->gpu_pending = 0;
  }
  return b->gpu_id;
}
// A texture with all its levels; the ones whose bytes changed are sent again. 0: a format not drawn with.
static u32 texture(Texture9 *t) {
  u32 format = dxgi_of(t->format), cube = t->o.kind == K_CUBETEXTURE;
  if (!format) return 0;   // a format without a number on the ring
  int target = (t->usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) != 0;
  if (!t->gpu_id) {
    t->gpu_id = resource(t->o.kind == K_VOLUMETEXTURE ? 3 : 2, format, t->width, t->height, cube ? 6 : t->depth, t->levels, (t->usage & USAGE_RENDERTARGET) ? 1 : (t->usage & USAGE_DEPTHSTENCIL) ? 2 : 0);
    stat.textures++;
  }
  if (target) return t->gpu_id;   // its pixels are the GPU's
  for (u32 i = 0; i < t->levels; i++) {
    if (cube) for (u32 f = 0; f < 6; f++) { Surface9 *s = t->surfaces[f * t->levels + i]; if (s->dirty != s->gpu_dirty) { write_texture_at(t->gpu_id, i, f, s->desc.width, s->desc.height, 1, s->bits, s->pitch); s->gpu_dirty = s->dirty; } }
    else if (t->o.kind == K_TEXTURE) { Surface9 *s = t->surfaces[i]; if (s->dirty != s->gpu_dirty) { write_texture(t->gpu_id, i, s->desc.width, s->desc.height, 1, s->bits, s->pitch); s->gpu_dirty = s->dirty; } }
    else { Volume9 *v = t->volumes[i]; if (v->dirty != v->gpu_dirty) { write_texture(t->gpu_id, i, v->desc.width, v->desc.height, v->desc.depth, v->bits, v->row_pitch); v->gpu_dirty = v->dirty; } }
  }
  return t->gpu_id;
}
static u32 bind_kind(const Texture9 *t) { return t->o.kind == K_CUBETEXTURE ? GPU_BIND_TEXTURE_CUBE : t->o.kind == K_VOLUMETEXTURE ? GPU_BIND_TEXTURE_3D : GPU_BIND_TEXTURE_2D; }
// A surface drawn into: a texture's level, or a texture of its own (the back buffer, a depth buffer)
static u32 target(Surface9 *s, u32 *mip) {
  *mip = 0;
  if (s->o.container) { *mip = s->level; return texture((Texture9 *)s->o.container); }
  if (!s->gpu_id) {
    u32 format = dxgi_of(s->desc.format);
    if (!format) return 0;
    s->gpu_id = resource(2, format, s->desc.width, s->desc.height, 1, 1, (s->desc.usage & USAGE_DEPTHSTENCIL) ? 2 : 1);
    stat.textures++;
  }
  return s->gpu_id;
}
void d3d9_gpu_release(Object9 *o) {
  switch (o->kind) {
  case K_SURFACE: destroy(((Surface9 *)o)->gpu_id); break;
  case K_TEXTURE: case K_VOLUMETEXTURE: case K_CUBETEXTURE: destroy(((Texture9 *)o)->gpu_id); break;
  case K_VERTEXBUFFER: case K_INDEXBUFFER: destroy(((Buffer9 *)o)->gpu_id); break;
  default: break;
  }
}

// ---- passes ----
static struct { int open; u32 color, mip, depth, color_format, depth_format, width, height; } pass;
static struct { int sent; Viewport9 vp; Rect9 scissor; } view;
// The pass draws go to: the device's target and depth buffer. With `flags` (D3DCLEAR_*) it starts with a clear.
static int set_pass(Device9 *d, u32 flags, u32 color, float z, u32 stencil) {
  Surface9 *t = d->targets[0], *ds = d->depth; u32 mip = 0, dmip = 0;
  u32 cid = t ? target(t, &mip) : 0, did = 0;
  if (!cid) { UNSURE_ONCE("Direct3D 9: a render target of format %u is not drawn into", t ? t->desc.format : 0); return 0; }
  if (ds) {
    if (ds->desc.width != t->desc.width || ds->desc.height != t->desc.height) UNSURE_ONCE("Direct3D 9: a %ux%u depth buffer with a %ux%u target: drawn without depth", ds->desc.width, ds->desc.height, t->desc.width, t->desc.height);
    else did = target(ds, &dmip);
  }
  if (!flags && pass.open && pass.color == cid && pass.mip == mip && pass.depth == did) return 1;
  float c[4]; color_rgba(color, c);
  u32 w[10] = {cid, mip, did, ((flags & 1) ? GPU_PASS_CLEAR_COLOR : 0) | ((flags & 2) && did ? GPU_PASS_CLEAR_DEPTH : 0) | ((flags & 4) && did ? GPU_PASS_CLEAR_STENCIL : 0),
               bits_of(c[0]), bits_of(c[1]), bits_of(c[2]), bits_of(c[3]), bits_of(z), stencil};
  rec(GPU_R_PASS, w, 10);
  pass.open = 1; pass.color = cid; pass.mip = mip; pass.depth = did; pass.width = t->desc.width; pass.height = t->desc.height;
  pass.color_format = dxgi_of(t->desc.format); pass.depth_format = did ? dxgi_of(ds->desc.format) : 0;
  stat.passes++;
  return 1;
}
static void set_view(const State9 *s) {
  Rect9 sc = s->rs[RS_SCISSORTESTENABLE] ? s->scissor : (Rect9){0, 0, (s32)pass.width, (s32)pass.height};
  if (view.sent && !memcmp(&view.vp, &s->viewport, sizeof view.vp) && !memcmp(&view.scissor, &sc, sizeof sc)) return;
  view.sent = 1; view.vp = s->viewport; view.scissor = sc;
  const Viewport9 *v = &s->viewport;
  u32 w[10] = {bits_of((float)v->x), bits_of((float)v->y), bits_of((float)v->width), bits_of((float)v->height), bits_of(v->min_z), bits_of(v->max_z),
               (u32)(sc.left < 0 ? 0 : sc.left), (u32)(sc.top < 0 ? 0 : sc.top), (u32)(sc.right > sc.left ? sc.right - sc.left : 0), (u32)(sc.bottom > sc.top ? sc.bottom - sc.top : 0)};
  rec(GPU_R_VIEWPORT, w, 10);
}
// Clear clears the viewport (the scissor rectangle too, when that test is on): a pass that starts by clearing does
// the whole target, which is the same thing as long as the viewport is all of it.
void d3d9_gpu_clear(Device9 *d, u32 flags, u32 color, float z, u32 stencil) {
  const State9 *s = &d->state; const Surface9 *t = d->targets[0];
  if (t && (s->viewport.x || s->viewport.y || s->viewport.width != t->desc.width || s->viewport.height != t->desc.height || s->rs[RS_SCISSORTESTENABLE]))
    UNSURE_ONCE("Direct3D 9: Clear of a part of the target (viewport %u,%u %ux%u of %ux%u) clears all of it", s->viewport.x, s->viewport.y, s->viewport.width, s->viewport.height, t->desc.width, t->desc.height);
  set_pass(d, flags & 7, color, z, stencil);
}

// ---- shaders, samplers, pipelines: made once for each key ----
typedef struct { FfpVertexKey key; u32 id; } VertexShader;
typedef struct { FfpFragmentKey key; u32 id; } FragmentShader;
static VertexShader *vertex_shaders; static FragmentShader *fragment_shaders; static u32 vertex_shader_count, fragment_shader_count;
static u32 shader_record(const char *text, size_t len) {
  u8 *p = rt_gpu_rec_begin(GPU_R_SHADER, 8 + (u32)len); u32 *w = (u32 *)p;
  w[0] = next_id++; w[1] = (u32)len; memcpy(p + 8, text, len); rt_gpu_rec_end_quiet();
  return w[0];
}
static char shader_text[16384];
// 0: the generator does not cover the key (said once for each such key)
static u32 vertex_shader(const FfpVertexKey *k) {
  for (u32 i = 0; i < vertex_shader_count; i++) if (!memcmp(&vertex_shaders[i].key, k, sizeof *k)) return vertex_shaders[i].id;
  char hex[2 * sizeof *k + 1], what[256]; ffp_key_hex(k, sizeof *k, hex); ffp_vertex_describe(k, what, sizeof what);
  size_t len = ffp_vertex_wgsl(k, shader_text, sizeof shader_text);
  u32 id = len ? shader_record(shader_text, len) : 0;
  if (!len) com_unsure("Direct3D 9: no fixed-function vertex shader for %s (%s): %s", hex, what, shader_text);
  else if (d3d9_stats) rt_log("Direct3D 9 shader vs %s: %s", hex, what);
  vertex_shaders = realloc(vertex_shaders, (vertex_shader_count + 1) * sizeof *vertex_shaders);
  vertex_shaders[vertex_shader_count++] = (VertexShader){*k, id};
  return id;
}
static u32 fragment_shader(const FfpFragmentKey *k) {
  for (u32 i = 0; i < fragment_shader_count; i++) if (!memcmp(&fragment_shaders[i].key, k, sizeof *k)) return fragment_shaders[i].id;
  char hex[2 * sizeof *k + 1], what[256]; ffp_key_hex(k, sizeof *k, hex); ffp_fragment_describe(k, what, sizeof what);
  size_t len = ffp_fragment_wgsl(k, shader_text, sizeof shader_text);
  u32 id = len ? shader_record(shader_text, len) : 0;
  if (!len) com_unsure("Direct3D 9: no fixed-function fragment shader for %s (%s): %s", hex, what, shader_text);
  else if (d3d9_stats) rt_log("Direct3D 9 shader fs %s: %s", hex, what);
  fragment_shaders = realloc(fragment_shaders, (fragment_shader_count + 1) * sizeof *fragment_shaders);
  fragment_shaders[fragment_shader_count++] = (FragmentShader){*k, id};
  return id;
}

// A sampler: D3DSAMP_* of stage 0. Direct3D's address modes 1 to 3 are the ring's; border and mirror-once clamp.
typedef struct { u8 u, v, w, mag, min, mip, aniso, no_mips; u32 id; } Sampler;
static Sampler *samplers; static u32 sampler_count;
static u32 sampler(const u32 *st) {
  Sampler k = {0};
  k.u = (u8)(st[SAMP_ADDRESSU] >= 1 && st[SAMP_ADDRESSU] <= 2 ? st[SAMP_ADDRESSU] : 3); k.v = (u8)(st[SAMP_ADDRESSV] >= 1 && st[SAMP_ADDRESSV] <= 2 ? st[SAMP_ADDRESSV] : 3);
  k.w = (u8)(st[SAMP_ADDRESSW] >= 1 && st[SAMP_ADDRESSW] <= 2 ? st[SAMP_ADDRESSW] : 3);
  // D3DTEXF_*: 1 point, 2 linear, 3 anisotropic (and the old flat-cubic and Gaussian ones: linear). No mip filter: only the first level.
  k.mag = st[SAMP_MAGFILTER] >= 2; k.min = st[SAMP_MINFILTER] >= 2; k.mip = st[SAMP_MIPFILTER] >= 2; k.no_mips = st[SAMP_MIPFILTER] == 0;
  k.aniso = 1;
  if (st[SAMP_MINFILTER] == 3 || st[SAMP_MAGFILTER] == 3) { k.aniso = (u8)(st[SAMP_MAXANISOTROPY] > 16 ? 16 : st[SAMP_MAXANISOTROPY] < 1 ? 1 : st[SAMP_MAXANISOTROPY]); if (k.aniso > 1) k.mag = k.min = k.mip = 1; }   // WebGPU: anisotropy only with every filter linear
  for (u32 i = 0; i < sampler_count; i++) if (!memcmp(&samplers[i], &k, 8)) return samplers[i].id;
  k.id = next_id++;
  u32 w[9] = {k.id, k.u, k.v, k.w, k.mag, k.min, k.mip, k.aniso, bits_of(k.no_mips ? 0.0f : 32.0f)};
  rec(GPU_R_SAMPLER, w, 9);
  samplers = realloc(samplers, (sampler_count + 1) * sizeof *samplers); samplers[sampler_count++] = k;
  return k.id;
}

enum { MAX_VB = 4, MAX_ATTR = 16, MAX_SAMPLERS = 16 };
typedef struct {
  u32 vs, fs, color_format, depth_format; s32 depth_bias; u32 slope;
  u8 topology, cull, write_mask, blend, src, dst, op, src_a, dst_a, op_a, depth_compare, depth_write, buffers, attributes, step, pad0;
  u16 uniform_bytes, sampler_mask;        // sampler N: a texture at binding 1 + 2N (of kind kinds[N]) and its sampler at 2 + 2N
  u16 stride[MAX_VB];
  u8 kinds[MAX_SAMPLERS];
  struct { u8 location, buffer, format, pad; u16 offset; } attr[MAX_ATTR];
} PipelineKey;   // no hidden padding: a key is hashed and compared as its bytes
_Static_assert(sizeof(PipelineKey) == 24 + 16 + 4 + 2 * MAX_VB + MAX_SAMPLERS + 6 * MAX_ATTR, "PipelineKey has no hidden padding");
static u32 binding_count(const PipelineKey *k) { return 1 + 2 * (u32)__builtin_popcount(k->sampler_mask); }
typedef struct { PipelineKey key; u32 hash, id; } Pipeline;
static Pipeline *pipelines; static u32 pipeline_count, pipeline_cap;
static u32 pipeline(const PipelineKey *k) {
  u32 h = 2166136261u; for (size_t i = 0; i < sizeof *k; i++) h = (h ^ ((const u8 *)k)[i]) * 16777619u;
  for (u32 i = 0; i < pipeline_count; i++) if (pipelines[i].hash == h && !memcmp(&pipelines[i].key, k, sizeof *k)) return pipelines[i].id;
  u32 w[GPU_PIPELINE_WORDS + 3 * (1 + 2 * MAX_SAMPLERS) + 2 * MAX_VB + 4 * MAX_ATTR], n = GPU_PIPELINE_WORDS, id = next_id++;
  u32 head[GPU_PIPELINE_WORDS] = {id, k->vs, k->fs, k->topology, k->cull, 1 /* the front face is the clockwise one */, k->color_format, k->write_mask, k->blend, k->src, k->dst, k->op, k->src_a, k->dst_a, k->op_a,
    k->depth_format, k->depth_compare, k->depth_write, (u32)k->depth_bias, k->slope, binding_count(k), k->buffers, k->attributes, 0};
  memcpy(w, head, sizeof head);
  w[n++] = 0; w[n++] = GPU_BIND_UNIFORM; w[n++] = k->uniform_bytes;
  for (u32 i = 0; i < MAX_SAMPLERS; i++) if (k->sampler_mask & (1u << i)) { w[n++] = 1 + 2 * i; w[n++] = k->kinds[i]; w[n++] = 0; w[n++] = 2 + 2 * i; w[n++] = GPU_BIND_SAMPLER; w[n++] = 0; }
  for (u32 i = 0; i < k->buffers; i++) { w[n++] = k->stride[i]; w[n++] = k->step; }
  for (u32 i = 0; i < k->attributes; i++) { w[n++] = k->attr[i].location; w[n++] = k->attr[i].buffer; w[n++] = k->attr[i].offset; w[n++] = k->attr[i].format; }
  rec(GPU_R_PIPELINE, w, n);
  if (pipeline_count == pipeline_cap) { pipeline_cap = pipeline_cap ? 2 * pipeline_cap : 64; pipelines = realloc(pipelines, pipeline_cap * sizeof *pipelines); }
  pipelines[pipeline_count++] = (Pipeline){*k, h, id};
  if (d3d9_stats) rt_log("Direct3D 9 pipeline %u: vs %u fs %u, topology %u, cull %u, blend %u (%u, %u, op %u), depth %u (compare %u, write %u, bias %d), %u attributes", pipeline_count, k->vs, k->fs, k->topology, k->cull,
                         k->blend, k->src, k->dst, k->op, k->depth_format, k->depth_compare, k->depth_write, k->depth_bias, k->attributes);
  return id;
}

// ---- triangle fans as indexed lists: fan triangle i is (0, i + 1, i + 2) ----
static u32 fan_buffer, fan_triangles;
static u32 fan_indices(u32 triangles) {
  if (triangles > 32766) return 0;   // 16-bit indices
  if (triangles <= fan_triangles) return fan_buffer;
  u32 n = fan_triangles ? fan_triangles : 1024; while (n < triangles) n *= 2;
  if (n > 32766) n = 32766;
  u16 *ix = malloc(6 * n);
  for (u32 i = 0; i < n; i++) { ix[3 * i] = 0; ix[3 * i + 1] = (u16)(i + 1); ix[3 * i + 2] = (u16)(i + 2); }
  destroy(fan_buffer); fan_buffer = resource(0, 0, 6 * n, 1, 1, 1, 0); fan_triangles = n;
  write_buffer(fan_buffer, 0, (const u8 *)ix, 6 * n, GPU_WRITE_ORDERED); free(ix);
  return fan_buffer;
}
// An indexed fan: its list, written out for this draw
static u32 scratch_buffer, scratch_size;
static u32 indexed_fan(const Buffer9 *ib, u32 first, u32 triangles, u32 *format32) {
  u32 wide = ib->format == FMT_INDEX32, bytes = 3 * triangles * (wide ? 4 : 2);
  if ((first + triangles + 2) * (wide ? 4 : 2) > ib->size) return 0;
  u8 *list = malloc(bytes);
  for (u32 i = 0; i < triangles; i++) for (u32 k = 0; k < 3; k++) {
    u32 from = first + (k ? i + k : 0);
    if (wide) ((u32 *)list)[3 * i + k] = ((const u32 *)ib->bits)[from]; else ((u16 *)list)[3 * i + k] = ((const u16 *)ib->bits)[from];
  }
  if (bytes > scratch_size) { destroy(scratch_buffer); scratch_size = bytes < 65536 ? 65536 : 2 * bytes; scratch_buffer = resource(0, 0, scratch_size, 1, 1, 1, 0); }
  write_buffer(scratch_buffer, 0, list, bytes, GPU_WRITE_DISCARD); free(list);
  *format32 = wide;
  return scratch_buffer;
}

// ---- the uniform block ----
static void mat_mul(float *r, const float *a, const float *b) {   // r = a x b, Direct3D's order
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a[4 * i + k] * b[4 * k + j]; r[4 * i + j] = v; }
}
// The matrix normals go through: the inverse transpose of m's 3x3 part (rows a, b, c: the cross products over the determinant)
static void normal_matrix(float *r, const float *m) {
  const float *a = m, *b = m + 4, *c = m + 8;
  float x[9] = {b[1] * c[2] - b[2] * c[1], b[2] * c[0] - b[0] * c[2], b[0] * c[1] - b[1] * c[0],
                c[1] * a[2] - c[2] * a[1], c[2] * a[0] - c[0] * a[2], c[0] * a[1] - c[1] * a[0],
                a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  float det = a[0] * x[0] + a[1] * x[1] + a[2] * x[2], inv = det != 0.0f ? 1.0f / det : 0.0f;
  memset(r, 0, 64);
  for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) r[4 * i + j] = x[3 * i + j] * inv;
  r[15] = 1.0f;
}
static void uniforms(const Device9 *d, const FfpVertexKey *vk, FfpUniforms *u) {
  const State9 *s = &d->state; const Viewport9 *vp = &s->viewport;
  float w = vp->width ? (float)vp->width : 1.0f, h = vp->height ? (float)vp->height : 1.0f;
  // Pixel centres (see the top of the file). A transformed position moves by half a pixel: +1/w, -1/h in clip
  // space after the divide. A screen-space position (x, y) is the pixel centre (x + 0.5, y + 0.5) of WebGPU.
  u->half_pixel[0] = 1.0f / w; u->half_pixel[1] = -1.0f / h;
  if (vk->pretransformed) {
    float range = vp->max_z - vp->min_z, sz = range != 0.0f ? 1.0f / range : 1.0f;
    u->screen_scale[0] = 2.0f / w; u->screen_scale[1] = -2.0f / h; u->screen_scale[2] = sz;
    u->screen_offset[0] = (1.0f - 2.0f * (float)vp->x) / w - 1.0f; u->screen_offset[1] = 1.0f + (2.0f * (float)vp->y - 1.0f) / h; u->screen_offset[2] = -vp->min_z * sz;
  } else {
    const float *view = s->transforms[TS_VIEW].m;
    for (unsigned i = 0; i < vk->matrices; i++) {
      mat_mul(u->world_view[i], s->transforms[TS_WORLD + i].m, view);
      if (vk->normal) normal_matrix(u->normal[i], u->world_view[i]);
    }
    memcpy(u->projection, s->transforms[TS_PROJECTION].m, 64);
    if (vk->lighting) {
      memcpy(u->material_diffuse, &s->material.diffuse, 16); memcpy(u->material_ambient, &s->material.ambient, 16);
      memcpy(u->material_specular, &s->material.specular, 16); memcpy(u->material_emissive, &s->material.emissive, 16);
      color_rgba(s->rs[RS_AMBIENT], u->ambient); u->misc[1] = s->material.power;
      unsigned n = 0;
      for (u32 i = 0; i < s->light_count && n < vk->lights; i++) {
        if (!s->lights[i].set || !s->lights[i].enabled) continue;
        const Light9 *l = &s->lights[i].light; FfpLight *o = &u->lights[n++];
        memcpy(o->diffuse, &l->diffuse, 16); memcpy(o->specular, &l->specular, 16); memcpy(o->ambient, &l->ambient, 16);
        for (int j = 0; j < 3; j++) o->position[j] = l->position[0] * view[j] + l->position[1] * view[4 + j] + l->position[2] * view[8 + j] + view[12 + j];
        o->position[3] = (float)l->type;
        float dir[3], len = 0;
        for (int j = 0; j < 3; j++) { dir[j] = l->direction[0] * view[j] + l->direction[1] * view[4 + j] + l->direction[2] * view[8 + j]; len += dir[j] * dir[j]; }
        len = len > 0 ? 1.0f / sqrtf(len) : 0.0f;
        for (int j = 0; j < 3; j++) o->direction[j] = dir[j] * len;
        o->direction[3] = l->range;
        o->attenuation[0] = l->attenuation[0]; o->attenuation[1] = l->attenuation[1]; o->attenuation[2] = l->attenuation[2]; o->attenuation[3] = l->falloff;
        o->spot[0] = cosf(l->theta * 0.5f); o->spot[1] = cosf(l->phi * 0.5f);
      }
    }
    if (vk->tex_transform) memcpy(u->tex_matrix, s->transforms[TS_TEXTURE0].m, 64);
  }
  if (s->rs[RS_FOGENABLE]) {
    float start = f32_of(s->rs[RS_FOGSTART]), end = f32_of(s->rs[RS_FOGEND]);
    color_rgba(s->rs[RS_FOGCOLOR], u->fog_color);
    u->fog[0] = start; u->fog[1] = end; u->fog[2] = end != start ? 1.0f / (end - start) : 0.0f; u->fog[3] = f32_of(s->rs[RS_FOGDENSITY]);
  }
  if (vk->point) {
    u->point[0] = f32_of(s->rs[RS_POINTSIZE]); u->point[1] = f32_of(s->rs[RS_POINTSIZE_MIN]); u->point[2] = f32_of(s->rs[RS_POINTSIZE_MAX]); u->point[3] = h;
    if (u->point[2] > 8192.0f) u->point[2] = 8192.0f;   // what the device says it can do (MaxPointSize)
    if (u->point[1] > u->point[2]) u->point[1] = u->point[2];
    u->point_scale[0] = f32_of(s->rs[RS_POINTSCALE_A]); u->point_scale[1] = f32_of(s->rs[RS_POINTSCALE_B]); u->point_scale[2] = f32_of(s->rs[RS_POINTSCALE_C]);
  }
  u->misc[0] = (float)(s->rs[RS_ALPHAREF] & 255);
  color_rgba(s->rs[RS_TEXTUREFACTOR], u->factor);
}

// ---- a draw ----
enum { DECL_FLOAT1 = 0, DECL_FLOAT2, DECL_FLOAT3, DECL_FLOAT4, DECL_D3DCOLOR };
enum { USE_POSITION = 0, USE_BLENDWEIGHT, USE_BLENDINDICES, USE_NORMAL, USE_PSIZE, USE_TEXCOORD, USE_POSITIONT = 9, USE_COLOR = 10 };
static const u8 float_format[4] = {DXGI_R32_FLOAT, DXGI_R32G32_FLOAT, DXGI_R32G32B32_FLOAT, DXGI_R32G32B32A32_FLOAT};
static void skip(const char *why) { stat.skipped++; (void)why; }
// ---- a draw, once its shaders, vertex layout and uniform block are known: the pass, the pipeline's own state, the
// textures and samplers it reads, its buffers, then the record (whose uniform block `fill` writes) ----
typedef struct { Texture9 *tex; const u32 *samp; } Bound;
typedef void (*FillUniforms)(const Device9 *d, const void *ctx, u8 *block);
static void ffp_fill(const Device9 *d, const void *ctx, u8 *block) { uniforms(d, (const FfpVertexKey *)ctx, (FfpUniforms *)block); }
static void draw_with(Device9 *d, PipelineKey *pkp, const u8 *streams, const Bound *bound, u32 type, s32 base, u32 first, u32 primitives, int indexed, FillUniforms fill, const void *ctx) {
  PipelineKey pk = *pkp; const State9 *s = &d->state;
  if (!set_pass(d, 0, 0, 0, 0)) { skip("no target"); return; }
  set_view(s);
  u32 bind_ids[2 * MAX_SAMPLERS], nbind = 0;
  for (u32 i = 0; i < MAX_SAMPLERS; i++) if (pk.sampler_mask & (1u << i)) {
    u32 t = texture(bound[i].tex);
    if (!t) { UNSURE_ONCE("Direct3D 9: a texture of format %u is not drawn with", bound[i].tex->format); skip("texture format"); return; }
    if (t == pass.color || t == pass.depth) { UNSURE_ONCE("Direct3D 9: a draw samples the texture it draws into: left out"); skip("feedback"); return; }
    bind_ids[nbind++] = t; bind_ids[nbind++] = sampler(bound[i].samp);
  }
  pk.color_format = pass.color_format; pk.depth_format = pass.depth_format;
  pk.write_mask = (u8)(s->rs[RS_COLORWRITEENABLE] & 15);
  if (s->rs[RS_ALPHABLENDENABLE]) {
    u32 src = s->rs[RS_SRCBLEND], dst = s->rs[RS_DESTBLEND];
    if (src == 12) { src = 5; dst = 6; } else if (src == 13) { src = 6; dst = 5; }   // D3DBLEND_BOTHSRCALPHA, BOTHINVSRCALPHA
    if (src < 1 || src > 11 || dst < 1 || dst > 11 || s->rs[RS_BLENDOP] < 1 || s->rs[RS_BLENDOP] > 5) UNSURE_ONCE("Direct3D 9: blending with factors %u, %u (operation %u) is drawn without blending", src, dst, s->rs[RS_BLENDOP]);
    else {
      pk.blend = 1; pk.src = pk.src_a = (u8)src; pk.dst = pk.dst_a = (u8)dst; pk.op = pk.op_a = (u8)s->rs[RS_BLENDOP];
      if (s->rs[RS_SEPARATEALPHABLENDENABLE] && s->rs[RS_SRCBLENDALPHA] >= 1 && s->rs[RS_SRCBLENDALPHA] <= 11 && s->rs[RS_DESTBLENDALPHA] >= 1 && s->rs[RS_DESTBLENDALPHA] <= 11) {
        pk.src_a = (u8)s->rs[RS_SRCBLENDALPHA]; pk.dst_a = (u8)s->rs[RS_DESTBLENDALPHA]; pk.op_a = (u8)(s->rs[RS_BLENDOPALPHA] >= 1 && s->rs[RS_BLENDOPALPHA] <= 5 ? s->rs[RS_BLENDOPALPHA] : 1);
      }
    }
  }
  if (pk.depth_format) {
    int z = s->rs[RS_ZENABLE] != 0;   // D3DZB_USEW (2) is drawn as a z buffer
    pk.depth_compare = (u8)(z && s->rs[RS_ZFUNC] >= 1 && s->rs[RS_ZFUNC] <= 8 ? s->rs[RS_ZFUNC] : 8); pk.depth_write = z && s->rs[RS_ZWRITEENABLE];
    if (type >= PT_TRIANGLELIST || type == PT_POINTLIST) {   // WebGPU biases triangles only
      float bias = f32_of(s->rs[RS_DEPTHBIAS]) * 16777216.0f;   // depth units -> steps of a 24-bit depth buffer
      pk.depth_bias = (s32)(bias < 0 ? bias - 0.5f : bias + 0.5f); pk.slope = s->rs[RS_SLOPESCALEDEPTHBIAS];
    }
  }
  if (s->rs[RS_STENCILENABLE]) UNSURE_ONCE("Direct3D 9: the stencil test is on: drawn without it");
  if (s->rs[RS_CLIPPLANEENABLE]) UNSURE_ONCE("Direct3D 9: user clip planes are on (0x%x): drawn without them", s->rs[RS_CLIPPLANEENABLE]);
  if (s->rs[RS_FILLMODE] != 3) UNSURE_ONCE("Direct3D 9: fill mode %u is drawn solid", s->rs[RS_FILLMODE]);
  if (s->rs[RS_SHADEMODE] == 1) UNSURE_ONCE("Direct3D 9: flat shading is drawn smooth");
  static const u8 topology[7] = {0, 5 /* points: a strip of four vertices for each */, 2, 3, 4, 5, 4 /* fans: indexed lists */};
  pk.topology = topology[type];
  pk.cull = (u8)(type < PT_TRIANGLELIST ? 0 : s->rs[RS_CULLMODE] == 2 ? 1 : s->rs[RS_CULLMODE] == 3 ? 2 : 0);
  pk.step = type == PT_POINTLIST;
  u32 vb[2 * MAX_VB];
  for (u32 i = 0; i < pk.buffers; i++) {
    Buffer9 *b = s->streams[streams[i]].buffer;
    if (!b) { skip("no vertex buffer"); return; }
    pk.stride[i] = (u16)s->streams[streams[i]].stride;
    vb[2 * i] = buffer(b); vb[2 * i + 1] = s->streams[streams[i]].offset;
  }
  u32 id = pipeline(&pk);

  // ---- the draw: Direct3D's primitive as WebGPU's ----
  u32 w[GPU_DRAW_WORDS + 2 * MAX_VB + 1 + 2 * MAX_SAMPLERS] = {id, 0, 0, 1, first, (u32)base, 0, 0, 0, pk.buffers, binding_count(&pk), pk.uniform_bytes}, n = GPU_DRAW_WORDS;
  u32 count = type == PT_POINTLIST ? primitives : type == PT_LINELIST ? 2 * primitives : type == PT_LINESTRIP ? primitives + 1 : type == PT_TRIANGLELIST ? 3 * primitives : primitives + 2;
  if (type == PT_POINTLIST) {
    if (indexed) { UNSURE_ONCE("Direct3D 9: an indexed point list is not drawn"); skip("indexed points"); return; }
    w[2] = 4; w[3] = primitives; w[4] = 0; w[6] = first;   // an instance for each point
  } else if (type == PT_TRIANGLEFAN && !indexed) {
    u32 fan = fan_indices(primitives);
    if (!fan) { UNSURE_ONCE("Direct3D 9: a fan of %u triangles is not drawn", primitives); skip("fan"); return; }
    w[1] = GPU_DRAW_INDEXED; w[2] = 3 * primitives; w[4] = 0; w[5] = first; w[7] = fan;   // the fan's first vertex is every index's base
  } else if (indexed) {
    Buffer9 *ib = s->indices; u32 wide = ib->format == FMT_INDEX32;
    if (type == PT_TRIANGLEFAN) {
      u32 list = indexed_fan(ib, first, primitives, &wide);
      if (!list) { skip("fan indices"); return; }
      w[2] = 3 * primitives; w[4] = 0; w[7] = list;
    } else { w[2] = count; w[7] = buffer(ib); }
    w[1] = GPU_DRAW_INDEXED | (wide ? GPU_DRAW_INDEX32 : 0);
  } else w[2] = count;
  for (u32 i = 0; i < pk.buffers; i++) { w[n++] = vb[2 * i]; w[n++] = vb[2 * i + 1]; }
  w[n++] = 0; for (u32 i = 0; i < nbind; i++) w[n++] = bind_ids[i];
  u8 *p = rt_gpu_rec_begin(GPU_R_DRAW, 4 * n + pk.uniform_bytes);
  memcpy(p, w, 4 * n);
  fill(d, ctx, p + 4 * n);   // the record's room is zeroed: what the shaders do not use stays 0
  rt_gpu_rec_end_quiet();
  stat.uniform_bytes += pk.uniform_bytes;
}


// ---- the shaders a title made: d3d9_sm.c's WGSL, made once for each bytecode and link key ----
typedef struct { u64 hash; u32 count; SmLinkKey link; u32 id; SmInfo info; } SmShader;
static SmShader *sm_shaders; static u32 sm_count, sm_cap;
static int sm_shader(Shader9 *sh, const SmLinkKey *link, SmShader *out) {
  if (!sh->hash) { u64 h = 0xcbf29ce484222325ull; const u8 *b = (const u8 *)sh->tokens; for (u32 i = 0; i < 4 * sh->count; i++) { h ^= b[i]; h *= 0x100000001b3ull; } sh->hash = h | 1; }
  for (u32 i = 0; i < sm_count; i++) if (sm_shaders[i].hash == sh->hash && sm_shaders[i].count == sh->count && !memcmp(&sm_shaders[i].link, link, sizeof *link)) { *out = sm_shaders[i]; return out->id != 0; }
  static char *text; enum { TEXT = 1 << 20 }; if (!text) text = malloc(TEXT);
  SmShader e; memset(&e, 0, sizeof e); e.hash = sh->hash; e.count = sh->count; e.link = *link;
  size_t len = sm_wgsl(sh->tokens, sh->count, link, &e.info, text, TEXT);
  char name[16]; sm_version_name(sh->tokens[0], name, sizeof name);
  if (len) { e.id = shader_record(text, len); if (d3d9_stats) rt_log("Direct3D 9 shader %s %016llx: %zu bytes of WGSL", name, (unsigned long long)sh->hash, len); }
  else com_unsure("Direct3D 9: %s %016llx is not translated: %s", name, (unsigned long long)sh->hash, text);
  if (sm_count == sm_cap) { sm_cap = sm_cap ? 2 * sm_cap : 64; sm_shaders = realloc(sm_shaders, sm_cap * sizeof *sm_shaders); }
  sm_shaders[sm_count++] = e; *out = e; return e.id != 0;
}
// a declaration's element type -> the ring's vertex format, and how the shader takes it (UDEC3, DEC3N: none)
static const struct { u8 dxgi, type; } decl_formats[17] = {
  {41, SM_INPUT_FLOAT}, {16, SM_INPUT_FLOAT}, {6, SM_INPUT_FLOAT}, {2, SM_INPUT_FLOAT},   // FLOAT1 .. FLOAT4
  {28, SM_INPUT_BGRA},                                                                    // D3DCOLOR
  {30, SM_INPUT_UINT},                                                                    // UBYTE4
  {38, SM_INPUT_SINT}, {14, SM_INPUT_SINT},                                               // SHORT2, SHORT4
  {28, SM_INPUT_FLOAT}, {37, SM_INPUT_FLOAT}, {13, SM_INPUT_FLOAT}, {35, SM_INPUT_FLOAT}, {11, SM_INPUT_FLOAT},   // UBYTE4N, SHORT2N, SHORT4N, USHORT2N, USHORT4N
  {0, 0}, {0, 0}, {34, SM_INPUT_FLOAT}, {10, SM_INPUT_FLOAT}};                           // UDEC3, DEC3N, FLOAT16_2, FLOAT16_4
// the uniform block (d3d9_sm.h SmUniforms), as far as the vertex shader's constants go
static void sm_fill(const Device9 *d, const void *ctx, u8 *block) {
  const SmInfo *vi = (const SmInfo *)ctx; const State9 *s = &d->state; const Viewport9 *vp = &s->viewport; SmUniforms *u = (SmUniforms *)block;
  float w = vp->width ? (float)vp->width : 1.0f, h = vp->height ? (float)vp->height : 1.0f;
  u->fx.half_pixel[0] = 1.0f / w; u->fx.half_pixel[1] = -1.0f / h;   // as the fixed-function vertex stage moves it (see uniforms())
  color_rgba(s->rs[RS_FOGCOLOR], u->fx.fog_color);
  u->fx.misc[0] = (float)(s->rs[RS_ALPHAREF] & 255);
  u->fx.point[0] = f32_of(s->rs[RS_POINTSIZE]); u->fx.point[1] = f32_of(s->rs[RS_POINTSIZE_MIN]); u->fx.point[2] = f32_of(s->rs[RS_POINTSIZE_MAX]); u->fx.point[3] = h;
  memcpy(u->ps_f, s->ps_float, sizeof u->ps_f);
  for (u32 i = 0; i < 16; i++) { memcpy(u->ps_i[i], s->ps_int[i], 16); u->ps_b[i][0] = s->ps_bool[i] != 0; memcpy(u->vs_i[i], s->vs_int[i], 16); u->vs_b[i][0] = s->vs_bool[i] != 0; }
  memcpy(u->vs_f, s->vs_float, 16 * (size_t)vi->float_count);
}
static void draw_shaders(Device9 *d, u32 type, s32 base, u32 first, u32 primitives, int indexed) {
  const State9 *s = &d->state; PipelineKey pk; memset(&pk, 0, sizeof pk);
  Shader9 *vsh = s->vertex_shader, *psh = s->pixel_shader;
  if (!vsh || !psh) { UNSURE_ONCE("Direct3D 9: a %s shader with the fixed-function %s stage is not drawn yet", vsh ? "vertex" : "pixel", vsh ? "pixel" : "vertex"); skip("one stage fixed"); return; }
  if (type == PT_POINTLIST) { UNSURE_ONCE("Direct3D 9: a point list with a vertex shader is not drawn yet"); skip("shader points"); return; }
  const Declaration9 *decl = s->declaration; if (!decl) { skip("no declaration"); return; }
  // the vertex shader's inputs, by the usage it declared them with: the declaration's element of the same usage
  SmLinkKey vl, pl; memset(&vl, 0, sizeof vl); memset(&pl, 0, sizeof pl);
  SmShader vs, ps;
  if (!sm_shader(vsh, &vl, &vs)) { skip("vertex shader"); return; }
  u8 streams[MAX_VB];
  for (u32 r = 0; r < 16; r++) if (vs.info.inputs & (1u << r)) {
    const Element9 *e = 0;
    for (u32 i = 0; i + 1 < decl->count && decl->elements[i].stream != 0xFF; i++)
      if (decl->elements[i].usage == vs.info.usage[r] && decl->elements[i].usage_index == vs.info.usage_index[r]) { e = &decl->elements[i]; break; }
    if (!e) { UNSURE_ONCE("Direct3D 9: a vertex shader reads v%u (usage %u, index %u), which the declaration does not have: not drawn", r, vs.info.usage[r], vs.info.usage_index[r]); skip("input"); return; }
    if (e->type > 16 || !decl_formats[e->type].dxgi) { UNSURE_ONCE("Direct3D 9: a vertex element of type %u is not drawn", e->type); skip("element type"); return; }
    vl.input_types[r] = decl_formats[e->type].type;
    u32 slot = 0; while (slot < pk.buffers && streams[slot] != e->stream) slot++;
    if (slot == pk.buffers) { if (slot == MAX_VB || e->stream >= STREAMS) { UNSURE_ONCE("Direct3D 9: a vertex declaration over more than %u streams is not drawn", MAX_VB); skip("streams"); return; } streams[pk.buffers++] = (u8)e->stream; }
    pk.attr[pk.attributes].location = (u8)r; pk.attr[pk.attributes].buffer = (u8)slot; pk.attr[pk.attributes].format = decl_formats[e->type].dxgi; pk.attr[pk.attributes].offset = e->offset; pk.attributes++;
  }
  if (!sm_shader(vsh, &vl, &vs)) { skip("vertex shader"); return; }
  // the pixel stage: fog and the alpha test after it, as Direct3D does them for shader model 2 and below
  pl.fog = s->rs[RS_FOGENABLE] != 0;
  if (s->rs[RS_ALPHATESTENABLE] && s->rs[RS_ALPHAFUNC] >= 1 && s->rs[RS_ALPHAFUNC] < 8) pl.alpha_test = (u8)s->rs[RS_ALPHAFUNC];
  if (((psh->tokens[0] >> 8) & 0xFF) == 1)   // ps_1_x declares no samplers: the bound textures' kinds
    for (u32 i = 0; i < 16; i++) if (s->textures[i]) pl.sampler_types[i] = s->textures[i]->o.kind == K_CUBETEXTURE ? SM_TEX_CUBE : s->textures[i]->o.kind == K_VOLUMETEXTURE ? SM_TEX_3D : SM_TEX_2D;
  if (!sm_shader(psh, &pl, &ps)) { skip("pixel shader"); return; }
  pk.vs = vs.id; pk.fs = ps.id;
  Bound bound[MAX_SAMPLERS]; memset(bound, 0, sizeof bound);
  for (u32 i = 0; i < MAX_SAMPLERS; i++) if (ps.info.samplers & (1u << i)) {
    Texture9 *t = s->textures[i];
    u32 want = ps.info.sampler_types[i] == SM_TEX_CUBE ? GPU_BIND_TEXTURE_CUBE : ps.info.sampler_types[i] == SM_TEX_3D ? GPU_BIND_TEXTURE_3D : GPU_BIND_TEXTURE_2D;
    if (!t) { UNSURE_ONCE("Direct3D 9: a pixel shader samples s%u, which has no texture: not drawn", i); skip("no texture"); return; }
    if (bind_kind(t) != want) { UNSURE_ONCE("Direct3D 9: s%u is a texture of another kind than the shader declares: not drawn", i); skip("texture kind"); return; }
    pk.sampler_mask |= (u16)(1u << i); pk.kinds[i] = (u8)want; bound[i].tex = t; bound[i].samp = s->samp[i];
  }
  pk.uniform_bytes = (u16)SM_UNIFORM_BYTES(vs.info.float_count);
  draw_with(d, &pk, streams, bound, type, base, first, primitives, indexed, sm_fill, &vs.info);
}

void d3d9_gpu_draw(Device9 *d, u32 type, s32 base, u32 first, u32 primitives, int indexed) {
  const State9 *s = &d->state;
  FfpVertexKey vk = {0}; FfpFragmentKey fk = {0}; PipelineKey pk; memset(&pk, 0, sizeof pk);
  stat.draws++;
  if (s->vertex_shader || s->pixel_shader) { draw_shaders(d, type, base, first, primitives, indexed); return; }

  // ---- the fragment key: stage 0, the alpha test, fog ----
  const u32 *tss = s->tss[0];
  Texture9 *tex = s->textures[0];
  if (tex && tex->format != FMT_A8R8G8B8 && tex->format != FMT_X8R8G8B8) { UNSURE_ONCE("Direct3D 9: a texture of format %u is not sampled: drawn as if none were bound", tex->format); tex = 0; }
  fk.color_op = (u8)tss[TSS_COLOROP];
  if (fk.color_op != 1) {   // D3DTOP_DISABLE: nothing else of the stage counts
    if (tss[TSS_COLOROP] > 255 || tss[TSS_ALPHAOP] > 255 || tss[TSS_COLORARG1] > 255 || tss[TSS_COLORARG2] > 255 || tss[TSS_ALPHAARG1] > 255 || tss[TSS_ALPHAARG2] > 255) { UNSURE_ONCE("Direct3D 9: texture stage 0 with operations %u / %u is not drawn", tss[TSS_COLOROP], tss[TSS_ALPHAOP]); skip("stage"); return; }
    fk.color_arg1 = (u8)tss[TSS_COLORARG1]; fk.color_arg2 = (u8)tss[TSS_COLORARG2];
    fk.alpha_op = (u8)tss[TSS_ALPHAOP]; fk.alpha_arg1 = (u8)tss[TSS_ALPHAARG1]; fk.alpha_arg2 = (u8)tss[TSS_ALPHAARG2];
    fk.texture = !tex ? FFP_TEXTURE_NONE : tex->o.kind == K_VOLUMETEXTURE ? FFP_TEXTURE_3D : FFP_TEXTURE_2D;
    if (!ffp_fragment_samples(&fk)) fk.texture = FFP_TEXTURE_NONE; else fk.texture_opaque = tex->format == FMT_X8R8G8B8;
    if (s->tss[1][TSS_COLOROP] != 1) UNSURE_ONCE("Direct3D 9: texture stage 1 is on (operation %u): only stage 0 is drawn", s->tss[1][TSS_COLOROP]);
  }
  int samples = fk.texture != FFP_TEXTURE_NONE;
  if (s->rs[RS_ALPHATESTENABLE] && s->rs[RS_ALPHAFUNC] >= 1 && s->rs[RS_ALPHAFUNC] < 8) fk.alpha_test = (u8)s->rs[RS_ALPHAFUNC];
  fk.fog = s->rs[RS_FOGENABLE] != 0; fk.specular_add = s->rs[RS_SPECULARENABLE] != 0;

  // ---- the vertex layout: the declaration's elements onto the shader's locations ----
  const Declaration9 *decl = s->declaration; if (!decl) { skip("no declaration"); return; }
  u32 coord_set = tss[TSS_TEXCOORDINDEX] & 0xFFFF, coord_gen = tss[TSS_TEXCOORDINDEX] >> 16;
  u8 streams[MAX_VB]; u32 located = 0;
  for (u32 i = 0; i + 1 < decl->count; i++) {
    const Element9 *e = &decl->elements[i]; int loc = -1; u32 format = 0;
    if (e->stream == 0xFF) break;
    switch (e->usage) {
    case USE_POSITIONT: case USE_POSITION:
      if (e->usage_index) break;
      if (e->usage == USE_POSITIONT && e->type == DECL_FLOAT4) { loc = FFP_LOC_POSITION; format = DXGI_R32G32B32A32_FLOAT; vk.pretransformed = 1; }
      else if (e->usage == USE_POSITION && e->type == DECL_FLOAT3) { loc = FFP_LOC_POSITION; format = DXGI_R32G32B32_FLOAT; }
      else { UNSURE_ONCE("Direct3D 9: a vertex position of declaration type %u is not drawn", e->type); skip("position"); return; }
      break;
    case USE_BLENDWEIGHT: if (!e->usage_index && e->type <= DECL_FLOAT3) { loc = FFP_LOC_WEIGHTS; format = float_format[e->type]; vk.weights = (u8)(e->type + 1); } break;
    case USE_NORMAL: if (!e->usage_index && e->type == DECL_FLOAT3) { loc = FFP_LOC_NORMAL; format = DXGI_R32G32B32_FLOAT; vk.normal = 1; } break;
    case USE_PSIZE: if (!e->usage_index && e->type == DECL_FLOAT1) { loc = FFP_LOC_PSIZE; format = DXGI_R32_FLOAT; vk.psize = 1; } break;
    case USE_COLOR:
      if (e->type != DECL_D3DCOLOR) break;
      if (e->usage_index == 0) { loc = FFP_LOC_DIFFUSE; vk.diffuse = 1; } else if (e->usage_index == 1) { loc = FFP_LOC_SPECULAR; vk.specular = 1; }
      format = DXGI_R8G8B8A8_UNORM;   // the bytes as they are; the shader puts them in order
      break;
    case USE_TEXCOORD: if (e->usage_index == coord_set && e->type <= DECL_FLOAT4) { loc = FFP_LOC_TEX; format = float_format[e->type]; vk.tex = (u8)(e->type + 1); } break;
    default: break;   // tangents, binormals, blend indices: nothing in the fixed-function pipeline reads them here
    }
    if (loc < 0 || (located & 1u << loc)) continue;   // a second element for the same input: the first counts
    located |= 1u << loc;
    u32 slot = 0; while (slot < pk.buffers && streams[slot] != e->stream) slot++;
    if (slot == pk.buffers) { if (slot == MAX_VB || e->stream >= STREAMS) { UNSURE_ONCE("Direct3D 9: a vertex declaration over more than %u streams is not drawn", MAX_VB); skip("streams"); return; } streams[pk.buffers++] = (u8)e->stream; }
    pk.attr[pk.attributes].location = (u8)loc; pk.attr[pk.attributes].buffer = (u8)slot; pk.attr[pk.attributes].format = (u8)format; pk.attr[pk.attributes].offset = e->offset; pk.attributes++;
  }
  if (!(located & 1u << FFP_LOC_POSITION)) { skip("no position"); return; }

  // ---- the vertex key: transform, lighting, fog, texture coordinates, points ----
  if (!vk.pretransformed) {
    u32 blend = s->rs[RS_VERTEXBLEND];
    if (blend > 3 && blend != 256) UNSURE_ONCE("Direct3D 9: vertex blending mode %u (tweening) is drawn as none", blend);
    if (s->rs[RS_INDEXEDVERTEXBLENDENABLE]) UNSURE_ONCE("Direct3D 9: indexed vertex blending is drawn as plain blending");
    vk.matrices = (u8)(blend >= 1 && blend <= 3 ? blend + 1 : 1);
    vk.lighting = s->rs[RS_LIGHTING] != 0;
    if (vk.lighting) {
      u32 on = 0; for (u32 i = 0; i < s->light_count; i++) on += s->lights[i].set && s->lights[i].enabled;
      if (on > 8) { UNSURE_ONCE("Direct3D 9: %u lights are on; the 8 with the lowest numbers light the draw", on); on = 8; }
      vk.lights = (u8)on; vk.normalize = s->rs[RS_NORMALIZENORMALS] != 0;
      vk.specular_on = s->rs[RS_SPECULARENABLE] != 0; vk.local_viewer = vk.specular_on && s->rs[RS_LOCALVIEWER];
      // a colour comes from the vertex only when D3DRS_COLORVERTEX is on and the vertex has it; else from the material
      #define SOURCE(state) (u8)(!s->rs[RS_COLORVERTEX] ? FFP_SOURCE_MATERIAL : s->rs[state] == 1 && vk.diffuse ? FFP_SOURCE_DIFFUSE : s->rs[state] == 2 && vk.specular ? FFP_SOURCE_SPECULAR : FFP_SOURCE_MATERIAL)
      vk.diffuse_source = SOURCE(RS_DIFFUSEMATERIALSOURCE); vk.ambient_source = SOURCE(RS_AMBIENTMATERIALSOURCE);
      vk.specular_source = vk.specular_on ? SOURCE(RS_SPECULARMATERIALSOURCE) : 0; vk.emissive_source = SOURCE(RS_EMISSIVEMATERIALSOURCE);
      #undef SOURCE
    }
  } else vk.matrices = 1;
  if (fk.fog && vk.pretransformed) {
    // the factor is the vertex's own, in its specular colour's alpha; a vertex without one is not fogged
    if (vk.specular) vk.fog = FFP_FOG_LINEAR; else fk.fog = 0;
  } else if (fk.fog) {
    if (s->rs[RS_FOGTABLEMODE]) UNSURE_ONCE("Direct3D 9: table fog (mode %u) is drawn as vertex fog", s->rs[RS_FOGTABLEMODE]);
    u32 mode = s->rs[RS_FOGTABLEMODE] ? s->rs[RS_FOGTABLEMODE] : s->rs[RS_FOGVERTEXMODE];
    vk.fog = (u8)(mode <= 3 ? mode : 0);
    if (!vk.fog) fk.fog = 0;   // no mode: the factor would again be the specular colour's alpha; drawn without fog
    vk.range_fog = vk.fog && s->rs[RS_RANGEFOGENABLE];
  }
  if (samples) {
    if (coord_gen > FFP_TEXGEN_REFLECTION) UNSURE_ONCE("Direct3D 9: texture coordinate generation %u is drawn as the vertex's own coordinates", coord_gen);
    else if (!vk.pretransformed) vk.tex_gen = (u8)coord_gen;
    u32 flags = tss[TSS_TEXTURETRANSFORMFLAGS];
    if (!vk.pretransformed && (flags & 0xFF) >= 1 && (flags & 0xFF) <= 4) { vk.tex_transform = (u8)(flags & 0xFF); if ((flags & 256) && vk.tex_transform > 1) fk.projected = vk.tex_transform; }
  } else vk.tex = 0;
  if (type == PT_POINTLIST) { vk.point = 1; vk.point_scale = !vk.pretransformed && s->rs[RS_POINTSCALEENABLE]; vk.point_sprite = samples && s->rs[RS_POINTSPRITEENABLE]; }
  // what the key does not use is not fetched: fewer shaders and pipelines
  if (vk.matrices == 1) vk.weights = 0;
  if (!vk.lighting && vk.tex_gen != FFP_TEXGEN_NORMAL && vk.tex_gen != FFP_TEXGEN_REFLECTION) vk.normal = 0;
  if (!vk.point) vk.psize = 0;
  if (vk.tex_gen || vk.point_sprite) vk.tex = 0;
  u32 kept = 0;
  for (u32 i = 0; i < pk.attributes; i++) {
    u8 loc = pk.attr[i].location;
    if ((loc == FFP_LOC_WEIGHTS && !vk.weights) || (loc == FFP_LOC_NORMAL && !vk.normal) || (loc == FFP_LOC_PSIZE && !vk.psize) || (loc == FFP_LOC_TEX && !vk.tex)) continue;
    pk.attr[kept++] = pk.attr[i];
  }
  memset(&pk.attr[kept], 0, (pk.attributes - kept) * sizeof pk.attr[0]); pk.attributes = (u8)kept;

  pk.vs = vertex_shader(&vk); pk.fs = fragment_shader(&fk);
  if (!pk.vs || !pk.fs) { skip("no shader"); return; }

  pk.uniform_bytes = (u16)FFP_UNIFORM_BYTES(vk.lighting ? vk.lights : 0);
  Bound bound[MAX_SAMPLERS] = {{0}};
  if (samples) { pk.sampler_mask = 1; pk.kinds[0] = fk.texture == FFP_TEXTURE_3D ? GPU_BIND_TEXTURE_3D : GPU_BIND_TEXTURE_2D; bound[0].tex = tex; bound[0].samp = s->samp[0]; }
  draw_with(d, &pk, streams, bound, type, base, first, primitives, indexed, ffp_fill, &vk);
}

// ---- the frame's end ----
void d3d9_gpu_present(Device9 *d) {
  u32 mip, id = target(d->back_buffer, &mip);
  if (id) rt_gpu_present(id, d->frames, 1);
}
void d3d9_gpu_shot(const char *name) {
  if (!rt_gpu_ring) return;
  char text[64]; int n = snprintf(text, sizeof text, "shot %s", name);
  u8 *p = rt_gpu_rec_begin(GPU_R_MARK, (u32)n); memcpy(p, text, (size_t)n); rt_gpu_rec_end();
}
void d3d9_gpu_stats(void) {
  if (!rt_gpu_ring) return;
  rt_log("== Direct3D 9 on the GPU: %u vertex shaders, %u fragment shaders, %u pipelines, %u samplers; %llu draws (%llu left out) in %llu passes; %u buffers, %llu writes, %llu bytes; %u textures, %llu writes, %llu bytes; %llu bytes of uniforms",
         vertex_shader_count, fragment_shader_count, pipeline_count, sampler_count, stat.draws, stat.skipped, stat.passes, stat.buffers, stat.buffer_writes, stat.buffer_bytes, stat.textures, stat.texture_writes, stat.texture_bytes, stat.uniform_bytes);
}
void d3d9_gpu_finish(void) { rt_gpu_drain(); }
