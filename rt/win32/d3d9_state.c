// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9, the device's state: render, texture stage and sampler states, transforms, lights, material,
// viewport, vertex format and streams, bound textures, targets; scenes, clears and draws. A draw is checked
// against what is bound and counted, then handed to d3d9_gpu.c with the state as it is. Between BeginStateBlock and EndStateBlock every
// Set* goes to the block being recorded and leaves the device's state alone, as Direct3D does (d3d9probe.txt).
// A new device starts in the state the oracle's starts in (d3d9_oracle.gen.h).
#include "d3d9.h"
#include "d3d9_oracle.gen.h"

#define DEV ((Device9 *)GP(ARG(0)))
#define D3DERR_NOTFOUND 0x88760866u
enum { RS_ZENABLE = 7, RS_LIGHTING = 137, RS_VERTEXBLEND = 151 };
// The state a Set* changes, and its mark in the block being recorded
#define TARGET(d) ((d)->recording ? &(d)->recording->s : &(d)->state)
#define MARK(d, klass, index) do { if ((d)->recording) d3d9_recorded((d)->recording, klass, index); } while (0)
#define MARK1(d, bit) do { if ((d)->recording) (d)->recording->single |= (bit); } while (0)

int d3d9_sampler_index(u32 s) { return s < 16 ? (int)s : s >= 256 && s <= 260 ? (int)(s - 240) : -1; }
Light9 d3d9_default_light;
LightSlot9 *d3d9_light(State9 *s, u32 index, int create) {
  if (index >= s->light_count) {
    if (!create) return 0;
    if (index >= 65536) rt_fatal("Direct3D 9: light index %u is beyond the table this device keeps", index);
    u32 n = s->light_count ? s->light_count : 16; while (n <= index) n *= 2;
    s->lights = realloc(s->lights, n * sizeof *s->lights); memset(s->lights + s->light_count, 0, (n - s->light_count) * sizeof *s->lights); s->light_count = n;
  }
  return &s->lights[index];
}
void d3d9_state_init(Device9 *d) {
  State9 *s = &d->state;
  memcpy(&d3d9_default_light, oracle_light, sizeof d3d9_default_light);
  for (unsigned i = 0; i < sizeof oracle_render_states / sizeof oracle_render_states[0]; i++) s->rs[oracle_render_states[i].state] = oracle_render_states[i].value;
  for (unsigned i = 0; i < sizeof oracle_stage_states / sizeof oracle_stage_states[0]; i++) s->tss[oracle_stage_states[i].stage][oracle_stage_states[i].state] = oracle_stage_states[i].value;
  for (unsigned i = 0; i < sizeof oracle_sampler_states / sizeof oracle_sampler_states[0]; i++) s->samp[d3d9_sampler_index(oracle_sampler_states[i].sampler)][oracle_sampler_states[i].state] = oracle_sampler_states[i].value;
  if (!d->auto_depth) s->rs[RS_ZENABLE] = 0;   // the oracle's device had a depth buffer: without one depth testing starts off
  for (unsigned i = 0; i < TRANSFORMS; i++) s->transforms[i].m[0] = s->transforms[i].m[5] = s->transforms[i].m[10] = s->transforms[i].m[15] = 1.0f;
  memcpy(&s->material, oracle_material, sizeof s->material);
  s->viewport = (Viewport9){0, 0, d->pp.width, d->pp.height, 0.0f, 1.0f};
  s->scissor = (Rect9){0, 0, (s32)d->pp.width, (s32)d->pp.height};
  for (unsigned i = 0; i < STREAMS; i++) s->streams[i].freq = 1;
}
void d3d9_state_free(State9 *s, int unhold) {
  if (unhold) {
    for (unsigned i = 0; i < SAMPLERS; i++) d3d9_unhold((Object9 *)s->textures[i]);
    for (unsigned i = 0; i < STREAMS; i++) d3d9_unhold((Object9 *)s->streams[i].buffer);
    d3d9_unhold((Object9 *)s->indices); d3d9_unhold((Object9 *)s->declaration);
  }
  free(s->lights); s->lights = 0; s->light_count = 0;
}
// A bound object: the state holds the new one before it lets go of the old (they may be the same)
static void bind(Object9 **slot, Object9 *o) { Object9 *old = *slot; d3d9_hold(o); *slot = o; d3d9_unhold(old); }

