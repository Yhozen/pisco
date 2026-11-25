// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9 state blocks for the null device. A block is a copy of the device's state with a list of the
// entries it covers: the ones set between BeginStateBlock and EndStateBlock (which go to the block and not to the
// device), or the ones its type names (CreateStateBlock). Capture copies those entries from the device; Apply
// copies them to it. Neither is allowed while a block is being recorded. A block holds the objects it binds.
// D3DSBT_ALL covers everything a block can hold; what D3DSBT_PIXELSTATE and D3DSBT_VERTEXSTATE cover is what the
// oracle's blocks of those types restore (d3d9probe.c changes every state and applies one: d3d9_oracle.gen.h).
#include "d3d9.h"
#include "d3d9_layout.gen.h"
#include "d3d9_oracle.gen.h"

#define DEV ((Device9 *)GP(ARG(0)))
#define ENTRY(klass, index) ((u16)((klass) << 12 | (index)))

void d3d9_recorded(StateBlock9 *b, u32 klass, u32 index) {
  if (b->seen[klass][index]) return;
  b->seen[klass][index] = 1;
  if (b->list_count == b->list_cap) { b->list_cap = b->list_cap ? 2 * b->list_cap : 32; b->list = realloc(b->list, b->list_cap * sizeof *b->list); }
  b->list[b->list_count++] = ENTRY(klass, index);
}
void d3d9_recorded_light(StateBlock9 *b, u32 index) {
  for (u32 i = 0; i < b->lights_count; i++) if (b->lights[i] == index) return;
  if (b->lights_count == b->lights_cap) { b->lights_cap = b->lights_cap ? 2 * b->lights_cap : 8; b->lights = realloc(b->lights, b->lights_cap * sizeof *b->lights); }
  b->lights[b->lights_count++] = index;
}
static void bind(Object9 **slot, Object9 *o) { Object9 *old = *slot; d3d9_hold(o); *slot = o; d3d9_unhold(old); }
// One entry from one state to another
static void copy(State9 *to, const State9 *from, u32 klass, u32 i) {
  switch (klass) {
  case C_RS: to->rs[i] = from->rs[i]; break;
  case C_TSS: to->tss[i / TSS_COUNT][i % TSS_COUNT] = from->tss[i / TSS_COUNT][i % TSS_COUNT]; break;
  case C_SAMP: to->samp[i / SAMP_COUNT][i % SAMP_COUNT] = from->samp[i / SAMP_COUNT][i % SAMP_COUNT]; break;
  case C_TEXTURE: bind((Object9 **)&to->textures[i], (Object9 *)from->textures[i]); break;
  case C_STREAM: bind((Object9 **)&to->streams[i].buffer, (Object9 *)from->streams[i].buffer); to->streams[i].offset = from->streams[i].offset; to->streams[i].stride = from->streams[i].stride; break;
  case C_STREAM_FREQ: to->streams[i].freq = from->streams[i].freq; break;
  case C_TRANSFORM: to->transforms[i] = from->transforms[i]; break;
  case C_CLIP_PLANE: memcpy(to->clip_planes[i], from->clip_planes[i], 16); break;
  case C_VS_FLOAT: memcpy(to->vs_float[i], from->vs_float[i], 16); break;
  case C_PS_FLOAT: memcpy(to->ps_float[i], from->ps_float[i], 16); break;
  case C_VS_INT: memcpy(to->vs_int[i], from->vs_int[i], 16); break;
  case C_PS_INT: memcpy(to->ps_int[i], from->ps_int[i], 16); break;
  case C_VS_BOOL: to->vs_bool[i] = from->vs_bool[i]; break;
  case C_PS_BOOL: to->ps_bool[i] = from->ps_bool[i]; break;
  }
}
static void copy_singles(State9 *to, const State9 *from, u32 single) {
  if (single & M_MATERIAL) to->material = from->material;
  if (single & M_VIEWPORT) to->viewport = from->viewport;
  if (single & M_SCISSOR) to->scissor = from->scissor;
  if (single & M_INDICES) bind((Object9 **)&to->indices, (Object9 *)from->indices);
  if (single & M_DECLARATION) { bind((Object9 **)&to->declaration, (Object9 *)from->declaration); to->fvf = from->fvf; }
  if (single & M_VERTEX_SHADER) bind((Object9 **)&to->vertex_shader, (Object9 *)from->vertex_shader);
  if (single & M_PIXEL_SHADER) bind((Object9 **)&to->pixel_shader, (Object9 *)from->pixel_shader);
  if (single & M_NPATCH) to->npatch = from->npatch;
}
// Capture: the block's entries take the device's values. A light the device does not have stays as the block has it.
static void capture(StateBlock9 *b) {
  const State9 *dev = &b->o.device->state;
  for (u32 i = 0; i < b->list_count; i++) copy(&b->s, dev, b->list[i] >> 12, b->list[i] & 0xFFF);
  copy_singles(&b->s, dev, b->single);
  for (u32 i = 0; i < b->lights_count; i++) {
    u32 n = b->lights[i]; LightSlot9 *mine = d3d9_light(&b->s, n, 1); const LightSlot9 *its = n < dev->light_count ? &dev->lights[n] : 0;
    if (!its || !its->set) continue;
    if (mine->has_light) { mine->light = its->light; mine->set = 1; }
    if (mine->has_enable) mine->enabled = its->enabled;
  }
}
// Apply: the device takes the block's entries. Enabling or disabling a light the device does not have gives it the default light.
static void apply(StateBlock9 *b) {
  State9 *dev = &b->o.device->state;
  for (u32 i = 0; i < b->list_count; i++) copy(dev, &b->s, b->list[i] >> 12, b->list[i] & 0xFFF);
  copy_singles(dev, &b->s, b->single);
  for (u32 i = 0; i < b->lights_count; i++) {
    u32 n = b->lights[i]; const LightSlot9 *mine = &b->s.lights[n]; LightSlot9 *its = d3d9_light(dev, n, 1);
    if (mine->has_light) { its->light = mine->light; its->set = 1; }
    if (mine->has_enable) { if (!its->set) { its->light = d3d9_default_light; its->set = 1; } its->enabled = mine->enabled; }
  }
}
static StateBlock9 *block_new(Device9 *d, u32 type) {
  StateBlock9 *b = (StateBlock9 *)GP(com_new("d3d9", &IDirect3DStateBlock9_interface, sizeof(StateBlock9)));
  b->o.kind = K_STATEBLOCK; b->o.device = d; b->type = type;
  return b;
}
void d3d9_stateblock_destroy(StateBlock9 *b) {
  d3d9_state_free(&b->s, 1);
  free(b->list); free(b->lights); b->o.vtable = 0; free(b);
}

