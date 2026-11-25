// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12 and D3D12.X, the front end: COM objects whose methods answer on the calling thread, command lists
// recorded into words, and one ordered ring of records (rt/d3d12_cmd.h) that a GPU back end consumes
// (web/gpu/backend.js in the browser, web/gpu/null.js in Node). Nothing here waits for the GPU except
// fences, and they wait on words the back end writes.
//
// Two flavours of the same objects: a device from D3D12CreateDevice has the PC vtables (d3d12.h order,
// rt/gdk/d3d12_layout.gen.h); one from D3D12XboxCreateDevice has the console's, whose differences follow
// XCL's findings on retail titles (github.com/ivall/xcl, GPL-2.0): the command list's methods from
// SetDescriptorHeaps on sit 4 slots later, the device's extensions start at #44 (frame clock #78-#80),
// the queue's PresentX is #30, and descriptor heaps return their start handles in RAX.
#include <time.h>
#include "rt.h"
#include "d3d12_cmd.h"
#include "d3d12_layout.gen.h"

#include "d3d12.h"
static const char *const cls_name[C_COUNT] = {"Device", "CommandQueue", "CommandAllocator", "CommandList", "Fence", "Resource", "DescriptorHeap", "RootSignature", "PipelineState", "Heap", "QueryHeap", "CommandSignature"};
static Device *the_device[2]; // [console]
static u32 next_id = 1;
static pthread_mutex_t d3d_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- fences: the back end writes `completed`, then bumps the epoch word and wakes its waiters ----
static u64 fence_value(Fence *f) { return __atomic_load_n(&f->completed, __ATOMIC_ACQUIRE); }
static void epoch_bump(void) { gpu_open(); __atomic_fetch_add(&ring[GPU_H_EPOCH], 1, __ATOMIC_ACQ_REL); __builtin_wasm_memory_atomic_notify((int *)&ring[GPU_H_EPOCH], 0x7FFFFFFF); }
static void fence_wait(Fence *f, u64 v) {
  gpu_open();
  while (fence_value(f) < v) {
    u32 e = __atomic_load_n(&ring[GPU_H_EPOCH], __ATOMIC_ACQUIRE);
    if (fence_value(f) >= v) break;
    __builtin_wasm_memory_atomic_wait32((int *)&ring[GPU_H_EPOCH], (int)e, 100000000);
  }
}
// SetEventOnCompletion with an event: one helper thread sets the events whose values were reached
typedef struct Waiter { Fence *f; u64 value, event; struct Waiter *next; } Waiter;
static Waiter *waiters; static int waiter_thread;
static pthread_mutex_t waiter_lock = PTHREAD_MUTEX_INITIALIZER;
static void set_event(u64 h) {
  Object *o = rt_handle_get(h); if (!o) return;
  pthread_mutex_lock(&rt_wait_lock); o->signaled = 1; rt_signal(); pthread_mutex_unlock(&rt_wait_lock);
}
static void *waiter_main(void *arg) {
  (void)arg;
  for (;;) {
    u32 e = __atomic_load_n(&ring[GPU_H_EPOCH], __ATOMIC_ACQUIRE);
    pthread_mutex_lock(&waiter_lock);
    for (Waiter **pw = &waiters; *pw;) {
      Waiter *w = *pw;
      if (fence_value(w->f) >= w->value) { *pw = w->next; set_event(w->event); free(w); } else pw = &w->next;
    }
    pthread_mutex_unlock(&waiter_lock);
    __builtin_wasm_memory_atomic_wait32((int *)&ring[GPU_H_EPOCH], (int)e, 100000000);
  }
  return 0;
}

// ---- vtables ----
static int parse_guid(const char *s, u8 out[16]) {
  unsigned a, b, c2, d[8];
  if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &b, &c2, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]) != 11) return 0;
  memcpy(out, &a, 4); u16 bb = (u16)b, cc = (u16)c2; memcpy(out + 4, &bb, 2); memcpy(out + 6, &cc, 2);
  for (int i = 0; i < 8; i++) out[8 + i] = (u8)d[i];
  return 1;
}
static const D3dLayout *layout_of(int cls) { for (unsigned i = 0; i < sizeof d3d_layouts / sizeof d3d_layouts[0]; i++) if (!strcmp(d3d_layouts[i].cls, cls_name[cls])) return &d3d_layouts[i]; return 0; }
// the function for method `m` of class `cls`: "<Class>_<m>", then "Object_<m>", else a named stub
static u64 method(int cls, const char *iface, const char *m) {
  char n[160]; snprintf(n, sizeof n, "%s_%s", cls_name[cls], m);
  u64 a = rt_dynamic_import("d3d12", n);
  if (!a) { snprintf(n, sizeof n, "Object_%s", m); a = rt_dynamic_import("d3d12", n); }
  if (!a) { snprintf(n, sizeof n, "d3d12.dll!%s::%s", iface, m); a = rt_stub_import(n); }
  return a;
}
// console slots that are not the PC's: (slot, PC method name, or our own console method)
typedef struct { int slot; const char *name; } Slot;
// (every other console slot is a named stub that logs and returns 0, as XCL answers extensions it does not model)
static const Slot device_x[] = {{44, "CreatePlacedResourceX"}, {48, "CreateCommandQueueX"}, {52, "SetDebugFlagsX"}, {55, "CreateDerivedPipelineStateX"},
  {56, "SerializePipelineStateX"}, {60, "CreateGraphicsPipelineStateX"}, {73, "CreateCommandListX"}, {74, "RegisterPagePoolX"}, {75, "UnregisterPagePoolX"},
  {76, "CreateCommittedResource"}, {78, "SetFrameIntervalX"}, {79, "ScheduleFrameEventX"}, {80, "WaitFrameEventX"}, {81, "GetFrameStatisticsX"}, {0, 0}};
static const Slot queue_x[] = {{23, "SignalX"}, {30, "PresentX"}, {0, 0}};
static const Slot list_x[] = {{60, "CopyMemoryToMemoryX"}, {61, "FillMemoryWith32BitValueX"}, {69, "Write32BitValueBottomOfPipeX"}, {70, "Write64BitValueBottomOfPipeX"},
  {71, "Wait32BitValueX"}, {72, "Wait64BitValueX"}, {81, "DrawIndexedX"}, {89, "Write64BitValueTopOfPipeX"}, {93, "KickoffX"}, {94, "RSSetDepthBiasX"},
  {146, "UavBarrierX"}, {148, "AllocateUploadMemoryX"}, {0, 0}};
// flavour: 0 PC, 1 console, 2 the console's DMA (copy) list, whose vtable has the copies right after Reset
static u64 vtables[C_COUNT][3];
static u64 vtable(int cls, int flavour) {
  if (vtables[cls][flavour]) return vtables[cls][flavour];
  int console = flavour != 0;
  const D3dLayout *l = layout_of(cls);
  unsigned n = l->count, total = n;
  if (console) total = cls == C_DEVICE ? 128 : cls == C_LIST ? (flavour == 2 ? 16 : 160) : cls == C_QUEUE ? 40 : n;
  u64 *vt = calloc(total, 8);
  char ext[96];
  for (unsigned i = 0; i < total; i++) {
    int pc = (int)i;
    if (flavour == 2) pc = i < 11 ? (int)i : i <= 14 ? (int)i + 4 : -1; // Close #9, Reset #10, CopyBufferRegion #11 ... CopyTiles #14
    else if (console && cls == C_LIST) { // XCL's console_remap: console slot -> PC slot
      if (i >= 28 && i <= 31) pc = (int)i + 24;
      else if (i >= 32 && i <= 55) pc = (int)i - 4;
      else if (i >= 56 && i <= 59) pc = (int)i;
      else if (i >= 104 && i <= 109) pc = (int)i - 44;
      else if (i == 124) pc = 77; else if (i == 125) pc = 78;
      else if (i >= 60) pc = -1;
    }
    if (console && cls == C_DEVICE && i >= 44) pc = -1;
    if (console && cls == C_QUEUE && i >= 19) pc = -1;
    const char *name = pc >= 0 && (unsigned)pc < n ? l->methods[pc] : 0;
    const Slot *ov = flavour != 1 ? 0 : cls == C_DEVICE ? device_x : cls == C_QUEUE ? queue_x : cls == C_LIST ? list_x : 0;
    for (; ov && ov->name; ov++) if (ov->slot == (int)i) name = ov->name;
    if (name) vt[i] = method(cls, l->iface, name);
    else { snprintf(ext, sizeof ext, "d3d12_xs.dll!%sX::#%u", l->iface, i); vt[i] = rt_stub_import(ext); }
  }
  return vtables[cls][flavour] = (uptr)vt;
}
static void *obj_new(int cls, size_t size, int flavour) {
  Obj *o = aligned_alloc(16, (size + 15) & ~15ul); memset(o, 0, size);
  o->vtbl = vtable(cls, flavour); o->magic = OMAGIC; o->cls = (u32)cls; o->refs = 1; o->console = flavour != 0;
  pthread_mutex_lock(&d3d_lock); o->id = next_id++; pthread_mutex_unlock(&d3d_lock);
  return o;
}
Obj *d3d12_obj(u64 p) { Obj *o = (Obj *)GP(p); return p && o->magic == OMAGIC ? o : 0; }
u32 d3d12_id_of(u64 p) { Obj *o = d3d12_obj(p); return o ? o->id : 0; }
#define THIS(T) T *self = (T *)GP(ARG(0))
static int iid_is(u64 riid, const char *s) { u8 g[16]; return riid && parse_guid(s, g) && !memcmp((void *)GP(riid), g, 16); }
static int has_iid(int cls, u64 riid) {
  const D3dLayout *l = layout_of(cls);
  for (unsigned i = 0; i < l->iid_count; i++) if (iid_is(riid, l->iids[i])) return 1;
  return 0;
}
// hands out `o` through (riid, ppv) as QueryInterface does
u32 d3d12_give(Obj *o, u64 riid, u64 ppv) {
  if (!ppv) return S_FALSE;
  if (riid && !has_iid((int)o->cls, riid)) { M64(ppv) = 0; return E_NOINTERFACE; }
  M64(ppv) = (uptr)o; return S_OK;
}

