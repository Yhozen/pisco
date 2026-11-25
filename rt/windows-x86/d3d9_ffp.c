// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9's fixed-function pipeline written out as WGSL (d3d9_ffp.h). Nothing here knows the runtime: the
// file is also compiled natively for tests/gfx/d3d9-ffp.sh, which validates what it writes with naga.
//
// The formulas are Direct3D 9's documented ones ("Mathematics of Lighting", "Fog Formulas", "Point Sprites",
// "Texture Blending" in the DirectX 9 SDK), written from that description. Where the documentation is silent,
// the behaviour follows what Wine's and DXVK's tests of Windows found, noted at each place.
//
// What differs between Direct3D 9 and WebGPU and is settled in the shaders (the rest is in d3d9_gpu.c):
//   - a D3DCOLOR in a vertex is four bytes B G R A: the attribute arrives as (b, g, r, a) and is swizzled once;
//   - pixel centres: Direct3D 9's are at whole numbers, WebGPU's at halves. A position is moved by half a
//     pixel (U.half_pixel), and XYZRHW positions go through U.screen_scale / screen_offset, which include it;
//   - clip space is the same (x, y in -w..w, z in 0..w), and so are the top-left fill rule and the winding;
//   - a point is one pixel in WebGPU: a point list is drawn as instanced quads and sized here;
//   - WebGPU has no alpha test: the fragment stage discards.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "d3d9_ffp.h"

typedef struct { char *p; size_t n, cap; int full; } Text;
static void put(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Text *t, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= t->cap - t->n) { t->full = 1; t->n = t->cap - 1; } else t->n += (size_t)n;
}
static size_t refuse(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static size_t refuse(char *out, size_t cap, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); vsnprintf(out, cap, fmt, ap); va_end(ap);
  return 0;
}

// The uniform block as the shaders declare it: FfpUniforms and FfpLight, member for member. A stage that reads
// no lights leaves the array out (the block it is given may be longer than it declares).
static void put_uniforms(Text *t, unsigned lights) {
  if (lights) put(t, "struct Light { diffuse: vec4f, specular: vec4f, ambient: vec4f, position: vec4f, direction: vec4f, attenuation: vec4f, spot: vec4f }\n");
  put(t, "struct U {\n"
         "  world_view: array<mat4x4f, 4>, normal: array<mat4x4f, 4>, projection: mat4x4f, tex_matrix: mat4x4f,\n"
         "  screen_scale: vec4f, screen_offset: vec4f, half_pixel: vec4f,\n"
         "  material_diffuse: vec4f, material_ambient: vec4f, material_specular: vec4f, material_emissive: vec4f,\n"
         "  ambient: vec4f, fog_color: vec4f, fog: vec4f, point: vec4f, point_scale: vec4f, misc: vec4f, factor: vec4f,\n");
  if (lights) put(t, "  lights: array<Light, %u>,\n", lights);
  put(t, "}\n@group(0) @binding(0) var<uniform> u: U;\n");
}
_Static_assert(sizeof(FfpLight) == 112 && offsetof(FfpUniforms, projection) == 512 && offsetof(FfpUniforms, screen_scale) == 640 &&
               offsetof(FfpUniforms, lights) == 864 && sizeof(FfpUniforms) == 864 + 8 * 112, "FfpUniforms is the WGSL block");
// What both stages agree on between them
static const char varyings[] = "  @location(0) diffuse: vec4f,\n  @location(1) specular: vec4f,\n  @location(2) tex: vec4f,\n  @location(3) fog: f32,\n";