// ---- render, texture stage and sampler states ----
// A state, a stage or a sampler beyond the ones there are: the oracle accepts the call and changes nothing (d3d9probe.txt)
static void ignored(const char *what, u32 index) { static int told; if (rt_trace && told++ < 8) rt_log("Direct3D 9: %s of index %u, which is out of range, changes nothing", what, index); }
IMP(d3d9, IDirect3DDevice9_SetRenderState) {
  STAT(device_SetRenderState); Device9 *d = DEV; u32 state = (u32)ARG(1);
  ARGK("device_SetRenderState", state, ARG(2));
  if (state >= RS_COUNT) { ignored("SetRenderState", state); RET(D3D_OK); return; }
  TARGET(d)->rs[state] = (u32)ARG(2); MARK(d, C_RS, state); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetRenderState) {
  STAT(device_GetRenderState); u32 state = (u32)ARG(1);
  if (!ARG(2) || state >= RS_COUNT) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(2)) = DEV->state.rs[state]; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetTextureStageState) {
  STAT(device_SetTextureStageState); Device9 *d = DEV; u32 stage = (u32)ARG(1), type = (u32)ARG(2);
  ARGK("device_SetTextureStageState", type, ARG(3));
  if (stage >= STAGES || type >= TSS_COUNT) { ignored("SetTextureStageState", stage >= STAGES ? stage : type); RET(D3D_OK); return; }
  TARGET(d)->tss[stage][type] = (u32)ARG(3); MARK(d, C_TSS, stage * TSS_COUNT + type); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetTextureStageState) {
  STAT(device_GetTextureStageState); u32 stage = (u32)ARG(1), type = (u32)ARG(2);
  if (!ARG(3) || stage >= STAGES || type >= TSS_COUNT) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(3)) = DEV->state.tss[stage][type]; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetSamplerState) {
  STAT(device_SetSamplerState); Device9 *d = DEV; int s = d3d9_sampler_index((u32)ARG(1)); u32 type = (u32)ARG(2);
  ARGK("device_SetSamplerState", type, ARG(3));
  if (s < 0 || type >= SAMP_COUNT) { ignored("SetSamplerState", s < 0 ? (u32)ARG(1) : type); RET(D3D_OK); return; }
  TARGET(d)->samp[s][type] = (u32)ARG(3); MARK(d, C_SAMP, (u32)s * SAMP_COUNT + type); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetSamplerState) {
  STAT(device_GetSamplerState); int s = d3d9_sampler_index((u32)ARG(1)); u32 type = (u32)ARG(2);
  if (!ARG(3) || s < 0 || type >= SAMP_COUNT) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(3)) = DEV->state.samp[s][type]; RET(D3D_OK);
}