// ---- IUnknown, ID3D12Object, ID3D12DeviceChild ----
u64 rt_dxgi_device(u64 d3d_device); // dxgi.c: the device's IDXGIDevice
int rt_d3d12_is_device_iid(u64 riid) { return has_iid(C_DEVICE, riid); }
IMP(d3d12, Object_QueryInterface) {
  Obj *o = d3d12_obj(ARG(0));
  if (o && has_iid((int)o->cls, ARG(1))) { o->refs++; M64(ARG(2)) = ARG(0); RET(S_OK); return; }
  // IDXGIDevice, IDXGIDevice1..4: the display side of the device (dxgi.c)
  if (o && o->cls == C_DEVICE && (iid_is(ARG(1), "54ec77fa-1377-44e6-8c32-88fd5f44c84c") || iid_is(ARG(1), "77db970f-6276-48ba-ba28-070143b4392c") || iid_is(ARG(1), "05008617-fbfd-4051-a790-144884b4f6a9") || iid_is(ARG(1), "6007896c-3244-4afd-bf18-a6d3beda5023") || iid_is(ARG(1), "95b4f95f-d8da-4ca4-9ee6-3b76d5968a10"))) {
    M64(ARG(2)) = rt_dxgi_device(ARG(0)); RET(S_OK); return; }
  if (rt_trace) { u8 *g = (u8 *)GP(ARG(1)); rt_log("d3d12: %s QueryInterface %02x%02x%02x%02x-...: no", o ? cls_name[o->cls] : "?", g[3], g[2], g[1], g[0]); }
  if (ARG(2)) M64(ARG(2)) = 0;
  RET(E_NOINTERFACE);
}
IMP(d3d12, Object_AddRef) { Obj *o = d3d12_obj(ARG(0)); RET(o ? ++o->refs : 1); }
IMP(d3d12, Object_Release) {
  Obj *o = d3d12_obj(ARG(0)); if (!o) { RET(0); return; }
  s32 r = --o->refs;
  if (r == 0 && o->cls == C_RESOURCE) { u32 w = o->id; rec_words(GPU_R_DESTROY, &w, 1); } // its guest backing is kept: the GPU may still read it
  RET(r < 0 ? 0 : r);
}
IMP(d3d12, Object_GetPrivateData) { RET(0x887A0002u); } // DXGI_ERROR_NOT_FOUND
IMP(d3d12, Object_SetPrivateData) { RET(S_OK); }
IMP(d3d12, Object_SetPrivateDataInterface) { RET(S_OK); }
IMP(d3d12, Object_SetName) { RET(S_OK); }
IMP(d3d12, Object_GetDevice) { Obj *o = d3d12_obj(ARG(0)); Device *d = the_device[o && o->console]; RET(d ? d3d12_give(&d->o, ARG(1), ARG(2)) : E_INVALIDARG); if (d) d->o.refs++; }

// ---- device creation ----
// d3d12.lib imports these two by ordinal
ALIAS(d3d12, "#101", D3D12CreateDevice, -1);
ALIAS(d3d12, "#102", D3D12GetDebugInterface, -1);
Device *d3d12_device(int console) {
  if (!the_device[console]) {
    the_device[console] = obj_new(C_DEVICE, sizeof(Device), console);
    rt_log("d3d12: %s device created", console ? "console (D3D12.X)" : "PC");
  }
  return the_device[console];
}
IMP(d3d12, D3D12CreateDevice) { // (adapter, minimum feature level, riid, ppv): ppv null asks whether it would work
  if (!ARG(3)) { RET(S_FALSE); return; }
  Device *d = d3d12_device(0); d->o.refs++; RET(d3d12_give(&d->o, ARG(2), ARG(3)));
}
IMP(d3d12_xs, D3D12XboxCreateDevice) { // (adapter, D3D12XBOX_CREATE_DEVICE_PARAMETERS*, riid, ppv)
  if (ARG(1)) rt_log("d3d12: D3D12XboxCreateDevice (SDK version 0x%x)", M32(ARG(1)));
  Device *d = d3d12_device(1); d->o.refs++; RET(d3d12_give(&d->o, ARG(2), ARG(3)));
}
IMP(d3d12, D3D12GetDebugInterface) { if (ARG(1)) M64(ARG(1)) = 0; RET(E_NOINTERFACE); }
IMP(d3d12_xs, D3D12GetDebugInterface) { if (ARG(1)) M64(ARG(1)) = 0; RET(E_NOINTERFACE); }
// d3d12_xs.dll's D3D12CreateDevice: the console device, as XCL's d3d12_xs hands it out
IMP(d3d12_xs, D3D12CreateDevice) { if (!ARG(3)) { RET(S_FALSE); return; } Device *d = d3d12_device(1); d->o.refs++; RET(d3d12_give(&d->o, ARG(2), ARG(3))); }

// ---- ID3DBlob ----
typedef struct { u64 vtbl; u32 magic; s32 refs; u64 data, size; } Blob;
#define BMAGIC 0x424C4F42u
static u64 blob_vt;
u64 rt_blob_new(const void *data, u64 size) {
  static const char *const names[] = {"QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize"};
  if (!blob_vt) blob_vt = rt_com_vtable("d3d12", "Blob_", names, 5);
  Blob *b = calloc(1, sizeof *b); b->vtbl = blob_vt; b->magic = BMAGIC; b->refs = 1;
  b->data = (uptr)malloc(size ? size : 1); memcpy((void *)GP(b->data), data, size); b->size = size;
  return (uptr)b;
}
IMP(d3d12, Blob_QueryInterface) { Blob *b = (Blob *)GP(ARG(0)); b->refs++; M64(ARG(2)) = ARG(0); RET(S_OK); }
IMP(d3d12, Blob_AddRef) { Blob *b = (Blob *)GP(ARG(0)); RET(++b->refs); }
IMP(d3d12, Blob_Release) { Blob *b = (Blob *)GP(ARG(0)); s32 r = --b->refs; if (!r) { free((void *)GP(b->data)); b->magic = 0; free(b); } RET(r < 0 ? 0 : r); }
IMP(d3d12, Blob_GetBufferPointer) { RET(((Blob *)GP(ARG(0)))->data); }
IMP(d3d12, Blob_GetBufferSize) { RET(((Blob *)GP(ARG(0)))->size); }

