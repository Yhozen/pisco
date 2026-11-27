// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL 1.1's fixed-function pipeline (with GL_ARB_multitexture's second unit and GL_EXT_texture_env_add) as WGSL:
// one vertex stage for every draw and a fragment stage made from a small key. A pure function of the key, built
// natively as well, so tests/gfx/gl-ffp.sh validates every key's text with naga.
//
// What the shaders take from OpenGL (the rest is in opengl32.c):
//   - matrices are OpenGL's, column by column, as WGSL reads a mat4x4f: `m * v` is OpenGL's product;
//   - clip space: OpenGL's z runs from -w to w, WebGPU's from 0 to w: the vertex stage moves it (z + w) / 2, and
//     scales x and y when the viewport is cut to the window (GlUniforms.view);
//   - texture environments per unit and per base format of the bound texture (GL 1.3 specification, table 3.22):
//     what a texture without colour (GL_ALPHA) or without alpha (GL_RGB, GL_LUMINANCE) gives is settled here;
//   - the alpha test, fog by the eye's distance (linear, exp, exp2) and one user clip plane are done per fragment.
#pragma once
#include <stddef.h>
#include <stdint.h>

// The vertex every draw sends (opengl32.c packs it): position xyzw, colour RGBA bytes, two sets of coordinates
enum { GL_LOC_POSITION = 0, GL_LOC_COLOR = 1, GL_LOC_TEX0 = 2, GL_LOC_TEX1 = 3 };
typedef struct { float pos[4]; uint8_t color[4]; float tex0[2], tex1[2]; } GlVertex;
_Static_assert(sizeof(GlVertex) == 36, "GlVertex is the vertex buffer's layout");

enum { GL_FFP_UNITS = 2 };
enum { GL_ENV_MODULATE = 0, GL_ENV_REPLACE, GL_ENV_DECAL, GL_ENV_BLEND, GL_ENV_ADD };
enum { GL_BASE_RGB = 0, GL_BASE_RGBA, GL_BASE_ALPHA };   // what the texture has: colour only, both, alpha only
enum { GL_FOG_OFF = 0, GL_FOG_LINEAR, GL_FOG_EXP, GL_FOG_EXP2 };
// ---- the fragment key: one byte a field, compared and printed as its bytes ----
typedef struct GlFragmentKey {
  uint8_t unit_on[GL_FFP_UNITS];   // the unit has GL_TEXTURE_2D on and a complete texture bound
  uint8_t env[GL_FFP_UNITS];       // GL_ENV_*
  uint8_t base[GL_FFP_UNITS];      // GL_BASE_*
  uint8_t alpha_test;              // 0 off, else the comparison 1 never ... 8 always (OpenGL's GL_NEVER + 1 ...)
  uint8_t fog;                     // GL_FOG_*
  uint8_t clip;                    // GL_CLIP_PLANE0 is on
  uint8_t pad[3];
} GlFragmentKey;
_Static_assert(sizeof(GlFragmentKey) == 12, "GlFragmentKey has no hidden padding");

// ---- the uniform block: the WGSL `U`, member for member ----
typedef struct GlUniforms {
  float mvp[16];          // projection x modelview
  float modelview[16];    // for the eye's distance (fog) and the clip plane
  float tex[GL_FFP_UNITS][16];   // the texture matrices
  float fog_color[4];
  float fog[4];           // start, end, 1 / (end - start), density
  float env_color[GL_FFP_UNITS][4];
  float misc[4];          // the alpha test's reference (0 to 1)
  float clip[4];          // clip plane 0, in eye coordinates
  float view[4];          // x' = x * view[0] + w * view[1], y' = y * view[2] + w * view[3]: OpenGL's viewport may reach
                          // past the window, WebGPU's may not; the part inside is WebGPU's and this keeps OpenGL's mapping
} GlUniforms;
_Static_assert(sizeof(GlUniforms) == 368, "GlUniforms is the WGSL block");

// Each writes WGSL text into `out` and returns its length, or 0 (with the reason in `out`) for a key out of range
size_t gl_vertex_wgsl(char *out, size_t cap);
size_t gl_fragment_wgsl(const GlFragmentKey *k, char *out, size_t cap);
void gl_fragment_describe(const GlFragmentKey *k, char *out, size_t cap);