// ---- recording ----
IMP(d3d9, IDirect3DDevice9_BeginStateBlock) {
  STAT(device_BeginStateBlock); Device9 *d = DEV;
  if (d->recording) { RET(D3DERR_INVALIDCALL); return; }
  d->recording = block_new(d, 0); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_EndStateBlock) {
  STAT(device_EndStateBlock); Device9 *d = DEV;
  if (!d->recording || !ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  StateBlock9 *b = d->recording; d->recording = 0;
  b->o.refs = 1; d->o.refs++;
  M32(ARG(1)) = (u32)(uptr)b; RET(D3D_OK);
}

// ---- CreateStateBlock(type, IDirect3DStateBlock9 **): ALL 1, PIXELSTATE 2, VERTEXSTATE 3 ----
// Beyond the three kinds of state: a pixel block has the pixel shader and its constants; a vertex block the vertex
// shader and its constants, the vertex format and the lights; only a block of everything has the bound textures,
// streams and indices, the transforms, the material, the viewport, the scissor rectangle and the clip planes.
// (The N-patch mode goes with the vertex state, as the documentation has it: the oracle's reads 0 whatever is set.)
IMP(d3d9, IDirect3DDevice9_CreateStateBlock) {
  STAT(device_CreateStateBlock); Device9 *d = DEV; u32 type = (u32)ARG(1);
  if (d->recording || type < 1 || type > 3 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  StateBlock9 *b = block_new(d, type); int all = type == 1, pixel = type == 2, vertex = type == 3;
  if (all) { for (u32 i = 7; i <= 209; i++) d3d9_recorded(b, C_RS, i); }
  else if (pixel) for (u32 i = 0; i < sizeof oracle_pixel_rs; i++) d3d9_recorded(b, C_RS, oracle_pixel_rs[i]);
  else for (u32 i = 0; i < sizeof oracle_vertex_rs; i++) d3d9_recorded(b, C_RS, oracle_vertex_rs[i]);
  for (u32 s = 0; s < STAGES; s++) {
    if (all) for (u32 i = 1; i < TSS_COUNT; i++) d3d9_recorded(b, C_TSS, s * TSS_COUNT + i);
    else if (pixel) for (u32 i = 0; i < sizeof oracle_pixel_tss; i++) d3d9_recorded(b, C_TSS, s * TSS_COUNT + oracle_pixel_tss[i]);
    else for (u32 i = 0; i < sizeof oracle_vertex_tss; i++) d3d9_recorded(b, C_TSS, s * TSS_COUNT + oracle_vertex_tss[i]);
  }
  for (u32 s = 0; s < SAMPLERS; s++) {
    if (all) for (u32 i = 1; i < SAMP_COUNT; i++) d3d9_recorded(b, C_SAMP, s * SAMP_COUNT + i);
    else if (pixel) for (u32 i = 0; i < sizeof oracle_pixel_samp; i++) d3d9_recorded(b, C_SAMP, s * SAMP_COUNT + oracle_pixel_samp[i]);
    else for (u32 i = 0; i < sizeof oracle_vertex_samp; i++) d3d9_recorded(b, C_SAMP, s * SAMP_COUNT + oracle_vertex_samp[i]);
  }
  if (all || pixel) { for (u32 i = 0; i < PS_FLOATS; i++) d3d9_recorded(b, C_PS_FLOAT, i); for (u32 i = 0; i < SHADER_INTS; i++) d3d9_recorded(b, C_PS_INT, i); for (u32 i = 0; i < SHADER_BOOLS; i++) d3d9_recorded(b, C_PS_BOOL, i); b->single |= M_PIXEL_SHADER; }
  if (all || vertex) {
    for (u32 i = 0; i < VS_FLOATS; i++) d3d9_recorded(b, C_VS_FLOAT, i); for (u32 i = 0; i < SHADER_INTS; i++) d3d9_recorded(b, C_VS_INT, i); for (u32 i = 0; i < SHADER_BOOLS; i++) d3d9_recorded(b, C_VS_BOOL, i);
    b->single |= M_VERTEX_SHADER | M_DECLARATION | M_NPATCH;
    for (u32 i = 0; i < d->state.light_count; i++) if (d->state.lights[i].set) { LightSlot9 *l = d3d9_light(&b->s, i, 1); l->has_light = l->has_enable = 1; d3d9_recorded_light(b, i); }
  }
  if (all) {
    for (u32 i = 0; i < SAMPLERS; i++) d3d9_recorded(b, C_TEXTURE, i);
    for (u32 i = 0; i < STREAMS; i++) { d3d9_recorded(b, C_STREAM, i); d3d9_recorded(b, C_STREAM_FREQ, i); }
    for (u32 i = 0; i < TRANSFORMS; i++) if (i == 2 || i == 3 || (i >= 16 && i <= 23) || i >= 256) d3d9_recorded(b, C_TRANSFORM, i);
    for (u32 i = 0; i < CLIP_PLANES; i++) d3d9_recorded(b, C_CLIP_PLANE, i);
    b->single |= M_MATERIAL | M_VIEWPORT | M_SCISSOR | M_INDICES;
  }
  capture(b);
  b->o.refs = 1; d->o.refs++;
  M32(ARG(2)) = (u32)(uptr)b; RET(D3D_OK);
}

// ---- IDirect3DStateBlock9 ----
#define SELF ((StateBlock9 *)GP(ARG(0)))
IMP(d3d9, IDirect3DStateBlock9_QueryInterface) {
  STAT(stateblock_QueryInterface); u64 out = ARG(2);
  if (!out) { RET(E_POINTER); return; }
  if (com_is_iid(ARG(1), IID_IUnknown_text) || com_is_iid(ARG(1), IDirect3DStateBlock9_interface.iid)) { d3d9_addref(&SELF->o); M32(out) = (u32)ARG(0); RET(D3D_OK); return; }
  M32(out) = 0; RET(E_NOINTERFACE);
}
IMP(d3d9, IDirect3DStateBlock9_AddRef) { STAT(stateblock_AddRef); RET(d3d9_addref(&SELF->o)); }
IMP(d3d9, IDirect3DStateBlock9_Release) { STAT(stateblock_Release); RET(d3d9_release(&SELF->o)); }
IMP(d3d9, IDirect3DStateBlock9_GetDevice) {
  STAT(stateblock_GetDevice); Device9 *d = SELF->o.device;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  d->o.refs++; M32(ARG(1)) = (u32)(uptr)d; RET(D3D_OK);
}
IMP(d3d9, IDirect3DStateBlock9_Capture) { STAT(stateblock_Capture); StateBlock9 *b = SELF; if (b->o.device->recording) { RET(D3DERR_INVALIDCALL); return; } capture(b); RET(D3D_OK); }
IMP(d3d9, IDirect3DStateBlock9_Apply) { STAT(stateblock_Apply); StateBlock9 *b = SELF; if (b->o.device->recording) { RET(D3DERR_INVALIDCALL); return; } apply(b); RET(D3D_OK); }