// ---- bound objects ----
IMP(d3d9, IDirect3DDevice9_SetTexture) {
  STAT(device_SetTexture); Device9 *d = DEV; int s = d3d9_sampler_index((u32)ARG(1)); Object9 *t = (Object9 *)GP(ARG(2));
  ARGV("device_SetTexture stage", ARG(1));
  if (s < 0) { ignored("SetTexture", (u32)ARG(1)); RET(D3D_OK); return; }
  if (t && t->kind != K_TEXTURE && t->kind != K_VOLUMETEXTURE) { RET(D3DERR_INVALIDCALL); return; }
  bind((Object9 **)&TARGET(d)->textures[s], t); MARK(d, C_TEXTURE, (u32)s); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetTexture) {
  STAT(device_GetTexture); int s = d3d9_sampler_index((u32)ARG(1));
  if (!ARG(2) || s < 0) { RET(D3DERR_INVALIDCALL); return; }
  Object9 *t = (Object9 *)DEV->state.textures[s]; if (t) d3d9_addref(t);
  M32(ARG(2)) = (u32)(uptr)t; RET(D3D_OK);
}
// SetStreamSource(stream, IDirect3DVertexBuffer9 *, offset, stride)
IMP(d3d9, IDirect3DDevice9_SetStreamSource) {
  STAT(device_SetStreamSource); Device9 *d = DEV; u32 n = (u32)ARG(1); Buffer9 *b = (Buffer9 *)GP(ARG(2));
  ARGV("device_SetStreamSource stream_idx", n);
  if (n >= STREAMS || (b && b->o.kind != K_VERTEXBUFFER)) { RET(D3DERR_INVALIDCALL); return; }
  State9 *s = TARGET(d);
  bind((Object9 **)&s->streams[n].buffer, (Object9 *)b);
  if (b) { s->streams[n].offset = (u32)ARG(3); s->streams[n].stride = (u32)ARG(4); }   // unbinding keeps the last offset and stride
  MARK(d, C_STREAM, n); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetStreamSource) { // (stream, IDirect3DVertexBuffer9 **, offset *, stride *)
  STAT(device_GetStreamSource); Device9 *d = DEV; u32 n = (u32)ARG(1);
  if (n >= STREAMS || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  Buffer9 *b = d->state.streams[n].buffer; if (b) d3d9_addref(&b->o);
  M32(ARG(2)) = (u32)(uptr)b; if (ARG(3)) M32(ARG(3)) = d->state.streams[n].offset; if (ARG(4)) M32(ARG(4)) = d->state.streams[n].stride;
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetStreamSourceFreq) {
  STAT(device_SetStreamSourceFreq); Device9 *d = DEV; u32 n = (u32)ARG(1);
  if (n >= STREAMS || !(ARG(2) & 0x3FFFFFFF)) { RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->streams[n].freq = (u32)ARG(2); MARK(d, C_STREAM_FREQ, n); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetStreamSourceFreq) {
  STAT(device_GetStreamSourceFreq); u32 n = (u32)ARG(1);
  if (n >= STREAMS || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(2)) = DEV->state.streams[n].freq; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetIndices) {
  STAT(device_SetIndices); Device9 *d = DEV; Buffer9 *b = (Buffer9 *)GP(ARG(1));
  if (b && b->o.kind != K_INDEXBUFFER) { RET(D3DERR_INVALIDCALL); return; }
  bind((Object9 **)&TARGET(d)->indices, (Object9 *)b); MARK1(d, M_INDICES); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetIndices) {
  STAT(device_GetIndices);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  Buffer9 *b = DEV->state.indices; if (b) d3d9_addref(&b->o);
  M32(ARG(1)) = (u32)(uptr)b; RET(D3D_OK);
}
// The vertex format is one thing set two ways: a declaration (whose FVF then reads as 0), or an FVF code, for
// which the device binds a declaration of its own (d3d9probe.txt)
IMP(d3d9, IDirect3DDevice9_SetFVF) {
  STAT(device_SetFVF); Device9 *d = DEV; ARGV("device_SetFVF fvf", ARG(1));
  if (!ARG(1)) { RET(D3D_OK); return; }   // 0 changes nothing: it is not an error
  State9 *s = TARGET(d); bind((Object9 **)&s->declaration, &d3d9_fvf_declaration(d, (u32)ARG(1))->o); s->fvf = (u32)ARG(1); MARK1(d, M_DECLARATION); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetFVF) { STAT(device_GetFVF); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } M32(ARG(1)) = DEV->state.fvf; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_SetVertexDeclaration) {
  STAT(device_SetVertexDeclaration); Device9 *d = DEV; Declaration9 *v = (Declaration9 *)GP(ARG(1));
  if (v && v->o.kind != K_DECLARATION) { RET(D3DERR_INVALIDCALL); return; }
  State9 *s = TARGET(d); bind((Object9 **)&s->declaration, (Object9 *)v); s->fvf = v ? v->fvf : 0; MARK1(d, M_DECLARATION); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetVertexDeclaration) {
  STAT(device_GetVertexDeclaration);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  Declaration9 *v = DEV->state.declaration; if (v) d3d9_addref(&v->o);
  M32(ARG(1)) = (u32)(uptr)v; RET(D3D_OK);
}
// Shaders cannot be created on this device yet, so the only shader there is to set is none
IMP(d3d9, IDirect3DDevice9_SetVertexShader) {
  STAT(device_SetVertexShader); Device9 *d = DEV;
  if (ARG(1)) { com_unsure("Direct3D 9: SetVertexShader of a shader this device did not make"); RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->vertex_shader = 0; MARK1(d, M_VERTEX_SHADER); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetVertexShader) { STAT(device_GetVertexShader); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } M32(ARG(1)) = 0; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_SetPixelShader) {
  STAT(device_SetPixelShader); Device9 *d = DEV;
  if (ARG(1)) { com_unsure("Direct3D 9: SetPixelShader of a shader this device did not make"); RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->pixel_shader = 0; MARK1(d, M_PIXEL_SHADER); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetPixelShader) { STAT(device_GetPixelShader); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } M32(ARG(1)) = 0; RET(D3D_OK); }
// Shader constants: (first register, values, count of registers). Kept although no shader can be made yet.
#define SET_CONSTANTS(FIELD, LIMIT, WIDTH, KLASS) do { Device9 *d = DEV; u32 first = (u32)ARG(1), n = (u32)ARG(3); \
    if (!ARG(2) || first > LIMIT || n > LIMIT - first) { RET(D3DERR_INVALIDCALL); return; } \
    memcpy(&TARGET(d)->FIELD[first], (void *)GP(ARG(2)), n * WIDTH); for (u32 i = 0; i < n; i++) MARK(d, KLASS, first + i); RET(D3D_OK); } while (0)
