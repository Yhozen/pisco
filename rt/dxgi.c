// SPDX-License-Identifier: GPL-2.0-or-later
// DXGI as a console title reaches it: the D3D12.X device's IDXGIDevice1 (QueryInterface on the device), its
// adapter (GetDesc with the device's LUID) and no outputs (the page's canvas is the display). Media
// Foundation's device manager (MFPlat), which console titles reset with the D3D12.X device, is here too.
#include "rt.h"

#define S_OK 0
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define E_POINTER 0x80004003u
#define DXGI_ERROR_NOT_FOUND 0x887A0002u

int rt_d3d12_is_device_iid(u64 riid); // d3d12.c
static int iid(u64 riid, u32 a, u16 b, u16 c2, u64 d) {
  u8 g[16]; memcpy(g, &a, 4); memcpy(g + 4, &b, 2); memcpy(g + 6, &c2, 2);
  for (int i = 0; i < 8; i++) g[8 + i] = (u8)(d >> (56 - 8 * i));
  return riid && !memcmp((void *)GP(riid), g, 16);
}
typedef struct { u64 vtbl; u32 magic; s32 refs; u64 device; } DxgiObj;
#define DMAGIC 0x44584749u
static DxgiObj *dev_obj, *adapter_obj;
static DxgiObj *make(const char *prefix, const char *const *names, unsigned n, u64 device) {
  DxgiObj *o = calloc(1, sizeof *o); o->vtbl = rt_com_vtable("dxgi", prefix, names, n); o->magic = DMAGIC; o->refs = 1; o->device = device;
  return o;
}
#define OBJECT_SLOTS "QueryInterface", "AddRef", "Release", "SetPrivateData", "SetPrivateDataInterface", "GetPrivateData", "GetParent"
u64 rt_dxgi_device(u64 d3d_device) {
  static const char *const names[] = {OBJECT_SLOTS, "GetAdapter", "CreateSurface", "QueryResourceResidency", "SetGPUThreadPriority", "GetGPUThreadPriority", "SetMaximumFrameLatency", "GetMaximumFrameLatency", "OfferResources", "ReclaimResources", "EnqueueSetEvent", "Trim"};
  if (!dev_obj) dev_obj = make("Device_", names, sizeof names / sizeof *names, d3d_device);
  dev_obj->refs++;
  return (uptr)dev_obj;
}
static u64 adapter(void) {
  static const char *const names[] = {OBJECT_SLOTS, "EnumOutputs", "GetDesc", "CheckInterfaceSupport", "GetDesc1", "GetDesc2", 0, 0, "QueryVideoMemoryInfo", "SetVideoMemoryReservation", 0, 0, "GetDesc3"};
  if (!adapter_obj) adapter_obj = make("Adapter_", names, sizeof names / sizeof *names, 0);
  adapter_obj->refs++;
  return (uptr)adapter_obj;
}
static DxgiObj *self_of(Cpu *c) { return (DxgiObj *)GP(ARG(0)); }
IMP(dxgi, Device_QueryInterface) {
  DxgiObj *o = self_of(c); u64 riid = ARG(1), ppv = ARG(2);
  if (!ppv) { RET(E_POINTER); return; }
  if (rt_d3d12_is_device_iid(riid)) { M64(ppv) = o->device; RET(S_OK); return; } // back to the D3D12 device itself
  if (iid(riid, 0x00000000, 0, 0, 0xC000000000000046ull) || iid(riid, 0xaec22fb8, 0x76f3, 0x4639, 0x9be028eb43a67a2eull) || iid(riid, 0x54ec77fa, 0x1377, 0x44e6, 0x8c3288fd5f44c84cull) || iid(riid, 0x77db970f, 0x6276, 0x48ba, 0xba28070143b4392cull)) { o->refs++; M64(ppv) = ARG(0); RET(S_OK); return; }
  M64(ppv) = 0; RET(E_NOINTERFACE);
}
IMP(dxgi, Device_AddRef) { RET(++self_of(c)->refs); }
IMP(dxgi, Device_Release) { DxgiObj *o = self_of(c); RET(o->refs > 1 ? --o->refs : 1); } // the device's own: kept
IMP(dxgi, Device_GetParent) { if (ARG(2)) M64(ARG(2)) = adapter(); RET(S_OK); }
IMP(dxgi, Device_GetAdapter) { if (!ARG(1)) { RET(E_INVALIDARG); return; } M64(ARG(1)) = adapter(); RET(S_OK); }
IMP(dxgi, Device_SetGPUThreadPriority) { RET(S_OK); }
IMP(dxgi, Device_GetGPUThreadPriority) { if (ARG(1)) M32(ARG(1)) = 0; RET(S_OK); }
IMP(dxgi, Device_SetMaximumFrameLatency) { RET(S_OK); }
IMP(dxgi, Device_GetMaximumFrameLatency) { if (ARG(1)) M32(ARG(1)) = 2; RET(S_OK); }
IMP(dxgi, Device_SetPrivateData) { RET(S_OK); }
IMP(dxgi, Device_GetPrivateData) { RET(DXGI_ERROR_NOT_FOUND); }
IMP(dxgi, Adapter_QueryInterface) {
  u64 riid = ARG(1), ppv = ARG(2); if (!ppv) { RET(E_POINTER); return; }
  // IUnknown, IDXGIObject, IDXGIAdapter, IDXGIAdapter1
  if (iid(riid, 0x00000000, 0, 0, 0xC000000000000046ull) || iid(riid, 0xaec22fb8, 0x76f3, 0x4639, 0x9be028eb43a67a2eull) || iid(riid, 0x2411e7e1, 0x12ac, 0x4ccf, 0xbd149798e8534dc0ull) || iid(riid, 0x29038f61, 0x3839, 0x4626, 0x91fd086879011a05ull)) { self_of(c)->refs++; M64(ppv) = ARG(0); RET(S_OK); return; }
  M64(ppv) = 0; RET(E_NOINTERFACE);
}
IMP(dxgi, Adapter_AddRef) { RET(++self_of(c)->refs); }
IMP(dxgi, Adapter_Release) { DxgiObj *o = self_of(c); RET(o->refs > 1 ? --o->refs : 1); }
IMP(dxgi, Adapter_EnumOutputs) { if (ARG(2)) M64(ARG(2)) = 0; RET(DXGI_ERROR_NOT_FOUND); } // the canvas is not a DXGI output
static void adapter_desc(Cpu *c, int one) { // DXGI_ADAPTER_DESC(1): description, ids, memory sizes, LUID (the device's), flags
  u64 d = ARG(1); if (!d) { RET(E_INVALIDARG); return; }
  memset((void *)GP(d), 0, one ? 304 : 304);
  const char *name = "pisco WebGPU (D3D12.X)";
  for (int i = 0; name[i]; i++) M16(d + 2 * i) = (u8)name[i];
  M64(d + 272) = 0; M64(d + 280) = 0; M64(d + 288) = 13824ull << 20; // shared system memory: the console's
  M32(d + 296) = 0x5357; M32(d + 300) = 0;                          // Device_GetAdapterLuid's LUID
  RET(S_OK);
}
IMP(dxgi, Adapter_GetDesc) { adapter_desc(c, 0); }
IMP(dxgi, Adapter_GetDesc1) { adapter_desc(c, 1); M32(ARG(1) + 304) = 0; } // flags: a hardware adapter
IMP(dxgi, Adapter_GetDesc2) { adapter_desc(c, 1); M32(ARG(1) + 304) = 0; M32(ARG(1) + 308) = 0; M32(ARG(1) + 312) = 0; }
IMP(dxgi, Adapter_GetDesc3) { adapter_desc(c, 1); M32(ARG(1) + 304) = 0; M32(ARG(1) + 308) = 0; M32(ARG(1) + 312) = 0; }
IMP(dxgi, Adapter_QueryVideoMemoryInfo) { u64 o = ARG(3); if (!o) { RET(E_INVALIDARG); return; } M64(o) = 4ull << 30; M64(o + 8) = 0; M64(o + 16) = 4ull << 30; M64(o + 24) = 0; RET(S_OK); } // budget, usage, available, reserved
IMP(dxgi, Adapter_SetVideoMemoryReservation) { RET(S_OK); }

