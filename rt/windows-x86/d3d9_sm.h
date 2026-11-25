// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9's shader bytecode (shader models 1.1 to 2.x, vertex and pixel) as WGSL. A pure function of the
// tokens and a small link key, like the fixed-function generator beside it (d3d9_ffp.h): no globals, no guest
// memory, so it is built natively and every shader it writes is checked with naga (tests/gfx/d3d9-sm.sh).
// What it does not know (an opcode, a register type, shader model 3) it refuses, with the reason: it never guesses.
//
// How a translated shader meets the rest of the pipeline:
// - Between the stages: the fixed-function generator's locations, extended. 0 the diffuse colour (oD0 / v0),
//   1 the specular colour (oD1 / v1), 2 texture coordinates 0 (oT0 / t0), 3 the fog factor (oFog), 4 to 10
//   texture coordinates 1 to 7. A vertex shader writes all eleven (zero where it writes nothing), so any pixel
//   stage pairs with it; a pixel shader reads the ones it uses. The fixed-function vertex stage writes 0 to 3.
// - Vertex inputs: register vN is @location(N), a vec4f (WebGPU fills missing components with 0, 0, 0, 1, as
//   Direct3D does). SmInfo says which usage and usage index each register was declared with: the pipeline maps
//   the vertex declaration's elements onto those locations.
// - Resources: group 0 is the vertex stage's, group 1 the pixel stage's. In each, binding 0 is the block both
//   stages share (SmFixed: the half-pixel offset, fog, the alpha reference), binding 1 the float constants
//   (array<vec4f, SmInfo.float_count>), binding 2 the integer and boolean constants (SmIntBool). A pixel shader's
//   sampler sN is binding 3 + 2N (the texture) and 4 + 2N (the sampler).
#pragma once
#include <stddef.h>
#include <stdint.h>

// what links a shader to the state it runs under; one byte each, like the fixed-function keys
typedef struct SmLinkKey {
  uint8_t fog;             // pixel stage: the colour goes toward U.fog_color by the fog factor (location 3) afterwards
  uint8_t alpha_test;      // pixel stage: 0 off, else D3DCMP_* (1 never ... 8 always) against U.misc.x (0 to 255)
  uint8_t sampler_types[16]; // pixel stage: SM_TEX_* for each sampler a ps_1_x shader reads (it declares none)
  uint8_t point;           // vertex stage: a point list, drawn as quads (the size from oPts or U.point.x)
} SmLinkKey;
enum { SM_TEX_NONE = 0, SM_TEX_2D = 2, SM_TEX_CUBE = 3, SM_TEX_3D = 4 };   // D3DSAMPLER_TEXTURE_TYPE >> 27

// the block of binding 0 (the WGSL struct SmFixed, member for member)
typedef struct SmFixedUniforms {
  float half_pixel[4];     // x y: what moves a clip-space position (times its w) by half a pixel
  float fog_color[4];
  float misc[4];           // the alpha reference (0 to 255)
  float point[4];          // D3DRS_POINTSIZE, _MIN, _MAX, the viewport's height
} SmFixedUniforms;
// binding 2: integer constants i0..i15 (x: count, y: start, z: step) and booleans b0..b15 (x: 0 or 1)
typedef struct SmIntBool { int32_t i[16][4]; uint32_t b[16][4]; } SmIntBool;

enum { SM_USAGE_POSITION = 0, SM_USAGE_BLENDWEIGHT, SM_USAGE_BLENDINDICES, SM_USAGE_NORMAL, SM_USAGE_PSIZE, SM_USAGE_TEXCOORD,
       SM_USAGE_TANGENT, SM_USAGE_BINORMAL, SM_USAGE_TESSFACTOR, SM_USAGE_POSITIONT, SM_USAGE_COLOR, SM_USAGE_FOG, SM_USAGE_DEPTH, SM_USAGE_SAMPLE };
typedef struct SmInfo {
  uint8_t pixel;           // 0 a vertex shader, 1 a pixel shader
  uint8_t major, minor;    // the shader model
  uint16_t inputs;         // vertex: the input registers it reads (bit N: vN)
  uint8_t usage[16], usage_index[16];   // vertex: what each input register was declared as (vs_1_1 without dcl: by number)
  uint16_t varyings;       // pixel: the locations it reads; vertex: the locations it writes (bit per location)
  uint16_t samplers;       // pixel: the samplers it reads (bit N: sN)
  uint8_t sampler_types[16]; // SM_TEX_* of each
  uint16_t float_count;    // the float constants it reads: binding 1 holds this many vec4f (at least 1)
  uint8_t int_bool;        // it reads integer or boolean constants (binding 2)
  uint8_t relative;        // it indexes the float constants with a0 or aL
  uint8_t writes_depth;    // pixel: oDepth
  uint8_t writes_psize;    // vertex: oPts
  uint8_t colors;          // pixel: the colour outputs it writes (bit N: oCN; ps_1_x: r0, bit 0)
} SmInfo;

// The WGSL for the bytecode (`count` 32-bit tokens, the version token first) into `out` (at most `cap` bytes with
// the final zero); `info` is filled in. Returns the text's length, or 0 when the shader asks for something not
// covered: `out` then holds the reason.
size_t sm_wgsl(const uint32_t *tokens, size_t count, const SmLinkKey *link, SmInfo *info, char *out, size_t cap);
// The bytecode's length in tokens up to and including its end token (0 when there is none within `max`)
size_t sm_length(const uint32_t *tokens, size_t max);
// "vs_2_0", "ps_1_4" ... for a version token
void sm_version_name(uint32_t version, char *out, size_t cap);
