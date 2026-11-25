// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9's fixed-function pipeline as WGSL shaders. A title that creates no shaders still draws through
// two programs Direct3D makes up from its state: this file's functions write them, one vertex stage and one
// fragment stage, each a pure function of a small key (no globals, no guest memory), so they are also built
// natively and their output is checked with naga (tests/gfx/d3d9-ffp.sh).
// The key says which program; everything else is data, in one uniform block both stages read (FfpUniforms).
// Covered: what docs/narbacular-drop.md lists. Vertices by FVF or declaration (XYZRHW, XYZ, 1 to 3 blend
// weights, normal, point size, diffuse, specular, one set of 1 to 4 texture coordinates), lighting with up to 8
// lights, material sources, vertex fog, point sprites, one texture stage, alpha test. Not covered (the functions
// refuse the key): stages past the first, bump mapping, the operations that need three arguments, table fog.
#pragma once
#include <stddef.h>
#include <stdint.h>

// ---- the keys: every field is one byte, so a key is compared, hashed and printed as its bytes ----
typedef struct FfpVertexKey {
  uint8_t pretransformed;  // XYZRHW: the position is in screen space and comes with 1/w; no transform, no lighting
  uint8_t weights;         // blend weights in the vertex: 0 to 3 floats
  uint8_t matrices;        // world matrices blended (D3DRS_VERTEXBLEND + 1): 1 to 4
  uint8_t normal, psize, diffuse, specular;   // the vertex has them
  uint8_t tex;             // components of the texture coordinates stage 0 reads: 0 (the vertex has none) to 4
  uint8_t lighting;        // D3DRS_LIGHTING
  uint8_t lights;          // lights that are on: 0 to 8 (their kinds are data: a light's position.w)
  uint8_t normalize;       // D3DRS_NORMALIZENORMALS
  uint8_t local_viewer;    // D3DRS_LOCALVIEWER
  uint8_t specular_on;     // D3DRS_SPECULARENABLE: the lights add a specular term
  uint8_t diffuse_source, ambient_source, specular_source, emissive_source;   // FFP_SOURCE_*: after D3DRS_COLORVERTEX and what the vertex has
  uint8_t fog;             // FFP_FOG_*: the vertex fog mode (D3DFOGMODE's numbers)
  uint8_t range_fog;       // D3DRS_RANGEFOGENABLE: the distance from the eye instead of the depth
  uint8_t tex_gen;         // FFP_TEXGEN_*: where stage 0's coordinates come from (D3DTSS_TEXCOORDINDEX's high word)
  uint8_t tex_transform;   // D3DTSS_TEXTURETRANSFORMFLAGS without PROJECTED: 0 off, 1 to 4
  uint8_t point;           // a point list: every point is drawn as a quad of four vertices, one instance each
  uint8_t point_scale;     // D3DRS_POINTSCALEENABLE
  uint8_t point_sprite;    // D3DRS_POINTSPRITEENABLE: the quad's corners are the texture coordinates
} FfpVertexKey;
enum { FFP_SOURCE_MATERIAL = 0, FFP_SOURCE_DIFFUSE = 1, FFP_SOURCE_SPECULAR = 2 };
enum { FFP_FOG_NONE = 0, FFP_FOG_EXP = 1, FFP_FOG_EXP2 = 2, FFP_FOG_LINEAR = 3 };
enum { FFP_TEXGEN_VERTEX = 0, FFP_TEXGEN_NORMAL = 1, FFP_TEXGEN_POSITION = 2, FFP_TEXGEN_REFLECTION = 3 };

typedef struct FfpFragmentKey {
  uint8_t color_op, color_arg1, color_arg2;   // stage 0: D3DTOP_*, D3DTA_* with the complement and alpha-replicate bits
  uint8_t alpha_op, alpha_arg1, alpha_arg2;
  uint8_t texture;         // what is bound to stage 0: FFP_TEXTURE_*
  uint8_t texture_opaque;  // its format has no alpha channel (X8R8G8B8): its alpha reads as 1
  uint8_t projected;       // D3DTTFF_PROJECTED: the coordinates are divided by this component (2 to 4); 0 off
  uint8_t alpha_test;      // 0 off, else D3DCMP_* (1 never ... 8 always) against the reference
  uint8_t fog;             // the colour goes toward the fog colour by the vertex's fog factor
  uint8_t specular_add;    // D3DRS_SPECULARENABLE: the specular colour is added after the stage
} FfpFragmentKey;
enum { FFP_TEXTURE_NONE = 0, FFP_TEXTURE_2D = 2, FFP_TEXTURE_3D = 3 };   // GPU_BIND_TEXTURE_2D / _3D