// ---- the factory (CreateDXGIFactory / 1 / 2): one adapter (the page's WebGPU device), swap chains for windows ----
u64 rt_dxgi_swapchain(u64 queue, u64 desc1, u32 win_w, u32 win_h); // d3d12.c
static u64 factory;
static void make_factory(Cpu *c, u64 ppv) {
  static const char *const names[] = {OBJECT_SLOTS, "EnumAdapters", "MakeWindowAssociation", "GetWindowAssociation", "CreateSwapChain", "CreateSoftwareAdapter",
    "EnumAdapters1", "IsCurrent", "IsWindowedStereoEnabled", "CreateSwapChainForHwnd", "CreateSwapChainForCoreWindow", "GetSharedResourceAdapterLuid", "RegisterStereoStatusWindow",
    "RegisterStereoStatusEvent", "UnregisterStereoStatus", "RegisterOcclusionStatusWindow", "RegisterOcclusionStatusEvent", "UnregisterOcclusionStatus", "CreateSwapChainForComposition",
    "GetCreationFlags", "EnumAdapterByLuid", "EnumWarpAdapter", "CheckFeatureSupport", "EnumAdapterByGpuPreference", "RegisterAdaptersChangedEvent", "UnregisterAdaptersChangedNotification"};
  if (!factory) factory = (uptr)make("Factory_", names, sizeof names / sizeof *names, 0);
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
static void one_adapter(Cpu *c, u32 i, u64 out) { if (!out) { RET(E_POINTER); return; } if (i) { M64(out) = 0; RET(DXGI_ERROR_NOT_FOUND); return; } M64(out) = adapter(); RET(S_OK); }
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
IMP(dxgi, Adapter_CheckInterfaceSupport) { RET(0x887A0004u); }

// ---- MFPlat: the DXGI device manager (IMFDXGIDeviceManager) and its console reset ----
typedef struct { u64 vtbl; u32 magic; s32 refs; u32 token; u64 device; } MfManager;
#define MFMAGIC 0x4D464D47u
IMP(mfplat, MFStartup) { RET(S_OK); }
IMP(mfplat, MFShutdown) { RET(S_OK); }
IMP(mfplat, MFCreateDXGIDeviceManager) { // (&reset token, &manager)
  static const char *const names[] = {"QueryInterface", "AddRef", "Release", "CloseDeviceHandle", "GetVideoService", "LockDevice", "OpenDeviceHandle", "ResetDevice", "TestDevice", "UnlockDevice"};
  static u64 vt; static u32 next = 0x5357;
  if (!ARG(0) || !ARG(1)) { RET(E_POINTER); return; }
  if (!vt) vt = rt_com_vtable("mfplat", "Manager_", names, 10);
  MfManager *m = calloc(1, sizeof *m); m->vtbl = vt; m->magic = MFMAGIC; m->refs = 1; m->token = next++;
  M32(ARG(0)) = m->token; M64(ARG(1)) = (uptr)m; RET(S_OK);
}
static MfManager *mgr(u64 p) { MfManager *m = (MfManager *)GP(p); return p && m->magic == MFMAGIC ? m : 0; }
static void reset(Cpu *c) { MfManager *m = mgr(ARG(0)); if (!m || !ARG(1) || (u32)ARG(2) != m->token) { RET(E_INVALIDARG); return; } m->device = ARG(1); RET(S_OK); }
IMP(mfplat, MFResetDXGIDeviceManagerX) { reset(c); } // (manager, device, token)
IMP(mfplat, Manager_ResetDevice) { reset(c); }
IMP(mfplat, Manager_QueryInterface) { if (ARG(2)) { M64(ARG(2)) = ARG(0); mgr(ARG(0))->refs++; } RET(S_OK); }
IMP(mfplat, Manager_AddRef) { RET(++mgr(ARG(0))->refs); }
IMP(mfplat, Manager_Release) { MfManager *m = mgr(ARG(0)); if (!m) { RET(0); return; } s32 r = --m->refs; if (!r) { m->magic = 0; free(m); } RET(r < 0 ? 0 : r); }
// a device handle names the manager's device; 0x887A0001 (MF_E_DXGI_DEVICE_NOT_INITIALIZED) without one
IMP(mfplat, Manager_OpenDeviceHandle) { MfManager *m = mgr(ARG(0)); if (!m || !ARG(1)) { RET(E_POINTER); return; } if (!m->device) { RET(0xC00D36D8u); return; } M64(ARG(1)) = 0x4D460001ull; RET(S_OK); }
IMP(mfplat, Manager_CloseDeviceHandle) { RET(ARG(1) == 0x4D460001ull ? S_OK : E_INVALIDARG); }
IMP(mfplat, Manager_TestDevice) { MfManager *m = mgr(ARG(0)); RET(m && m->device && ARG(1) == 0x4D460001ull ? S_OK : E_INVALIDARG); }
IMP(mfplat, Manager_LockDevice) { MfManager *m = mgr(ARG(0)); if (!m || !m->device) { RET(E_INVALIDARG); return; } if (ARG(3)) M64(ARG(3)) = m->device; RET(S_OK); }
IMP(mfplat, Manager_UnlockDevice) { RET(S_OK); }
IMP(mfplat, Manager_GetVideoService) { if (ARG(3)) M64(ARG(3)) = 0; RET(E_NOINTERFACE); }