// D3D12SerializeRootSignature(desc, version, &blob, &errors): the RTS0 bytes of a version 1.0 description
static void put32(u8 **p, u32 v) { memcpy(*p, &v, 4); *p += 4; }
static void serialize_root(Cpu *c) {
  u64 d = ARG(0); u32 np = M32(d), ns = M32(d + 16); u64 params = M64(d + 8), samplers = M64(d + 24);
  size_t cap = 24 + 12 * np + 52 * ns; for (u32 i = 0; i < np; i++) cap += 8 + 12 + 20ull * (M32(params + 32 * i) == 0 ? M32(params + 32 * i + 8) : 0);
  u8 *buf = calloc(1, cap), *p = buf;
  put32(&p, 1); put32(&p, np); put32(&p, 24); put32(&p, ns); put32(&p, 0); put32(&p, M32(d + 32));
  u32 data = 24 + 12 * np;
  for (u32 i = 0; i < np; i++) { // the parameters, their data after them
    u64 q = params + 32 * i; u32 type = M32(q), n = type == 0 ? 8 + 20 * M32(q + 8) : type == 1 ? 12 : 8;
    put32(&p, type); put32(&p, M32(q + 24)); put32(&p, data);
    u8 *e = buf + data;
    if (type == 0) { u32 k = M32(q + 8); put32(&e, k); put32(&e, data + 8); memcpy(e, (void *)GP(M64(q + 16)), 20ull * k); }
    else if (type == 1) { put32(&e, M32(q + 8)); put32(&e, M32(q + 12)); put32(&e, M32(q + 16)); }
    else { put32(&e, M32(q + 8)); put32(&e, M32(q + 12)); }
    data += n;
  }
  u32 *hdr = (u32 *)buf; hdr[4] = data;
  if (ns) memcpy(buf + data, (void *)GP(samplers), 52ull * ns);
  data += 52 * ns;
  if (ARG(2)) M64(ARG(2)) = rt_blob_new(buf, data);
  if (ARG(3)) M64(ARG(3)) = 0;
  free(buf);
  RET(S_OK);
}
IMP(d3d12, D3D12SerializeRootSignature) { serialize_root(c); }
IMP(d3d12_xs, D3D12SerializeRootSignature) { serialize_root(c); }
IMP(d3d12, D3D12EnableExperimentalFeatures) { RET(E_NOINTERFACE); }

