// SPDX-License-Identifier: GPL-2.0-or-later
// OpenGL 1.1's fixed-function pipeline written out as WGSL (gl_ffp.h). Nothing here knows the runtime: the file is
// also compiled natively for tests/gfx/gl-ffp.sh, which validates what it writes with naga.
//
// The formulas are OpenGL's (the OpenGL 1.3 specification: 2.10 coordinate transformations, 3.10 fog, 3.8.13
// texture environments and table 3.22, 4.1.4 the alpha test), written from that description.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gl_ffp.h"

typedef struct { char *p; size_t n, cap; int full; } Text;
static void put(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Text *t, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= t->cap - t->n) { t->full = 1; t->n = t->cap - 1; } else t->n += (size_t)n;
}
static size_t done(Text *t) { if (t->full) { snprintf(t->p, t->cap, "the text does not fit in %zu bytes", t->cap); return 0; } return t->n; }

static const char uniforms[] =
  "struct U {\n"
  "  mvp: mat4x4f, modelview: mat4x4f, tex: array<mat4x4f, 2>,\n"
  "  fog_color: vec4f, fog: vec4f, env_color: array<vec4f, 2>, misc: vec4f, clip: vec4f, view: vec4f,\n"
  "}\n@group(0) @binding(0) var<uniform> u: U;\n"
  "struct V {\n  @builtin(position) position: vec4f,\n  @location(0) color: vec4f,\n  @location(1) tex0: vec4f,\n  @location(2) tex1: vec4f,\n"
  "  @location(3) eye: vec4f,\n}\n";

size_t gl_vertex_wgsl(char *out, size_t cap) {
  Text t = {out, 0, cap, 0};
  put(&t, "%s", uniforms);
  put(&t, "struct VIn {\n  @location(%d) position: vec4f,\n  @location(%d) color: vec4f,\n  @location(%d) tex0: vec2f,\n  @location(%d) tex1: vec2f,\n}\n",
      GL_LOC_POSITION, GL_LOC_COLOR, GL_LOC_TEX0, GL_LOC_TEX1);
  put(&t, "@vertex fn main(v: VIn) -> V {\n"
          "  var o: V;\n"
          "  var p = u.mvp * v.position;\n"
          "  p.z = (p.z + p.w) * 0.5;\n"      // OpenGL's -w..w -> WebGPU's 0..w
          "  p.x = p.x * u.view.x + p.w * u.view.y;\n"
          "  p.y = p.y * u.view.z + p.w * u.view.w;\n"
          "  o.position = p;\n"
          "  o.color = v.color;\n"
          "  o.tex0 = u.tex[0] * vec4f(v.tex0, 0.0, 1.0);\n"
          "  o.tex1 = u.tex[1] * vec4f(v.tex1, 0.0, 1.0);\n"
          "  o.eye = u.modelview * v.position;\n"
          "  return o;\n}\n");
  return done(&t);
}

static const char *const env_names[] = {"modulate", "replace", "decal", "blend", "add"};
static const char *const base_names[] = {"rgb", "rgba", "alpha"};
void gl_fragment_describe(const GlFragmentKey *k, char *out, size_t cap) {
  Text t = {out, 0, cap, 0};
  for (int i = 0; i < GL_FFP_UNITS; i++) if (k->unit_on[i]) put(&t, "unit %d %s %s, ", i, k->env[i] < 5 ? env_names[k->env[i]] : "?", k->base[i] < 3 ? base_names[k->base[i]] : "?");
  if (k->alpha_test) put(&t, "alpha test %u, ", k->alpha_test);
  if (k->fog) put(&t, "fog %u, ", k->fog);
  if (k->clip) put(&t, "clip plane, ");
  if (t.n >= 2) out[t.n - 2] = 0; else snprintf(out, cap, "the vertex colour");
}