#define GET_CONSTANTS(FIELD, LIMIT, WIDTH) do { u32 first = (u32)ARG(1), n = (u32)ARG(3); \
    if (!ARG(2) || first > LIMIT || n > LIMIT - first) { RET(D3DERR_INVALIDCALL); return; } \
    memcpy((void *)GP(ARG(2)), &DEV->state.FIELD[first], n * WIDTH); RET(D3D_OK); } while (0)
IMP(d3d9, IDirect3DDevice9_SetVertexShaderConstantF) { STAT(device_SetVertexShaderConstantF); SET_CONSTANTS(vs_float, VS_FLOATS, 16, C_VS_FLOAT); }
IMP(d3d9, IDirect3DDevice9_GetVertexShaderConstantF) { STAT(device_GetVertexShaderConstantF); GET_CONSTANTS(vs_float, VS_FLOATS, 16); }
IMP(d3d9, IDirect3DDevice9_SetVertexShaderConstantI) { STAT(device_SetVertexShaderConstantI); SET_CONSTANTS(vs_int, SHADER_INTS, 16, C_VS_INT); }
IMP(d3d9, IDirect3DDevice9_GetVertexShaderConstantI) { STAT(device_GetVertexShaderConstantI); GET_CONSTANTS(vs_int, SHADER_INTS, 16); }
IMP(d3d9, IDirect3DDevice9_SetVertexShaderConstantB) { STAT(device_SetVertexShaderConstantB); SET_CONSTANTS(vs_bool, SHADER_BOOLS, 4, C_VS_BOOL); }
IMP(d3d9, IDirect3DDevice9_GetVertexShaderConstantB) { STAT(device_GetVertexShaderConstantB); GET_CONSTANTS(vs_bool, SHADER_BOOLS, 4); }
IMP(d3d9, IDirect3DDevice9_SetPixelShaderConstantF) { STAT(device_SetPixelShaderConstantF); SET_CONSTANTS(ps_float, PS_FLOATS, 16, C_PS_FLOAT); }
IMP(d3d9, IDirect3DDevice9_GetPixelShaderConstantF) { STAT(device_GetPixelShaderConstantF); GET_CONSTANTS(ps_float, PS_FLOATS, 16); }
IMP(d3d9, IDirect3DDevice9_SetPixelShaderConstantI) { STAT(device_SetPixelShaderConstantI); SET_CONSTANTS(ps_int, SHADER_INTS, 16, C_PS_INT); }
IMP(d3d9, IDirect3DDevice9_GetPixelShaderConstantI) { STAT(device_GetPixelShaderConstantI); GET_CONSTANTS(ps_int, SHADER_INTS, 16); }
IMP(d3d9, IDirect3DDevice9_SetPixelShaderConstantB) { STAT(device_SetPixelShaderConstantB); SET_CONSTANTS(ps_bool, SHADER_BOOLS, 4, C_PS_BOOL); }
IMP(d3d9, IDirect3DDevice9_GetPixelShaderConstantB) { STAT(device_GetPixelShaderConstantB); GET_CONSTANTS(ps_bool, SHADER_BOOLS, 4); }

