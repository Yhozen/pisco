// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9, the null device's resources: surfaces, 2D and volume textures with their levels, vertex and index
// buffers, vertex declarations. Each owns its bytes in guest memory, so locking, UpdateSurface and UpdateTexture
// work on real data (D3DX decodes image files into them and reads them back). What the oracle does in the
// corner cases (locking twice, unlocking what is not locked, who holds which reference) is recorded in
// tools/oracle-wine/d3d9probe.txt, and followed here.
#include "d3d9.h"
#include "d3d9_layout.gen.h"

#define SELF ((Object9 *)GP(ARG(0)))
#define DEV ((Device9 *)GP(ARG(0)))
#define FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
#define IID_IDirect3DResource9_text IDirect3DResource9_interface.iid
#define IID_IDirect3DBaseTexture9_text IDirect3DBaseTexture9_interface.iid

// ---- formats: D3DFORMAT, bytes a pixel; block formats: the block's side in pixels and its bytes ----
static const Format9 formats[] = {
  {20, 3}, {21, 4}, {22, 4}, {23, 2}, {24, 2}, {25, 2}, {26, 2}, {27, 1}, {28, 1}, {29, 2}, {30, 2}, {31, 4}, {32, 4}, {33, 4}, {34, 4}, {35, 4}, {36, 8},   // RGB
  {40, 2}, {41, 1}, {50, 1}, {51, 2}, {52, 1}, {81, 2},                                                                                              // palette, luminance
  {60, 2}, {61, 2}, {62, 4}, {63, 4}, {64, 4}, {67, 4},                                                                                              // bump
  {70, 2, 0, 0, 1}, {71, 4, 0, 0, 1}, {73, 2, 0, 0, 1}, {75, 4, 0, 0, 1}, {77, 4, 0, 0, 1}, {79, 4, 0, 0, 1}, {80, 2, 0, 0, 1}, {82, 4, 0, 0, 1}, {83, 4, 0, 0, 1}, // depth
  {111, 2}, {112, 4}, {113, 8}, {114, 4}, {115, 8}, {116, 16},                                                                                       // float
  {FOURCC('D', 'X', 'T', '1'), 0, 4, 8}, {FOURCC('D', 'X', 'T', '2'), 0, 4, 16}, {FOURCC('D', 'X', 'T', '3'), 0, 4, 16}, {FOURCC('D', 'X', 'T', '4'), 0, 4, 16}, {FOURCC('D', 'X', 'T', '5'), 0, 4, 16},
  {FMT_VERTEXDATA, 1}, {FMT_INDEX16, 2}, {FMT_INDEX32, 4},
};
const Format9 *d3d9_format(u32 format) {
  for (unsigned i = 0; i < sizeof formats / sizeof formats[0]; i++) if (formats[i].format == format) return &formats[i];
  return 0;
}
// Rows of pixels are padded to four bytes, as the oracle's are; block formats come in rows of blocks
u32 d3d9_pitch(const Format9 *f, u32 width) { return f->block ? ((width + f->block - 1) / f->block) * f->block_bytes : (width * f->bytes + 3) & ~3u; }
u32 d3d9_rows(const Format9 *f, u32 height) { return f->block ? (height + f->block - 1) / f->block : height; }

static u32 query(Cpu *c, Object9 *o, const char *a, const char *b, const char *k) {
  u64 riid = ARG(1), out = ARG(2);
  if (!out) return E_POINTER;
  if (com_is_iid(riid, IID_IUnknown_text) || (a && com_is_iid(riid, a)) || (b && com_is_iid(riid, b)) || (k && com_is_iid(riid, k))) { d3d9_addref(o); M32(out) = (u32)(uptr)o; return D3D_OK; }
  M32(out) = 0; return E_NOINTERFACE;
}
static u32 get_device(Cpu *c, Object9 *o) {
  if (!ARG(1)) return D3DERR_INVALIDCALL;
  o->device->o.refs++; M32(ARG(1)) = (u32)(uptr)o->device; return D3D_OK;
}