size_t gl_fragment_wgsl(const GlFragmentKey *k, char *out, size_t cap) {
  for (int i = 0; i < GL_FFP_UNITS; i++) if (k->unit_on[i] && (k->env[i] > GL_ENV_ADD || k->base[i] > GL_BASE_ALPHA)) { snprintf(out, cap, "unit %d: environment %u, base format %u", i, k->env[i], k->base[i]); return 0; }
  if (k->alpha_test > 8 || k->fog > GL_FOG_EXP2) { snprintf(out, cap, "alpha test %u, fog %u", k->alpha_test, k->fog); return 0; }
  Text t = {out, 0, cap, 0};
  put(&t, "%s", uniforms);
  for (int i = 0; i < GL_FFP_UNITS; i++) if (k->unit_on[i])
    put(&t, "@group(0) @binding(%d) var t%d: texture_2d<f32>;\n@group(0) @binding(%d) var s%d: sampler;\n", 1 + 2 * i, i, 2 + 2 * i, i);
  put(&t, "@fragment fn main(v: V) -> @location(0) vec4f {\n");
  if (k->clip) put(&t, "  if (dot(u.clip, v.eye) < 0.0) { discard; }\n");
  put(&t, "  var c = v.color;\n");
  // a unit's texture: sampled at the coordinates its matrix made (divided by q, as OpenGL does)
  for (int i = 0; i < GL_FFP_UNITS; i++) {
    if (!k->unit_on[i]) continue;
    put(&t, "  let x%d = textureSample(t%d, s%d, v.tex%d.xy / v.tex%d.w);\n", i, i, i, i, i);
    const char *rgb = "c.rgb", *a = "c.a";   // what stays as it was
    char rgb_e[160], a_e[96];
    int has_c = k->base[i] != GL_BASE_ALPHA, has_a = k->base[i] != GL_BASE_RGB;
    switch (k->env[i]) {
    case GL_ENV_REPLACE:
      if (has_c) { snprintf(rgb_e, sizeof rgb_e, "x%d.rgb", i); rgb = rgb_e; }
      if (has_a) { snprintf(a_e, sizeof a_e, "x%d.a", i); a = a_e; }
      break;
    case GL_ENV_MODULATE:
      if (has_c) { snprintf(rgb_e, sizeof rgb_e, "c.rgb * x%d.rgb", i); rgb = rgb_e; }
      if (has_a) { snprintf(a_e, sizeof a_e, "c.a * x%d.a", i); a = a_e; }
      break;
    case GL_ENV_DECAL:   // an RGB texture replaces the colour; an RGBA one is laid over it by its alpha; alpha stays
      if (k->base[i] == GL_BASE_RGB) { snprintf(rgb_e, sizeof rgb_e, "x%d.rgb", i); rgb = rgb_e; }
      else if (k->base[i] == GL_BASE_RGBA) { snprintf(rgb_e, sizeof rgb_e, "mix(c.rgb, x%d.rgb, x%d.a)", i, i); rgb = rgb_e; }
      break;
    case GL_ENV_BLEND:
      if (has_c) { snprintf(rgb_e, sizeof rgb_e, "mix(c.rgb, u.env_color[%d].rgb, x%d.rgb)", i, i); rgb = rgb_e; }
      if (has_a) { snprintf(a_e, sizeof a_e, "c.a * x%d.a", i); a = a_e; }
      break;
    case GL_ENV_ADD:
      if (has_c) { snprintf(rgb_e, sizeof rgb_e, "c.rgb + x%d.rgb", i); rgb = rgb_e; }
      if (has_a) { snprintf(a_e, sizeof a_e, "c.a * x%d.a", i); a = a_e; }
      break;
    }
    put(&t, "  c = clamp(vec4f(%s, %s), vec4f(0.0), vec4f(1.0));\n", rgb, a);
  }
  if (k->alpha_test) {
    static const char *const pass[9] = {"", "false", "ca < r", "ca == r", "ca <= r", "ca > r", "ca != r", "ca >= r", "true"};
    // the alpha is compared as the colour buffer would hold it, at 8 bits; the reference is too (opengl32.c)
    put(&t, "  let r = u.misc.x;\n  let ca = round(c.a * 255.0) / 255.0;\n");
    put(&t, "  if (!(%s)) { discard; }\n", pass[k->alpha_test]);
  }
  if (k->fog) {
    put(&t, "  let z = abs(v.eye.z / v.eye.w);\n");
    if (k->fog == GL_FOG_LINEAR) put(&t, "  let f = clamp((u.fog.y - z) * u.fog.z, 0.0, 1.0);\n");
    else if (k->fog == GL_FOG_EXP) put(&t, "  let f = clamp(exp(-u.fog.w * z), 0.0, 1.0);\n");
    else put(&t, "  let f = clamp(exp(-(u.fog.w * z) * (u.fog.w * z)), 0.0, 1.0);\n");
    put(&t, "  c = vec4f(mix(u.fog_color.rgb, c.rgb, f), c.a);\n");
  }
  put(&t, "  return c;\n}\n");
  return done(&t);
}
