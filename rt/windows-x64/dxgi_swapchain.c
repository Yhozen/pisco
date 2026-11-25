// SPDX-License-Identifier: GPL-2.0-or-later
// DXGI for Windows desktop titles: the factory, and swap chains whose back buffers are textures of the D3D12 front
// end (rt/gdk/d3d12.c).
#include "../gdk/d3d12.h"
#include "../gdk/dxgi.h"
#define E_POINTER 0x80004003u
#define DXGI_ERROR_NOT_FOUND 0x887A0002u

// ---- the factory (CreateDXGIFactory / 1 / 2): one adapter (the page's WebGPU device), swap chains for windows ----
u64 rt_dxgi_swapchain(u64 queue, u64 desc1, u32 win_w, u32 win_h); // below
static u64 factory;
static void make_factory(Cpu *c, u64 ppv) {
  static const char *const names[] = {OBJECT_SLOTS, "EnumAdapters", "MakeWindowAssociation", "GetWindowAssociation", "CreateSwapChain", "CreateSoftwareAdapter",
    "EnumAdapters1", "IsCurrent", "IsWindowedStereoEnabled", "CreateSwapChainForHwnd", "CreateSwapChainForCoreWindow", "GetSharedResourceAdapterLuid", "RegisterStereoStatusWindow",
    "RegisterStereoStatusEvent", "UnregisterStereoStatus", "RegisterOcclusionStatusWindow", "RegisterOcclusionStatusEvent", "UnregisterOcclusionStatus", "CreateSwapChainForComposition",
    "GetCreationFlags", "EnumAdapterByLuid", "EnumWarpAdapter", "CheckFeatureSupport", "EnumAdapterByGpuPreference", "RegisterAdaptersChangedEvent", "UnregisterAdaptersChangedNotification"};
  if (!factory) factory = (uptr)rt_dxgi_object("Factory_", names, sizeof names / sizeof *names, 0);
  if (!ppv) { RET(E_POINTER); return; }
  M64(ppv) = factory; RET(S_OK);
}
IMP(dxgi, CreateDXGIFactory) { make_factory(c, ARG(1)); }  // (riid, ppv)
IMP(dxgi, CreateDXGIFactory1) { make_factory(c, ARG(1)); }
IMP(dxgi, CreateDXGIFactory2) { make_factory(c, ARG(2)); } // (flags, riid, ppv)
IMP(dxgi, DXGIGetDebugInterface1) { if (ARG(2)) M64(ARG(2)) = 0; RET(E_NOINTERFACE); }
IMP(dxgi, Factory_QueryInterface) { if (!ARG(2)) { RET(E_POINTER); return; } M64(ARG(2)) = ARG(0); RET(S_OK); } // IDXGIFactory .. IDXGIFactory7
IMP(dxgi, Factory_AddRef) { RET(2); }
IMP(dxgi, Factory_Release) { RET(1); }
static void one_adapter(Cpu *c, u32 i, u64 out) { if (!out) { RET(E_POINTER); return; } if (i) { M64(out) = 0; RET(DXGI_ERROR_NOT_FOUND); return; } M64(out) = rt_dxgi_adapter(); RET(S_OK); }
IMP(dxgi, Factory_EnumAdapters) { one_adapter(c, (u32)ARG(1), ARG(2)); }
IMP(dxgi, Factory_EnumAdapters1) { one_adapter(c, (u32)ARG(1), ARG(2)); }
IMP(dxgi, Factory_EnumAdapterByGpuPreference) { one_adapter(c, (u32)ARG(1), ARG(4)); } // (index, preference, riid, ppv)
IMP(dxgi, Factory_EnumAdapterByLuid) { one_adapter(c, 0, ARG(3)); }                    // (luid, riid, ppv)
IMP(dxgi, Factory_EnumWarpAdapter) { if (ARG(2)) M64(ARG(2)) = 0; RET(DXGI_ERROR_NOT_FOUND); }
IMP(dxgi, Factory_MakeWindowAssociation) { RET(S_OK); }
IMP(dxgi, Factory_IsCurrent) { RET(1); }
IMP(dxgi, Factory_GetCreationFlags) { RET(0); }
IMP(dxgi, Factory_CheckFeatureSupport) { if (ARG(2)) memset((void *)GP(ARG(2)), 0, (size_t)(u32)ARG(3)); RET(S_OK); } // no tearing
IMP(dxgi, Factory_CreateSwapChainForHwnd) { // (device: the queue, window, desc1, fullscreen desc, output, &swap chain)
  if (!ARG(3) || !ARG(6)) { RET(E_INVALIDARG); return; }
  M64(ARG(6)) = rt_dxgi_swapchain(ARG(1), ARG(3), 1920, 1080); RET(S_OK);
}