// ---- transforms, viewport, material, lights, clip planes ----
IMP(d3d9, IDirect3DDevice9_SetTransform) {
  STAT(device_SetTransform); Device9 *d = DEV; u32 n = (u32)ARG(1);
  ARGK("device_SetTransform", n, 0);
  if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  if (n >= TRANSFORMS) { ignored("SetTransform", n); RET(D3D_OK); return; }
  TARGET(d)->transforms[n] = *(const Matrix9 *)GP(ARG(2)); MARK(d, C_TRANSFORM, n); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetTransform) {
  STAT(device_GetTransform); u32 n = (u32)ARG(1);
  if (n >= TRANSFORMS || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  *(Matrix9 *)GP(ARG(2)) = DEV->state.transforms[n]; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_MultiplyTransform) { // the new matrix is (the one given) x (the current one)
  STAT(device_MultiplyTransform); Device9 *d = DEV; u32 n = (u32)ARG(1);
  if (n >= TRANSFORMS || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  const Matrix9 *a = (const Matrix9 *)GP(ARG(2)); Matrix9 b = TARGET(d)->transforms[n], r;
  for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) { float v = 0; for (int k = 0; k < 4; k++) v += a->m[4 * i + k] * b.m[4 * k + j]; r.m[4 * i + j] = v; }
  TARGET(d)->transforms[n] = r; MARK(d, C_TRANSFORM, n); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetViewport) {
  STAT(device_SetViewport); Device9 *d = DEV;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->viewport = *(const Viewport9 *)GP(ARG(1)); MARK1(d, M_VIEWPORT); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetViewport) { STAT(device_GetViewport); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } *(Viewport9 *)GP(ARG(1)) = DEV->state.viewport; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_SetScissorRect) {
  STAT(device_SetScissorRect); Device9 *d = DEV;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->scissor = *(const Rect9 *)GP(ARG(1)); MARK1(d, M_SCISSOR); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetScissorRect) { STAT(device_GetScissorRect); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } *(Rect9 *)GP(ARG(1)) = DEV->state.scissor; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_SetMaterial) {
  STAT(device_SetMaterial); Device9 *d = DEV;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  TARGET(d)->material = *(const Material9 *)GP(ARG(1)); MARK1(d, M_MATERIAL); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetMaterial) { STAT(device_GetMaterial); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } *(Material9 *)GP(ARG(1)) = DEV->state.material; RET(D3D_OK); }