// ---- the uniform block: this structure is the WGSL `U` (and `Light`) the shaders declare, member for member.
// Matrices are Direct3D's, row by row, as a title hands them over: WGSL reads 16 floats as columns, so the
// shader's matrix is the transpose and `m * v` there is Direct3D's `v x m`.
typedef struct FfpLight {
  float diffuse[4], specular[4], ambient[4];
  float position[4];       // in camera space; w: D3DLIGHTTYPE (1 point, 2 spot, 3 directional)
  float direction[4];      // in camera space, unit length; w: the range
  float attenuation[4];    // constant, linear, quadratic; w: the spot's falloff
  float spot[4];           // cos(theta / 2), cos(phi / 2)
} FfpLight;
typedef struct FfpUniforms {
  float world_view[4][16]; // world matrix i x view
  float normal[4][16];     // the inverse transpose of each (its 3x3 part)
  float projection[16];
  float tex_matrix[16];    // D3DTS_TEXTURE0
  float screen_scale[4], screen_offset[4];   // XYZRHW: screen position * scale + offset is the position after the divide
  float half_pixel[4];     // x y: what moves a clip-space position (times its w) by half a pixel; also a pixel in clip space / 2
  float material_diffuse[4], material_ambient[4], material_specular[4], material_emissive[4];
  float ambient[4];        // D3DRS_AMBIENT
  float fog_color[4];
  float fog[4];            // start, end, 1 / (end - start), density
  float point[4];          // D3DRS_POINTSIZE, _MIN, _MAX, the viewport's height
  float point_scale[4];    // D3DRS_POINTSCALE_A, _B, _C
  float misc[4];           // the alpha reference (0 to 255), the material's power
  float factor[4];         // D3DRS_TEXTUREFACTOR
  FfpLight lights[8];      // the first `lights` of the key are sent
} FfpUniforms;
#define FFP_UNIFORM_BYTES(count) (offsetof(FfpUniforms, lights) + (size_t)(count) * sizeof(FfpLight))

// vertex attribute locations (the pipeline maps a declaration's elements onto them)
enum { FFP_LOC_POSITION = 0, FFP_LOC_WEIGHTS, FFP_LOC_NORMAL, FFP_LOC_PSIZE, FFP_LOC_DIFFUSE, FFP_LOC_SPECULAR, FFP_LOC_TEX, FFP_LOC_COUNT };

// ---- the generators: WGSL text for the key into `out` (at most `cap` bytes with the final zero). They return
// the text's length, or 0 when the key asks for something not covered: `out` then holds the reason.
size_t ffp_vertex_wgsl(const FfpVertexKey *k, char *out, size_t cap);
size_t ffp_fragment_wgsl(const FfpFragmentKey *k, char *out, size_t cap);
// Whether the fragment stage reads the stage's texture: then the bind layout has a texture and a sampler after
// the uniform block. An operation that reads the texture while none is bound passes the colour before it on
// instead (what Direct3D does: Wine's tests of Windows show it), and reads nothing.
int ffp_fragment_samples(const FfpFragmentKey *k);
// A key as text, for logs and for the test: its bytes in hexadecimal (2 * bytes + 1 characters)
void ffp_key_hex(const void *key, size_t bytes, char *out);
int ffp_key_parse(const char *hex, void *key, size_t bytes);   // 1 if `hex` is exactly a key of that size
// The same for people: "xyz n t2 lit3 fog" ...
void ffp_vertex_describe(const FfpVertexKey *k, char *out, size_t cap);
void ffp_fragment_describe(const FfpFragmentKey *k, char *out, size_t cap);