// ---- the vertex stage ----
static const char *const float_types[5] = {"", "f32", "vec2f", "vec3f", "vec4f"};
static const char *source(unsigned s, const char *material) { return s == FFP_SOURCE_DIFFUSE ? "vd" : s == FFP_SOURCE_SPECULAR ? "vs" : material; }
size_t ffp_vertex_wgsl(const FfpVertexKey *k, char *out, size_t cap) {
  if (k->weights > 3 || k->matrices < 1 || k->matrices > 4) return refuse(out, cap, "vertex blending with %u weights and %u matrices", k->weights, k->matrices);
  if (k->tex > 4 || k->lights > 8 || k->fog > FFP_FOG_LINEAR || k->tex_gen > FFP_TEXGEN_REFLECTION || k->tex_transform > 4) return refuse(out, cap, "a vertex key out of range");
  if (k->diffuse_source > 2 || k->ambient_source > 2 || k->specular_source > 2 || k->emissive_source > 2) return refuse(out, cap, "a material source out of range");
  int lit = k->lighting && !k->pretransformed;
  unsigned lights = lit ? k->lights : 0;
  Text t = {out, 0, cap, 0};
  put_uniforms(&t, lights);
  put(&t, "struct VIn {\n  @location(%d) position: %s,\n", FFP_LOC_POSITION, k->pretransformed ? "vec4f" : "vec3f");
  if (k->weights) put(&t, "  @location(%d) weights: %s,\n", FFP_LOC_WEIGHTS, float_types[k->weights]);
  if (k->normal) put(&t, "  @location(%d) normal: vec3f,\n", FFP_LOC_NORMAL);
  if (k->psize) put(&t, "  @location(%d) psize: f32,\n", FFP_LOC_PSIZE);
  if (k->diffuse) put(&t, "  @location(%d) diffuse: vec4f,\n", FFP_LOC_DIFFUSE);
  if (k->specular) put(&t, "  @location(%d) specular: vec4f,\n", FFP_LOC_SPECULAR);
  if (k->tex) put(&t, "  @location(%d) tex: %s,\n", FFP_LOC_TEX, float_types[k->tex]);
  put(&t, "}\nstruct VOut {\n  @builtin(position) position: vec4f,\n%s}\n", varyings);
  put(&t, "@vertex fn main(v: VIn%s) -> VOut {\n  var o: VOut;\n", k->point ? ", @builtin(vertex_index) corner: u32" : "");
  // the vertex's own colours: D3DCOLOR is B G R A in memory; a vertex without them is white, with no specular
  put(&t, "  let vd = %s;\n  let vs = %s;\n", k->diffuse ? "v.diffuse.zyxw" : "vec4f(1.0)", k->specular ? "v.specular.zyxw" : "vec4f(0.0)");
  if (k->pretransformed) {
    // Already in screen space: x, y in pixels of the target, z the depth, w = 1 / (the w it was divided by).
    // An rhw of 0 counts as 1.
    put(&t, "  let rhw = select(1.0, v.position.w, v.position.w != 0.0);\n"
            "  let w = 1.0 / rhw;\n"
            "  let clip = vec4f((v.position.xyz * u.screen_scale.xyz + u.screen_offset.xyz) * w, w);\n"
            "  let eye = vec4f(0.0, 0.0, 0.0, 1.0);\n  let n = vec3f(0.0);\n");
  } else {
    put(&t, "  let p = vec4f(v.position, 1.0);\n");
    if (k->matrices == 1) {
      put(&t, "  let eye = u.world_view[0] * p;\n");
      if (k->normal) put(&t, "  var n = (u.normal[0] * vec4f(v.normal, 0.0)).xyz;\n");
    } else {
      // Vertex blending: matrix i weighs by weight i of the vertex, the last by what is left of 1
      static const char *const comp[3] = {"x", "y", "z"};
      char last[96] = "1.0";
      for (unsigned i = 0; i + 1 < k->matrices; i++) {
        if (i < k->weights) put(&t, "  let b%u = v.weights%s%s;\n", i, k->weights > 1 ? "." : "", k->weights > 1 ? comp[i] : "");
        else put(&t, "  let b%u = 0.0;\n", i);
        snprintf(last + strlen(last), sizeof last - strlen(last), " - b%u", i);
      }
      put(&t, "  let b%u = %s;\n", k->matrices - 1, last);
      put(&t, "  let eye = ");
      for (unsigned i = 0; i < k->matrices; i++) put(&t, "%s(u.world_view[%u] * p) * b%u", i ? " + " : "", i, i);
      put(&t, ";\n");
      if (k->normal) {
        put(&t, "  var n = ");
        for (unsigned i = 0; i < k->matrices; i++) put(&t, "%s(u.normal[%u] * vec4f(v.normal, 0.0)).xyz * b%u", i ? " + " : "", i, i);
        put(&t, ";\n");
      }
    }
    if (!k->normal) put(&t, "  var n = vec3f(0.0);\n");
    else if (k->normalize) put(&t, "  n = normalize(n);\n");
    // the half pixel between Direct3D 9's pixel centres and WebGPU's
    put(&t, "  let c0 = u.projection * eye;\n  let clip = vec4f(c0.xy + u.half_pixel.xy * c0.w, c0.zw);\n");
  }
  if (lit) {
    // For each light: the direction to it, its attenuation (0 past its range; a spot's cone), then
    //   diffuse  = emissive + ambient colour * (global ambient + sum of light ambient) + diffuse colour * sum of (N.L * light diffuse)
    //   specular = specular colour * sum of ((N.H) ^ power * light specular)
    // with each colour the material's or one of the vertex's (the sources), and the diffuse colour's alpha.
    put(&t, "  var dsum = vec3f(0.0);\n  var asum = vec3f(0.0);\n  var ssum = vec3f(0.0);\n");
    if (k->specular_on) put(&t, "  let to_eye = %s;\n", k->local_viewer ? "normalize(-eye.xyz)" : "vec3f(0.0, 0.0, -1.0)");
    if (lights) {
      put(&t, "  for (var i = 0u; i < %uu; i++) {\n"
              "    let l = u.lights[i];\n"
              "    var dir = -l.direction.xyz;\n"
              "    var att = 1.0;\n"
              "    if (l.position.w != 3.0) {\n"
              "      let d = l.position.xyz - eye.xyz;\n"
              "      let dist = length(d);\n"
              "      dir = d / max(dist, 1e-20);\n"
              "      att = 0.0;\n"
              "      if (dist <= l.direction.w) { att = 1.0 / max(l.attenuation.x + l.attenuation.y * dist + l.attenuation.z * dist * dist, 1e-20); }\n"
              "      if (l.position.w == 2.0) {\n"
              "        let rho = dot(-dir, l.direction.xyz);\n"
              "        if (rho <= l.spot.y) { att = 0.0; }\n"
              "        else if (rho <= l.spot.x) { att = att * pow(max((rho - l.spot.y) / max(l.spot.x - l.spot.y, 1e-20), 0.0), l.attenuation.w); }\n"
              "      }\n"
              "    }\n"
              "    let nl = max(dot(n, dir), 0.0);\n"
              "    dsum += l.diffuse.rgb * (nl * att);\n"
              "    asum += l.ambient.rgb * att;\n", lights);
      if (k->specular_on) put(&t, "    if (nl > 0.0) { ssum += l.specular.rgb * (pow(max(dot(n, normalize(dir + to_eye)), 0.0), u.misc.y) * att); }\n");
      put(&t, "  }\n");
    }
    const char *dc = source(k->diffuse_source, "u.material_diffuse");
    put(&t, "  o.diffuse = clamp(vec4f(%s.rgb + %s.rgb * (u.ambient.rgb + asum) + %s.rgb * dsum, %s.a), vec4f(0.0), vec4f(1.0));\n",
        source(k->emissive_source, "u.material_emissive"), source(k->ambient_source, "u.material_ambient"), dc, dc);
    if (k->specular_on) put(&t, "  o.specular = clamp(vec4f(%s.rgb * ssum, 0.0), vec4f(0.0), vec4f(1.0));\n", source(k->specular_source, "u.material_specular"));
    else put(&t, "  o.specular = vec4f(0.0);\n");
  } else put(&t, "  o.diffuse = vd;\n  o.specular = vs;\n");
  // Vertex fog: 1 is no fog. Pretransformed vertices bring their own factor in the specular colour's alpha
  // (none without one).
  if (!k->fog) put(&t, "  o.fog = 1.0;\n");
  else if (k->pretransformed) put(&t, "  o.fog = %s;\n", k->specular ? "vs.a" : "1.0");
  else {
    put(&t, "  let fog_d = %s;\n", k->range_fog ? "length(eye.xyz)" : "abs(eye.z)");
    if (k->fog == FFP_FOG_LINEAR) put(&t, "  o.fog = clamp((u.fog.y - fog_d) * u.fog.z, 0.0, 1.0);\n");
    else if (k->fog == FFP_FOG_EXP) put(&t, "  o.fog = clamp(exp(-u.fog.w * fog_d), 0.0, 1.0);\n");
    else put(&t, "  o.fog = clamp(exp(-(u.fog.w * fog_d) * (u.fog.w * fog_d)), 0.0, 1.0);\n");
  }
  // Texture coordinates of stage 0
  if (k->tex_gen == FFP_TEXGEN_NORMAL) put(&t, "  var tc = vec4f(n, 1.0);\n");
  else if (k->tex_gen == FFP_TEXGEN_POSITION) put(&t, "  var tc = vec4f(eye.xyz, 1.0);\n");
  else if (k->tex_gen == FFP_TEXGEN_REFLECTION) put(&t, "  var tc = vec4f(reflect(normalize(eye.xyz), n), 1.0);\n");
  else {
    // A texture matrix sees coordinates padded with a 1 right after the last one the vertex gives, then zeros
    static const char *const plain[5] = {"vec4f(0.0, 0.0, 0.0, 1.0)", "vec4f(v.tex, 0.0, 0.0, 1.0)", "vec4f(v.tex, 0.0, 1.0)", "vec4f(v.tex, 1.0)", "v.tex"};
    static const char *const padded[5] = {"vec4f(0.0, 0.0, 0.0, 1.0)", "vec4f(v.tex, 1.0, 0.0, 0.0)", "vec4f(v.tex, 1.0, 0.0)", "vec4f(v.tex, 1.0)", "v.tex"};
    put(&t, "  var tc = %s;\n", k->tex_transform ? padded[k->tex] : plain[k->tex]);
  }
  if (k->tex_transform) put(&t, "  tc = u.tex_matrix * tc;\n");
  if (k->point) {
    // A point as a quad: four vertices of a strip for each point, `corner` 0 to 3. Its side in pixels is the
    // vertex's size or D3DRS_POINTSIZE; with scaling, times the viewport's height over the distance term.
    put(&t, "  let cq = vec2f(f32(corner & 1u), f32((corner >> 1u) & 1u));\n  var size = %s;\n", k->psize ? "v.psize" : "u.point.x");
    if (k->point_scale && !k->pretransformed)
      put(&t, "  let pd = length(eye.xyz);\n  size = u.point.w * size * sqrt(1.0 / max(u.point_scale.x + u.point_scale.y * pd + u.point_scale.z * pd * pd, 1e-20));\n");
    put(&t, "  size = clamp(size, u.point.y, u.point.z);\n"
            "  o.position = vec4f(clip.xy + (cq - vec2f(0.5)) * (2.0 * size * clip.w) * u.half_pixel.xy, clip.zw);\n");
    if (k->point_sprite) put(&t, "  tc = vec4f(cq, 0.0, 1.0);\n");
  } else put(&t, "  o.position = clip;\n");
  put(&t, "  o.tex = tc;\n  return o;\n}\n");
  if (t.full) return refuse(out, cap, "the vertex shader does not fit %zu bytes", cap);
  return t.n;
}