// ---- DXGI swap chains (PC titles): back buffers are textures of the queue's device; Present presents the
// current one through the same path as PresentX, then the next buffer is current ----
typedef struct { u64 vtbl; u32 magic; s32 refs; u32 count, index, w, h, format, flags, console; u64 frames; u64 buffers[16]; } SwapChain;
#define SMAGIC 0x53574150u
static SwapChain *swapchain(u64 p) { SwapChain *s = (SwapChain *)GP(p); return p && s->magic == SMAGIC ? s : 0; }
static void make_buffers(SwapChain *s) {
  for (u32 i = 0; i < s->count && i < 16; i++) {
    u8 desc[56] = {0}; u32 dim = 3, fmt = s->format, flags = 1; u64 w = s->w; u16 one = 1;
    memcpy(desc, &dim, 4); memcpy(desc + 16, &w, 8); memcpy(desc + 24, &s->h, 4); memcpy(desc + 28, &one, 2); memcpy(desc + 30, &one, 2);
    memcpy(desc + 32, &fmt, 4); u32 q = 1; memcpy(desc + 36, &q, 4); memcpy(desc + 48, &flags, 4);
    u64 out = 0; d3d12_create_resource(&d3d12_device(s->console)->o, 1, 0, (uptr)desc, 0, 0, (uptr)&out);
    s->buffers[i] = out;
  }
  s->index = 0;
}
// (queue, window, DXGI_SWAP_CHAIN_DESC1*, window size): dxgi.c's CreateSwapChainForHwnd
u64 rt_dxgi_swapchain(u64 queue, u64 d, u32 win_w, u32 win_h) {
  static const char *const names[] = {"QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent", "GetDevice",
    "Present", "GetBuffer", "SetFullscreenState", "GetFullscreenState", "GetDesc", "ResizeBuffers", "ResizeTarget", "GetContainingOutput", "GetFrameStatistics", "GetLastPresentCount",
    "GetDesc1", "GetFullscreenDesc", "GetHwnd", "GetCoreWindow", "Present1", "IsTemporaryMonoSupported", "GetRestrictToOutput", "SetBackgroundColor", "GetBackgroundColor", "SetRotation", "GetRotation",
    "SetSourceSize", "GetSourceSize", "SetMaximumFrameLatency", "GetMaximumFrameLatency", "GetFrameLatencyWaitableObject", "SetMatrixTransform", "GetMatrixTransform",
    "GetCurrentBackBufferIndex", "CheckColorSpaceSupport", "SetColorSpace1", "ResizeBuffers1", "SetHDRMetaData"};
  static u64 vt; if (!vt) vt = rt_com_vtable("dxgi", "SwapChain_", names, sizeof names / sizeof *names);
  Obj *q = d3d12_obj(queue);
  SwapChain *s = calloc(1, sizeof *s); s->vtbl = vt; s->magic = SMAGIC; s->refs = 1; s->console = q && q->console;
  s->w = M32(d) ? M32(d) : win_w; s->h = M32(d + 4) ? M32(d + 4) : win_h; s->format = M32(d + 8); s->count = M32(d + 28); s->flags = M32(d + 44);
  if (!s->count) s->count = 2; if (s->count > 16) s->count = 16;
  make_buffers(s);
  rt_log("dxgi: swap chain %ux%u format %u, %u buffers", s->w, s->h, s->format, s->count);
  return (uptr)s;
}
#define SC SwapChain *self = swapchain(ARG(0)); if (!self) { RET(E_INVALIDARG); return; }
IMP(dxgi, SwapChain_QueryInterface) { SC; self->refs++; M64(ARG(2)) = ARG(0); RET(S_OK); } // IDXGISwapChain1..4, IDXGIObject, IUnknown
IMP(dxgi, SwapChain_AddRef) { SC; RET(++self->refs); }
IMP(dxgi, SwapChain_Release) { SC; RET(self->refs > 1 ? --self->refs : 0); }
IMP(dxgi, SwapChain_GetBuffer) { SC; u32 i = (u32)ARG(1); if (i >= self->count) { RET(E_INVALIDARG); return; } Obj *b = d3d12_obj(self->buffers[i]); b->refs++; RET(d3d12_give(b, ARG(2), ARG(3))); }
// (sync interval, flags): with an interval, the present waits for the next 60 Hz vertical blank (SW_PACE=off: none)
IMP(dxgi, SwapChain_Present) {
  SC; u32 interval = (u32)ARG(1);
  rt_gpu_present(d3d12_id_of(self->buffers[self->index]), ++self->frames, interval); self->index = (self->index + 1) % self->count;
  if (interval && !d3d12_pace_off()) {
    static u64 next; u64 now = (u64)host_now_us(), period = 16667ull * (interval > 4 ? 4 : interval);
    if (!next || now > next + period) next = now;
    next += period;
    if (next > now) { struct timespec ts = {(time_t)((next - now) / 1000000), (long)((next - now) % 1000000) * 1000}; nanosleep(&ts, 0); }
  }
  RET(S_OK);
}
IMP(dxgi, SwapChain_Present1) { imp_dxgi_SwapChain_Present(c); }
IMP(dxgi, SwapChain_GetCurrentBackBufferIndex) { SC; RET(self->index); }
IMP(dxgi, SwapChain_ResizeBuffers) { // (count, width, height, format, flags): 0 keeps the current value
  SC; if (ARG(1)) self->count = (u32)ARG(1) > 16 ? 16 : (u32)ARG(1); if (ARG(2)) self->w = (u32)ARG(2); if (ARG(3)) self->h = (u32)ARG(3); if ((u32)ARG(4)) self->format = (u32)ARG(4);
  for (u32 i = 0; i < 16; i++) if (self->buffers[i]) { Obj *b = d3d12_obj(self->buffers[i]); if (b && --b->refs <= 0) { u32 id = b->id; rec_words(GPU_R_DESTROY, &id, 1); } self->buffers[i] = 0; }
  make_buffers(self); RET(S_OK);
}
IMP(dxgi, SwapChain_ResizeBuffers1) { imp_dxgi_SwapChain_ResizeBuffers(c); }
static void sc_desc1(SwapChain *s, u64 d) { memset((void *)GP(d), 0, 48); M32(d) = s->w; M32(d + 4) = s->h; M32(d + 8) = s->format; M32(d + 16) = 1; M32(d + 24) = 0x20; M32(d + 28) = s->count; M32(d + 36) = 3; M32(d + 44) = s->flags; }
IMP(dxgi, SwapChain_GetDesc1) { SC; sc_desc1(self, ARG(1)); RET(S_OK); }
IMP(dxgi, SwapChain_GetDesc) { // DXGI_SWAP_CHAIN_DESC: mode (w, h, refresh, format, ...), samples, usage, count, window, windowed, effect, flags
  SC; u64 d = ARG(1); memset((void *)GP(d), 0, 72); M32(d) = self->w; M32(d + 4) = self->h; M32(d + 8) = 60; M32(d + 12) = 1; M32(d + 16) = self->format;
  M32(d + 28) = 1; M32(d + 36) = 0x20; M32(d + 40) = self->count; M32(d + 56) = 1; M32(d + 60) = 3; M32(d + 64) = self->flags; RET(S_OK);
}
IMP(dxgi, SwapChain_SetFullscreenState) { RET(S_OK); }
IMP(dxgi, SwapChain_GetFullscreenState) { if (ARG(1)) M32(ARG(1)) = 0; if (ARG(2)) M64(ARG(2)) = 0; RET(S_OK); }
IMP(dxgi, SwapChain_GetContainingOutput) { if (ARG(1)) M64(ARG(1)) = 0; RET(0x887A0002u); } // DXGI_ERROR_NOT_FOUND: the canvas is no output
IMP(dxgi, SwapChain_GetLastPresentCount) { SC; M32(ARG(1)) = (u32)self->frames; RET(S_OK); }
IMP(dxgi, SwapChain_CheckColorSpaceSupport) { if (ARG(2)) M32(ARG(2)) = (u32)ARG(1) == 0 ? 1 : 0; RET(S_OK); } // sRGB (G22, P709) only
IMP(dxgi, SwapChain_SetColorSpace1) { RET((u32)ARG(1) == 0 ? S_OK : E_INVALIDARG); }
IMP(dxgi, SwapChain_SetHDRMetaData) { RET(S_OK); }
IMP(dxgi, SwapChain_SetMaximumFrameLatency) { RET(S_OK); }
IMP(dxgi, SwapChain_GetMaximumFrameLatency) { if (ARG(1)) M32(ARG(1)) = 2; RET(S_OK); }
IMP(dxgi, SwapChain_SetPrivateData) { RET(S_OK); }
IMP(dxgi, SwapChain_SetBackgroundColor) { RET(S_OK); }
IMP(dxgi, SwapChain_SetRotation) { RET(S_OK); }
IMP(dxgi, SwapChain_GetDevice) { SC; Device *d = d3d12_device(self->console); d->o.refs++; RET(d3d12_give(&d->o, ARG(1), ARG(2))); }