// Lights (d3d9probe.txt): SetLight leaves a new light disabled; LightEnable of a light never set gives it the
// default light first (a white directional light along +z), whether it enables or disables. The type must be
// point (1), spot (2) or directional (3) (the oracle also takes 4, which Direct3D 9 does not define: refused here),
// and a point or spot light's attenuation must not be negative.
IMP(d3d9, IDirect3DDevice9_SetLight) {
  STAT(device_SetLight); Device9 *d = DEV; const Light9 *l = (const Light9 *)GP(ARG(2));
  if (!ARG(2) || l->type < 1 || l->type > 3) { RET(D3DERR_INVALIDCALL); return; }
  if (l->type != 3 && (l->attenuation[0] < 0 || l->attenuation[1] < 0 || l->attenuation[2] < 0)) { RET(D3DERR_INVALIDCALL); return; }
  ARGV("device_SetLight type", l->type);
  LightSlot9 *s = d3d9_light(TARGET(d), (u32)ARG(1), 1);
  s->light = *l; s->set = 1; s->has_light = 1;
  if (d->recording) d3d9_recorded_light(d->recording, (u32)ARG(1));
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetLight) {
  STAT(device_GetLight); LightSlot9 *s = d3d9_light(&DEV->state, (u32)ARG(1), 0);
  if (!ARG(2) || !s || !s->set) { RET(D3DERR_INVALIDCALL); return; }
  *(Light9 *)GP(ARG(2)) = s->light; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_LightEnable) {
  STAT(device_LightEnable); Device9 *d = DEV; ARGV("device_LightEnable enable", ARG(2));
  LightSlot9 *s = d3d9_light(TARGET(d), (u32)ARG(1), 1);
  if (!s->set && !d->recording) { s->light = d3d9_default_light; s->set = 1; }
  s->enabled = ARG(2) != 0; s->has_enable = 1;
  if (d->recording) d3d9_recorded_light(d->recording, (u32)ARG(1));
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetLightEnable) { // an enabled light reads as 128, as Direct3D's own does
  STAT(device_GetLightEnable); LightSlot9 *s = d3d9_light(&DEV->state, (u32)ARG(1), 0);
  if (!ARG(2) || !s || !s->set) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(2)) = s->enabled ? 128 : 0; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetClipPlane) {
  STAT(device_SetClipPlane); Device9 *d = DEV; u32 n = (u32)ARG(1);
  if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  if (n >= CLIP_PLANES) { ignored("SetClipPlane", n); RET(D3D_OK); return; }
  memcpy(TARGET(d)->clip_planes[n], (void *)GP(ARG(2)), 16); MARK(d, C_CLIP_PLANE, n); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetClipPlane) {
  STAT(device_GetClipPlane); u32 n = (u32)ARG(1);
  if (n >= CLIP_PLANES || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  memcpy((void *)GP(ARG(2)), DEV->state.clip_planes[n], 16); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetNPatchMode) { STAT(device_SetNPatchMode); Device9 *d = DEV; ARGV("device_SetNPatchMode segment_count", ARG(1)); TARGET(d)->npatch = MF32(R[RSP] + 8); MARK1(d, M_NPATCH); RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_GetNPatchMode) { STAT(device_GetNPatchMode); fpush(c, DEV->state.npatch); } // a float comes back on the x87 stack
IMP(d3d9, IDirect3DDevice9_SetSoftwareVertexProcessing) { STAT(device_SetSoftwareVertexProcessing); DEV->software_vp = ARG(1) != 0; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_GetSoftwareVertexProcessing) { STAT(device_GetSoftwareVertexProcessing); RET(DEV->software_vp); }

// ---- targets ----
// SetRenderTarget(index, surface): target 0 cannot be none; setting it resets the viewport and the scissor
// rectangle to the whole surface (d3d9probe.txt). Targets are no part of a state block.
IMP(d3d9, IDirect3DDevice9_SetRenderTarget) {
  STAT(device_SetRenderTarget); Device9 *d = DEV; u32 n = (u32)ARG(1); Surface9 *s = (Surface9 *)GP(ARG(2));
  if (n >= 4 || (!n && !s) || (s && (s->o.kind != K_SURFACE || !(s->desc.usage & USAGE_RENDERTARGET)))) { RET(D3DERR_INVALIDCALL); return; }
  ARGV("device_SetRenderTarget to", !s ? 0 : s == d->back_buffer ? 1 : s->o.container ? 2 : 3);   // none, the back buffer, a texture, another surface
  bind((Object9 **)&d->targets[n], (Object9 *)s);
  if (!n) { d->state.viewport = (Viewport9){0, 0, s->desc.width, s->desc.height, 0.0f, 1.0f}; d->state.scissor = (Rect9){0, 0, (s32)s->desc.width, (s32)s->desc.height}; }
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetRenderTarget) {
  STAT(device_GetRenderTarget); Device9 *d = DEV; u32 n = (u32)ARG(1);
  if (n >= 4 || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  Surface9 *s = d->targets[n]; M32(ARG(2)) = (u32)(uptr)s;
  if (!s) { RET(D3DERR_NOTFOUND); return; }
  d3d9_addref(&s->o); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetDepthStencilSurface) {
  STAT(device_SetDepthStencilSurface); Device9 *d = DEV; Surface9 *s = (Surface9 *)GP(ARG(1));
  if (s && (s->o.kind != K_SURFACE || !(s->desc.usage & USAGE_DEPTHSTENCIL))) { RET(D3DERR_INVALIDCALL); return; }
  bind((Object9 **)&d->depth, (Object9 *)s); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetDepthStencilSurface) {
  STAT(device_GetDepthStencilSurface); Device9 *d = DEV;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(1)) = (u32)(uptr)d->depth;
  if (!d->depth) { RET(D3DERR_NOTFOUND); return; }
  d3d9_addref(&d->depth->o); RET(D3D_OK);
}

// ---- scenes, clears, draws ----
IMP(d3d9, IDirect3DDevice9_BeginScene) { STAT(device_BeginScene); Device9 *d = DEV; if (d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 1; RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_EndScene) { STAT(device_EndScene); Device9 *d = DEV; if (!d->in_scene) { RET(D3DERR_INVALIDCALL); return; } d->in_scene = 0; RET(D3D_OK); }
// Clear(rectangle count, rectangles, flags, colour, z, stencil). Flags: TARGET 1, ZBUFFER 2, STENCIL 4. Clearing a
// depth or stencil buffer that is not there is an error by Direct3D's documentation (the oracle lets the stencil
// case pass: d3d9probe.txt).
IMP(d3d9, IDirect3DDevice9_Clear) {
  STAT(device_Clear); Device9 *d = DEV; u32 flags = (u32)ARG(3);
  ARGV("device_Clear flags", flags);
  if ((ARG(1) != 0) != (ARG(2) != 0)) { RET(D3DERR_INVALIDCALL); return; }
  if ((flags & 6) && !d->depth) { RET(D3DERR_INVALIDCALL); return; }
  if ((flags & 4) && d->depth->desc.format != 75 && d->depth->desc.format != 73 && d->depth->desc.format != 79 && d->depth->desc.format != 83) { RET(D3DERR_INVALIDCALL); return; } // only D24S8, D15S1, D24X4S4, D24FS8 have stencil bits
  if (ARG(1)) { static int told; if (!told++) com_unsure("Direct3D 9: Clear of %u rectangles clears the whole target", (u32)ARG(1)); }
  d3d9_gpu_clear(d, flags, (u32)ARG(4), MF32(R[RSP] + 4 + 4 * 5), (u32)ARG(6));
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_ValidateDevice) { STAT(device_ValidateDevice); if (ARG(1)) M32(ARG(1)) = 1; RET(D3D_OK); }

static u32 vertices_of(u32 type, u32 primitives) { // D3DPRIMITIVETYPE: points, line list, line strip, triangle list, strip, fan
  switch (type) { case 1: return primitives; case 2: return 2 * primitives; case 3: return primitives + 1; case 4: return 3 * primitives; case 5: case 6: return primitives + 2; default: return 0; }
}
// What a draw would use, for the statistics: the vertex format, vertex blending, the lights that are on, the target
static void draw_stats(Device9 *d, const char *fvf, const char *blend, const char *lights, const char *target) {
  const State9 *s = &d->state; u32 on = 0;
  if (s->rs[RS_LIGHTING]) for (u32 i = 0; i < s->light_count; i++) on += s->lights[i].set && s->lights[i].enabled;
  d3d9_arg_count(fvf, 0xFFFFFFFFu, s->fvf); d3d9_arg_count(blend, 0xFFFFFFFFu, s->rs[RS_VERTEXBLEND]); d3d9_arg_count(lights, 0xFFFFFFFFu, s->rs[RS_LIGHTING] ? on : 0xFFu);
  d3d9_arg_count(target, 0xFFFFFFFFu, d->targets[0] == d->back_buffer ? 1 : d->targets[0]->o.container ? 2 : 3);
}
// A draw needs a vertex format (the oracle refuses one without) and vertices to read: a draw that reaches past
// its buffers is reported once, since nothing here would otherwise notice it.
static void out_of_range(const char *what, u32 need, u32 have) {
  static int told; if (told++ < 4) rt_log("Direct3D 9: a draw reads %s up to %u of %u", what, need, have);
}
IMP(d3d9, IDirect3DDevice9_DrawPrimitive) { // (primitive type, first vertex, primitive count)
  STAT(device_DrawPrimitive); Device9 *d = DEV; const State9 *s = &d->state; u32 type = (u32)ARG(1), n = vertices_of(type, (u32)ARG(3));
  ARGV("device_DrawPrimitive primitive_type", type);
  if (!s->declaration && !s->fvf) { RET(D3DERR_INVALIDCALL); return; }
  if (type < 1 || type > 6) { com_unsure("Direct3D 9: DrawPrimitive of primitive type %u, which is none", type); RET(D3DERR_INVALIDCALL); return; }
  if (!ARG(3)) { RET(D3D_OK); return; }   // nothing to draw is not an error, nor is a draw outside a scene or without a stream source (d3d9probe.txt)
  if (d3d9_stats) draw_stats(d, "draw DrawPrimitive fvf", "draw DrawPrimitive vertexblend", "draw DrawPrimitive lights", "draw DrawPrimitive target");
  const Buffer9 *b = s->streams[0].buffer;
  if (b && s->streams[0].stride) { u32 have = (b->size - (s->streams[0].offset < b->size ? s->streams[0].offset : b->size)) / s->streams[0].stride; if ((u32)ARG(2) + n > have) out_of_range("vertices", (u32)ARG(2) + n, have); }
  else if (!b) out_of_range("vertices of a stream that has no buffer", n, 0);
  d3d9_gpu_draw(d, type, 0, (u32)ARG(2), (u32)ARG(3), 0);
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_DrawIndexedPrimitive) { // (primitive type, base vertex, lowest index, vertex count, first index, primitive count)
  STAT(device_DrawIndexedPrimitive); Device9 *d = DEV; const State9 *s = &d->state; u32 type = (u32)ARG(1), n = vertices_of(type, (u32)ARG(6));
  ARGV("device_DrawIndexedPrimitive primitive_type", type);
  if (!s->indices || (!s->declaration && !s->fvf)) { RET(D3DERR_INVALIDCALL); return; }
  if (type < 1 || type > 6) { com_unsure("Direct3D 9: DrawIndexedPrimitive of primitive type %u, which is none", type); RET(D3DERR_INVALIDCALL); return; }
  if (!ARG(6)) { RET(D3D_OK); return; }
  if (d3d9_stats) draw_stats(d, "draw DrawIndexedPrimitive fvf", "draw DrawIndexedPrimitive vertexblend", "draw DrawIndexedPrimitive lights", "draw DrawIndexedPrimitive target");
  u32 have = s->indices->size / (s->indices->format == FMT_INDEX16 ? 2 : 4);
  if ((u32)ARG(5) + n > have) out_of_range("indices", (u32)ARG(5) + n, have);
  const Buffer9 *b = s->streams[0].buffer;
  if (b && s->streams[0].stride) { have = (b->size - (s->streams[0].offset < b->size ? s->streams[0].offset : b->size)) / s->streams[0].stride; u32 need = (u32)((s32)ARG(2) + (s32)ARG(3) + (s32)ARG(4)); if (need > have) out_of_range("vertices", need, have); }
  else if (!b) out_of_range("vertices of a stream that has no buffer", n, 0);
  d3d9_gpu_draw(d, type, (s32)ARG(2), (u32)ARG(5), (u32)ARG(6), 1);
  RET(D3D_OK);
}
// The vertices (and indices) come with the call: counted and checked, not drawn yet (no title here has used them). Afterwards stream 0 (and the index buffer) are unbound, as Direct3D leaves them.
IMP(d3d9, IDirect3DDevice9_DrawPrimitiveUP) { // (primitive type, primitive count, vertices, stride)
  STAT(device_DrawPrimitiveUP); Device9 *d = DEV; State9 *s = &d->state; u32 type = (u32)ARG(1);
  ARGV("device_DrawPrimitiveUP primitive_type", type);
  if ((!s->declaration && !s->fvf) || !vertices_of(type, (u32)ARG(2)) || !ARG(2) || !ARG(3) || !ARG(4)) { RET(D3DERR_INVALIDCALL); return; }
  if (d3d9_stats) draw_stats(d, "draw DrawPrimitiveUP fvf", "draw DrawPrimitiveUP vertexblend", "draw DrawPrimitiveUP lights", "draw DrawPrimitiveUP target");
  { static int told; if (!told++) com_unsure("Direct3D 9: DrawPrimitiveUP is not drawn"); }
  bind((Object9 **)&s->streams[0].buffer, 0); s->streams[0].offset = s->streams[0].stride = 0;
  RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_DrawIndexedPrimitiveUP) { // (primitive type, lowest index, vertex count, primitive count, indices, index format, vertices, stride)
  STAT(device_DrawIndexedPrimitiveUP); Device9 *d = DEV; State9 *s = &d->state; u32 type = (u32)ARG(1);
  ARGV("device_DrawIndexedPrimitiveUP primitive_type", type);
  if ((!s->declaration && !s->fvf) || !vertices_of(type, (u32)ARG(4)) || !ARG(4) || !ARG(5) || !ARG(7) || !ARG(8) || (ARG(6) != FMT_INDEX16 && ARG(6) != FMT_INDEX32)) { RET(D3DERR_INVALIDCALL); return; }
  if (d3d9_stats) draw_stats(d, "draw DrawIndexedPrimitiveUP fvf", "draw DrawIndexedPrimitiveUP vertexblend", "draw DrawIndexedPrimitiveUP lights", "draw DrawIndexedPrimitiveUP target");
  { static int told; if (!told++) com_unsure("Direct3D 9: DrawIndexedPrimitiveUP is not drawn"); }
  bind((Object9 **)&s->streams[0].buffer, 0); s->streams[0].offset = s->streams[0].stride = 0; bind((Object9 **)&s->indices, 0);
  RET(D3D_OK);
}