// ---- the fragment stage ----
enum { TOP_DISABLE = 1, TOP_SELECTARG1, TOP_SELECTARG2, TOP_MODULATE, TOP_MODULATE2X, TOP_MODULATE4X, TOP_ADD, TOP_ADDSIGNED, TOP_ADDSIGNED2X, TOP_SUBTRACT,
       TOP_ADDSMOOTH, TOP_BLENDDIFFUSEALPHA, TOP_BLENDTEXTUREALPHA, TOP_BLENDFACTORALPHA, TOP_BLENDTEXTUREALPHAPM, TOP_BLENDCURRENTALPHA };
enum { TA_DIFFUSE = 0, TA_CURRENT, TA_TEXTURE, TA_TFACTOR, TA_SPECULAR, TA_SELECTMASK = 0xF, TA_COMPLEMENT = 0x10, TA_ALPHAREPLICATE = 0x20 };
static int op_known(unsigned op) { return op >= TOP_DISABLE && op <= TOP_BLENDCURRENTALPHA && op != TOP_BLENDTEXTUREALPHAPM; }
static int op_uses1(unsigned op) { return op != TOP_DISABLE && op != TOP_SELECTARG2; }
static int op_uses2(unsigned op) { return op != TOP_DISABLE && op != TOP_SELECTARG1; }
// Does this operation read the stage's texture?
static int op_reads_texture(unsigned op, unsigned a1, unsigned a2) {
  if (op == TOP_DISABLE) return 0;
  return (op_uses1(op) && (a1 & TA_SELECTMASK) == TA_TEXTURE) || (op_uses2(op) && (a2 & TA_SELECTMASK) == TA_TEXTURE) || op == TOP_BLENDTEXTUREALPHA;
}
int ffp_fragment_samples(const FfpFragmentKey *k) {
  if (!k->texture || k->color_op == TOP_DISABLE) return 0;
  return op_reads_texture(k->color_op, k->color_arg1, k->color_arg2) || op_reads_texture(k->alpha_op, k->alpha_arg1, k->alpha_arg2);
}
// An argument as an expression: a vec3f for colour, an f32 for alpha. Stage 0's "current" is the diffuse colour.
static int arg(char *e, size_t cap, unsigned a, int alpha) {
  const char *base;
  switch (a & TA_SELECTMASK) {
  case TA_DIFFUSE: case TA_CURRENT: base = "f.diffuse"; break;
  case TA_TEXTURE: base = "texel"; break;
  case TA_TFACTOR: base = "u.factor"; break;
  case TA_SPECULAR: base = "f.specular"; break;
  default: return 0;
  }
  if (a & ~(unsigned)(TA_SELECTMASK | TA_COMPLEMENT | TA_ALPHAREPLICATE)) return 0;
  const char *open = (a & TA_COMPLEMENT) ? (alpha ? "(1.0 - " : "(vec3f(1.0) - ") : "", *close = (a & TA_COMPLEMENT) ? ")" : "";
  if (alpha) snprintf(e, cap, "%s%s.a%s", open, base, close);
  else if (a & TA_ALPHAREPLICATE) snprintf(e, cap, "%svec3f(%s.a)%s", open, base, close);
  else snprintf(e, cap, "%s%s.rgb%s", open, base, close);
  return 1;
}
// One operation over its two arguments, as an expression
static int operation(char *e, size_t cap, unsigned op, unsigned a1, unsigned a2, int alpha) {
  char x[64] = "", y[64] = "";
  if (op_uses1(op) && !arg(x, sizeof x, a1, alpha)) return 0;
  if (op_uses2(op) && !arg(y, sizeof y, a2, alpha)) return 0;
  const char *half = alpha ? "0.5" : "vec3f(0.5)";
  switch (op) {
  case TOP_SELECTARG1: snprintf(e, cap, "%s", x); break;
  case TOP_SELECTARG2: snprintf(e, cap, "%s", y); break;
  case TOP_MODULATE: snprintf(e, cap, "%s * %s", x, y); break;
  case TOP_MODULATE2X: snprintf(e, cap, "%s * %s * 2.0", x, y); break;
  case TOP_MODULATE4X: snprintf(e, cap, "%s * %s * 4.0", x, y); break;
  case TOP_ADD: snprintf(e, cap, "%s + %s", x, y); break;
  case TOP_ADDSIGNED: snprintf(e, cap, "%s + %s - %s", x, y, half); break;
  case TOP_ADDSIGNED2X: snprintf(e, cap, "(%s + %s - %s) * 2.0", x, y, half); break;
  case TOP_SUBTRACT: snprintf(e, cap, "%s - %s", x, y); break;
  case TOP_ADDSMOOTH: snprintf(e, cap, "%s + %s - %s * %s", x, y, x, y); break;
  case TOP_BLENDDIFFUSEALPHA: case TOP_BLENDCURRENTALPHA: snprintf(e, cap, "mix(%s, %s, f.diffuse.a)", y, x); break;
  case TOP_BLENDTEXTUREALPHA: snprintf(e, cap, "mix(%s, %s, texel.a)", y, x); break;
  case TOP_BLENDFACTORALPHA: snprintf(e, cap, "mix(%s, %s, u.factor.a)", y, x); break;
  default: return 0;
  }
  return 1;
}
size_t ffp_fragment_wgsl(const FfpFragmentKey *k, char *out, size_t cap) {
  if (k->texture != FFP_TEXTURE_NONE && k->texture != FFP_TEXTURE_2D && k->texture != FFP_TEXTURE_3D) return refuse(out, cap, "a texture of kind %u", k->texture);
  if (k->alpha_test > 8 || k->projected > 4 || k->projected == 1) return refuse(out, cap, "a fragment key out of range");
  unsigned cop = k->color_op, ca1 = k->color_arg1, ca2 = k->color_arg2, aop = k->alpha_op, aa1 = k->alpha_arg1, aa2 = k->alpha_arg2;
  if (!op_known(cop) || !op_known(aop)) return refuse(out, cap, "texture operation %u / %u", cop, aop);
  int samples = ffp_fragment_samples(k);
  // Without a texture, an operation that reads it hands on the colour before it. A disabled alpha operation
  // does the same; a disabled colour operation turns the whole stage off.
  if (!k->texture && op_reads_texture(cop, ca1, ca2)) { cop = TOP_SELECTARG1; ca1 = TA_CURRENT; }
  if (aop == TOP_DISABLE || (!k->texture && op_reads_texture(aop, aa1, aa2))) { aop = TOP_SELECTARG1; aa1 = TA_CURRENT; }
  char color[256], alpha[256];
  if (cop == TOP_DISABLE) { snprintf(color, sizeof color, "f.diffuse.rgb"); snprintf(alpha, sizeof alpha, "f.diffuse.a"); }
  else if (!operation(color, sizeof color, cop, ca1, ca2, 0) || !operation(alpha, sizeof alpha, aop, aa1, aa2, 1))
    return refuse(out, cap, "texture stage arguments 0x%x 0x%x / 0x%x 0x%x", ca1, ca2, aa1, aa2);
  Text t = {out, 0, cap, 0};
  put_uniforms(&t, 0);
  if (samples) put(&t, "@group(0) @binding(1) var t: %s;\n@group(0) @binding(2) var s: sampler;\n", k->texture == FFP_TEXTURE_3D ? "texture_3d<f32>" : "texture_2d<f32>");
  put(&t, "struct FIn {\n%s}\n@fragment fn main(f: FIn) -> @location(0) vec4f {\n", varyings);
  if (samples) {
    static const char *const comp[5] = {"", "", "y", "z", "w"};
    char div[32] = "";
    if (k->projected) snprintf(div, sizeof div, " / f.tex.%s", comp[k->projected]);
    put(&t, "  var texel = textureSample(t, s, f.tex.%s%s);\n", k->texture == FFP_TEXTURE_3D ? "xyz" : "xy", div);
    if (k->texture_opaque) put(&t, "  texel.a = 1.0;\n");
  }
  put(&t, "  var color = clamp(%s, vec3f(0.0), vec3f(1.0));\n  let alpha = clamp(%s, 0.0, 1.0);\n", color, alpha);
  if (k->alpha_test) {
    // The comparison is between 8-bit values, as the reference is
    static const char *const cmp[9] = {"", "false", "a8 < u.misc.x", "a8 == u.misc.x", "a8 <= u.misc.x", "a8 > u.misc.x", "a8 != u.misc.x", "a8 >= u.misc.x", "true"};
    put(&t, "  let a8 = floor(alpha * 255.0 + 0.5);\n  if (!(%s)) { discard; }\n", cmp[k->alpha_test]);
  }
  if (k->specular_add) put(&t, "  color = min(color + f.specular.rgb, vec3f(1.0));\n");
  if (k->fog) put(&t, "  color = mix(u.fog_color.rgb, color, f.fog);\n");
  put(&t, "  return vec4f(color, alpha);\n}\n");
  if (t.full) return refuse(out, cap, "the fragment shader does not fit %zu bytes", cap);
  return t.n;
}

