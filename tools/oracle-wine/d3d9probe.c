// SPDX-License-Identifier: GPL-2.0-or-later
// d3d9probe: asks the oracle's Direct3D 9 what an app can ask before and just after it creates its device, and
// prints the answers one fact a line, so our own Direct3D 9 can give the same ones (tools/gen-d3d9-oracle.py turns
// the output into rt/windows-x86/d3d9_oracle.gen.h):
//   the adapter's identity and display modes, CheckDeviceFormat for the usual formats and uses, and a new device's
//   state: every render state, texture stage state and sampler state, the viewport, the material, what a light is
//   before it is set. The lines that start with "note" are behaviour checked once and kept for the record.
// Build and run (32-bit, as the apps are):
//   $MINGW/bin/i686-w64-mingw32-clang tools/oracle-wine/d3d9probe.c -o out/caps32/d3d9probe.exe -ld3d9 -luser32
//   tools/oracle-wine/oracle.sh run <title> out/caps32/d3d9probe.exe | tr -d '\r' > tools/oracle-wine/d3d9probe.txt
// in an app's prefix: its 1024x768 virtual desktop decides the display modes (a prefix without one has a single mode).
#define COBJMACROS
#include <d3d9.h>
#include <stdio.h>

static unsigned long refs(IUnknown *u) { IUnknown_AddRef(u); return IUnknown_Release(u); }
int main(void) {
  IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION); D3DADAPTER_IDENTIFIER9 id; D3DDISPLAYMODE m;
  if (!d3d) { printf("Direct3DCreate9 failed\n"); return 1; }
  printf("# What the oracle's Direct3D 9 answers (Wine 11.22, wined3d on OpenGL, llvmpipe). Made by tools/oracle-wine/d3d9probe.c.\n");
  printf("adapters %u\n", IDirect3D9_GetAdapterCount(d3d));
  if (IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id) == D3D_OK) {
    printf("driver %s\ndescription %s\ndevice_name %s\n", id.Driver, id.Description, id.DeviceName);
    printf("driver_version 0x%08lx 0x%08lx\n", (unsigned long)id.DriverVersion.HighPart, (unsigned long)id.DriverVersion.LowPart);
    printf("vendor 0x%04lx\ndevice 0x%04lx\nsubsys 0x%08lx\nrevision 0x%lx\nwhql %lu\n", id.VendorId, id.DeviceId, id.SubSysId, id.Revision, id.WHQLLevel);
    printf("identifier"); for (unsigned i = 0; i < 16; i++) printf(" %02x", ((unsigned char *)&id.DeviceIdentifier)[i]); printf("\n");
  }
  if (IDirect3D9_GetAdapterDisplayMode(d3d, 0, &m) == D3D_OK) printf("display %u %u %u %u\n", m.Width, m.Height, m.RefreshRate, (unsigned)m.Format);
  for (unsigned f = 20; f <= 35; f++) {   // mode <asked format> <index> <width> <height> <refresh> <format>
    unsigned n = IDirect3D9_GetAdapterModeCount(d3d, 0, f);
    for (unsigned i = 0; i < n; i++) if (IDirect3D9_EnumAdapterModes(d3d, 0, f, i, &m) == D3D_OK) printf("mode %u %u %u %u %u %u\n", f, i, m.Width, m.Height, m.RefreshRate, (unsigned)m.Format);
  }
  // format <format> <usage> <resource type> <HRESULT>: against an X8R8G8B8 display, on the HAL device
  static const unsigned formats[] = {20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 40, 41, 50, 51, 52, 60, 61, 62, 63, 64, 67, 70, 71, 73, 75, 77, 79, 80, 81, 82, 83,
    110, 111, 112, 113, 114, 115, 116, 117, MAKEFOURCC('D', 'X', 'T', '1'), MAKEFOURCC('D', 'X', 'T', '2'), MAKEFOURCC('D', 'X', 'T', '3'), MAKEFOURCC('D', 'X', 'T', '4'), MAKEFOURCC('D', 'X', 'T', '5'),
    MAKEFOURCC('U', 'Y', 'V', 'Y'), MAKEFOURCC('Y', 'U', 'Y', '2'),
    MAKEFOURCC('R', 'G', 'B', 'G'), MAKEFOURCC('G', 'R', 'G', 'B')};   // 110, 117 and the last two: what D3DX asks for Tag
  // usages: none, render target, depth, dynamic, generated mip maps, and the queries (post-pixel-shader blending 0x80000, filtering 0x20000, sRGB read 0x10000)
  static const unsigned usages[] = {0, 1, 2, 0x200, 0x400, 0x201, 0x401, 0x80000, 0x80001, 0x20000, 0x10000}, types[] = {1, 3, 4, 5};
  for (unsigned t = 0; t < sizeof types / sizeof types[0]; t++) for (unsigned u = 0; u < sizeof usages / sizeof usages[0]; u++) for (unsigned f = 0; f < sizeof formats / sizeof formats[0]; f++)
    printf("format %u 0x%x %u 0x%08lx\n", formats[f], usages[u], types[t], (unsigned long)IDirect3D9_CheckDeviceFormat(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usages[u], types[t], formats[f]));
  { D3DCAPS9 hal, ref; HRESULT h1 = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &hal), h2 = IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_REF, &ref); unsigned differ = 0;
    for (unsigned i = 1; i < sizeof hal / 4; i++) differ += ((DWORD *)&hal)[i] != ((DWORD *)&ref)[i];
    printf("note GetDeviceCaps of the reference device: 0x%08lx (HAL: 0x%08lx), its DeviceType %u, fields after it that differ from the HAL's: %u; of device type 3: 0x%08lx\n", (unsigned long)h2, (unsigned long)h1, (unsigned)ref.DeviceType, differ, (unsigned long)IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_SW, &ref)); }
  static const unsigned depths[] = {70, 71, 73, 75, 77, 79, 80, 82, 83};
  for (unsigned f = 0; f < sizeof depths / sizeof depths[0]; f++) for (unsigned t = 21; t <= 23; t++)
    printf("depth_match %u %u 0x%08lx\n", t, depths[f], (unsigned long)IDirect3D9_CheckDepthStencilMatch(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, t, depths[f]));
  for (unsigned t = 20; t <= 25; t++) for (unsigned w = 0; w < 2; w++)
    printf("device_type %u %u 0x%08lx\n", t, w, (unsigned long)IDirect3D9_CheckDeviceType(d3d, 0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, t, w));

  // a device like the title's: windowed, 640x480, hardware vertex processing (not pure, so that its state can be read)
  HWND w = CreateWindowA("static", "probe", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, 0, 0, 0, 0);
  { // what CreateDevice fills in when the app leaves it open, and what it does to the x87 control word
    RECT cr; GetClientRect(w, &cr); D3DPRESENT_PARAMETERS q = {0}; q.Windowed = TRUE; q.SwapEffect = D3DSWAPEFFECT_DISCARD; IDirect3DDevice9 *d2 = 0; DWORD z = 77;
    unsigned short cw0 = 0, cw1 = 0; __asm__("fnstcw %0" : "=m"(cw0));
    HRESULT h = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &q, &d2); __asm__("fnstcw %0" : "=m"(cw1));
    printf("note CreateDevice with sizes, format and count left 0 and no depth buffer: 0x%08lx, back buffer %ux%u (the client area is %ldx%ld) format %u count %u, device window %s", (unsigned long)h, q.BackBufferWidth, q.BackBufferHeight, cr.right, cr.bottom, (unsigned)q.BackBufferFormat, q.BackBufferCount, q.hDeviceWindow ? "set" : "left 0");
    if (d2) { IDirect3DDevice9_GetRenderState(d2, D3DRS_ZENABLE, &z); printf(", ZENABLE %lu", z); IDirect3DSurface9 *ds = (void *)1; printf(", GetDepthStencilSurface 0x%08lx", (unsigned long)IDirect3DDevice9_GetDepthStencilSurface(d2, &ds));
      printf("\nnote a draw on a device whose vertex format was never set: 0x%08lx", (unsigned long)IDirect3DDevice9_DrawPrimitive(d2, D3DPT_TRIANGLELIST, 0, 1)); IDirect3DDevice9_Release(d2); }
    printf("\nnote the x87 control word: 0x%04x before CreateDevice, 0x%04x after (without D3DCREATE_FPU_PRESERVE)\n", cw0, cw1); __asm__("fldcw %0" : : "m"(cw0)); }
  D3DPRESENT_PARAMETERS pp = {0}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferWidth = 640; pp.BackBufferHeight = 480;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24X8; pp.hDeviceWindow = w;
  IDirect3DDevice9 *dev = 0;
  HRESULT hr = IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  if (hr != D3D_OK) { printf("CreateDevice failed: 0x%08lx\n", (unsigned long)hr); return 1; }
  printf("note CreateDevice: Direct3D references %lu, device references %lu, back buffers %u, back buffer format %u\n", refs((IUnknown *)d3d), refs((IUnknown *)dev), pp.BackBufferCount, (unsigned)pp.BackBufferFormat);
  DWORD v;
  for (unsigned i = 7; i <= 209; i++) if (IDirect3DDevice9_GetRenderState(dev, i, &v) == D3D_OK) printf("rs %u 0x%08lx\n", i, v);
  for (unsigned s = 0; s < 8; s++) for (unsigned i = 1; i <= 32; i++) if (IDirect3DDevice9_GetTextureStageState(dev, s, i, &v) == D3D_OK) printf("tss %u %u 0x%08lx\n", s, i, v);
  static const unsigned samplers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 256, 257, 258, 259, 260};
  for (unsigned s = 0; s < sizeof samplers / sizeof samplers[0]; s++) for (unsigned i = 1; i <= 13; i++) if (IDirect3DDevice9_GetSamplerState(dev, samplers[s], i, &v) == D3D_OK) printf("samp %u %u 0x%08lx\n", samplers[s], i, v);
  D3DVIEWPORT9 vp; if (IDirect3DDevice9_GetViewport(dev, &vp) == D3D_OK) printf("viewport %lu %lu %lu %lu %g %g\n", vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ);
  D3DMATERIAL9 mat; if (IDirect3DDevice9_GetMaterial(dev, &mat) == D3D_OK) { printf("material"); for (unsigned i = 0; i < 17; i++) printf(" %g", ((float *)&mat)[i]); printf("\n"); }
  RECT sc; if (IDirect3DDevice9_GetScissorRect(dev, &sc) == D3D_OK) printf("scissor %ld %ld %ld %ld\n", sc.left, sc.top, sc.right, sc.bottom);
  DWORD fvf = 77; hr = IDirect3DDevice9_GetFVF(dev, &fvf); printf("fvf 0x%lx (0x%08lx)\n", fvf, (unsigned long)hr);
  printf("npatch %g\n", IDirect3DDevice9_GetNPatchMode(dev));
  printf("texture_mem 0x%08x\n", IDirect3DDevice9_GetAvailableTextureMem(dev));
  printf("cooperative 0x%08lx\n", (unsigned long)IDirect3DDevice9_TestCooperativeLevel(dev));
  float plane[4] = {9, 9, 9, 9}; hr = IDirect3DDevice9_GetClipPlane(dev, 0, plane); printf("clip_plane %g %g %g %g (0x%08lx)\n", plane[0], plane[1], plane[2], plane[3], (unsigned long)hr);
  D3DMATRIX mx; static const unsigned ts[] = {2, 3, 16, 23, 256, 257, 511};
  for (unsigned i = 0; i < sizeof ts / sizeof ts[0]; i++) { hr = IDirect3DDevice9_GetTransform(dev, ts[i], &mx); printf("transform %u (0x%08lx)", ts[i], (unsigned long)hr); for (unsigned k = 0; k < 16; k++) printf(" %g", ((float *)&mx)[k]); printf("\n"); }
  // lights: one that was never set, one enabled without being set, one set
  D3DLIGHT9 l; BOOL on = 77;
  hr = IDirect3DDevice9_GetLight(dev, 3, &l); printf("note GetLight of a light never set: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DDevice9_GetLightEnable(dev, 3, &on); printf("note GetLightEnable of a light never set: 0x%08lx, %d\n", (unsigned long)hr, (int)on);
  hr = IDirect3DDevice9_LightEnable(dev, 5, TRUE); printf("note LightEnable(5, TRUE) of a light never set: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DDevice9_GetLight(dev, 5, &l);
  printf("light %u", (unsigned)l.Type); for (unsigned i = 1; i < 26; i++) printf(" %g", ((float *)&l)[i]); printf(" (0x%08lx)\n", (unsigned long)hr);
  on = 77; hr = IDirect3DDevice9_GetLightEnable(dev, 5, &on); printf("note GetLightEnable after LightEnable(TRUE): 0x%08lx, %d\n", (unsigned long)hr, (int)on);
  hr = IDirect3DDevice9_LightEnable(dev, 9, FALSE); printf("note LightEnable(9, FALSE) of a light never set: 0x%08lx", (unsigned long)hr); hr = IDirect3DDevice9_GetLight(dev, 9, &l); printf(", GetLight then: 0x%08lx\n", (unsigned long)hr);
  memset(&l, 0, sizeof l); l.Type = D3DLIGHT_POINT; l.Range = 10; l.Attenuation0 = 1;
  hr = IDirect3DDevice9_SetLight(dev, 20, &l); on = 77; IDirect3DDevice9_GetLightEnable(dev, 20, &on); printf("note SetLight(20): 0x%08lx, enabled %d\n", (unsigned long)hr, (int)on);
  l.Type = 0; hr = IDirect3DDevice9_SetLight(dev, 21, &l); printf("note SetLight with type 0: 0x%08lx\n", (unsigned long)hr);
  l.Type = D3DLIGHT_POINT; l.Range = -1; hr = IDirect3DDevice9_SetLight(dev, 21, &l); printf("note SetLight, a point light with range -1: 0x%08lx\n", (unsigned long)hr);
  l.Range = 10; l.Attenuation0 = -1; hr = IDirect3DDevice9_SetLight(dev, 22, &l); printf("note SetLight, a point light with attenuation -1: 0x%08lx", (unsigned long)hr);
  l.Type = D3DLIGHT_DIRECTIONAL; hr = IDirect3DDevice9_SetLight(dev, 22, &l); printf(", a directional light with it: 0x%08lx", (unsigned long)hr); l.Type = 4; printf(", type 4: 0x%08lx\n", (unsigned long)IDirect3DDevice9_SetLight(dev, 23, &l));
  // out-of-range indices
  printf("note out of range: SetRenderState(300) 0x%08lx, SetTextureStageState(stage 8) 0x%08lx, (state 40) 0x%08lx, SetSamplerState(sampler 16) 0x%08lx, (state 20) 0x%08lx, SetTexture(stage 16) 0x%08lx, SetStreamSource(16) 0x%08lx, SetTransform(600) 0x%08lx, SetClipPlane(8) 0x%08lx\n",
    (unsigned long)IDirect3DDevice9_SetRenderState(dev, 300, 1), (unsigned long)IDirect3DDevice9_SetTextureStageState(dev, 8, 1, 1), (unsigned long)IDirect3DDevice9_SetTextureStageState(dev, 0, 40, 1),
    (unsigned long)IDirect3DDevice9_SetSamplerState(dev, 16, 1, 1), (unsigned long)IDirect3DDevice9_SetSamplerState(dev, 0, 20, 1), (unsigned long)IDirect3DDevice9_SetTexture(dev, 16, 0),
    (unsigned long)IDirect3DDevice9_SetStreamSource(dev, 16, 0, 0, 0), (unsigned long)IDirect3DDevice9_SetTransform(dev, 600, &mx), (unsigned long)IDirect3DDevice9_SetClipPlane(dev, 8, plane));
  { IDirect3DSurface9 *none = (void *)1; hr = IDirect3DDevice9_GetRenderTarget(dev, 1, &none); printf("note GetRenderTarget(1) when there is none: 0x%08lx, surface %p\n", (unsigned long)hr, (void *)none); }
  printf("note Clear: stencil with a D24X8 depth buffer 0x%08lx, target and depth 0x%08lx\n", (unsigned long)IDirect3DDevice9_Clear(dev, 0, 0, D3DCLEAR_STENCIL, 0, 1.0f, 0), (unsigned long)IDirect3DDevice9_Clear(dev, 0, 0, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0));
  // queries
  for (unsigned t = 0; t <= 18; t++) printf("query %u 0x%08lx\n", t, (unsigned long)IDirect3DDevice9_CreateQuery(dev, t, 0));

  // references: a texture, its surface, the device
  unsigned long dev0 = refs((IUnknown *)dev);
  IDirect3DTexture9 *tex = 0; IDirect3DSurface9 *surf = 0, *surf2 = 0; D3DLOCKED_RECT lr; D3DSURFACE_DESC sd;
  hr = IDirect3DDevice9_CreateTexture(dev, 256, 128, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, 0);
  printf("note CreateTexture: 0x%08lx, texture references %lu, device references %lu (before: %lu)\n", (unsigned long)hr, refs((IUnknown *)tex), refs((IUnknown *)dev), dev0);
  hr = IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf); printf("note GetSurfaceLevel: texture references %lu, surface references %lu, device %lu\n", refs((IUnknown *)tex), refs((IUnknown *)surf), refs((IUnknown *)dev));
  hr = IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf2); printf("note GetSurfaceLevel again: the same surface: %d, texture references %lu, surface references %lu\n", surf == surf2, refs((IUnknown *)tex), refs((IUnknown *)surf));
  printf("note surface Release returns %lu", IDirect3DSurface9_Release(surf2)); printf(", then texture references %lu\n", refs((IUnknown *)tex));
  hr = IDirect3DTexture9_LockRect(tex, 0, &lr, 0, 0); printf("note LockRect 256x128 A8R8G8B8: 0x%08lx, pitch %d\n", (unsigned long)hr, lr.Pitch);
  hr = IDirect3DTexture9_LockRect(tex, 0, &lr, 0, 0); printf("note LockRect while locked: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DTexture9_UnlockRect(tex, 0); printf("note UnlockRect: 0x%08lx", (unsigned long)hr); hr = IDirect3DTexture9_UnlockRect(tex, 0); printf(", again: 0x%08lx\n", (unsigned long)hr);
  printf("note UnlockRect of the surface itself when it is not locked: 0x%08lx\n", (unsigned long)IDirect3DSurface9_UnlockRect(surf));
  hr = IDirect3DTexture9_GetLevelDesc(tex, 0, &sd); printf("note GetLevelDesc: format %u type %u usage 0x%lx pool %u multisample %u quality %lu %ux%u\n", (unsigned)sd.Format, (unsigned)sd.Type, sd.Usage, (unsigned)sd.Pool, (unsigned)sd.MultiSampleType, sd.MultiSampleQuality, sd.Width, sd.Height);
  hr = IDirect3DTexture9_GetLevelDesc(tex, 1, &sd); printf("note GetLevelDesc of a level that is not there: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex); printf("note SetTexture: texture references %lu\n", refs((IUnknown *)tex));
  printf("note texture Release while bound and with a surface held returns %lu", IDirect3DTexture9_Release(tex)); printf(", surface references then %lu, device %lu\n", refs((IUnknown *)surf), refs((IUnknown *)dev));
  IDirect3DBaseTexture9 *bound = 0; hr = IDirect3DDevice9_GetTexture(dev, 0, &bound); printf("note GetTexture after that: 0x%08lx, the same texture: %d", (unsigned long)hr, (void *)bound == (void *)tex);
  if (bound) printf(", references %lu", refs((IUnknown *)bound)); printf("\n");
  if (bound) IDirect3DBaseTexture9_Release(bound);
  printf("note surface Release returns %lu", IDirect3DSurface9_Release(surf)); printf(", device references %lu\n", refs((IUnknown *)dev));
  IDirect3DDevice9_SetTexture(dev, 0, 0); printf("note after SetTexture(0, NULL): device references %lu\n", refs((IUnknown *)dev));
  // a texture with every level, and a render target
  hr = IDirect3DDevice9_CreateTexture(dev, 256, 64, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, 0); printf("note CreateTexture 256x64 with 0 levels: %lu levels\n", IDirect3DTexture9_GetLevelCount(tex)); IDirect3DTexture9_Release(tex);
  hr = IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_MANAGED, &tex, 0); printf("note CreateTexture, a render target in the managed pool: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DDevice9_CreateTexture(dev, 64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, 0); hr = IDirect3DTexture9_LockRect(tex, 0, &lr, 0, 0);
  printf("note LockRect of a texture in the default pool that is not dynamic: 0x%08lx\n", (unsigned long)hr); if (hr == D3D_OK) IDirect3DTexture9_UnlockRect(tex, 0); IDirect3DTexture9_Release(tex);
  hr = IDirect3DDevice9_CreateTexture(dev, 256, 256, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &tex, 0);
  hr = IDirect3DTexture9_LockRect(tex, 0, &lr, 0, 0); printf("note LockRect of a render target texture: 0x%08lx\n", (unsigned long)hr); if (hr == D3D_OK) IDirect3DTexture9_UnlockRect(tex, 0);
  IDirect3DTexture9_GetSurfaceLevel(tex, 0, &surf); IDirect3DSurface9 *rt0 = 0, *ds0 = 0;
  IDirect3DDevice9_GetRenderTarget(dev, 0, &rt0); printf("note GetRenderTarget: back buffer references %lu\n", refs((IUnknown *)rt0));
  IDirect3DSurface9_GetDesc(rt0, &sd); printf("note back buffer: format %u type %u usage 0x%lx pool %u multisample %u %ux%u\n", (unsigned)sd.Format, (unsigned)sd.Type, sd.Usage, (unsigned)sd.Pool, (unsigned)sd.MultiSampleType, sd.Width, sd.Height);
  hr = IDirect3DDevice9_GetDepthStencilSurface(dev, &ds0); if (hr == D3D_OK) { IDirect3DSurface9_GetDesc(ds0, &sd); printf("note depth buffer: format %u type %u usage 0x%lx pool %u multisample %u %ux%u, references %lu\n", (unsigned)sd.Format, (unsigned)sd.Type, sd.Usage, (unsigned)sd.Pool, (unsigned)sd.MultiSampleType, sd.Width, sd.Height, refs((IUnknown *)ds0)); }
  IDirect3DDevice9_GetViewport(dev, &vp); vp.X = 10; vp.Width = 100; IDirect3DDevice9_SetViewport(dev, &vp);
  hr = IDirect3DDevice9_SetRenderTarget(dev, 0, surf); IDirect3DDevice9_GetViewport(dev, &vp);
  printf("note SetRenderTarget to a 256x256 texture: 0x%08lx, viewport then %lu %lu %lu %lu %g %g, texture references %lu\n", (unsigned long)hr, vp.X, vp.Y, vp.Width, vp.Height, vp.MinZ, vp.MaxZ, refs((IUnknown *)tex));
  hr = IDirect3DDevice9_SetRenderTarget(dev, 0, 0); printf("note SetRenderTarget(0, NULL): 0x%08lx\n", (unsigned long)hr);
  IDirect3DDevice9_SetRenderTarget(dev, 0, rt0); IDirect3DSurface9_Release(surf); IDirect3DTexture9_Release(tex); IDirect3DSurface9_Release(rt0); if (ds0) IDirect3DSurface9_Release(ds0);
  // a volume texture as the app makes it
  IDirect3DVolumeTexture9 *vt = 0; IDirect3DVolume9 *vol = 0; D3DVOLUME_DESC vd; D3DLOCKED_BOX lb;
  hr = IDirect3DDevice9_CreateVolumeTexture(dev, 256, 256, 4, 9, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vt, 0); printf("note CreateVolumeTexture 256x256x4 with 9 levels: 0x%08lx, %lu levels\n", (unsigned long)hr, vt ? IDirect3DVolumeTexture9_GetLevelCount(vt) : 0);
  for (unsigned i = 0; vt && i < 9; i++) { IDirect3DVolumeTexture9_GetVolumeLevel(vt, i, &vol); IDirect3DVolume9_GetDesc(vol, &vd); hr = IDirect3DVolume9_LockBox(vol, &lb, 0, 0);
    printf("note volume level %u: %ux%ux%u, row pitch %d, slice pitch %d, volume references %lu, texture references %lu\n", i, vd.Width, vd.Height, vd.Depth, lb.RowPitch, lb.SlicePitch, refs((IUnknown *)vol), refs((IUnknown *)vt)); IDirect3DVolume9_UnlockBox(vol); IDirect3DVolume9_Release(vol); }
  if (vt) { IDirect3DVolumeTexture9_GetVolumeLevel(vt, 0, &vol); printf("note UnlockBox of a volume that is not locked: 0x%08lx, through its texture: 0x%08lx\n", (unsigned long)IDirect3DVolume9_UnlockBox(vol), (unsigned long)IDirect3DVolumeTexture9_UnlockBox(vt, 0)); IDirect3DVolume9_Release(vol); }
  if (vt) IDirect3DVolumeTexture9_Release(vt);
  // buffers
  IDirect3DVertexBuffer9 *vb = 0; void *p = 0; D3DVERTEXBUFFER_DESC vbd;
  hr = IDirect3DDevice9_CreateVertexBuffer(dev, 1024, D3DUSAGE_WRITEONLY, 0x42, D3DPOOL_DEFAULT, &vb, 0); IDirect3DVertexBuffer9_GetDesc(vb, &vbd);
  printf("note vertex buffer: format %u type %u usage 0x%lx pool %u size %u fvf 0x%lx\n", (unsigned)vbd.Format, (unsigned)vbd.Type, vbd.Usage, (unsigned)vbd.Pool, vbd.Size, vbd.FVF);
  hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, &p, D3DLOCK_DISCARD); printf("note Lock with DISCARD on a buffer that is not dynamic: 0x%08lx", (unsigned long)hr); void *q = 0;
  hr = IDirect3DVertexBuffer9_Lock(vb, 16, 0, &q, 0); printf(", a second Lock at offset 16: 0x%08lx, %ld bytes further\n", (unsigned long)hr, (long)((char *)q - (char *)p));
  hr = IDirect3DVertexBuffer9_Unlock(vb); hr = IDirect3DVertexBuffer9_Unlock(vb); hr = IDirect3DVertexBuffer9_Unlock(vb); printf("note a third Unlock after two Locks: 0x%08lx\n", (unsigned long)hr);
  hr = IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 20); printf("note SetStreamSource: buffer references %lu", refs((IUnknown *)vb)); printf(", Release while bound returns %lu\n", IDirect3DVertexBuffer9_Release(vb));
  IDirect3DVertexBuffer9 *vb2 = 0; UINT off = 0, stride = 0; hr = IDirect3DDevice9_GetStreamSource(dev, 0, &vb2, &off, &stride); printf("note GetStreamSource after that: 0x%08lx, the same buffer: %d, stride %u\n", (unsigned long)hr, vb2 == vb, stride);
  if (vb2) IDirect3DVertexBuffer9_Release(vb2); IDirect3DDevice9_SetStreamSource(dev, 0, 0, 0, 0);
  hr = IDirect3DDevice9_GetStreamSource(dev, 0, &vb2, &off, &stride); printf("note after SetStreamSource(0, NULL, 0, 0): buffer %p, offset %u, stride %u\n", (void *)vb2, off, stride);
  { UINT f = 77; hr = IDirect3DDevice9_GetStreamSourceFreq(dev, 0, &f); printf("note stream frequency: %u at first; SetStreamSourceFreq(0, 0): 0x%08lx\n", f, (unsigned long)IDirect3DDevice9_SetStreamSourceFreq(dev, 0, 0)); }
  { // the vertex format, set as a declaration and read as an FVF, and the other way round
    static const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT3, 0, D3DDECLUSAGE_POSITION, 0}, {0, 12, D3DDECLTYPE_FLOAT2, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
    IDirect3DVertexDeclaration9 *decl = 0, *got = (void *)1; DWORD f = 77; UINT n = 0;
    IDirect3DDevice9_CreateVertexDeclaration(dev, el, &decl); IDirect3DDevice9_SetVertexDeclaration(dev, decl); IDirect3DDevice9_GetFVF(dev, &f);
    printf("note GetFVF after SetVertexDeclaration of position + texture coordinates: 0x%lx; declaration references %lu", f, refs((IUnknown *)decl));
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_TEX1); hr = IDirect3DDevice9_GetVertexDeclaration(dev, &got);
    printf("; GetVertexDeclaration after SetFVF: 0x%08lx, %s", (unsigned long)hr, !got ? "none" : got == decl ? "the same declaration" : "a declaration of Direct3D's own");
    if (got) { IDirect3DVertexDeclaration9_GetDeclaration(got, 0, &n); printf(" with %u elements", n); IDirect3DVertexDeclaration9_Release(got); }
    IDirect3DDevice9_SetVertexDeclaration(dev, 0); f = 77; IDirect3DDevice9_GetFVF(dev, &f); printf("; GetFVF after SetVertexDeclaration(NULL): 0x%lx\n", f); IDirect3DVertexDeclaration9_Release(decl); }
  // state blocks: what a recording does to the device, what Capture and Apply cover
  IDirect3DStateBlock9 *sb = 0; DWORD z0, z1, z2, c0, c1;
  IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &z0);
  hr = IDirect3DDevice9_BeginStateBlock(dev); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, 0); IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &z1);
  printf("note BeginStateBlock twice: 0x%08lx\n", (unsigned long)IDirect3DDevice9_BeginStateBlock(dev));
  hr = IDirect3DDevice9_EndStateBlock(dev, &sb); IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &z2);
  printf("note ZENABLE: %lu before the recording, %lu read while 0 is recorded, %lu after EndStateBlock; block references %lu, device %lu\n", z0, z1, z2, refs((IUnknown *)sb), refs((IUnknown *)dev));
  IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, 2); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, 1);
  IDirect3DStateBlock9_Capture(sb); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, 1); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, 2);
  IDirect3DStateBlock9_Apply(sb); IDirect3DDevice9_GetRenderState(dev, D3DRS_ZENABLE, &c0); IDirect3DDevice9_GetRenderState(dev, D3DRS_CULLMODE, &c1);
  printf("note after Capture (ZENABLE 2, CULLMODE 1), other values set (1, 2) and Apply: ZENABLE %lu, CULLMODE %lu\n", c0, c1);
  printf("note EndStateBlock without a recording: 0x%08lx\n", (unsigned long)IDirect3DDevice9_EndStateBlock(dev, &sb));
  IDirect3DStateBlock9_Release(sb);
  { // lights in a recorded block: enabling one the device does not have, setting one without enabling it
    D3DLIGHT9 a = {0}, b; BOOL e1 = 77, e2 = 77; a.Type = D3DLIGHT_POINT; a.Range = 5; a.Attenuation0 = 1; a.Diffuse.r = 0.5f;
    IDirect3DDevice9_BeginStateBlock(dev); IDirect3DDevice9_LightEnable(dev, 40, TRUE); IDirect3DDevice9_SetLight(dev, 41, &a); IDirect3DDevice9_EndStateBlock(dev, &sb);
    HRESULT h0 = IDirect3DDevice9_GetLight(dev, 40, &b), h1 = IDirect3DDevice9_GetLight(dev, 41, &b); IDirect3DStateBlock9_Apply(sb);
    HRESULT h2 = IDirect3DDevice9_GetLight(dev, 40, &b); unsigned type40 = b.Type; HRESULT h3 = IDirect3DDevice9_GetLightEnable(dev, 40, &e1), h4 = IDirect3DDevice9_GetLight(dev, 41, &b), h5 = IDirect3DDevice9_GetLightEnable(dev, 41, &e2);
    printf("note lights recorded in a block (40 enabled, 41 set): on the device before Apply GetLight 0x%08lx, 0x%08lx; after Apply light 40: 0x%08lx type %u, enabled 0x%08lx %d; light 41: 0x%08lx range %g, enabled 0x%08lx %d\n",
      (unsigned long)h0, (unsigned long)h1, (unsigned long)h2, type40, (unsigned long)h3, (int)e1, (unsigned long)h4, b.Range, (unsigned long)h5, (int)e2);
    a.Range = 9; IDirect3DDevice9_SetLight(dev, 41, &a); IDirect3DDevice9_LightEnable(dev, 40, FALSE); IDirect3DStateBlock9_Capture(sb);
    a.Range = 3; IDirect3DDevice9_SetLight(dev, 41, &a); IDirect3DDevice9_LightEnable(dev, 40, TRUE); IDirect3DStateBlock9_Apply(sb);
    IDirect3DDevice9_GetLight(dev, 41, &b); IDirect3DDevice9_GetLightEnable(dev, 40, &e1);
    printf("note after Capture (light 41 range 9, light 40 disabled), changes (range 3, enabled) and Apply: range %g, light 40 enabled %d\n", b.Range, (int)e1); IDirect3DStateBlock9_Release(sb); }
  { // textures and stream sources in a block: who holds a reference
    IDirect3DTexture9 *t2 = 0; IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t2, 0);
    IDirect3DDevice9_BeginStateBlock(dev); IDirect3DDevice9_SetTexture(dev, 1, (IDirect3DBaseTexture9 *)t2); IDirect3DDevice9_EndStateBlock(dev, &sb);
    IDirect3DBaseTexture9 *cur = (void *)1; IDirect3DDevice9_GetTexture(dev, 1, &cur);
    printf("note a texture set while recording: the device's stage holds %s, texture references %lu", cur ? "it" : "nothing", refs((IUnknown *)t2)); if (cur) IDirect3DBaseTexture9_Release(cur);
    printf("; texture Release returns %lu", IDirect3DTexture9_Release(t2)); IDirect3DStateBlock9_Apply(sb); cur = 0; IDirect3DDevice9_GetTexture(dev, 1, &cur);
    printf("; after Apply the stage holds %s\n", cur == (IDirect3DBaseTexture9 *)t2 ? "the released texture" : cur ? "another texture" : "nothing"); if (cur) IDirect3DBaseTexture9_Release(cur);
    IDirect3DDevice9_SetTexture(dev, 1, 0); IDirect3DStateBlock9_Release(sb); }
  for (unsigned t = 0; t <= 4; t++) { sb = 0; hr = IDirect3DDevice9_CreateStateBlock(dev, t, &sb); printf("note CreateStateBlock type %u: 0x%08lx\n", t, (unsigned long)hr); if (sb) IDirect3DStateBlock9_Release(sb); }
  // What a block of each type covers: make one, change everything, apply it, and see what came back.
  // stateblock <type> rs|tss|samp <the states restored>; stateblock <type> other <the other things restored>
  for (unsigned t = 1; t <= 3; t++) {
    DWORD rs0[210], ts0[33], sa0[14], sv0[14]; D3DMATERIAL9 m0, m1; D3DVIEWPORT9 v0, v1; RECT r0, r1; D3DMATRIX x0, x1, w0, w1; D3DLIGHT9 l0 = {0}, l1; BOOL e1; float p0[4] = {1, 2, 3, 4}, p1[4], c0[4] = {1, 1, 1, 1}, c1[4];
    DWORD f0, f1; UINT st; IDirect3DTexture9 *tx = 0; IDirect3DBaseTexture9 *tb = 0; IDirect3DVertexBuffer9 *b0 = 0, *b1 = 0; IDirect3DIndexBuffer9 *i0 = 0, *i1 = 0;
    IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tx, 0); IDirect3DDevice9_CreateVertexBuffer(dev, 64, 0, D3DFVF_XYZ, D3DPOOL_MANAGED, &b0, 0); IDirect3DDevice9_CreateIndexBuffer(dev, 64, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &i0, 0);
    l0.Type = D3DLIGHT_POINT; l0.Range = 7; l0.Attenuation0 = 1; IDirect3DDevice9_SetLight(dev, 50, &l0); IDirect3DDevice9_LightEnable(dev, 50, FALSE);
    IDirect3DDevice9_SetTexture(dev, 0, 0); IDirect3DDevice9_SetStreamSource(dev, 0, 0, 0, 0); IDirect3DDevice9_SetIndices(dev, 0); IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ); IDirect3DDevice9_SetNPatchMode(dev, 0.0f);
    IDirect3DDevice9_SetClipPlane(dev, 0, p0); IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c0, 1); IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
    for (unsigned i = 7; i <= 209; i++) IDirect3DDevice9_GetRenderState(dev, i, &rs0[i]);
    for (unsigned i = 1; i <= 32; i++) IDirect3DDevice9_GetTextureStageState(dev, 0, i, &ts0[i]);
    for (unsigned i = 1; i <= 13; i++) { IDirect3DDevice9_GetSamplerState(dev, 0, i, &sa0[i]); IDirect3DDevice9_GetSamplerState(dev, 257, i, &sv0[i]); }
    IDirect3DDevice9_GetMaterial(dev, &m0); IDirect3DDevice9_GetViewport(dev, &v0); IDirect3DDevice9_GetScissorRect(dev, &r0); IDirect3DDevice9_GetTransform(dev, D3DTS_VIEW, &x0); IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &w0);
    sb = 0; if (IDirect3DDevice9_CreateStateBlock(dev, t, &sb) != D3D_OK) continue;
    for (unsigned i = 7; i <= 209; i++) IDirect3DDevice9_SetRenderState(dev, i, rs0[i] ^ 1);
    for (unsigned i = 1; i <= 32; i++) IDirect3DDevice9_SetTextureStageState(dev, 0, i, ts0[i] ^ 1);
    for (unsigned i = 1; i <= 13; i++) { IDirect3DDevice9_SetSamplerState(dev, 0, i, sa0[i] ^ 1); IDirect3DDevice9_SetSamplerState(dev, 257, i, sv0[i] ^ 1); }
    m1 = m0; m1.Power += 1; IDirect3DDevice9_SetMaterial(dev, &m1); v1 = v0; v1.X += 1; v1.Width -= 1; IDirect3DDevice9_SetViewport(dev, &v1); r1 = r0; r1.left += 1; IDirect3DDevice9_SetScissorRect(dev, &r1);
    x1 = x0; x1.m[3][0] += 1; IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &x1); w1 = w0; w1.m[3][0] += 1; IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &w1);
    l1 = l0; l1.Range = 8; IDirect3DDevice9_SetLight(dev, 50, &l1); IDirect3DDevice9_LightEnable(dev, 50, TRUE); p1[0] = 5; p1[1] = p1[2] = p1[3] = 0; IDirect3DDevice9_SetClipPlane(dev, 0, p1);
    c1[0] = c1[1] = c1[2] = c1[3] = 2; IDirect3DDevice9_SetVertexShaderConstantF(dev, 0, c1, 1); IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c1, 1);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tx); IDirect3DDevice9_SetStreamSource(dev, 0, b0, 0, 12); IDirect3DDevice9_SetIndices(dev, i0); IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_NORMAL); IDirect3DDevice9_SetNPatchMode(dev, 2.0f);
    IDirect3DStateBlock9_Apply(sb);
    printf("stateblock %u rs", t); for (unsigned i = 7; i <= 209; i++) { DWORD v; IDirect3DDevice9_GetRenderState(dev, i, &v); if (v == rs0[i]) printf(" %u", i); else IDirect3DDevice9_SetRenderState(dev, i, rs0[i]); } printf("\n");
    printf("stateblock %u tss", t); for (unsigned i = 1; i <= 32; i++) { DWORD v; IDirect3DDevice9_GetTextureStageState(dev, 0, i, &v); if (v == ts0[i]) printf(" %u", i); else IDirect3DDevice9_SetTextureStageState(dev, 0, i, ts0[i]); } printf("\n");
    printf("stateblock %u samp", t); for (unsigned i = 1; i <= 13; i++) { DWORD v; IDirect3DDevice9_GetSamplerState(dev, 0, i, &v); if (v == sa0[i]) printf(" %u", i); else IDirect3DDevice9_SetSamplerState(dev, 0, i, sa0[i]); } printf("\n");
    printf("stateblock %u vertex_samp", t); for (unsigned i = 1; i <= 13; i++) { DWORD v; IDirect3DDevice9_GetSamplerState(dev, 257, i, &v); if (v == sv0[i]) printf(" %u", i); else IDirect3DDevice9_SetSamplerState(dev, 257, i, sv0[i]); } printf("\n");
    printf("stateblock %u other", t);
    IDirect3DDevice9_GetMaterial(dev, &m1); if (m1.Power == m0.Power) printf(" material"); IDirect3DDevice9_GetViewport(dev, &v1); if (v1.X == v0.X) printf(" viewport"); IDirect3DDevice9_GetScissorRect(dev, &r1); if (r1.left == r0.left) printf(" scissor");
    IDirect3DDevice9_GetTransform(dev, D3DTS_VIEW, &x1); if (x1.m[3][0] == x0.m[3][0]) printf(" view"); IDirect3DDevice9_GetTransform(dev, D3DTS_WORLD, &w1); if (w1.m[3][0] == w0.m[3][0]) printf(" world");
    IDirect3DDevice9_GetLight(dev, 50, &l1); if (l1.Range == 7) printf(" light"); IDirect3DDevice9_GetLightEnable(dev, 50, &e1); if (!e1) printf(" light_enable");
    IDirect3DDevice9_GetClipPlane(dev, 0, p1); if (p1[0] == 1) printf(" clip_plane"); IDirect3DDevice9_GetVertexShaderConstantF(dev, 0, c1, 1); if (c1[0] == 1) printf(" vs_constant"); IDirect3DDevice9_GetPixelShaderConstantF(dev, 0, c1, 1); if (c1[0] == 1) printf(" ps_constant");
    IDirect3DDevice9_GetTexture(dev, 0, &tb); if (!tb) printf(" texture"); else IDirect3DBaseTexture9_Release(tb); IDirect3DDevice9_GetStreamSource(dev, 0, &b1, &st, &st); if (!b1) printf(" stream"); else IDirect3DVertexBuffer9_Release(b1);
    IDirect3DDevice9_GetIndices(dev, &i1); if (!i1) printf(" indices"); else IDirect3DIndexBuffer9_Release(i1); IDirect3DDevice9_GetFVF(dev, &f1); if (f1 == D3DFVF_XYZ) printf(" fvf"); if (IDirect3DDevice9_GetNPatchMode(dev) == 0.0f) printf(" npatch");
    printf("\n");
    IDirect3DDevice9_SetMaterial(dev, &m0); IDirect3DDevice9_SetViewport(dev, &v0); IDirect3DDevice9_SetScissorRect(dev, &r0); IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &x0); IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &w0);
    IDirect3DDevice9_SetTexture(dev, 0, 0); IDirect3DDevice9_SetStreamSource(dev, 0, 0, 0, 0); IDirect3DDevice9_SetIndices(dev, 0); IDirect3DDevice9_SetNPatchMode(dev, 0.0f);
    IDirect3DStateBlock9_Release(sb); IDirect3DTexture9_Release(tx); IDirect3DVertexBuffer9_Release(b0); IDirect3DIndexBuffer9_Release(i0);
  }
  // scenes and presenting
  printf("note EndScene without BeginScene: 0x%08lx", (unsigned long)IDirect3DDevice9_EndScene(dev)); IDirect3DDevice9_BeginScene(dev);
  printf(", BeginScene twice: 0x%08lx", (unsigned long)IDirect3DDevice9_BeginScene(dev)); printf(", Present inside a scene: 0x%08lx\n", (unsigned long)IDirect3DDevice9_Present(dev, 0, 0, 0, 0)); IDirect3DDevice9_EndScene(dev);
  printf("note SetFVF(0): 0x%08lx, SetIndices(NULL): 0x%08lx\n", (unsigned long)IDirect3DDevice9_SetFVF(dev, 0), (unsigned long)IDirect3DDevice9_SetIndices(dev, 0));
  IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ); IDirect3DDevice9_BeginScene(dev);
  printf("note with an FVF and no stream source: DrawPrimitive 0x%08lx, DrawIndexedPrimitive without indices 0x%08lx, DrawPrimitive of 0 primitives 0x%08lx, outside a scene: ", (unsigned long)IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1),
    (unsigned long)IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1), (unsigned long)IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0));
  IDirect3DDevice9_EndScene(dev); printf("0x%08lx\n", (unsigned long)IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1));
  D3DDEVICE_CREATION_PARAMETERS cp; IDirect3DDevice9_GetCreationParameters(dev, &cp); printf("note creation parameters: adapter %u type %u flags 0x%lx\n", cp.AdapterOrdinal, (unsigned)cp.DeviceType, cp.BehaviorFlags);
  printf("note device Release returns %lu", IDirect3DDevice9_Release(dev)); printf(", then Direct3D Release returns %lu\n", IDirect3D9_Release(d3d));
  return 0;
}