// ---- surfaces ----
Surface9 *d3d9_surface_new(Device9 *d, Object9 *container, u32 level, u32 width, u32 height, u32 format, u32 usage, u32 pool, u32 multisample, u32 quality) {
  const Format9 *f = d3d9_format(format);
  Surface9 *s = (Surface9 *)GP(com_new("d3d9", &IDirect3DSurface9_interface, sizeof(Surface9)));
  s->o.kind = K_SURFACE; s->o.device = d; s->o.container = container; s->level = level;
  s->desc = (SurfaceDesc9){format, RTYPE_SURFACE, usage, pool, multisample, quality, width, height};
  s->pitch = d3d9_pitch(f, width); s->size = s->pitch * d3d9_rows(f, height);
  s->bits = calloc(1, s->size ? s->size : 1);
  // What can be locked (d3d9probe.txt): not a render target, not a texture of the default pool unless it is
  // dynamic; a depth buffer only in the two formats made for it. The back buffer and CreateRenderTarget's
  // surfaces are lockable when their maker says so.
  s->lockable = !(usage & USAGE_RENDERTARGET) && (!(usage & USAGE_DEPTHSTENCIL) || format == 70 || format == 82) && !(container && pool == POOL_DEFAULT && !(usage & USAGE_DYNAMIC));
  return s;
}
static void surface_free(Surface9 *s) { free(s->bits); s->o.vtable = 0; free(s); }
// LockRect(D3DLOCKED_RECT *: pitch, bits; RECT * or null for all of it; flags)
static u32 surface_lock(Surface9 *s, u64 out, u64 rect, u32 flags) {
  if (!out) return D3DERR_INVALIDCALL;
  M32(out) = 0; M32(out + 4) = 0;
  if (s->locked) return D3DERR_INVALIDCALL;
  if (!s->lockable) return D3DERR_INVALIDCALL;
  const Format9 *f = d3d9_format(s->desc.format); u8 *p = s->bits;
  if (rect) {
    const Rect9 *r = (const Rect9 *)GP(rect);
    if (r->left < 0 || r->top < 0 || r->left >= r->right || r->top >= r->bottom || (u32)r->right > s->desc.width || (u32)r->bottom > s->desc.height) return D3DERR_INVALIDCALL;
    p += f->block ? ((u32)r->top / f->block) * s->pitch + ((u32)r->left / f->block) * f->block_bytes : (u32)r->top * s->pitch + (u32)r->left * f->bytes;
  }
  s->locked = 1; s->lock_flags = flags;
  M32(out) = s->pitch; M32(out + 4) = (u32)(uptr)p;
  return D3D_OK;
}
static int surface_unlock(Surface9 *s) { if (!s->locked) return 0; s->locked = 0; if (!(s->lock_flags & LOCK_READONLY)) s->dirty++; return 1; }
IMP(d3d9, IDirect3DSurface9_QueryInterface) { STAT(surface_QueryInterface); RET(query(c, SELF, IDirect3DSurface9_interface.iid, IID_IDirect3DResource9_text, 0)); }
IMP(d3d9, IDirect3DSurface9_AddRef) { STAT(surface_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DSurface9_Release) { STAT(surface_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DSurface9_GetDevice) { STAT(surface_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DSurface9_GetType) { STAT(surface_GetType); RET(RTYPE_SURFACE); }
IMP(d3d9, IDirect3DSurface9_GetDesc) {
  STAT(surface_GetDesc);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  *(SurfaceDesc9 *)GP(ARG(1)) = ((Surface9 *)SELF)->desc; RET(D3D_OK);
}
// GetContainer(riid, void **): the texture a level belongs to; the swap chain for the back buffer; else the device
IMP(d3d9, IDirect3DSurface9_GetContainer) {
  STAT(surface_GetContainer); Surface9 *s = (Surface9 *)SELF; u64 riid = ARG(1), out = ARG(2);
  if (!out) { RET(E_POINTER); return; }
  Object9 *k = s->o.container; const char *own = k ? IDirect3DTexture9_interface.iid : 0;
  if (!k) { if (s == s->o.device->back_buffer) { k = &s->o.device->swapchain->o; own = IDirect3DSwapChain9_interface.iid; } else { k = &s->o.device->o; own = IDirect3DDevice9_interface.iid; } }
  if (com_is_iid(riid, IID_IUnknown_text) || com_is_iid(riid, own) || (s->o.container && (com_is_iid(riid, IID_IDirect3DResource9_text) || com_is_iid(riid, IID_IDirect3DBaseTexture9_text)))) {
    if (k->kind == K_DEVICE) k->refs++; else d3d9_addref(k);
    M32(out) = (u32)(uptr)k; RET(D3D_OK); return;
  }
  M32(out) = 0; RET(E_NOINTERFACE);
}
IMP(d3d9, IDirect3DSurface9_LockRect) { STAT(surface_LockRect); ARGV("surface_LockRect flags", ARG(3)); RET(surface_lock((Surface9 *)SELF, ARG(1), ARG(2), (u32)ARG(3))); }
// Unlocking what is not locked is an error, except for a level of a 2D texture (d3d9probe.txt)
IMP(d3d9, IDirect3DSurface9_UnlockRect) { STAT(surface_UnlockRect); Surface9 *s = (Surface9 *)SELF; RET(surface_unlock(s) || s->o.container ? D3D_OK : D3DERR_INVALIDCALL); }

// ---- textures ----
static u32 full_chain(u32 w, u32 h, u32 d) { u32 n = 1; while (w > 1 || h > 1 || d > 1) { w >>= 1; h >>= 1; d >>= 1; n++; } return n; }
static u32 dim(u32 v, u32 level) { v >>= level; return v ? v : 1; }
// CreateTexture(width, height, levels, usage, format, pool, IDirect3DTexture9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateTexture) {
  STAT(device_CreateTexture); Device9 *d = DEV;
  u32 w = (u32)ARG(1), h = (u32)ARG(2), levels = (u32)ARG(3), usage = (u32)ARG(4), format = (u32)ARG(5), pool = (u32)ARG(6); u64 out = ARG(7);
  ARGV("device_CreateTexture width", w); ARGV("device_CreateTexture height", h); ARGV("device_CreateTexture levels", levels); ARGV("device_CreateTexture usage", usage); ARGV("device_CreateTexture format", format); ARGV("device_CreateTexture pool", pool);
  if (!out) { RET(D3DERR_INVALIDCALL); return; }
  M32(out) = 0;
  const Format9 *f = d3d9_format(format);
  if (!w || !h || pool > POOL_SCRATCH || ARG(8) || ((usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && pool != POOL_DEFAULT)) { RET(D3DERR_INVALIDCALL); return; }
  if (!f) { com_unsure("Direct3D 9: CreateTexture with format %u (0x%x): its layout is not known here", format, format); RET(D3DERR_INVALIDCALL); return; }
  u32 chain = full_chain(w, h, 1);
  if (usage & USAGE_AUTOGENMIPMAP) { if (levels > 1) { RET(D3DERR_INVALIDCALL); return; } levels = 1; }
  if (!levels) levels = chain; if (levels > chain) { RET(D3DERR_INVALIDCALL); return; }
  Texture9 *t = (Texture9 *)GP(com_new("d3d9", &IDirect3DTexture9_interface, sizeof(Texture9)));
  d3d9_object_init(&t->o, K_TEXTURE, d, 0);
  t->format = format; t->usage = usage; t->pool = pool; t->width = w; t->height = h; t->depth = 1; t->levels = levels; t->autogen_filter = 2; // D3DTEXF_LINEAR
  t->surfaces = calloc(levels, sizeof *t->surfaces);
  for (u32 i = 0; i < levels; i++) t->surfaces[i] = d3d9_surface_new(d, &t->o, i, dim(w, i), dim(h, i), format, usage, pool, 0, 0);
  M32(out) = (u32)(uptr)t; RET(D3D_OK);
}
#define TEX ((Texture9 *)GP(ARG(0)))
IMP(d3d9, IDirect3DTexture9_QueryInterface) { STAT(texture_2d_QueryInterface); RET(query(c, SELF, IDirect3DTexture9_interface.iid, IID_IDirect3DBaseTexture9_text, IID_IDirect3DResource9_text)); }
IMP(d3d9, IDirect3DTexture9_AddRef) { STAT(texture_2d_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DTexture9_Release) { STAT(texture_2d_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DTexture9_GetDevice) { STAT(texture_2d_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DTexture9_SetPriority) { STAT(texture_2d_SetPriority); Texture9 *t = TEX; u32 old = t->priority; if (t->pool == POOL_MANAGED) t->priority = (u32)ARG(1); RET(old); }
IMP(d3d9, IDirect3DTexture9_GetPriority) { STAT(texture_2d_GetPriority); RET(TEX->priority); }
IMP(d3d9, IDirect3DTexture9_PreLoad) { STAT(texture_2d_PreLoad); }
IMP(d3d9, IDirect3DTexture9_GetType) { STAT(texture_2d_GetType); RET(RTYPE_TEXTURE); }
IMP(d3d9, IDirect3DTexture9_SetLOD) { STAT(texture_2d_SetLOD); Texture9 *t = TEX; u32 old = t->lod; if (t->pool == POOL_MANAGED) t->lod = ARG(1) < t->levels ? (u32)ARG(1) : t->levels - 1; RET(old); }
IMP(d3d9, IDirect3DTexture9_GetLOD) { STAT(texture_2d_GetLOD); RET(TEX->lod); }
IMP(d3d9, IDirect3DTexture9_GetLevelCount) { STAT(texture_2d_GetLevelCount); RET(TEX->levels); }
IMP(d3d9, IDirect3DTexture9_SetAutoGenFilterType) { STAT(texture_2d_SetAutoGenFilterType); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } TEX->autogen_filter = (u32)ARG(1); RET(D3D_OK); }
IMP(d3d9, IDirect3DTexture9_GetAutoGenFilterType) { STAT(texture_2d_GetAutoGenFilterType); RET(TEX->autogen_filter); }
IMP(d3d9, IDirect3DTexture9_GenerateMipSubLevels) { STAT(texture_2d_GenerateMipSubLevels); }
IMP(d3d9, IDirect3DTexture9_GetLevelDesc) {
  STAT(texture_2d_GetLevelDesc); ARGV("texture_2d_GetLevelDesc level", ARG(1)); Texture9 *t = TEX;
  if (ARG(1) >= t->levels || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  *(SurfaceDesc9 *)GP(ARG(2)) = t->surfaces[ARG(1)]->desc; RET(D3D_OK);
}
IMP(d3d9, IDirect3DTexture9_GetSurfaceLevel) {
  STAT(texture_2d_GetSurfaceLevel); ARGV("texture_2d_GetSurfaceLevel level", ARG(1)); Texture9 *t = TEX;
  if (ARG(1) >= t->levels || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&t->o); M32(ARG(2)) = (u32)(uptr)t->surfaces[ARG(1)]; RET(D3D_OK);
}
IMP(d3d9, IDirect3DTexture9_LockRect) { // (level, D3DLOCKED_RECT *, RECT *, flags)
  STAT(texture_2d_LockRect); ARGV("texture_2d_LockRect level", ARG(1)); ARGV("texture_2d_LockRect flags", ARG(4)); Texture9 *t = TEX;
  if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
  RET(surface_lock(t->surfaces[ARG(1)], ARG(2), ARG(3), (u32)ARG(4)));
}
IMP(d3d9, IDirect3DTexture9_UnlockRect) {
  STAT(texture_2d_UnlockRect); Texture9 *t = TEX;
  if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
  surface_unlock(t->surfaces[ARG(1)]); RET(D3D_OK);
}
IMP(d3d9, IDirect3DTexture9_AddDirtyRect) { STAT(texture_2d_AddDirtyRect); TEX->surfaces[0]->dirty++; RET(D3D_OK); }

// ---- cube textures ----
// CreateCubeTexture(edge length, levels, usage, format, pool, IDirect3DCubeTexture9 **, shared handle): six faces
// (D3DCUBEMAP_FACE_POSITIVE_X 0 ... NEGATIVE_Z 5), each a chain of `levels` surfaces
IMP(d3d9, IDirect3DDevice9_CreateCubeTexture) {
  STAT(device_CreateCubeTexture); Device9 *d = DEV;
  u32 e = (u32)ARG(1), levels = (u32)ARG(2), usage = (u32)ARG(3), format = (u32)ARG(4), pool = (u32)ARG(5); u64 out = ARG(6);
  ARGV("device_CreateCubeTexture levels", levels); ARGV("device_CreateCubeTexture usage", usage); ARGV("device_CreateCubeTexture format", format); ARGV("device_CreateCubeTexture pool", pool);
  if (!out) { RET(D3DERR_INVALIDCALL); return; }
  M32(out) = 0;
  const Format9 *f = d3d9_format(format);
  if (!e || pool > POOL_SCRATCH || ARG(7) || ((usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL)) && pool != POOL_DEFAULT)) { RET(D3DERR_INVALIDCALL); return; }
  if (!f) { com_unsure("Direct3D 9: CreateCubeTexture with format %u (0x%x): its layout is not known here", format, format); RET(D3DERR_INVALIDCALL); return; }
  u32 chain = full_chain(e, e, 1);
  if (usage & USAGE_AUTOGENMIPMAP) { if (levels > 1) { RET(D3DERR_INVALIDCALL); return; } levels = 1; }
  if (!levels) levels = chain; if (levels > chain) { RET(D3DERR_INVALIDCALL); return; }
  Texture9 *t = (Texture9 *)GP(com_new("d3d9", &IDirect3DCubeTexture9_interface, sizeof(Texture9)));
  d3d9_object_init(&t->o, K_CUBETEXTURE, d, 0);
  t->format = format; t->usage = usage; t->pool = pool; t->width = t->height = e; t->depth = 1; t->levels = levels; t->autogen_filter = 2;
  t->surfaces = calloc(6 * levels, sizeof *t->surfaces);
  for (u32 face = 0; face < 6; face++) for (u32 i = 0; i < levels; i++) t->surfaces[face * levels + i] = d3d9_surface_new(d, &t->o, i, dim(e, i), dim(e, i), format, usage, pool, 0, 0);
  M32(out) = (u32)(uptr)t; RET(D3D_OK);
}
static Surface9 *cube_face(Texture9 *t, u64 face, u64 level) { return face < 6 && level < t->levels ? t->surfaces[face * t->levels + level] : 0; }
IMP(d3d9, IDirect3DCubeTexture9_QueryInterface) { STAT(texture_cube_QueryInterface); RET(query(c, SELF, IDirect3DCubeTexture9_interface.iid, IID_IDirect3DBaseTexture9_text, IID_IDirect3DResource9_text)); }
IMP(d3d9, IDirect3DCubeTexture9_AddRef) { STAT(texture_cube_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DCubeTexture9_Release) { STAT(texture_cube_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DCubeTexture9_GetDevice) { STAT(texture_cube_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DCubeTexture9_SetPriority) { STAT(texture_cube_SetPriority); Texture9 *t = TEX; u32 old = t->priority; if (t->pool == POOL_MANAGED) t->priority = (u32)ARG(1); RET(old); }
IMP(d3d9, IDirect3DCubeTexture9_GetPriority) { STAT(texture_cube_GetPriority); RET(TEX->priority); }
IMP(d3d9, IDirect3DCubeTexture9_PreLoad) { STAT(texture_cube_PreLoad); }
IMP(d3d9, IDirect3DCubeTexture9_GetType) { STAT(texture_cube_GetType); RET(RTYPE_CUBETEXTURE); }
IMP(d3d9, IDirect3DCubeTexture9_SetLOD) { STAT(texture_cube_SetLOD); Texture9 *t = TEX; u32 old = t->lod; if (t->pool == POOL_MANAGED) t->lod = ARG(1) < t->levels ? (u32)ARG(1) : t->levels - 1; RET(old); }
IMP(d3d9, IDirect3DCubeTexture9_GetLOD) { STAT(texture_cube_GetLOD); RET(TEX->lod); }
IMP(d3d9, IDirect3DCubeTexture9_GetLevelCount) { STAT(texture_cube_GetLevelCount); RET(TEX->levels); }
IMP(d3d9, IDirect3DCubeTexture9_SetAutoGenFilterType) { STAT(texture_cube_SetAutoGenFilterType); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } TEX->autogen_filter = (u32)ARG(1); RET(D3D_OK); }
IMP(d3d9, IDirect3DCubeTexture9_GetAutoGenFilterType) { STAT(texture_cube_GetAutoGenFilterType); RET(TEX->autogen_filter); }
IMP(d3d9, IDirect3DCubeTexture9_GenerateMipSubLevels) { STAT(texture_cube_GenerateMipSubLevels); }
IMP(d3d9, IDirect3DCubeTexture9_GetLevelDesc) {
  STAT(texture_cube_GetLevelDesc); Texture9 *t = TEX;
  if (ARG(1) >= t->levels || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  *(SurfaceDesc9 *)GP(ARG(2)) = t->surfaces[ARG(1)]->desc; RET(D3D_OK);
}
IMP(d3d9, IDirect3DCubeTexture9_GetCubeMapSurface) { // (face, level, IDirect3DSurface9 **)
  STAT(texture_cube_GetCubeMapSurface); Texture9 *t = TEX; Surface9 *s = cube_face(t, ARG(1), ARG(2));
  if (!s || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&t->o); M32(ARG(3)) = (u32)(uptr)s; RET(D3D_OK);
}
IMP(d3d9, IDirect3DCubeTexture9_LockRect) { // (face, level, D3DLOCKED_RECT *, RECT *, flags)
  STAT(texture_cube_LockRect); Surface9 *s = cube_face(TEX, ARG(1), ARG(2));
  if (!s) { RET(D3DERR_INVALIDCALL); return; }
  RET(surface_lock(s, ARG(3), ARG(4), (u32)ARG(5)));
}
IMP(d3d9, IDirect3DCubeTexture9_UnlockRect) { STAT(texture_cube_UnlockRect); Surface9 *s = cube_face(TEX, ARG(1), ARG(2)); if (!s) { RET(D3DERR_INVALIDCALL); return; } surface_unlock(s); RET(D3D_OK); }
IMP(d3d9, IDirect3DCubeTexture9_AddDirtyRect) { STAT(texture_cube_AddDirtyRect); Surface9 *s = cube_face(TEX, ARG(1), 0); if (s) s->dirty++; RET(D3D_OK); }

// ---- volume textures ----
// CreateVolumeTexture(width, height, depth, levels, usage, format, pool, IDirect3DVolumeTexture9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateVolumeTexture) {
  STAT(device_CreateVolumeTexture); Device9 *d = DEV;
  u32 w = (u32)ARG(1), h = (u32)ARG(2), depth = (u32)ARG(3), levels = (u32)ARG(4), usage = (u32)ARG(5), format = (u32)ARG(6), pool = (u32)ARG(7); u64 out = ARG(8);
  ARGV("device_CreateVolumeTexture depth", depth); ARGV("device_CreateVolumeTexture levels", levels); ARGV("device_CreateVolumeTexture usage", usage);
  ARGV("device_CreateVolumeTexture format", format); ARGV("device_CreateVolumeTexture pool", pool);
  if (!out) { RET(D3DERR_INVALIDCALL); return; }
  M32(out) = 0;
  const Format9 *f = d3d9_format(format);
  if (!w || !h || !depth || pool > POOL_SCRATCH || ARG(9) || (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL))) { RET(D3DERR_INVALIDCALL); return; }
  if (!f || f->block) { com_unsure("Direct3D 9: CreateVolumeTexture with format %u (0x%x) is not implemented", format, format); RET(D3DERR_INVALIDCALL); return; }
  u32 chain = full_chain(w, h, depth);
  if (!levels) levels = chain; if (levels > chain) { RET(D3DERR_INVALIDCALL); return; }
  Texture9 *t = (Texture9 *)GP(com_new("d3d9", &IDirect3DVolumeTexture9_interface, sizeof(Texture9)));
  d3d9_object_init(&t->o, K_VOLUMETEXTURE, d, 0);
  t->format = format; t->usage = usage; t->pool = pool; t->width = w; t->height = h; t->depth = depth; t->levels = levels; t->autogen_filter = 2;
  t->volumes = calloc(levels, sizeof *t->volumes);
  for (u32 i = 0; i < levels; i++) {
    Volume9 *v = (Volume9 *)GP(com_new("d3d9", &IDirect3DVolume9_interface, sizeof(Volume9)));
    v->o.kind = K_VOLUME; v->o.device = d; v->o.container = &t->o; v->level = i;
    v->desc = (VolumeDesc9){format, RTYPE_VOLUME, usage, pool, dim(w, i), dim(h, i), dim(depth, i)};
    v->row_pitch = v->desc.width * f->bytes; v->slice_pitch = v->row_pitch * v->desc.height; v->size = v->slice_pitch * v->desc.depth;
    v->bits = calloc(1, v->size); t->volumes[i] = v;
  }
  M32(out) = (u32)(uptr)t; RET(D3D_OK);
}
// LockBox(D3DLOCKED_BOX *: row pitch, slice pitch, bits; D3DBOX * or null; flags)
static u32 volume_lock(Volume9 *v, u64 out, u64 box, u32 flags) {
  if (!out) return D3DERR_INVALIDCALL;
  M32(out) = 0; M32(out + 4) = 0; M32(out + 8) = 0;
  if (v->locked) return D3DERR_INVALIDCALL;
  u8 *p = v->bits;
  if (box) {
    const Box9 *b = (const Box9 *)GP(box);
    if (b->left >= b->right || b->top >= b->bottom || b->front >= b->back || b->right > v->desc.width || b->bottom > v->desc.height || b->back > v->desc.depth) return D3DERR_INVALIDCALL;
    p += b->front * v->slice_pitch + b->top * v->row_pitch + b->left * d3d9_format(v->desc.format)->bytes;
  }
  v->locked = 1 + !(flags & LOCK_READONLY);
  M32(out) = v->row_pitch; M32(out + 4) = v->slice_pitch; M32(out + 8) = (u32)(uptr)p;
  return D3D_OK;
}
static int volume_unlock(Volume9 *v) { if (!v->locked) return 0; if (v->locked == 2) v->dirty++; v->locked = 0; return 1; }
IMP(d3d9, IDirect3DVolumeTexture9_QueryInterface) { STAT(texture_3d_QueryInterface); RET(query(c, SELF, IDirect3DVolumeTexture9_interface.iid, IID_IDirect3DBaseTexture9_text, IID_IDirect3DResource9_text)); }
IMP(d3d9, IDirect3DVolumeTexture9_AddRef) { STAT(texture_3d_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DVolumeTexture9_Release) { STAT(texture_3d_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DVolumeTexture9_GetDevice) { STAT(texture_3d_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DVolumeTexture9_SetPriority) { STAT(texture_3d_SetPriority); Texture9 *t = TEX; u32 old = t->priority; if (t->pool == POOL_MANAGED) t->priority = (u32)ARG(1); RET(old); }
IMP(d3d9, IDirect3DVolumeTexture9_GetPriority) { STAT(texture_3d_GetPriority); RET(TEX->priority); }
IMP(d3d9, IDirect3DVolumeTexture9_PreLoad) { STAT(texture_3d_PreLoad); }
IMP(d3d9, IDirect3DVolumeTexture9_GetType) { STAT(texture_3d_GetType); RET(RTYPE_VOLUMETEXTURE); }
IMP(d3d9, IDirect3DVolumeTexture9_SetLOD) { STAT(texture_3d_SetLOD); Texture9 *t = TEX; u32 old = t->lod; if (t->pool == POOL_MANAGED) t->lod = ARG(1) < t->levels ? (u32)ARG(1) : t->levels - 1; RET(old); }
IMP(d3d9, IDirect3DVolumeTexture9_GetLOD) { STAT(texture_3d_GetLOD); RET(TEX->lod); }
IMP(d3d9, IDirect3DVolumeTexture9_GetLevelCount) { STAT(texture_3d_GetLevelCount); RET(TEX->levels); }
IMP(d3d9, IDirect3DVolumeTexture9_SetAutoGenFilterType) { STAT(texture_3d_SetAutoGenFilterType); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } TEX->autogen_filter = (u32)ARG(1); RET(D3D_OK); }
IMP(d3d9, IDirect3DVolumeTexture9_GetAutoGenFilterType) { STAT(texture_3d_GetAutoGenFilterType); RET(TEX->autogen_filter); }
IMP(d3d9, IDirect3DVolumeTexture9_GenerateMipSubLevels) { STAT(texture_3d_GenerateMipSubLevels); }
IMP(d3d9, IDirect3DVolumeTexture9_GetLevelDesc) {
  STAT(texture_3d_GetLevelDesc); Texture9 *t = TEX;
  if (ARG(1) >= t->levels || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  *(VolumeDesc9 *)GP(ARG(2)) = t->volumes[ARG(1)]->desc; RET(D3D_OK);
}
IMP(d3d9, IDirect3DVolumeTexture9_GetVolumeLevel) {
  STAT(texture_3d_GetVolumeLevel); ARGV("texture_3d_GetVolumeLevel level", ARG(1)); Texture9 *t = TEX;
  if (ARG(1) >= t->levels || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&t->o); M32(ARG(2)) = (u32)(uptr)t->volumes[ARG(1)]; RET(D3D_OK);
}
IMP(d3d9, IDirect3DVolumeTexture9_LockBox) {
  STAT(texture_3d_LockBox); ARGV("texture_3d_LockBox flags", ARG(4)); Texture9 *t = TEX;
  if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
  RET(volume_lock(t->volumes[ARG(1)], ARG(2), ARG(3), (u32)ARG(4)));
}
IMP(d3d9, IDirect3DVolumeTexture9_UnlockBox) {
  STAT(texture_3d_UnlockBox); Texture9 *t = TEX;
  if (ARG(1) >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
  RET(volume_unlock(t->volumes[ARG(1)]) ? D3D_OK : D3DERR_INVALIDCALL);
}
IMP(d3d9, IDirect3DVolumeTexture9_AddDirtyBox) { STAT(texture_3d_AddDirtyBox); TEX->volumes[0]->dirty++; RET(D3D_OK); }
IMP(d3d9, IDirect3DVolume9_QueryInterface) { STAT(volume_QueryInterface); RET(query(c, SELF, IDirect3DVolume9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DVolume9_AddRef) { STAT(volume_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DVolume9_Release) { STAT(volume_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DVolume9_GetDevice) { STAT(volume_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DVolume9_GetDesc) {
  STAT(volume_GetDesc);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  *(VolumeDesc9 *)GP(ARG(1)) = ((Volume9 *)SELF)->desc; RET(D3D_OK);
}
IMP(d3d9, IDirect3DVolume9_LockBox) { STAT(volume_LockBox); ARGV("volume_LockBox flags", ARG(3)); RET(volume_lock((Volume9 *)SELF, ARG(1), ARG(2), (u32)ARG(3))); }
IMP(d3d9, IDirect3DVolume9_UnlockBox) { STAT(volume_UnlockBox); RET(volume_unlock((Volume9 *)SELF) ? D3D_OK : D3DERR_INVALIDCALL); }

// ---- stand-alone surfaces ----
static u32 create_surface(Cpu *c, u32 w, u32 h, u32 format, u32 usage, u32 pool, u32 multisample, u32 quality, u64 out, u64 shared) {
  if (!out) return D3DERR_INVALIDCALL;
  M32(out) = 0;
  if (!w || !h || shared) return D3DERR_INVALIDCALL;
  if (!d3d9_format(format) || multisample) { com_unsure("Direct3D 9: a surface of format %u (0x%x) with multisampling %u is not implemented", format, format, multisample); return D3DERR_INVALIDCALL; }
  Surface9 *s = d3d9_surface_new(DEV, 0, 0, w, h, format, usage, pool, multisample, quality);
  s->o.refs = 1; DEV->o.refs++;
  M32(out) = (u32)(uptr)s; return D3D_OK;
}
// CreateRenderTarget(width, height, format, multisample, quality, lockable, IDirect3DSurface9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateRenderTarget) {
  STAT(device_CreateRenderTarget); u32 hr = create_surface(c, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), USAGE_RENDERTARGET, POOL_DEFAULT, (u32)ARG(4), (u32)ARG(5), ARG(7), ARG(8));
  if (hr == D3D_OK) ((Surface9 *)GP(M32(ARG(7))))->lockable = ARG(6) != 0;
  RET(hr);
}
// CreateDepthStencilSurface(width, height, format, multisample, quality, discard, IDirect3DSurface9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateDepthStencilSurface) {
  STAT(device_CreateDepthStencilSurface); const Format9 *f = d3d9_format((u32)ARG(3));
  if (f && !f->depth) { RET(D3DERR_INVALIDCALL); return; }
  RET(create_surface(c, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), USAGE_DEPTHSTENCIL, POOL_DEFAULT, (u32)ARG(4), (u32)ARG(5), ARG(7), ARG(8)));
}
// CreateOffscreenPlainSurface(width, height, format, pool, IDirect3DSurface9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateOffscreenPlainSurface) {
  STAT(device_CreateOffscreenPlainSurface);
  if (ARG(4) == POOL_MANAGED) { if (ARG(5)) M32(ARG(5)) = 0; RET(D3DERR_INVALIDCALL); return; }
  RET(create_surface(c, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), 0, (u32)ARG(4), 0, 0, ARG(5), ARG(6)));
}

// ---- copies between surfaces ----
// Copies `w` x `h` pixels (block formats: a multiple of the block, or up to the edge) from (sx, sy) to (dx, dy)
static void copy_pixels(Surface9 *dst, u32 dx, u32 dy, const Surface9 *src, u32 sx, u32 sy, u32 w, u32 h) {
  const Format9 *f = d3d9_format(src->desc.format);
  u32 unit = f->block ? f->block : 1, bytes = f->block ? f->block_bytes : f->bytes;
  u32 rows = (h + unit - 1) / unit, row_bytes = ((w + unit - 1) / unit) * bytes;
  for (u32 y = 0; y < rows; y++) memmove(dst->bits + (dy / unit + y) * dst->pitch + (dx / unit) * bytes, src->bits + (sy / unit + y) * src->pitch + (sx / unit) * bytes, row_bytes);
  dst->dirty++;
}
// UpdateSurface(source, RECT * or null, destination, POINT * or null): from system memory to the default pool
IMP(d3d9, IDirect3DDevice9_UpdateSurface) {
  STAT(device_UpdateSurface); Surface9 *src = (Surface9 *)GP(ARG(1)), *dst = (Surface9 *)GP(ARG(3));
  if (!src || !dst || src->desc.pool != POOL_SYSTEMMEM || dst->desc.pool != POOL_DEFAULT || src->desc.format != dst->desc.format || src->locked || dst->locked
      || (src->desc.usage & USAGE_DEPTHSTENCIL) || (dst->desc.usage & USAGE_DEPTHSTENCIL)) { RET(D3DERR_INVALIDCALL); return; }
  u32 sx = 0, sy = 0, w = src->desc.width, h = src->desc.height, dx = 0, dy = 0;
  if (ARG(2)) { const Rect9 *r = (const Rect9 *)GP(ARG(2)); if (r->left < 0 || r->top < 0 || r->left >= r->right || r->top >= r->bottom || (u32)r->right > w || (u32)r->bottom > h) { RET(D3DERR_INVALIDCALL); return; }
    sx = (u32)r->left; sy = (u32)r->top; w = (u32)(r->right - r->left); h = (u32)(r->bottom - r->top); }
  if (ARG(4)) { dx = M32(ARG(4)); dy = M32(ARG(4) + 4); }
  if ((s32)dx < 0 || (s32)dy < 0 || dx + w > dst->desc.width || dy + h > dst->desc.height) { RET(D3DERR_INVALIDCALL); return; }
  const Format9 *f = d3d9_format(src->desc.format);
  if (f->block && ((sx | sy | dx | dy) % f->block || (w % f->block && sx + w != src->desc.width) || (h % f->block && sy + h != src->desc.height))) { RET(D3DERR_INVALIDCALL); return; }
  copy_pixels(dst, dx, dy, src, sx, sy, w, h); RET(D3D_OK);
}
// UpdateTexture(source, destination): every level of a system-memory texture to a default-pool one of the same
// kind and format. The source may have more levels: its smaller ones are matched from the bottom.
IMP(d3d9, IDirect3DDevice9_UpdateTexture) {
  STAT(device_UpdateTexture); Texture9 *src = (Texture9 *)GP(ARG(1)), *dst = (Texture9 *)GP(ARG(2));
  if (!src || !dst || src->o.kind != dst->o.kind || src->pool != POOL_SYSTEMMEM || dst->pool != POOL_DEFAULT || src->format != dst->format) { RET(D3DERR_INVALIDCALL); return; }
  u32 skip = 0; while (skip < src->levels && (dim(src->width, skip) != dst->width || dim(src->height, skip) != dst->height || dim(src->depth, skip) != dst->depth)) skip++;
  if (skip == src->levels || src->levels - skip < dst->levels) { if (!(dst->usage & USAGE_AUTOGENMIPMAP) || skip == src->levels) { RET(D3DERR_INVALIDCALL); return; } }
  for (u32 i = 0; i < dst->levels && skip + i < src->levels; i++) {
    if (src->o.kind == K_CUBETEXTURE) for (u32 f = 0; f < 6; f++) { Surface9 *a = src->surfaces[f * src->levels + skip + i], *b = dst->surfaces[f * dst->levels + i]; memcpy(b->bits, a->bits, a->size < b->size ? a->size : b->size); b->dirty++; }
    else if (src->o.kind == K_TEXTURE) { Surface9 *a = src->surfaces[skip + i], *b = dst->surfaces[i]; memcpy(b->bits, a->bits, a->size < b->size ? a->size : b->size); b->dirty++; }
    else { Volume9 *a = src->volumes[skip + i], *b = dst->volumes[i]; memcpy(b->bits, a->bits, a->size < b->size ? a->size : b->size); b->dirty++; }
  }
  RET(D3D_OK);
}
// GetRenderTargetData(render target, system-memory surface of its size and format): the target's pixels into the
// surface, read back from the GPU (the app waits for them). With Node's null back end, which has no pixels, they
// are the bytes the runtime holds for the target (what the app wrote, or zeros).
IMP(d3d9, IDirect3DDevice9_GetRenderTargetData) {
  STAT(device_GetRenderTargetData); Surface9 *a = (Surface9 *)GP(ARG(1)), *b = (Surface9 *)GP(ARG(2));
  if (!a || !b || a->o.kind != K_SURFACE || b->o.kind != K_SURFACE || !(a->desc.usage & USAGE_RENDERTARGET) || b->desc.pool != POOL_SYSTEMMEM
      || a->desc.width != b->desc.width || a->desc.height != b->desc.height || a->desc.format != b->desc.format || a->desc.multisample) { RET(D3DERR_INVALIDCALL); return; }
  memcpy(b->bits, a->bits, a->size < b->size ? a->size : b->size);   // what a back end without pixels leaves
  d3d9_gpu_read(a, b->bits, b->pitch); b->dirty++;
  RET(D3D_OK);
}
// ColorFill(surface, RECT * or null, colour): the 32-bit and 16-bit RGB formats
IMP(d3d9, IDirect3DDevice9_ColorFill) {
  STAT(device_ColorFill); Surface9 *s = (Surface9 *)GP(ARG(1)); u32 color = (u32)ARG(3);
  if (!s || s->desc.pool != POOL_DEFAULT || s->locked) { RET(D3DERR_INVALIDCALL); return; }
  Rect9 all = {0, 0, (s32)s->desc.width, (s32)s->desc.height}; const Rect9 *r = ARG(2) ? (const Rect9 *)GP(ARG(2)) : &all;
  u32 fmt = s->desc.format;
  if (fmt != FMT_A8R8G8B8 && fmt != FMT_X8R8G8B8) { com_unsure("Direct3D 9: ColorFill of a surface of format %u is not implemented", fmt); RET(D3DERR_INVALIDCALL); return; }
  for (s32 y = r->top < 0 ? 0 : r->top; y < r->bottom && (u32)y < s->desc.height; y++) for (s32 x = r->left < 0 ? 0 : r->left; x < r->right && (u32)x < s->desc.width; x++) M32((uptr)s->bits + (u32)y * s->pitch + 4 * (u32)x) = color;
  s->dirty++; RET(D3D_OK);
}

// ---- vertex and index buffers ----
static u32 create_buffer(Cpu *c, const ComInterface *iface, u32 kind, u32 size, u32 usage, u32 format, u32 fvf, u32 pool, u64 out, u64 shared) {
  if (!out) return D3DERR_INVALIDCALL;
  M32(out) = 0;
  if (!size || pool > POOL_SYSTEMMEM || shared) return D3DERR_INVALIDCALL;   // not the scratch pool
  Buffer9 *b = (Buffer9 *)GP(com_new("d3d9", iface, sizeof(Buffer9)));
  d3d9_object_init(&b->o, kind, DEV, 0);
  b->format = format; b->usage = usage; b->pool = pool; b->size = size; b->fvf = fvf; b->bits = calloc(1, size);
  M32(out) = (u32)(uptr)b; return D3D_OK;
}
// CreateVertexBuffer(length, usage, FVF, pool, IDirect3DVertexBuffer9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateVertexBuffer) {
  STAT(device_CreateVertexBuffer); ARGV("device_CreateVertexBuffer usage", ARG(2)); ARGV("device_CreateVertexBuffer fvf", ARG(3)); ARGV("device_CreateVertexBuffer pool", ARG(4));
  RET(create_buffer(c, &IDirect3DVertexBuffer9_interface, K_VERTEXBUFFER, (u32)ARG(1), (u32)ARG(2), FMT_VERTEXDATA, (u32)ARG(3), (u32)ARG(4), ARG(5), ARG(6)));
}
// CreateIndexBuffer(length, usage, format, pool, IDirect3DIndexBuffer9 **, shared handle)
IMP(d3d9, IDirect3DDevice9_CreateIndexBuffer) {
  STAT(device_CreateIndexBuffer); ARGV("device_CreateIndexBuffer usage", ARG(2)); ARGV("device_CreateIndexBuffer format", ARG(3)); ARGV("device_CreateIndexBuffer pool", ARG(4));
  if (ARG(3) != FMT_INDEX16 && ARG(3) != FMT_INDEX32) { if (ARG(5)) M32(ARG(5)) = 0; RET(D3DERR_INVALIDCALL); return; }
  RET(create_buffer(c, &IDirect3DIndexBuffer9_interface, K_INDEXBUFFER, (u32)ARG(1), (u32)ARG(2), (u32)ARG(3), 0, (u32)ARG(4), ARG(5), ARG(6)));
}
// Lock(offset, size, void **, flags): a buffer can be locked more than once, and one Unlock too many is not an error (d3d9probe.txt)
static u32 buffer_lock(Cpu *c, Buffer9 *b) {
  u32 offset = (u32)ARG(1), size = (u32)ARG(2); u64 out = ARG(3);
  if (!out) return D3DERR_INVALIDCALL;
  if (offset > b->size || size > b->size - offset) { M32(out) = 0; return D3DERR_INVALIDCALL; }
  b->locked++; if (!(ARG(4) & LOCK_READONLY)) { b->dirty++; d3d9_gpu_buffer_locked(b, offset, size, (u32)ARG(4)); }
  M32(out) = (u32)(uptr)(b->bits + offset); return D3D_OK;
}
#define BUF ((Buffer9 *)GP(ARG(0)))
IMP(d3d9, IDirect3DVertexBuffer9_QueryInterface) { STAT(vertexbuffer_QueryInterface); RET(query(c, SELF, IDirect3DVertexBuffer9_interface.iid, IID_IDirect3DResource9_text, 0)); }
IMP(d3d9, IDirect3DVertexBuffer9_AddRef) { STAT(vertexbuffer_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DVertexBuffer9_Release) { STAT(vertexbuffer_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DVertexBuffer9_GetDevice) { STAT(vertexbuffer_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DVertexBuffer9_SetPriority) { STAT(vertexbuffer_SetPriority); Buffer9 *b = BUF; u32 old = b->priority; if (b->pool == POOL_MANAGED) b->priority = (u32)ARG(1); RET(old); }
IMP(d3d9, IDirect3DVertexBuffer9_GetPriority) { STAT(vertexbuffer_GetPriority); RET(BUF->priority); }
IMP(d3d9, IDirect3DVertexBuffer9_PreLoad) { STAT(vertexbuffer_PreLoad); }
IMP(d3d9, IDirect3DVertexBuffer9_GetType) { STAT(vertexbuffer_GetType); RET(RTYPE_VERTEXBUFFER); }
IMP(d3d9, IDirect3DVertexBuffer9_Lock) { STAT(vertexbuffer_Lock); ARGV("vertexbuffer_Lock flags", ARG(4)); RET(buffer_lock(c, BUF)); }
IMP(d3d9, IDirect3DVertexBuffer9_Unlock) { STAT(vertexbuffer_Unlock); Buffer9 *b = BUF; if (b->locked) b->locked--; RET(D3D_OK); }
IMP(d3d9, IDirect3DVertexBuffer9_GetDesc) {
  STAT(vertexbuffer_GetDesc); Buffer9 *b = BUF;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  *(VertexBufferDesc9 *)GP(ARG(1)) = (VertexBufferDesc9){FMT_VERTEXDATA, RTYPE_VERTEXBUFFER, b->usage, b->pool, b->size, b->fvf}; RET(D3D_OK);
}
IMP(d3d9, IDirect3DIndexBuffer9_QueryInterface) { STAT(indexbuffer_QueryInterface); RET(query(c, SELF, IDirect3DIndexBuffer9_interface.iid, IID_IDirect3DResource9_text, 0)); }
IMP(d3d9, IDirect3DIndexBuffer9_AddRef) { STAT(indexbuffer_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DIndexBuffer9_Release) { STAT(indexbuffer_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DIndexBuffer9_GetDevice) { STAT(indexbuffer_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DIndexBuffer9_SetPriority) { STAT(indexbuffer_SetPriority); Buffer9 *b = BUF; u32 old = b->priority; if (b->pool == POOL_MANAGED) b->priority = (u32)ARG(1); RET(old); }
IMP(d3d9, IDirect3DIndexBuffer9_GetPriority) { STAT(indexbuffer_GetPriority); RET(BUF->priority); }
IMP(d3d9, IDirect3DIndexBuffer9_PreLoad) { STAT(indexbuffer_PreLoad); }
IMP(d3d9, IDirect3DIndexBuffer9_GetType) { STAT(indexbuffer_GetType); RET(RTYPE_INDEXBUFFER); }
IMP(d3d9, IDirect3DIndexBuffer9_Lock) { STAT(indexbuffer_Lock); ARGV("indexbuffer_Lock flags", ARG(4)); RET(buffer_lock(c, BUF)); }
IMP(d3d9, IDirect3DIndexBuffer9_Unlock) { STAT(indexbuffer_Unlock); Buffer9 *b = BUF; if (b->locked) b->locked--; RET(D3D_OK); }
IMP(d3d9, IDirect3DIndexBuffer9_GetDesc) {
  STAT(indexbuffer_GetDesc); Buffer9 *b = BUF;
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  *(IndexBufferDesc9 *)GP(ARG(1)) = (IndexBufferDesc9){b->format, RTYPE_INDEXBUFFER, b->usage, b->pool, b->size}; RET(D3D_OK);
}

// ---- vertex declarations ----
// CreateVertexDeclaration(D3DVERTEXELEMENT9 *, IDirect3DVertexDeclaration9 **): elements up to D3DDECL_END (stream 0xFF)
IMP(d3d9, IDirect3DDevice9_CreateVertexDeclaration) {
  STAT(device_CreateVertexDeclaration); const Element9 *e = (const Element9 *)GP(ARG(1)); u64 out = ARG(2);
  if (!ARG(1) || !out) { RET(D3DERR_INVALIDCALL); return; }
  u32 n = 0; while (e[n].stream != 0xFF && n < 65) n++;
  if (n > 64) { M32(out) = 0; RET(D3DERR_INVALIDCALL); return; }
  for (u32 i = 0; i < n; i++) if (e[i].stream >= STREAMS || e[i].type > 16 || (e[i].offset & 3)) { M32(out) = 0; RET(0x80004005u); return; } // E_FAIL, as Direct3D answers a malformed declaration
  Declaration9 *v = (Declaration9 *)GP(com_new("d3d9", &IDirect3DVertexDeclaration9_interface, sizeof(Declaration9)));
  d3d9_object_init(&v->o, K_DECLARATION, DEV, 0);
  v->count = n + 1; v->elements = malloc((n + 1) * sizeof *e); memcpy(v->elements, e, (n + 1) * sizeof *e);
  M32(out) = (u32)(uptr)v; RET(D3D_OK);
}
// The declaration an FVF code stands for, made once a code and kept by the device (SetFVF binds it; an app that
// asks GetVertexDeclaration afterwards gets it, as from the oracle). The layout is the documented one: position
// (with its blend weights, the last of them indices if the code says so), normal, point size, diffuse, specular,
// then the texture coordinate sets, each 1 to 4 floats.
Declaration9 *d3d9_fvf_declaration(Device9 *d, u32 fvf) {
  for (Declaration9 *v = d->fvf_declarations; v; v = v->next_fvf) if (v->fvf == fvf) return v;
  enum { FLOAT1, FLOAT2, FLOAT3, FLOAT4, COLOR, UBYTE4 };                                   // D3DDECLTYPE
  enum { POSITION, BLENDWEIGHT, BLENDINDICES, NORMAL, PSIZE, TEXCOORD, POSITIONT = 9, COLORS = 10 };   // D3DDECLUSAGE
  static const u8 size[] = {4, 8, 12, 16, 4, 4};
  Element9 e[20]; u32 n = 0, at = 0;
  #define ADD(TYPE, USAGE, INDEX) (e[n++] = (Element9){0, (u16)at, TYPE, 0, USAGE, INDEX}, at += size[TYPE])
  u32 position = fvf & 0x400E, weights = 0;   // D3DFVF_POSITION_MASK: XYZ 2, XYZRHW 4, XYZB1..5 6..0xE, XYZW 0x4002
  if (position == 4) ADD(FLOAT4, POSITIONT, 0); else if (position == 0x4002) ADD(FLOAT4, POSITION, 0); else if (position) ADD(FLOAT3, POSITION, 0);
  if (position >= 6 && position <= 0xE) weights = (position - 4) / 2;
  if (weights) {
    int indexed = (fvf & 0x9000) != 0;        // D3DFVF_LASTBETA_UBYTE4 0x1000, D3DFVF_LASTBETA_D3DCOLOR 0x8000: the last "weight" is four matrix indices
    u32 floats = weights - (u32)indexed; if (floats > 4) floats = 4;
    if (floats) ADD(floats - 1, BLENDWEIGHT, 0);
    if (indexed || weights == 5) ADD((fvf & 0x8000) ? COLOR : UBYTE4, BLENDINDICES, 0);
  }
  if (fvf & 0x10) ADD(FLOAT3, NORMAL, 0);
  if (fvf & 0x20) ADD(FLOAT1, PSIZE, 0);
  if (fvf & 0x40) ADD(COLOR, COLORS, 0);
  if (fvf & 0x80) ADD(COLOR, COLORS, 1);
  for (u32 i = 0, sets = (fvf >> 8) & 0xF; i < sets && i < 8; i++) { u32 form = (fvf >> (16 + 2 * i)) & 3; ADD(form == 0 ? FLOAT2 : form == 1 ? FLOAT3 : form == 2 ? FLOAT4 : FLOAT1, TEXCOORD, (u8)i); }
  #undef ADD
  e[n++] = (Element9){0xFF, 0, 17, 0, 0, 0};   // D3DDECL_END
  Declaration9 *v = (Declaration9 *)GP(com_new("d3d9", &IDirect3DVertexDeclaration9_interface, sizeof(Declaration9)));
  v->o.kind = K_DECLARATION; v->o.device = d; v->o.held = 1;   // the device's own: no reference of the app's until it asks
  v->count = n; v->fvf = fvf; v->elements = malloc(n * sizeof *e); memcpy(v->elements, e, n * sizeof *e);
  v->next_fvf = d->fvf_declarations; d->fvf_declarations = v;
  return v;
}
IMP(d3d9, IDirect3DVertexDeclaration9_QueryInterface) { STAT(vertex_declaration_QueryInterface); RET(query(c, SELF, IDirect3DVertexDeclaration9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DVertexDeclaration9_AddRef) { STAT(vertex_declaration_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DVertexDeclaration9_Release) { STAT(vertex_declaration_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DVertexDeclaration9_GetDevice) { STAT(vertex_declaration_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DVertexDeclaration9_GetDeclaration) { // (D3DVERTEXELEMENT9 * or null, count *)
  STAT(vertex_declaration_GetDeclaration); Declaration9 *v = (Declaration9 *)SELF;
  if (!ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  M32(ARG(2)) = v->count; if (ARG(1)) memcpy((void *)GP(ARG(1)), v->elements, v->count * sizeof(Element9));
  RET(D3D_OK);
}

// ---- the end of an object: no reference and no use left ----
void d3d9_destroy(Object9 *o) {
  d3d9_gpu_release(o);
  switch (o->kind) {
  case K_SURFACE: surface_free((Surface9 *)o); return;
  case K_TEXTURE: { Texture9 *t = (Texture9 *)o; for (u32 i = 0; i < t->levels; i++) surface_free(t->surfaces[i]); free(t->surfaces); break; }
  case K_CUBETEXTURE: { Texture9 *t = (Texture9 *)o; for (u32 i = 0; i < 6 * t->levels; i++) surface_free(t->surfaces[i]); free(t->surfaces); break; }
  case K_VOLUMETEXTURE: { Texture9 *t = (Texture9 *)o; for (u32 i = 0; i < t->levels; i++) { free(t->volumes[i]->bits); t->volumes[i]->o.vtable = 0; free(t->volumes[i]); } free(t->volumes); break; }
  case K_VERTEXBUFFER: case K_INDEXBUFFER: free(((Buffer9 *)o)->bits); break;
  case K_DECLARATION: free(((Declaration9 *)o)->elements); break;
  case K_VERTEXSHADER: case K_PIXELSHADER: free(((Shader9 *)o)->tokens); break;
  case K_STATEBLOCK: d3d9_stateblock_destroy((StateBlock9 *)o); return;
  default: return;   // the swap chain goes with its device
  }
  o->vtable = 0; free(o);
}

// ---- shaders: CreateVertexShader / CreatePixelShader (bytecode, IDirect3D*Shader9 **) keep a copy of the tokens. A
// shader of a model the translator does not take is refused here, as a device without that model refuses it.
#include "d3d9_sm.h"
static u32 create_shader(Cpu *c, u32 kind) {
  u64 out = ARG(2); const u32 *t = (const u32 *)GP(ARG(1));
  if (!ARG(1) || !out) return D3DERR_INVALIDCALL;
  M32(out) = 0;
  u32 v = t[0], major = (v >> 8) & 0xFF;
  if ((v >> 16) != (kind == K_VERTEXSHADER ? 0xFFFEu : 0xFFFFu) || major < 1 || major > 2) {
    char name[16]; sm_version_name(v, name, sizeof name);
    com_unsure("Direct3D 9: Create%sShader of %s: shader model 3 and others are not translated", kind == K_VERTEXSHADER ? "Vertex" : "Pixel", name);
    return D3DERR_INVALIDCALL;
  }
  size_t n = sm_length(t, 1u << 16);
  if (!n) return D3DERR_INVALIDCALL;
  Shader9 *s = (Shader9 *)GP(com_new("d3d9", kind == K_VERTEXSHADER ? &IDirect3DVertexShader9_interface : &IDirect3DPixelShader9_interface, sizeof(Shader9)));
  d3d9_object_init(&s->o, kind, DEV, 0);
  s->count = (u32)n; s->tokens = malloc(4 * n); memcpy(s->tokens, t, 4 * n);
  // SW_D3D9_SHADERS=1: each bytecode's length and hash, to set against the ones captured under Wine (docs/tag.md:
  // tools/oracle-wine/d3dp.c writes them; tools/oracle-wine/sm-census.py --hashes prints the same line for each)
  static int show = -1; if (show < 0) { const char *e = getenv("SW_D3D9_SHADERS"); show = e && *e == '1'; }
  if (show) { u64 h = 0xcbf29ce484222325ull; for (size_t i = 0; i < 4 * n; i++) { h ^= ((const u8 *)t)[i]; h *= 0x100000001b3ull; }
    char name[16]; sm_version_name(v, name, sizeof name); rt_log("Direct3D 9 bytecode %s %zu bytes fnv64 %016llx", name, 4 * n, (unsigned long long)h); }
  M32(out) = (u32)(uptr)s; return D3D_OK;
}
IMP(d3d9, IDirect3DDevice9_CreateVertexShader) { STAT(device_CreateVertexShader); RET(create_shader(c, K_VERTEXSHADER)); }
IMP(d3d9, IDirect3DDevice9_CreatePixelShader) { STAT(device_CreatePixelShader); RET(create_shader(c, K_PIXELSHADER)); }
// GetFunction(void * or null, UINT *size): the bytecode, or its size in bytes
static u32 get_function(Cpu *c, Shader9 *s) {
  if (!ARG(2)) return D3DERR_INVALIDCALL;
  if (!ARG(1)) { M32(ARG(2)) = 4 * s->count; return D3D_OK; }
  if (M32(ARG(2)) < 4 * s->count) return D3DERR_INVALIDCALL;
  memcpy((void *)GP(ARG(1)), s->tokens, 4 * s->count); return D3D_OK;
}
IMP(d3d9, IDirect3DVertexShader9_QueryInterface) { STAT(vertexshader_QueryInterface); RET(query(c, SELF, IDirect3DVertexShader9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DVertexShader9_AddRef) { STAT(vertexshader_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DVertexShader9_Release) { STAT(vertexshader_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DVertexShader9_GetDevice) { STAT(vertexshader_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DVertexShader9_GetFunction) { STAT(vertexshader_GetFunction); RET(get_function(c, (Shader9 *)SELF)); }
IMP(d3d9, IDirect3DPixelShader9_QueryInterface) { STAT(pixelshader_QueryInterface); RET(query(c, SELF, IDirect3DPixelShader9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DPixelShader9_AddRef) { STAT(pixelshader_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DPixelShader9_Release) { STAT(pixelshader_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DPixelShader9_GetDevice) { STAT(pixelshader_GetDevice); RET(get_device(c, SELF)); }
IMP(d3d9, IDirect3DPixelShader9_GetFunction) { STAT(pixelshader_GetFunction); RET(get_function(c, (Shader9 *)SELF)); }

// ---- the buffers of the UP draws: the device's own, invisible to the app (no reference of its changes), grown
// as needed and written whole for each draw (a DISCARD: the back end may hand out new storage)
Buffer9 *d3d9_up_buffer(Device9 *d, int which, const void *src, u32 bytes, u32 format) {
  Buffer9 *b = d->up[which];
  if (!b || b->size < bytes) {
    u32 size = 1u << 16; while (size < bytes) size *= 2;
    if (b) { d3d9_gpu_release(&b->o); free(b->bits); b->o.vtable = 0; free(b); }
    b = (Buffer9 *)GP(com_new("d3d9", which ? &IDirect3DIndexBuffer9_interface : &IDirect3DVertexBuffer9_interface, sizeof(Buffer9)));
    b->o.kind = which ? K_INDEXBUFFER : K_VERTEXBUFFER; b->o.device = d; b->o.refs = 1;
    b->usage = USAGE_DYNAMIC; b->pool = POOL_DEFAULT; b->size = size; b->bits = calloc(1, size);
    d->up[which] = b;
  }
  b->format = format;
  memcpy(b->bits, src, bytes); b->dirty++;
  d3d9_gpu_buffer_locked(b, 0, bytes, LOCK_DISCARD);
  return b;
}