// ---- device ----
IMP(d3d12, Device_GetNodeCount) { RET(1); }
IMP(d3d12, Device_CreateCommandQueue) { // (desc, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); Queue *q = obj_new(C_QUEUE, sizeof(Queue), d->console); q->type = M32(ARG(1));
  RET(d3d12_give(&q->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_CreateCommandAllocator) { Obj *d = d3d12_obj(ARG(0)); Simple *a = obj_new(C_ALLOCATOR, sizeof(Simple), d->console); RET(d3d12_give(&a->o, ARG(2), ARG(3))); }
IMP(d3d12, Device_CreateCommandList) { // (node mask, type, allocator, initial PSO, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); List *l = obj_new(C_LIST, sizeof(List), d->console); l->type = (u32)ARG(2);
  if (ARG(4)) { u32 w[2] = {GPU_L_PSO | 1u << 16, d3d12_id_of(ARG(4))}; l->cap = 64; l->w = malloc(4 * l->cap); memcpy(l->w, w, 8); l->n = 2; }
  RET(d3d12_give(&l->o, ARG(5), ARG(6)));
}
// console: CreateCommandQueueX #48 (desc: type first, riid, ppv), CreateCommandListX #73 (desc: type first,
// allocator, PSO, riid, ppv; a DMA list, type 0x10 or its own IID, has the short copy-list vtable)
IMP(d3d12, Device_CreateCommandQueueX) { imp_d3d12_Device_CreateCommandQueue(c); }
IMP(d3d12, Device_CreateCommandListX) {
  Obj *d = d3d12_obj(ARG(0)); u32 type = M32(ARG(1));
  int dma = type == 0x10 || iid_is(ARG(4), "30bc77b9-8379-4b25-95de-b8588c42275d");
  List *l = obj_new(C_LIST, sizeof(List), dma ? 2 : d->console ? 1 : 0); l->type = type;
  if (ARG(3)) { u32 w[2] = {GPU_L_PSO | 1u << 16, d3d12_id_of(ARG(3))}; l->cap = 64; l->w = malloc(4 * l->cap); memcpy(l->w, w, 8); l->n = 2; }
  RET(d3d12_give(&l->o, dma ? 0 : ARG(4), ARG(5)));
}
IMP(d3d12, Device_CreateCommandList1) { // (node mask, type, flags, riid, ppv): created closed
  Obj *d = d3d12_obj(ARG(0)); List *l = obj_new(C_LIST, sizeof(List), d->console); l->type = (u32)ARG(2);
  RET(d3d12_give(&l->o, ARG(4), ARG(5)));
}
IMP(d3d12, Device_CheckFeatureSupport) { // (feature, data, size)
  u32 f = (u32)ARG(1); u64 p = ARG(2), n = ARG(3);
  switch (f) {
  case 0: if (n >= 60) { memset((void *)GP(p), 0, n); M32(p + 16) = 1; M32(p + 8) = 3; } RET(S_OK); return; // D3D12_OPTIONS: tiled resources tier 1, binding tier 3
  case 2: if (n >= 16) { u32 cnt = M32(p), best = 0; for (u32 i = 0; i < cnt; i++) { u32 lv = M32(M64(p + 8) + 4 * i); if (lv <= 0xc100 && lv > best) best = lv; } M32(p + 12) = best; } RET(S_OK); return; // feature levels
  case 3: if (n >= 12) { M32(p + 4) = 0xFFFFFFFF; M32(p + 8) = 0xFFFFFFFF; } RET(S_OK); return; // format support: everything
  case 7: if (n >= 4) M32(p) = 0x60; RET(S_OK); return; // shader model 6.0
  case 1: if (n >= 8) { M32(p) = 0; M32(p + 4) = 1; } RET(S_OK); return; // architecture: UMA
  case 4: if (n >= 16) { M32(p + 12) = 1; } RET(S_OK); return; // multisample quality levels
  case 27: memset((void *)GP(p), 0, n); RET(S_OK); return; // D3D12_OPTIONS5: no render passes tier, no raytracing (WebGPU has none)
  case 5: if (n >= 8) M8(p + 4) = 1; RET(S_OK); return; // format info: one plane
  case 6: if (n >= 8) { M32(p) = 40; M32(p + 4) = 40; } RET(S_OK); return; // GPU virtual address bits per resource / process
  case 12: if (n >= 4) { if (M32(p) > 2) M32(p) = 2; } RET(S_OK); return; // root signature: up to version 1.1
  case 16: memset((void *)GP(p + 4), 0, n > 4 ? n - 4 : 0); if (n >= 12) M32(p + 8) = 1; RET(S_OK); return; // architecture1 {node, tile-based, UMA, cache-coherent UMA, isolated MMU}: UMA
  case 19: if (n >= 4) M32(p) = 0; RET(S_OK); return; // shader cache: none
  case 20: if (n >= 12) M32(p + 8) = M32(p + 4) == 0; RET(S_OK); return; // command queue priority: normal only
  default: { // an options structure we do not know: all zero (nothing optional supported)
    static u32 told[4]; if (f < 128 && !(told[f / 32] >> (f % 32) & 1)) { told[f / 32] |= 1u << (f % 32); rt_log("d3d12: CheckFeatureSupport(%u): answered with zeros", f); }
    memset((void *)GP(p), 0, n); RET(S_OK);
  }
  }
}
IMP(d3d12, Device_CreateDescriptorHeap) { // ({type, count, flags, node}, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); DescHeap *h = obj_new(C_HEAP, sizeof(DescHeap), d->console);
  h->type = M32(ARG(1)); h->count = M32(ARG(1) + 4); h->flags = M32(ARG(1) + 8);
  u64 bytes = (u64)(h->count ? h->count : 1) * GPU_DESC_BYTES;
  if (d->console) h->storage = rt_gfx_alloc(bytes, 65536, 0); // in the GPU-visible region's window 0, as on the console
  else { h->storage = (uptr)aligned_alloc(64, (size_t)bytes); memset((void *)GP(h->storage), 0, (size_t)bytes); }
  RET(d3d12_give(&h->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_GetDescriptorHandleIncrementSize) { RET(GPU_DESC_BYTES); }
IMP(d3d12, Device_CreateRootSignature) { // (node, blob, size, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); Simple *r = obj_new(C_ROOTSIG, sizeof(Simple), d->console);
  u32 n = (u32)ARG(3); u8 *p = rec_begin(GPU_R_ROOTSIG, 8 + n);
  ((u32 *)p)[0] = r->o.id; ((u32 *)p)[1] = n; memcpy(p + 8, (void *)GP(ARG(2)), n); rec_end();
  RET(d3d12_give(&r->o, ARG(4), ARG(5)));
}
// views: 8 words into the descriptor at `handle`
static Resource *res(u64 p) { Obj *o = d3d12_obj(p); return o && o->cls == C_RESOURCE ? (Resource *)o : 0; }
static void put_view(u64 handle, u32 kind, u64 resource, u32 format, u32 dim, u32 mip, u32 mips, u32 slice, u32 slices) {
  Resource *r = res(resource);
  u32 *w = (u32 *)GP(handle);
  w[0] = kind; w[1] = r ? r->o.id : 0; w[2] = format ? format : r ? r->format : 0; w[3] = dim; w[4] = mip; w[5] = mips; w[6] = slice; w[7] = slices;
}
IMP(d3d12, Device_CreateRenderTargetView) { // (resource, desc, handle)
  u64 d = ARG(2);
  if (d) put_view(ARG(3), GPU_D_RTV, ARG(1), M32(d), M32(d + 4), M32(d + 8), 1, M32(d + 4) == 4 ? 0 : M32(d + 12), 1);
  else put_view(ARG(3), GPU_D_RTV, ARG(1), 0, 4, 0, 1, 0, 1);
}
IMP(d3d12, Device_CreateDepthStencilView) { // desc: format, dim, flags, then the union
  u64 d = ARG(2);
  if (d) put_view(ARG(3), GPU_D_DSV, ARG(1), M32(d), M32(d + 4), M32(d + 12), 1, 0, 1);
  else put_view(ARG(3), GPU_D_DSV, ARG(1), 0, 3, 0, 1, 0, 1);
}
IMP(d3d12, Device_CreateShaderResourceView) { // desc: format, dim, 4-component mapping, then the union at 16
  u64 d = ARG(2); Resource *r = res(ARG(1));
  if (d && M32(d + 4) == 11) { // a raytracing acceleration structure: the console's BVH descriptor (no raytracing on WebGPU)
    u64 va = M64(d + 16); u32 *w = (u32 *)GP(ARG(3)); memset(w, 0, GPU_DESC_BYTES);
    w[0] = (u32)(va >> 8); w[1] = ((u32)(va >> 40) & 0xFF) | 1u << 31; w[2] = 0xFFFFFFFFu; return;
  }
  if (!d) { put_view(ARG(3), GPU_D_SRV, ARG(1), 0, r && r->dim == 3 ? 8 : r && r->depth > 1 ? 5 : 4, 0, r ? r->mips : 1, 0, r ? r->depth : 1); return; }
  u32 dim = M32(d + 4);
  if (dim == 1) { put_view(ARG(3), GPU_D_SRV, ARG(1), M32(d), dim, (u32)M64(d + 16), M32(d + 24), M32(d + 28), M32(d + 32)); return; } // buffer: first, count, stride, flags
  u32 mip = M32(d + 16), mips = M32(d + 20), slice = 0, slices = 1;
  if (dim == 5 || dim == 10) { slice = M32(d + 24); slices = M32(d + 28); } // 2D array, cube array
  if (dim == 9) slices = 6;
  put_view(ARG(3), GPU_D_SRV, ARG(1), M32(d), dim, mip, mips, slice, slices);
}
IMP(d3d12, Device_CreateUnorderedAccessView) { // (resource, counter, desc, handle)
  u64 d = ARG(3);
  if (!d) { put_view(ARG(4), GPU_D_UAV, ARG(1), 0, 4, 0, 1, 0, 1); return; }
  u32 dim = M32(d + 4);
  if (dim == 1) put_view(ARG(4), GPU_D_UAV, ARG(1), M32(d), dim, (u32)M64(d + 8), M32(d + 16), M32(d + 20), M32(d + 28));
  else put_view(ARG(4), GPU_D_UAV, ARG(1), M32(d), dim, M32(d + 8), 1, M32(d + 12), M32(d + 16));
}
IMP(d3d12, Device_CreateConstantBufferView) { // ({va, size}, handle)
  u32 *w = (u32 *)GP(ARG(2)); u64 d = ARG(1);
  memset(w, 0, GPU_DESC_BYTES); w[0] = GPU_D_CBV;
  if (d) { u64 va = M64(d); w[2] = (u32)va; w[3] = (u32)(va >> 32); w[4] = M32(d + 8); }
}
IMP(d3d12, Device_CreateSampler) { // D3D12_SAMPLER_DESC: filter, u, v, w, bias, aniso, comparison, border[4], min lod, max lod
  u64 d = ARG(1); u32 *w = (u32 *)GP(ARG(2));
  w[0] = GPU_D_SAMPLER; w[1] = M32(d); w[2] = M32(d + 4); w[3] = M32(d + 8); w[4] = M32(d + 12); w[5] = M32(d + 20); w[6] = M32(d + 24); w[7] = M32(d + 48);
}
IMP(d3d12, Device_CopyDescriptorsSimple) { memmove((void *)GP(ARG(2)), (void *)GP(ARG(3)), (size_t)(u32)ARG(1) * GPU_DESC_BYTES); }
IMP(d3d12, Device_CopyDescriptors) { // (dst ranges count, dst starts, dst sizes, src ranges count, src starts, src sizes, type)
  u32 nd = (u32)ARG(1), ns = (u32)ARG(4); u64 ds = ARG(2), dz = ARG(3), ss = ARG(5), sz = ARG(6);
  u32 di = 0, doff = 0, si = 0, soff = 0;
  while (di < nd && si < ns) {
    u32 dn = dz ? M32(dz + 4 * di) : 1, sn = sz ? M32(sz + 4 * si) : 1;
    memmove((void *)GP(M64(ds + 8 * di) + doff * GPU_DESC_BYTES), (void *)GP(M64(ss + 8 * si) + soff * GPU_DESC_BYTES), GPU_DESC_BYTES);
    if (++doff >= dn) { di++; doff = 0; }
    if (++soff >= sn) { si++; soff = 0; }
  }
}

// formats: bytes per element (per 4x4 block for the compressed ones)
static u32 fmt_bytes(u32 f, u32 *block) {
  *block = 1;
  switch (f) {
  case 2: case 1: return 16;                                     // R32G32B32A32
  case 6: case 5: return 12;                                     // R32G32B32
  case 10: case 11: case 12: case 13: case 14: case 9: case 16: case 15: case 17: case 18: case 19: case 20: case 21: case 22: return 8; // 16x4, 32x2
  case 70: case 71: case 72: case 79: case 80: case 81: *block = 4; return 8;   // BC1, BC4
  case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84: case 94: case 95: case 96: case 97: case 98: case 99: *block = 4; return 16; // BC2 3 5 6H 7
  case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 85: case 86: case 115: return 2; // 8x2, 16, B5G6R5
  case 60: case 61: case 62: case 63: case 64: case 65: return 1; // R8
  default: return 4;
  }
}
void d3d12_footprints(const u8 *desc, u32 first, u32 num, u64 base, u64 layouts, u64 rows, u64 sizes, u64 total) {
  u32 dim = *(u32 *)desc; u64 width = *(u64 *)(desc + 16); u32 height = *(u32 *)(desc + 24); u16 da = *(u16 *)(desc + 28), mips = *(u16 *)(desc + 30); u32 fmt = *(u32 *)(desc + 32);
  if (!mips) mips = 1;
  u64 off = base;
  for (u32 i = 0; i < num; i++) {
    u32 sub = first + i, mip = sub % mips;
    u32 block, bpe = fmt_bytes(fmt, &block);
    u64 w = width >> mip; if (!w) w = 1;
    u32 h = height >> mip; if (!h) h = 1;
    u32 d = dim == 4 ? (da >> mip ? da >> mip : 1) : 1;
    u64 row, nrows, pitch;
    if (dim == 1) { row = width; nrows = 1; pitch = (width + 255) & ~255ull; h = 1; d = 1; w = width; fmt = 0; }
    else { u64 bw = (w + block - 1) / block; nrows = (h + block - 1) / block; row = bw * bpe; pitch = (row + 255) & ~255ull; }
    off = (off + 511) & ~511ull;
    if (layouts) { u64 l = layouts + 32ull * i; M64(l) = off; M32(l + 8) = fmt; M32(l + 12) = (u32)w; M32(l + 16) = h; M32(l + 20) = d; M32(l + 24) = (u32)pitch; }
    if (rows) M32(rows + 4ull * i) = (u32)nrows;
    if (sizes) M64(sizes + 8ull * i) = row;
    off += pitch * nrows * d;
    if (dim == 1) off = base + width;
  }
  if (total) M64(total) = off - base;
}
IMP(d3d12, Device_GetCopyableFootprints) { d3d12_footprints((u8 *)GP(ARG(1)), (u32)ARG(2), (u32)ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8)); }
// every subresource: a 3D texture's depth is not slices
u64 d3d12_alloc_info(const u8 *desc) { u64 t = 0; u16 mips = *(u16 *)(desc + 30); d3d12_footprints(desc, 0, (*(u32 *)desc == 4 ? 1 : *(u16 *)(desc + 28)) * (mips ? mips : 1), 0, 0, 0, 0, (uptr)&t); return (t + 65535) & ~65535ull; }
IMP(d3d12, Device_GetResourceAllocationInfo) { // hidden return pointer (PC), then (visible mask, count, descs)
  u64 ret = ARG(1); u64 n = ARG(3), descs = ARG(4), size = 0;
  for (u64 i = 0; i < n; i++) size += d3d12_alloc_info((u8 *)GP(descs + 56 * i));
  M64(ret) = size; M64(ret + 8) = 65536; RET(ret);
}
static u64 va_next = 0x200000000ull; // GPU addresses of PC buffers without guest backing: above every guest address
// heap: D3D12_HEAP_TYPE (4 custom: cpu_page says whether the CPU sees it); at: where its memory is (a placed
// resource: title memory or a heap's), else 0.
// PC device: upload / readback buffers have guest backing, the others live on the GPU only.
// Console device (UMA): every buffer is guest memory in the GPU-visible region, its GPU address its CPU one;
// upload and readback buffers up to 64 MiB go to the coherent window 3 (XCL's rule), the rest to window 0.
// A texture placed at title memory (CreatePlacedResourceX) has its texels there.
u32 d3d12_create_resource(Obj *dev, u32 heap, u32 cpu_page, u64 desc, u64 at, u64 riid, u64 ppv) {
  Resource *r = obj_new(C_RESOURCE, sizeof(Resource), dev->console);
  memcpy(r->desc, (void *)GP(desc), 56);
  r->dim = M32(desc); r->width = M64(desc + 16); r->height = M32(desc + 24); r->depth = M16(desc + 28); r->mips = M16(desc + 30);
  r->format = M32(desc + 32); r->samples = M32(desc + 36); r->flags = M32(desc + 48);
  r->heap = heap ? heap : 1;
  if (!r->mips) { u64 m = r->width > r->height ? r->width : r->height; r->mips = 1; while (m > 1) { m >>= 1; r->mips++; } if (r->dim == 1) r->mips = 1; }
  if (!r->depth) r->depth = 1;
  u32 cpu = r->heap == 2 || r->heap == 3 || (r->heap == 4 && cpu_page >= 2);
  if (dev->console) {
    if (at) { r->va = at; r->backing = (u32)at; }
    else if (r->dim == 1) {
      r->va = rt_gfx_alloc(r->width, 65536, (r->heap == 2 || r->heap == 3) && r->width <= (64ull << 20) ? 3 : 0);
      if (!r->va) return E_OUTOFMEMORY;
      r->backing = (u32)r->va;
    }
  } else if (r->dim == 1) {
    if (cpu) { r->backing = (uptr)aligned_alloc(256, (size_t)((r->width + 255) & ~255ull)); if (!r->backing) return E_OUTOFMEMORY; memset((void *)GP(r->backing), 0, (size_t)r->width); r->va = r->backing; }
    else { pthread_mutex_lock(&d3d_lock); r->va = va_next; va_next += (r->width + 65535) & ~65535ull; pthread_mutex_unlock(&d3d_lock); }
  }
  if (rt_trace) rt_log("d3d12: resource %u dim %u %llux%u format %u heap %u backing 0x%llx va 0x%llx", r->o.id, r->dim, r->width, r->height, r->format, r->heap, r->backing, r->va);
  u32 w[14] = {r->o.id, r->dim == 1 ? 0 : r->dim - 1, r->format, (u32)r->width, (u32)(r->width >> 32), r->height, r->depth, r->mips, r->samples ? r->samples : 1, r->flags | (r->dim == 1 ? GPU_RESOURCE_CBV_TAIL : 0), r->heap, (u32)r->backing, (u32)r->va, (u32)(r->va >> 32)};
  rec_words(GPU_R_RESOURCE, w, 14);
  if (!ppv) return S_FALSE;
  return d3d12_give(&r->o, riid, ppv);
}
IMP(d3d12, Device_CreateCommittedResource) { // (heap properties, heap flags, desc, state, clear value, riid, ppv)
  u64 hp = ARG(1); RET(d3d12_create_resource(d3d12_obj(ARG(0)), hp ? M32(hp) : 1, hp ? M32(hp + 4) : 0, ARG(3), 0, ARG(6), ARG(7)));
}
IMP(d3d12, Device_CreatePlacedResource) { // (heap, offset, desc, state, clear, riid, ppv)
  Obj *ho = d3d12_obj(ARG(1)); HeapObj *h = ho && ho->cls == C_HEAPOBJ ? (HeapObj *)ho : 0;
  // console: a buffer placed in a heap is the heap's memory at that offset (aliasing included); textures stay
  // the GPU's own. PC: a resource of its own.
  u64 at = h && h->va && M32(ARG(3)) == 1 ? h->va + ARG(2) : 0;
  RET(d3d12_create_resource(d3d12_obj(ARG(0)), h ? h->type : 1, h ? h->cpu_page : 0, ARG(3), at, ARG(6), ARG(7)));
}
IMP(d3d12, Device_CreatePlacedResourceX) { // #44 (title address, desc, state, clear, riid, ppv): the resource is that memory
  RET(d3d12_create_resource(d3d12_obj(ARG(0)), 1, 0, ARG(2), ARG(1), ARG(5), ARG(6)));
}
IMP(d3d12, Device_CreateHeap) { // ({size, properties {type, cpu page, pool, ...}, alignment, flags}, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); HeapObj *h = obj_new(C_HEAPOBJ, sizeof(HeapObj), d->console); u64 desc = ARG(1);
  h->size = M64(desc); h->type = M32(desc + 8); h->cpu_page = M32(desc + 12);
  if (d->console) { h->va = rt_gfx_alloc(h->size, 65536, (h->type == 2 || h->type == 3) && h->size <= (64ull << 20) ? 3 : 0); if (!h->va) { RET(E_OUTOFMEMORY); return; } }
  RET(d3d12_give(&h->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_CreateQueryHeap) { // ({type, count, node}, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); QueryHeap *q = obj_new(C_QUERYHEAP, sizeof(QueryHeap), d->console);
  q->type = M32(ARG(1)); q->count = M32(ARG(1) + 4); RET(d3d12_give(&q->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_SetDebugFlagsX) { RET(S_OK); }
IMP(d3d12, Device_SerializePipelineStateX) { RET(0x80070005u); } // E_ACCESSDENIED: no console pipeline cache here
// page pools (#74 RegisterPagePoolX(address, pages, &id), #75 UnregisterPagePoolX(id)): memory is already the GPU's
static u64 pools[64];
IMP(d3d12, Device_RegisterPagePoolX) {
  pthread_mutex_lock(&d3d_lock); int i = 0; while (i < 64 && pools[i]) i++; if (i < 64) pools[i] = ARG(1) ? ARG(1) : 1; pthread_mutex_unlock(&d3d_lock);
  if (i == 64) { RET(E_OUTOFMEMORY); return; }
  if (ARG(3)) M32(ARG(3)) = (u32)i; RET(S_OK);
}
IMP(d3d12, Device_UnregisterPagePoolX) { u64 i = ARG(1); if (i >= 64 || !pools[i]) { RET(E_INVALIDARG); return; } pools[i] = 0; RET(S_OK); }
IMP(d3d12, Device_CreateFence) { // (initial value, flags, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); Fence *f = obj_new(C_FENCE, sizeof(Fence), d->console); f->completed = ARG(1);
  RET(d3d12_give(&f->o, ARG(3), ARG(4)));
}
IMP(d3d12, Device_GetDeviceRemovedReason) { RET(S_OK); }
IMP(d3d12, Device_MakeResident) { RET(S_OK); }
IMP(d3d12, Device_Evict) { RET(S_OK); }
IMP(d3d12, Device_SetStablePowerState) { RET(S_OK); }
IMP(d3d12, Device_GetAdapterLuid) { u64 ret = ARG(1); M64(ret) = 0x5357; RET(ret); }
IMP(d3d12, Device_GetCustomHeapProperties) { u64 ret = ARG(1); M32(ret) = 4; M32(ret + 4) = M32(ret + 8) = 0; M32(ret + 12) = 1; M32(ret + 16) = 1; u32 t = (u32)ARG(3); if (t == 2 || t == 3) { M32(ret + 4) = t == 2 ? 2 : 3; M32(ret + 8) = 1; } else { M32(ret + 4) = 1; M32(ret + 8) = 1; } RET(ret); }

// pipeline states: the desc goes into the record with its pointers turned into offsets in the record
static u32 blob_put(u8 *rec, u32 *at, u64 ptr, u64 size) { u32 o = *at; if (ptr && size) { memcpy(rec + o, (void *)GP(ptr), (size_t)size); *at += ((u32)size + 7) & ~7u; } return ptr && size ? o : 0; }
static void pso_keep(Pso *p, const u8 *rec, u32 bytes) { p->rec = malloc(bytes); memcpy(p->rec, rec, bytes); p->bytes = bytes; }
static u32 graphics_pso(Obj *d, u64 desc, u64 riid, u64 ppv) {
  Pso *p = obj_new(C_PSO, sizeof(Pso), d->console);
  u32 ne = M32(desc + 560); u64 elems = M64(desc + 552);
  u64 bytes = 16 + 656 + 32ull * ne;
  for (int s = 0; s < 5; s++) bytes += (M64(desc + 16 + 16 * s) + 7) & ~7ull;
  for (u32 i = 0; i < ne; i++) bytes += strlen(GSTR(M64(elems + 32 * i))) + 8;
  u8 *rec = rec_begin(GPU_R_PSO, (u32)bytes);
  u32 *h = (u32 *)rec; h[0] = p->o.id; h[1] = d3d12_id_of(M64(desc)); h[2] = 0; h[3] = ne;
  u8 *dd = rec + 16; memcpy(dd, (void *)GP(desc), 656);
  u32 at = 16 + 656 + 32 * ne;
  for (int s = 0; s < 5; s++) { u64 *bc = (u64 *)(dd + 8 + 16 * s); bc[0] = blob_put(rec, &at, bc[0], bc[1]); }
  for (u32 i = 0; i < ne; i++) {
    u8 *e = rec + 16 + 656 + 32 * i; memcpy(e, (void *)GP(elems + 32 * i), 32);
    const char *nm = GSTR(M64(elems + 32 * i)); size_t l = strlen(nm) + 1; memcpy(rec + at, nm, l); *(u64 *)e = at; at += ((u32)l + 7) & ~7u;
  }
  *(u64 *)(dd + 552) = 16 + 656;
  *(u64 *)dd = 0; *(u64 *)(dd + 88) = 0; *(u64 *)(dd + 104) = 0; *(u64 *)(dd + 632) = 0; // root signature, stream output, cached PSO
  pso_keep(p, rec, (u32)bytes);
  rec_end();
  return d3d12_give(&p->o, riid, ppv);
}
IMP(d3d12, Device_CreateGraphicsPipelineState) { RET(graphics_pso(d3d12_obj(ARG(0)), ARG(1), ARG(2), ARG(3))); } // (desc, riid, ppv)
IMP(d3d12, Device_CreateGraphicsPipelineStateX) { RET(graphics_pso(d3d12_obj(ARG(0)), ARG(1), ARG(4), ARG(5))); } // #60 (desc, flags, ?, riid, ppv)
// #55 CreateDerivedPipelineStateX(base, count, patches, riid, ppv): the base's description with parts replaced.
// A patch is {u32 part; payload} every 0x14C bytes; parts (XCL's findings): 0 depth-stencil, 1 rasterizer,
// 2 blend, 3 sample mask; 5 is seen on retail titles and not understood (ignored); others fail.
IMP(d3d12, Device_CreateDerivedPipelineStateX) {
  Obj *d = d3d12_obj(ARG(0)), *bo = d3d12_obj(ARG(1)); u32 n = (u32)ARG(2); u64 patches = ARG(3);
  Pso *base = bo && bo->cls == C_PSO ? (Pso *)bo : 0;
  if (!base || !base->rec || ((u32 *)base->rec)[2] != 0) { RET(E_INVALIDARG); return; }
  u8 *rec = malloc(base->bytes); memcpy(rec, base->rec, base->bytes);
  for (u32 i = 0; i < n; i++) {
    u64 q = patches + 0x14Cull * i; u32 part = M32(q), at, len;
    switch (part) { case 0: at = 0x1F0; len = 0x34; break; case 1: at = 0x1C4; len = 0x2C; break; case 2: at = 0x78; len = 0x148; break; case 3: at = 0x1C0; len = 4; break;
      case 5: continue; default: free(rec); rt_log("d3d12: CreateDerivedPipelineStateX: unknown patch part %u", part); RET(E_INVALIDARG); return; }
    memcpy(rec + 16 + at, (void *)GP(q + 4), len);
  }
  Pso *p = obj_new(C_PSO, sizeof(Pso), d->console);
  ((u32 *)rec)[0] = p->o.id;
  u8 *r = rec_begin(GPU_R_PSO, base->bytes); memcpy(r, rec, base->bytes); rec_end();
  p->rec = rec; p->bytes = base->bytes;
  RET(d3d12_give(&p->o, ARG(4), ARG(5)));
}
IMP(d3d12, Device_CreateComputePipelineState) { // ({root signature, CS {ptr, size}, node, cached, flags}, riid, ppv)
  Obj *d = d3d12_obj(ARG(0)); u64 desc = ARG(1);
  Pso *p = obj_new(C_PSO, sizeof(Pso), d->console);
  u64 n = M64(desc + 16);
  u8 *rec = rec_begin(GPU_R_PSO, (u32)(16 + 56 + ((n + 7) & ~7ull)));
  u32 *h = (u32 *)rec; h[0] = p->o.id; h[1] = d3d12_id_of(M64(desc)); h[2] = 1; h[3] = 0;
  memcpy(rec + 16, (void *)GP(desc), 56);
  u32 at = 16 + 56; *(u64 *)(rec + 16 + 8) = blob_put(rec, &at, M64(desc + 8), n); *(u64 *)(rec + 16) = 0;
  rec_end();
  RET(d3d12_give(&p->o, ARG(2), ARG(3)));
}

// ---- queue ----
// a list's words go out whole; those a KickoffX already ran come first under a REPLAY word (state only)
void d3d12_execute_words(u32 queue, List *l) {
  u32 extra = l->kick ? 2 : 0;
  u8 *p = rec_begin(GPU_R_EXECUTE, 4 + 4 * (extra + l->n));
  *(u32 *)p = queue;
  if (extra) { ((u32 *)(p + 4))[0] = GPU_L_REPLAY | 1u << 16; ((u32 *)(p + 4))[1] = l->kick; }
  memcpy(p + 4 + 4 * extra, l->w, 4ull * l->n);
  rec_end();
}
static void execute(Queue *q, List *l) { d3d12_execute_words(q->o.id, l); }
IMP(d3d12, CommandQueue_ExecuteCommandLists) { // (count, lists)
  THIS(Queue);
  for (u32 i = 0; i < (u32)ARG(1); i++) { Obj *o = d3d12_obj(M64(ARG(2) + 8 * i)); if (o && o->cls == C_LIST) execute(self, (List *)o); }
  __atomic_fetch_add(&ring[GPU_H_SUBMITTED], 1, __ATOMIC_RELAXED);
}
static void signal_rec(u32 op, u64 fence, u64 value) { u32 w[3] = {(u32)fence, (u32)value, (u32)(value >> 32)}; rec_words(op, w, 3); }
IMP(d3d12, CommandQueue_Signal) { Obj *f = d3d12_obj(ARG(1)); if (!f || f->cls != C_FENCE) { RET(E_INVALIDARG); return; } signal_rec(GPU_R_SIGNAL, ARG(1), ARG(2)); RET(S_OK); }
IMP(d3d12, CommandQueue_SignalX) { imp_d3d12_CommandQueue_Signal(c); }
IMP(d3d12, CommandQueue_Wait) { Obj *f = d3d12_obj(ARG(1)); if (!f || f->cls != C_FENCE) { RET(E_INVALIDARG); return; } signal_rec(GPU_R_WAIT, ARG(1), ARG(2)); RET(S_OK); }
IMP(d3d12, CommandQueue_GetTimestampFrequency) { M64(ARG(1)) = 1000000; RET(S_OK); }
IMP(d3d12, CommandQueue_GetClockCalibration) { u64 t = (u64)host_now_us(); if (ARG(1)) M64(ARG(1)) = t; if (ARG(2)) M64(ARG(2)) = t; RET(S_OK); }
IMP(d3d12, CommandQueue_GetDesc) { u64 ret = ARG(1); THIS(Queue); M32(ret) = self->type; M32(ret + 4) = 0; M32(ret + 8) = 0; M32(ret + 12) = 0; RET(ret); }
IMP(d3d12, CommandQueue_SetMarker) {}
IMP(d3d12, CommandQueue_BeginEvent) {}
IMP(d3d12, CommandQueue_EndEvent) {}
IMP(d3d12, CommandQueue_PresentX) { // (plane count, planes, parameters); a plane: token, resource count, resources, ...
  u64 planes = ARG(2);
  if (!ARG(1) || !planes || !M32(planes + 8)) { RET(S_OK); return; }
  rt_gpu_present(d3d12_id_of(M64(M64(planes + 0x10))), M64(planes), 1);
  RET(S_OK);
}

// ---- allocator, fence, resource, descriptor heap ----
IMP(d3d12, CommandAllocator_Reset) { RET(S_OK); }
IMP(d3d12, Fence_GetCompletedValue) { THIS(Fence); RET(fence_value(self)); }
IMP(d3d12, Fence_SetEventOnCompletion) { // (value, event): no event = wait here
  THIS(Fence); u64 v = ARG(1), ev = ARG(2);
  if (!ev) { fence_wait(self, v); RET(S_OK); return; }
  if (fence_value(self) >= v) { set_event(ev); RET(S_OK); return; }
  Waiter *w = calloc(1, sizeof *w); w->f = self; w->value = v; w->event = ev;
  pthread_mutex_lock(&waiter_lock); w->next = waiters; waiters = w;
  if (!waiter_thread) { waiter_thread = 1; pthread_t t; pthread_create(&t, 0, waiter_main, 0); pthread_detach(t); }
  pthread_mutex_unlock(&waiter_lock);
  epoch_bump(); // the helper looks at the list again
  RET(S_OK);
}
void rt_fence_signal_cpu(u64 fence, u64 value) { Obj *o = d3d12_obj(fence); if (!o || o->cls != C_FENCE) return; __atomic_store_n(&((Fence *)o)->completed, value, __ATOMIC_RELEASE); epoch_bump(); } // DirectStorage
IMP(d3d12, Fence_Signal) { THIS(Fence); __atomic_store_n(&self->completed, ARG(1), __ATOMIC_RELEASE); epoch_bump(); RET(S_OK); }
IMP(d3d12, Fence_GetCreationFlags) { RET(0); }
IMP(d3d12, Resource_Map) { // (subresource, read range, &data): the memory (what the GPU wrote is there once its fence is)
  THIS(Resource);
  if (!self->backing) { if (ARG(3)) M64(ARG(3)) = 0; RET(E_INVALIDARG); return; }
  if (ARG(3)) M64(ARG(3)) = self->o.console ? self->va : self->backing;
  RET(S_OK);
}
IMP(d3d12, Resource_Unmap) {}
IMP(d3d12, Resource_GetGPUVirtualAddress) { THIS(Resource); RET(self->va); }
IMP(d3d12, Resource_GetDesc) { THIS(Resource); u64 ret = ARG(1); memcpy((void *)GP(ret), self->desc, 56); RET(ret); }
IMP(d3d12, Resource_GetHeapProperties) { THIS(Resource); if (ARG(1)) { memset((void *)GP(ARG(1)), 0, 20); M32(ARG(1)) = self->heap; M32(ARG(1) + 12) = 1; M32(ARG(1) + 16) = 1; } if (ARG(2)) M32(ARG(2)) = 0; RET(S_OK); }
IMP(d3d12, Resource_WriteToSubresource) { RET(E_NOTIMPL); }
IMP(d3d12, Resource_ReadFromSubresource) { RET(E_NOTIMPL); }
IMP(d3d12, DescriptorHeap_GetDesc) { THIS(DescHeap); u64 ret = ARG(1); M32(ret) = self->type; M32(ret + 4) = self->count; M32(ret + 8) = self->flags; M32(ret + 12) = 0; RET(ret); }
// PC: a hidden pointer to the returned handle; console: the handle in RAX
IMP(d3d12, DescriptorHeap_GetCPUDescriptorHandleForHeapStart) { THIS(DescHeap); if (self->o.console) { RET(self->storage); return; } M64(ARG(1)) = self->storage; RET(ARG(1)); }
IMP(d3d12, DescriptorHeap_GetGPUDescriptorHandleForHeapStart) { THIS(DescHeap); u64 v = self->flags & 1 ? self->storage : 0; if (self->o.console) { RET(v); return; } M64(ARG(1)) = v; RET(ARG(1)); }

// ---- command lists: words ----
u32 *d3d12_emit(List *l, u32 op, u32 n) {
  if (l->n + n + 1 > l->cap) { l->cap = (l->n + n + 1) * 2 + 64; l->w = realloc(l->w, 4ull * l->cap); }
  u32 *p = l->w + l->n; p[0] = op | n << 16; l->n += n + 1;
  return p + 1;
}
IMP(d3d12, CommandList_GetType) { LIST; RET(self->type); }
IMP(d3d12, CommandList_Close) { RET(S_OK); }
IMP(d3d12, CommandList_Reset) { LIST; self->n = 0; self->kick = 0; self->up = self->up_base; if (ARG(2)) d3d12_emit(self, GPU_L_PSO, 1)[0] = d3d12_id_of(ARG(2)); RET(S_OK); }
IMP(d3d12, CommandList_ClearState) { LIST; if (ARG(1)) d3d12_emit(self, GPU_L_PSO, 1)[0] = d3d12_id_of(ARG(1)); }
IMP(d3d12, CommandList_DrawInstanced) { LIST; u32 *p = d3d12_emit(self, GPU_L_DRAW, 4); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); }
IMP(d3d12, CommandList_DrawIndexedInstanced) { LIST; u32 *p = d3d12_emit(self, GPU_L_DRAW_INDEXED, 5); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); p[4] = (u32)ARG(5); }
IMP(d3d12, CommandList_Dispatch) { LIST; u32 *p = d3d12_emit(self, GPU_L_DISPATCH, 3); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); }
IMP(d3d12, CommandList_CopyBufferRegion) { // (dst, dst offset, src, src offset, bytes)
  LIST; u32 *p = d3d12_emit(self, GPU_L_COPY_BUFFER, 8); u64 a = ARG(2), b = ARG(4), n = ARG(5);
  p[0] = d3d12_id_of(ARG(1)); p[1] = (u32)a; p[2] = (u32)(a >> 32); p[3] = d3d12_id_of(ARG(3)); p[4] = (u32)b; p[5] = (u32)(b >> 32); p[6] = (u32)n; p[7] = (u32)(n >> 32);
}
static void put_location(u32 *p, u64 loc) { // resource id, type, subresource, footprint offset lo hi, format, width, height, depth, row pitch
  p[0] = d3d12_id_of(M64(loc)); p[1] = M32(loc + 8);
  if (p[1] == 0) { p[2] = M32(loc + 16); return; }
  u64 off = M64(loc + 16); p[3] = (u32)off; p[4] = (u32)(off >> 32);
  for (int i = 0; i < 5; i++) p[5 + i] = M32(loc + 24 + 4 * i);
}
IMP(d3d12, CommandList_CopyTextureRegion) { // (dst location, x, y, z, src location, box)
  LIST; u32 *p = d3d12_emit(self, GPU_L_COPY_TEXTURE, 30); memset(p, 0, 4 * 30); // (a list's words are not zeroed: no box must say so)
  put_location(p, ARG(1)); p[10] = (u32)ARG(2); p[11] = (u32)ARG(3); p[12] = (u32)ARG(4);
  put_location(p + 13, ARG(5));
  u64 box = ARG(6);
  if (box) { p[23] = 1; for (int i = 0; i < 6; i++) p[24 + i] = M32(box + 4 * i); }
}
IMP(d3d12, CommandList_CopyResource) { LIST; u32 *p = d3d12_emit(self, GPU_L_COPY_RESOURCE, 2); p[0] = d3d12_id_of(ARG(1)); p[1] = d3d12_id_of(ARG(2)); }
IMP(d3d12, CommandList_ResolveSubresource) { LIST; u32 *p = d3d12_emit(self, GPU_L_COPY_RESOURCE, 2); p[0] = d3d12_id_of(ARG(1)); p[1] = d3d12_id_of(ARG(3)); }
IMP(d3d12, CommandList_IASetPrimitiveTopology) { LIST; d3d12_emit(self, GPU_L_TOPOLOGY, 1)[0] = (u32)ARG(1); }
IMP(d3d12, CommandList_RSSetViewports) { LIST; u32 n = (u32)ARG(1); u32 *p = d3d12_emit(self, GPU_L_VIEWPORTS, 1 + 6 * n); p[0] = n; memcpy(p + 1, (void *)GP(ARG(2)), 24ull * n); }
IMP(d3d12, CommandList_RSSetScissorRects) { LIST; u32 n = (u32)ARG(1); u32 *p = d3d12_emit(self, GPU_L_SCISSORS, 1 + 4 * n); p[0] = n; memcpy(p + 1, (void *)GP(ARG(2)), 16ull * n); }
IMP(d3d12, CommandList_OMSetBlendFactor) { LIST; u32 *p = d3d12_emit(self, GPU_L_BLEND_FACTOR, 4); if (ARG(1)) memcpy(p, (void *)GP(ARG(1)), 16); else { float one = 1; for (int i = 0; i < 4; i++) memcpy(p + i, &one, 4); } }
IMP(d3d12, CommandList_OMSetStencilRef) { LIST; d3d12_emit(self, GPU_L_STENCIL_REF, 1)[0] = (u32)ARG(1); }
IMP(d3d12, CommandList_SetPipelineState) { LIST; d3d12_emit(self, GPU_L_PSO, 1)[0] = d3d12_id_of(ARG(1)); }
IMP(d3d12, CommandList_ResourceBarrier) {}
IMP(d3d12, CommandList_SetDescriptorHeaps) {} // descriptor tables are addresses in the heaps' storage
static void root_sig(Cpu *c, u32 bp) { LIST; u32 *p = d3d12_emit(self, GPU_L_ROOTSIG, 2); p[0] = bp; p[1] = d3d12_id_of(ARG(1)); }
static void table(Cpu *c, u32 bp) { LIST; u32 *p = d3d12_emit(self, GPU_L_TABLE, 4); u64 h = ARG(2); p[0] = bp; p[1] = (u32)ARG(1); p[2] = (u32)h; p[3] = (u32)(h >> 32); }
static void constants(Cpu *c, u32 bp, u32 n, u64 data, u32 offset) { LIST; u32 *p = d3d12_emit(self, GPU_L_CONSTANTS, 4 + n); p[0] = bp; p[1] = (u32)ARG(1); p[2] = offset; p[3] = n; if (data) memcpy(p + 4, (void *)GP(data), 4ull * n); }
static void root_view(Cpu *c, u32 bp, u32 kind) { LIST; u32 *p = d3d12_emit(self, GPU_L_ROOT_VIEW, 5); u64 va = ARG(2); p[0] = bp; p[1] = (u32)ARG(1); p[2] = kind; p[3] = (u32)va; p[4] = (u32)(va >> 32); }
IMP(d3d12, CommandList_SetComputeRootSignature) { root_sig(c, 1); }
IMP(d3d12, CommandList_SetGraphicsRootSignature) { root_sig(c, 0); }
IMP(d3d12, CommandList_SetComputeRootDescriptorTable) { table(c, 1); }
IMP(d3d12, CommandList_SetGraphicsRootDescriptorTable) { table(c, 0); }
IMP(d3d12, CommandList_SetComputeRoot32BitConstant) { u32 v = (u32)ARG(2); constants(c, 1, 1, (uptr)&v, (u32)ARG(3)); }
IMP(d3d12, CommandList_SetGraphicsRoot32BitConstant) { u32 v = (u32)ARG(2); constants(c, 0, 1, (uptr)&v, (u32)ARG(3)); }
IMP(d3d12, CommandList_SetComputeRoot32BitConstants) { constants(c, 1, (u32)ARG(2), ARG(3), (u32)ARG(4)); }
IMP(d3d12, CommandList_SetGraphicsRoot32BitConstants) { constants(c, 0, (u32)ARG(2), ARG(3), (u32)ARG(4)); }
IMP(d3d12, CommandList_SetComputeRootConstantBufferView) { root_view(c, 1, GPU_D_CBV); }
IMP(d3d12, CommandList_SetGraphicsRootConstantBufferView) { root_view(c, 0, GPU_D_CBV); }
IMP(d3d12, CommandList_SetComputeRootShaderResourceView) { root_view(c, 1, GPU_D_SRV); }
IMP(d3d12, CommandList_SetGraphicsRootShaderResourceView) { root_view(c, 0, GPU_D_SRV); }
IMP(d3d12, CommandList_SetComputeRootUnorderedAccessView) { root_view(c, 1, GPU_D_UAV); }
IMP(d3d12, CommandList_SetGraphicsRootUnorderedAccessView) { root_view(c, 0, GPU_D_UAV); }
IMP(d3d12, CommandList_IASetIndexBuffer) { // ({va, size, format})
  LIST; u32 *p = d3d12_emit(self, GPU_L_IB, 4); u64 v = ARG(1);
  if (v) { u64 va = M64(v); p[0] = (u32)va; p[1] = (u32)(va >> 32); p[2] = M32(v + 8); p[3] = M32(v + 12); } else memset(p, 0, 16);
}
IMP(d3d12, CommandList_IASetVertexBuffers) { // (start, count, views {va, size, stride})
  LIST; u32 n = (u32)ARG(2); u32 *p = d3d12_emit(self, GPU_L_VB, 2 + 4 * n); p[0] = (u32)ARG(1); p[1] = n;
  for (u32 i = 0; i < n; i++) { u64 v = ARG(3) + 16ull * i; if (ARG(3)) { u64 va = M64(v); p[2 + 4 * i] = (u32)va; p[3 + 4 * i] = (u32)(va >> 32); p[4 + 4 * i] = M32(v + 8); p[5 + 4 * i] = M32(v + 12); } }
}
// render targets and clears carry copies of their descriptors: the heap may change before the GPU runs
static void copy_desc(u32 *dst, u64 handle) { if (handle) memcpy(dst, (void *)GP(handle), GPU_DESC_BYTES); else memset(dst, 0, GPU_DESC_BYTES); }
IMP(d3d12, CommandList_OMSetRenderTargets) { // (count, handles, single range, dsv handle*)
  LIST; u32 n = (u32)ARG(1); u64 hs = ARG(2); int single = (int)(u32)ARG(3); u64 dsv = ARG(4);
  u32 *p = d3d12_emit(self, GPU_L_SET_RTS, 1 + 8 * (n + 1)); p[0] = n;
  for (u32 i = 0; i < n; i++) copy_desc(p + 1 + 8 * i, single ? M64(hs) + GPU_DESC_BYTES * i : M64(hs + 8 * i));
  copy_desc(p + 1 + 8 * n, dsv ? M64(dsv) : 0);
}
IMP(d3d12, CommandList_ClearRenderTargetView) { // (handle, color[4], rect count, rects)
  LIST; u32 *p = d3d12_emit(self, GPU_L_CLEAR_RTV, 12); copy_desc(p, ARG(1)); memcpy(p + 8, (void *)GP(ARG(2)), 16);
}
IMP(d3d12, CommandList_ClearDepthStencilView) { // (handle, flags, depth (xmm3), stencil, rect count, rects)
  LIST; u32 *p = d3d12_emit(self, GPU_L_CLEAR_DSV, 11); copy_desc(p, ARG(1)); p[8] = (u32)ARG(2); { float dz = X[3].ps[0]; memcpy(p + 9, &dz, 4); } p[10] = (u8)ARG(4);
}
IMP(d3d12, CommandList_DiscardResource) {}
IMP(d3d12, CommandList_SetMarker) {}
IMP(d3d12, CommandList_BeginEvent) {}
IMP(d3d12, CommandList_EndEvent) {}
static void query(Cpu *c, u32 op) { LIST; u32 *p = d3d12_emit(self, op, 3); p[0] = d3d12_id_of(ARG(1)); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); }
IMP(d3d12, CommandList_BeginQuery) { query(c, GPU_L_QUERY_BEGIN); } // (heap, type, index)
IMP(d3d12, CommandList_EndQuery) { query(c, GPU_L_QUERY_END); }
IMP(d3d12, CommandList_ResolveQueryData) { // (heap, type, first, count, destination, offset)
  LIST; u32 *p = d3d12_emit(self, GPU_L_QUERY_RESOLVE, 7); u64 off = ARG(6);
  p[0] = d3d12_id_of(ARG(1)); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); p[4] = d3d12_id_of(ARG(5)); p[5] = (u32)off; p[6] = (u32)(off >> 32);
}

int d3d12_pace_off(void) { static int off = -1; if (off < 0) { const char *e = getenv("SW_PACE"); off = e && !strcmp(e, "off"); } return off; }