// ---- keys as text ----
void ffp_key_hex(const void *key, size_t bytes, char *out) {
  for (size_t i = 0; i < bytes; i++) snprintf(out + 2 * i, 3, "%02x", ((const uint8_t *)key)[i]);
  out[2 * bytes] = 0;
}
int ffp_key_parse(const char *hex, void *key, size_t bytes) {
  if (strlen(hex) != 2 * bytes) return 0;
  for (size_t i = 0; i < bytes; i++) { unsigned v; if (sscanf(hex + 2 * i, "%2x", &v) != 1) return 0; ((uint8_t *)key)[i] = (uint8_t)v; }
  return 1;
}
void ffp_vertex_describe(const FfpVertexKey *k, char *out, size_t cap) {
  Text t = {out, 0, cap, 0};
  put(&t, "%s", k->pretransformed ? "xyzrhw" : "xyz");
  if (k->weights || k->matrices > 1) put(&t, " %u weights x %u matrices", k->weights, k->matrices);
  if (k->normal) put(&t, " normal%s", k->normalize ? " (normalised)" : "");
  if (k->psize) put(&t, " psize");
  if (k->diffuse) put(&t, " diffuse");
  if (k->specular) put(&t, " specular");
  if (k->tex) put(&t, " tex%u", k->tex);
  if (k->tex_gen) put(&t, " texgen %u", k->tex_gen);
  if (k->tex_transform) put(&t, " texture matrix (%u)", k->tex_transform);
  if (k->lighting && !k->pretransformed) {
    static const char *const src[3] = {"material", "diffuse", "specular"};
    put(&t, "; lit by %u, sources %s/%s/%s/%s%s%s", k->lights, src[k->diffuse_source], src[k->ambient_source], src[k->specular_source], src[k->emissive_source],
        k->specular_on ? ", specular" : "", k->specular_on && k->local_viewer ? " (local viewer)" : "");
  }
  if (k->fog) put(&t, "; fog %s%s", k->fog == FFP_FOG_LINEAR ? "linear" : k->fog == FFP_FOG_EXP ? "exp" : "exp2", k->range_fog ? " by range" : "");
  if (k->point) put(&t, "; points%s%s", k->point_scale ? " scaled" : "", k->point_sprite ? " as sprites" : "");
}
void ffp_fragment_describe(const FfpFragmentKey *k, char *out, size_t cap) {
  static const char *const tex[4] = {"no texture", "?", "2D texture", "volume texture"};
  Text t = {out, 0, cap, 0};
  put(&t, "colour op %u(0x%x, 0x%x) alpha op %u(0x%x, 0x%x), %s%s", k->color_op, k->color_arg1, k->color_arg2, k->alpha_op, k->alpha_arg1, k->alpha_arg2,
      tex[k->texture & 3], k->texture && !ffp_fragment_samples(k) ? " (not read)" : k->texture_opaque ? " without alpha" : "");
  if (k->projected) put(&t, ", projected by %u", k->projected);
  if (k->alpha_test) put(&t, ", alpha test %u", k->alpha_test);
  if (k->specular_add) put(&t, ", specular added");
  if (k->fog) put(&t, ", fog");
}
