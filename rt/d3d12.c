// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12 and D3D12.X, the front end: COM objects whose methods answer on the calling thread, command lists
// recorded into words, and one ordered ring of records (rt/d3d12_cmd.h) that a GPU back end consumes
// (web/gpu/backend.js in the browser, web/gpu/null.js in Node). Nothing here waits for the GPU except
// fences, and they wait on words the back end writes.
//
// Two flavours of the same objects: a device from D3D12CreateDevice has the PC vtables (d3d12.h order,
// rt/d3d12_layout.gen.h); one from D3D12XboxCreateDevice has the console's, whose differences follow
// XCL's findings on retail titles (github.com/ivall/xcl, GPL-2.0): the command list's methods from
// SetDescriptorHeaps on sit 4 slots later, the device's extensions start at #44 (frame clock #78-#80),
// the queue's PresentX is #30, and descriptor heaps return their start handles in RAX.
#include <time.h>
#include "rt.h"
#include "d3d12_cmd.h"
#include "d3d12_layout.gen.h"

#define S_OK 0
#define S_FALSE 1
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define DXGI_ERROR_UNSUPPORTED 0x887A0004u

enum { C_DEVICE, C_QUEUE, C_ALLOCATOR, C_LIST, C_FENCE, C_RESOURCE, C_HEAP, C_ROOTSIG, C_PSO, C_HEAPOBJ, C_QUERYHEAP, C_CMDSIG, C_COUNT };
static const char *const cls_name[C_COUNT] = {"Device", "CommandQueue", "CommandAllocator", "CommandList", "Fence", "Resource", "DescriptorHeap", "RootSignature", "PipelineState", "Heap", "QueryHeap", "CommandSignature"};
#define OMAGIC 0x44334432u

typedef struct Obj { u64 vtbl; u32 magic, cls, id; s32 refs; u32 console; u32 pad; } Obj;
typedef struct { Obj o; } Device;
typedef struct { Obj o; u32 type; } Queue;
typedef struct { Obj o; u32 type; u32 *w; u32 n, cap; u32 kick; u64 up_base, up, up_end; } List; // kick: words KickoffX ran; up: AllocateUploadMemoryX
typedef struct { Obj o; u8 *rec; u32 bytes; } Pso; // the PSO record, kept for CreateDerivedPipelineStateX
typedef struct { Obj o; u64 completed; } Fence; // `completed` is 8-aligned: the back end writes it
_Static_assert(__builtin_offsetof(Fence, completed) == GPU_FENCE_VALUE, "fence layout");
typedef struct { Obj o; u32 dim, format, height, depth, mips, samples, flags, heap; u64 width, backing, va; u8 desc[56]; } Resource;
typedef struct { Obj o; u32 type, count, flags; u64 storage; } DescHeap;
typedef struct { Obj o; u32 type, cpu_page; u64 size, va; } HeapObj; // va: its memory (console: every heap has some)
typedef struct { Obj o; u32 type, count; } QueryHeap;
typedef struct { Obj o; } Simple;

static Device *the_device[2]; // [console]
static u32 next_id = 1;
static pthread_mutex_t d3d_lock = PTHREAD_MUTEX_INITIALIZER;

// ---- the ring: its writer is rt/gpu_ring.c, shared with the other front ends ----
#include "gpu_ring.h"
#define ring rt_gpu_ring
#define gpu_open rt_gpu_open
#define rec_begin rt_gpu_rec_begin
#define rec_end rt_gpu_rec_end
#define rec_words rt_gpu_rec_words

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
static Obj *obj(u64 p) { Obj *o = (Obj *)GP(p); return p && o->magic == OMAGIC ? o : 0; }
static u32 id_of(u64 p) { Obj *o = obj(p); return o ? o->id : 0; }
#define THIS(T) T *self = (T *)GP(ARG(0))
static int iid_is(u64 riid, const char *s) { u8 g[16]; return riid && parse_guid(s, g) && !memcmp((void *)GP(riid), g, 16); }
static int has_iid(int cls, u64 riid) {
  const D3dLayout *l = layout_of(cls);
  for (unsigned i = 0; i < l->iid_count; i++) if (iid_is(riid, l->iids[i])) return 1;
  return 0;
}
// hands out `o` through (riid, ppv) as QueryInterface does
static u32 give(Obj *o, u64 riid, u64 ppv) {
  if (!ppv) return S_FALSE;
  if (riid && !has_iid((int)o->cls, riid)) { M64(ppv) = 0; return E_NOINTERFACE; }
  M64(ppv) = (uptr)o; return S_OK;
}

// ---- IUnknown, ID3D12Object, ID3D12DeviceChild ----
u64 rt_dxgi_device(u64 d3d_device); // dxgi.c: the device's IDXGIDevice
int rt_d3d12_is_device_iid(u64 riid) { return has_iid(C_DEVICE, riid); }
IMP(d3d12, Object_QueryInterface) {
  Obj *o = obj(ARG(0));
  if (o && has_iid((int)o->cls, ARG(1))) { o->refs++; M64(ARG(2)) = ARG(0); RET(S_OK); return; }
  // IDXGIDevice, IDXGIDevice1..4: the display side of the device (dxgi.c)
  if (o && o->cls == C_DEVICE && (iid_is(ARG(1), "54ec77fa-1377-44e6-8c32-88fd5f44c84c") || iid_is(ARG(1), "77db970f-6276-48ba-ba28-070143b4392c") || iid_is(ARG(1), "05008617-fbfd-4051-a790-144884b4f6a9") || iid_is(ARG(1), "6007896c-3244-4afd-bf18-a6d3beda5023") || iid_is(ARG(1), "95b4f95f-d8da-4ca4-9ee6-3b76d5968a10"))) {
    M64(ARG(2)) = rt_dxgi_device(ARG(0)); RET(S_OK); return; }
  if (rt_trace) { u8 *g = (u8 *)GP(ARG(1)); rt_log("d3d12: %s QueryInterface %02x%02x%02x%02x-...: no", o ? cls_name[o->cls] : "?", g[3], g[2], g[1], g[0]); }
  if (ARG(2)) M64(ARG(2)) = 0;
  RET(E_NOINTERFACE);
}
IMP(d3d12, Object_AddRef) { Obj *o = obj(ARG(0)); RET(o ? ++o->refs : 1); }
IMP(d3d12, Object_Release) {
  Obj *o = obj(ARG(0)); if (!o) { RET(0); return; }
  s32 r = --o->refs;
  if (r == 0 && o->cls == C_RESOURCE) { u32 w = o->id; rec_words(GPU_R_DESTROY, &w, 1); } // its guest backing is kept: the GPU may still read it
  RET(r < 0 ? 0 : r);
}
IMP(d3d12, Object_GetPrivateData) { RET(0x887A0002u); } // DXGI_ERROR_NOT_FOUND
IMP(d3d12, Object_SetPrivateData) { RET(S_OK); }
IMP(d3d12, Object_SetPrivateDataInterface) { RET(S_OK); }
IMP(d3d12, Object_SetName) { RET(S_OK); }
IMP(d3d12, Object_GetDevice) { Obj *o = obj(ARG(0)); Device *d = the_device[o && o->console]; RET(d ? give(&d->o, ARG(1), ARG(2)) : E_INVALIDARG); if (d) d->o.refs++; }

// ---- device creation ----
static Device *device(int console) {
  if (!the_device[console]) {
    the_device[console] = obj_new(C_DEVICE, sizeof(Device), console);
    rt_log("d3d12: %s device created", console ? "console (D3D12.X)" : "PC");
  }
  return the_device[console];
}
IMP(d3d12, D3D12CreateDevice) { // (adapter, minimum feature level, riid, ppv): ppv null asks whether it would work
  if (!ARG(3)) { RET(S_FALSE); return; }
  Device *d = device(0); d->o.refs++; RET(give(&d->o, ARG(2), ARG(3)));
}
IMP(d3d12_xs, D3D12XboxCreateDevice) { // (adapter, D3D12XBOX_CREATE_DEVICE_PARAMETERS*, riid, ppv)
  if (ARG(1)) rt_log("d3d12: D3D12XboxCreateDevice (SDK version 0x%x)", M32(ARG(1)));
  Device *d = device(1); d->o.refs++; RET(give(&d->o, ARG(2), ARG(3)));
}
IMP(d3d12, D3D12GetDebugInterface) { if (ARG(1)) M64(ARG(1)) = 0; RET(E_NOINTERFACE); }
IMP(d3d12_xs, D3D12GetDebugInterface) { if (ARG(1)) M64(ARG(1)) = 0; RET(E_NOINTERFACE); }
// d3d12_xs.dll's D3D12CreateDevice: the console device, as XCL's d3d12_xs hands it out
IMP(d3d12_xs, D3D12CreateDevice) { if (!ARG(3)) { RET(S_FALSE); return; } Device *d = device(1); d->o.refs++; RET(give(&d->o, ARG(2), ARG(3))); }

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
  Obj *d = obj(ARG(0)); Queue *q = obj_new(C_QUEUE, sizeof(Queue), d->console); q->type = M32(ARG(1));
  RET(give(&q->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_CreateCommandAllocator) { Obj *d = obj(ARG(0)); Simple *a = obj_new(C_ALLOCATOR, sizeof(Simple), d->console); RET(give(&a->o, ARG(2), ARG(3))); }
IMP(d3d12, Device_CreateCommandList) { // (node mask, type, allocator, initial PSO, riid, ppv)
  Obj *d = obj(ARG(0)); List *l = obj_new(C_LIST, sizeof(List), d->console); l->type = (u32)ARG(2);
  if (ARG(4)) { u32 w[2] = {GPU_L_PSO | 1u << 16, id_of(ARG(4))}; l->cap = 64; l->w = malloc(4 * l->cap); memcpy(l->w, w, 8); l->n = 2; }
  RET(give(&l->o, ARG(5), ARG(6)));
}
// console: CreateCommandQueueX #48 (desc: type first, riid, ppv), CreateCommandListX #73 (desc: type first,
// allocator, PSO, riid, ppv; a DMA list, type 0x10 or its own IID, has the short copy-list vtable)
IMP(d3d12, Device_CreateCommandQueueX) { imp_d3d12_Device_CreateCommandQueue(c); }
IMP(d3d12, Device_CreateCommandListX) {
  Obj *d = obj(ARG(0)); u32 type = M32(ARG(1));
  int dma = type == 0x10 || iid_is(ARG(4), "30bc77b9-8379-4b25-95de-b8588c42275d");
  List *l = obj_new(C_LIST, sizeof(List), dma ? 2 : d->console ? 1 : 0); l->type = type;
  if (ARG(3)) { u32 w[2] = {GPU_L_PSO | 1u << 16, id_of(ARG(3))}; l->cap = 64; l->w = malloc(4 * l->cap); memcpy(l->w, w, 8); l->n = 2; }
  RET(give(&l->o, dma ? 0 : ARG(4), ARG(5)));
}
IMP(d3d12, Device_CreateCommandList1) { // (node mask, type, flags, riid, ppv): created closed
  Obj *d = obj(ARG(0)); List *l = obj_new(C_LIST, sizeof(List), d->console); l->type = (u32)ARG(2);
  RET(give(&l->o, ARG(4), ARG(5)));
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
  Obj *d = obj(ARG(0)); DescHeap *h = obj_new(C_HEAP, sizeof(DescHeap), d->console);
  h->type = M32(ARG(1)); h->count = M32(ARG(1) + 4); h->flags = M32(ARG(1) + 8);
  u64 bytes = (u64)(h->count ? h->count : 1) * GPU_DESC_BYTES;
  if (d->console) h->storage = rt_gfx_alloc(bytes, 65536, 0); // in the GPU-visible region's window 0, as on the console
  else { h->storage = (uptr)aligned_alloc(64, (size_t)bytes); memset((void *)GP(h->storage), 0, (size_t)bytes); }
  RET(give(&h->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_GetDescriptorHandleIncrementSize) { RET(GPU_DESC_BYTES); }
IMP(d3d12, Device_CreateRootSignature) { // (node, blob, size, riid, ppv)
  Obj *d = obj(ARG(0)); Simple *r = obj_new(C_ROOTSIG, sizeof(Simple), d->console);
  u32 n = (u32)ARG(3); u8 *p = rec_begin(GPU_R_ROOTSIG, 8 + n);
  ((u32 *)p)[0] = r->o.id; ((u32 *)p)[1] = n; memcpy(p + 8, (void *)GP(ARG(2)), n); rec_end();
  RET(give(&r->o, ARG(4), ARG(5)));
}
// views: 8 words into the descriptor at `handle`
static Resource *res(u64 p) { Obj *o = obj(p); return o && o->cls == C_RESOURCE ? (Resource *)o : 0; }
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
static void footprints(const u8 *desc, u32 first, u32 num, u64 base, u64 layouts, u64 rows, u64 sizes, u64 total) {
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
IMP(d3d12, Device_GetCopyableFootprints) { footprints((u8 *)GP(ARG(1)), (u32)ARG(2), (u32)ARG(3), ARG(4), ARG(5), ARG(6), ARG(7), ARG(8)); }
// every subresource: a 3D texture's depth is not slices
static u64 alloc_info(const u8 *desc) { u64 t = 0; u16 mips = *(u16 *)(desc + 30); footprints(desc, 0, (*(u32 *)desc == 4 ? 1 : *(u16 *)(desc + 28)) * (mips ? mips : 1), 0, 0, 0, 0, (uptr)&t); return (t + 65535) & ~65535ull; }
IMP(d3d12, Device_GetResourceAllocationInfo) { // hidden return pointer (PC), then (visible mask, count, descs)
  u64 ret = ARG(1); u64 n = ARG(3), descs = ARG(4), size = 0;
  for (u64 i = 0; i < n; i++) size += alloc_info((u8 *)GP(descs + 56 * i));
  M64(ret) = size; M64(ret + 8) = 65536; RET(ret);
}
static u64 va_next = 0x200000000ull; // GPU addresses of PC buffers without guest backing: above every guest address
// heap: D3D12_HEAP_TYPE (4 custom: cpu_page says whether the CPU sees it); at: where its memory is (a placed
// resource: title memory or a heap's), else 0.
// PC device: upload / readback buffers have guest backing, the others live on the GPU only.
// Console device (UMA): every buffer is guest memory in the GPU-visible region, its GPU address its CPU one;
// upload and readback buffers up to 64 MiB go to the coherent window 3 (XCL's rule), the rest to window 0.
// A texture placed at title memory (CreatePlacedResourceX) has its texels there.
static u32 create_resource(Obj *dev, u32 heap, u32 cpu_page, u64 desc, u64 at, u64 riid, u64 ppv) {
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
  return give(&r->o, riid, ppv);
}
IMP(d3d12, Device_CreateCommittedResource) { // (heap properties, heap flags, desc, state, clear value, riid, ppv)
  u64 hp = ARG(1); RET(create_resource(obj(ARG(0)), hp ? M32(hp) : 1, hp ? M32(hp + 4) : 0, ARG(3), 0, ARG(6), ARG(7)));
}
IMP(d3d12, Device_CreatePlacedResource) { // (heap, offset, desc, state, clear, riid, ppv)
  Obj *ho = obj(ARG(1)); HeapObj *h = ho && ho->cls == C_HEAPOBJ ? (HeapObj *)ho : 0;
  // console: a buffer placed in a heap is the heap's memory at that offset (aliasing included); textures stay
  // the GPU's own. PC: a resource of its own.
  u64 at = h && h->va && M32(ARG(3)) == 1 ? h->va + ARG(2) : 0;
  RET(create_resource(obj(ARG(0)), h ? h->type : 1, h ? h->cpu_page : 0, ARG(3), at, ARG(6), ARG(7)));
}
IMP(d3d12, Device_CreatePlacedResourceX) { // #44 (title address, desc, state, clear, riid, ppv): the resource is that memory
  RET(create_resource(obj(ARG(0)), 1, 0, ARG(2), ARG(1), ARG(5), ARG(6)));
}
IMP(d3d12, Device_CreateHeap) { // ({size, properties {type, cpu page, pool, ...}, alignment, flags}, riid, ppv)
  Obj *d = obj(ARG(0)); HeapObj *h = obj_new(C_HEAPOBJ, sizeof(HeapObj), d->console); u64 desc = ARG(1);
  h->size = M64(desc); h->type = M32(desc + 8); h->cpu_page = M32(desc + 12);
  if (d->console) { h->va = rt_gfx_alloc(h->size, 65536, (h->type == 2 || h->type == 3) && h->size <= (64ull << 20) ? 3 : 0); if (!h->va) { RET(E_OUTOFMEMORY); return; } }
  RET(give(&h->o, ARG(2), ARG(3)));
}
IMP(d3d12, Device_CreateQueryHeap) { // ({type, count, node}, riid, ppv)
  Obj *d = obj(ARG(0)); QueryHeap *q = obj_new(C_QUERYHEAP, sizeof(QueryHeap), d->console);
  q->type = M32(ARG(1)); q->count = M32(ARG(1) + 4); RET(give(&q->o, ARG(2), ARG(3)));
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
  Obj *d = obj(ARG(0)); Fence *f = obj_new(C_FENCE, sizeof(Fence), d->console); f->completed = ARG(1);
  RET(give(&f->o, ARG(3), ARG(4)));
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
  u32 *h = (u32 *)rec; h[0] = p->o.id; h[1] = id_of(M64(desc)); h[2] = 0; h[3] = ne;
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
  return give(&p->o, riid, ppv);
}
IMP(d3d12, Device_CreateGraphicsPipelineState) { RET(graphics_pso(obj(ARG(0)), ARG(1), ARG(2), ARG(3))); } // (desc, riid, ppv)
IMP(d3d12, Device_CreateGraphicsPipelineStateX) { RET(graphics_pso(obj(ARG(0)), ARG(1), ARG(4), ARG(5))); } // #60 (desc, flags, ?, riid, ppv)
// #55 CreateDerivedPipelineStateX(base, count, patches, riid, ppv): the base's description with parts replaced.
// A patch is {u32 part; payload} every 0x14C bytes; parts (XCL's findings): 0 depth-stencil, 1 rasterizer,
// 2 blend, 3 sample mask; 5 is seen on retail titles and not understood (ignored); others fail.
IMP(d3d12, Device_CreateDerivedPipelineStateX) {
  Obj *d = obj(ARG(0)), *bo = obj(ARG(1)); u32 n = (u32)ARG(2); u64 patches = ARG(3);
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
  RET(give(&p->o, ARG(4), ARG(5)));
}
IMP(d3d12, Device_CreateComputePipelineState) { // ({root signature, CS {ptr, size}, node, cached, flags}, riid, ppv)
  Obj *d = obj(ARG(0)); u64 desc = ARG(1);
  Pso *p = obj_new(C_PSO, sizeof(Pso), d->console);
  u64 n = M64(desc + 16);
  u8 *rec = rec_begin(GPU_R_PSO, (u32)(16 + 56 + ((n + 7) & ~7ull)));
  u32 *h = (u32 *)rec; h[0] = p->o.id; h[1] = id_of(M64(desc)); h[2] = 1; h[3] = 0;
  memcpy(rec + 16, (void *)GP(desc), 56);
  u32 at = 16 + 56; *(u64 *)(rec + 16 + 8) = blob_put(rec, &at, M64(desc + 8), n); *(u64 *)(rec + 16) = 0;
  rec_end();
  RET(give(&p->o, ARG(2), ARG(3)));
}

// ---- the console's frame clock (device #78-#80): origins every interval, tokens count them ----
static u64 frame_interval, frame_base, frame_last;
static int pace_off = -1;
IMP(d3d12, Device_SetFrameIntervalX) { // (output, interval µs, periodicity, flags)
  frame_interval = (u32)ARG(2); if (!frame_base) frame_base = (u64)host_now_us();
  rt_log("d3d12: frame interval %llu µs", frame_interval); RET(S_OK);
}
IMP(d3d12, Device_ScheduleFrameEventX) { RET(frame_interval ? S_OK : E_INVALIDARG); } // (type, offset µs, ancillary, flags)
IMP(d3d12, Device_WaitFrameEventX) { // (type, timeout ms, ancillary, flags, &token)
  if (!frame_interval) { RET(E_INVALIDARG); return; }
  if (pace_off < 0) { const char *e = getenv("SW_PACE"); pace_off = e && !strcmp(e, "off"); }
  u64 k;
  if (pace_off) k = ++frame_last;
  else {
    u64 now = (u64)host_now_us(); k = (now - frame_base) / frame_interval + 1;
    if (k <= frame_last) k = frame_last + 1;
    u64 due = frame_base + k * frame_interval;
    if (due > now) { struct timespec ts = {(time_t)((due - now) / 1000000), (long)((due - now) % 1000000) * 1000}; nanosleep(&ts, 0); }
    frame_last = k;
  }
  u64 tp = ARG(5); if (tp) M64(tp) = k;
  RET(S_OK);
}
IMP(d3d12, Device_GetFrameStatisticsX) { // (token or 0, flags, &count, statistics[]): the frames presented, newest first
  u64 out = ARG(4); u32 n = ARG(3) ? M32(ARG(3)) : 0;
  if (n && out) { memset((void *)GP(out), 0, 0x40); M64(out) = ARG(1) ? ARG(1) : frame_last; M64(out + 8) = (u64)host_now_us(); }
  if (ARG(3)) M32(ARG(3)) = n ? 1 : 0;
  RET(S_OK);
}

// ---- queue ----
// a list's words go out whole; those a KickoffX already ran come first under a REPLAY word (state only)
static void execute_words(u32 queue, List *l) {
  u32 extra = l->kick ? 2 : 0;
  u8 *p = rec_begin(GPU_R_EXECUTE, 4 + 4 * (extra + l->n));
  *(u32 *)p = queue;
  if (extra) { ((u32 *)(p + 4))[0] = GPU_L_REPLAY | 1u << 16; ((u32 *)(p + 4))[1] = l->kick; }
  memcpy(p + 4 + 4 * extra, l->w, 4ull * l->n);
  rec_end();
}
static void execute(Queue *q, List *l) { execute_words(q->o.id, l); }
IMP(d3d12, CommandQueue_ExecuteCommandLists) { // (count, lists)
  THIS(Queue);
  for (u32 i = 0; i < (u32)ARG(1); i++) { Obj *o = obj(M64(ARG(2) + 8 * i)); if (o && o->cls == C_LIST) execute(self, (List *)o); }
  __atomic_fetch_add(&ring[GPU_H_SUBMITTED], 1, __ATOMIC_RELAXED);
}
static void signal_rec(u32 op, u64 fence, u64 value) { u32 w[3] = {(u32)fence, (u32)value, (u32)(value >> 32)}; rec_words(op, w, 3); }
IMP(d3d12, CommandQueue_Signal) { Obj *f = obj(ARG(1)); if (!f || f->cls != C_FENCE) { RET(E_INVALIDARG); return; } signal_rec(GPU_R_SIGNAL, ARG(1), ARG(2)); RET(S_OK); }
IMP(d3d12, CommandQueue_SignalX) { imp_d3d12_CommandQueue_Signal(c); }
IMP(d3d12, CommandQueue_Wait) { Obj *f = obj(ARG(1)); if (!f || f->cls != C_FENCE) { RET(E_INVALIDARG); return; } signal_rec(GPU_R_WAIT, ARG(1), ARG(2)); RET(S_OK); }
IMP(d3d12, CommandQueue_GetTimestampFrequency) { M64(ARG(1)) = 1000000; RET(S_OK); }
IMP(d3d12, CommandQueue_GetClockCalibration) { u64 t = (u64)host_now_us(); if (ARG(1)) M64(ARG(1)) = t; if (ARG(2)) M64(ARG(2)) = t; RET(S_OK); }
IMP(d3d12, CommandQueue_GetDesc) { u64 ret = ARG(1); THIS(Queue); M32(ret) = self->type; M32(ret + 4) = 0; M32(ret + 8) = 0; M32(ret + 12) = 0; RET(ret); }
IMP(d3d12, CommandQueue_SetMarker) {}
IMP(d3d12, CommandQueue_BeginEvent) {}
IMP(d3d12, CommandQueue_EndEvent) {}
IMP(d3d12, CommandQueue_PresentX) { // (plane count, planes, parameters); a plane: token, resource count, resources, ...
  u64 planes = ARG(2);
  if (!ARG(1) || !planes || !M32(planes + 8)) { RET(S_OK); return; }
  rt_gpu_present(id_of(M64(M64(planes + 0x10))), M64(planes), 1);
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
void rt_fence_signal_cpu(u64 fence, u64 value) { Obj *o = obj(fence); if (!o || o->cls != C_FENCE) return; __atomic_store_n(&((Fence *)o)->completed, value, __ATOMIC_RELEASE); epoch_bump(); } // DirectStorage
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
static u32 *emit(List *l, u32 op, u32 n) {
  if (l->n + n + 1 > l->cap) { l->cap = (l->n + n + 1) * 2 + 64; l->w = realloc(l->w, 4ull * l->cap); }
  u32 *p = l->w + l->n; p[0] = op | n << 16; l->n += n + 1;
  return p + 1;
}
#define LIST List *self = (List *)GP(ARG(0))
IMP(d3d12, CommandList_GetType) { LIST; RET(self->type); }
IMP(d3d12, CommandList_Close) { RET(S_OK); }
IMP(d3d12, CommandList_Reset) { LIST; self->n = 0; self->kick = 0; self->up = self->up_base; if (ARG(2)) emit(self, GPU_L_PSO, 1)[0] = id_of(ARG(2)); RET(S_OK); }
IMP(d3d12, CommandList_ClearState) { LIST; if (ARG(1)) emit(self, GPU_L_PSO, 1)[0] = id_of(ARG(1)); }
IMP(d3d12, CommandList_DrawInstanced) { LIST; u32 *p = emit(self, GPU_L_DRAW, 4); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); }
IMP(d3d12, CommandList_DrawIndexedInstanced) { LIST; u32 *p = emit(self, GPU_L_DRAW_INDEXED, 5); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); p[4] = (u32)ARG(5); }
IMP(d3d12, CommandList_Dispatch) { LIST; u32 *p = emit(self, GPU_L_DISPATCH, 3); p[0] = (u32)ARG(1); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); }
IMP(d3d12, CommandList_CopyBufferRegion) { // (dst, dst offset, src, src offset, bytes)
  LIST; u32 *p = emit(self, GPU_L_COPY_BUFFER, 8); u64 a = ARG(2), b = ARG(4), n = ARG(5);
  p[0] = id_of(ARG(1)); p[1] = (u32)a; p[2] = (u32)(a >> 32); p[3] = id_of(ARG(3)); p[4] = (u32)b; p[5] = (u32)(b >> 32); p[6] = (u32)n; p[7] = (u32)(n >> 32);
}
static void put_location(u32 *p, u64 loc) { // resource id, type, subresource, footprint offset lo hi, format, width, height, depth, row pitch
  p[0] = id_of(M64(loc)); p[1] = M32(loc + 8);
  if (p[1] == 0) { p[2] = M32(loc + 16); return; }
  u64 off = M64(loc + 16); p[3] = (u32)off; p[4] = (u32)(off >> 32);
  for (int i = 0; i < 5; i++) p[5 + i] = M32(loc + 24 + 4 * i);
}
IMP(d3d12, CommandList_CopyTextureRegion) { // (dst location, x, y, z, src location, box)
  LIST; u32 *p = emit(self, GPU_L_COPY_TEXTURE, 30); memset(p, 0, 4 * 30); // (a list's words are not zeroed: no box must say so)
  put_location(p, ARG(1)); p[10] = (u32)ARG(2); p[11] = (u32)ARG(3); p[12] = (u32)ARG(4);
  put_location(p + 13, ARG(5));
  u64 box = ARG(6);
  if (box) { p[23] = 1; for (int i = 0; i < 6; i++) p[24 + i] = M32(box + 4 * i); }
}
IMP(d3d12, CommandList_CopyResource) { LIST; u32 *p = emit(self, GPU_L_COPY_RESOURCE, 2); p[0] = id_of(ARG(1)); p[1] = id_of(ARG(2)); }
IMP(d3d12, CommandList_ResolveSubresource) { LIST; u32 *p = emit(self, GPU_L_COPY_RESOURCE, 2); p[0] = id_of(ARG(1)); p[1] = id_of(ARG(3)); }
IMP(d3d12, CommandList_IASetPrimitiveTopology) { LIST; emit(self, GPU_L_TOPOLOGY, 1)[0] = (u32)ARG(1); }
IMP(d3d12, CommandList_RSSetViewports) { LIST; u32 n = (u32)ARG(1); u32 *p = emit(self, GPU_L_VIEWPORTS, 1 + 6 * n); p[0] = n; memcpy(p + 1, (void *)GP(ARG(2)), 24ull * n); }
IMP(d3d12, CommandList_RSSetScissorRects) { LIST; u32 n = (u32)ARG(1); u32 *p = emit(self, GPU_L_SCISSORS, 1 + 4 * n); p[0] = n; memcpy(p + 1, (void *)GP(ARG(2)), 16ull * n); }
IMP(d3d12, CommandList_OMSetBlendFactor) { LIST; u32 *p = emit(self, GPU_L_BLEND_FACTOR, 4); if (ARG(1)) memcpy(p, (void *)GP(ARG(1)), 16); else { float one = 1; for (int i = 0; i < 4; i++) memcpy(p + i, &one, 4); } }
IMP(d3d12, CommandList_OMSetStencilRef) { LIST; emit(self, GPU_L_STENCIL_REF, 1)[0] = (u32)ARG(1); }
IMP(d3d12, CommandList_SetPipelineState) { LIST; emit(self, GPU_L_PSO, 1)[0] = id_of(ARG(1)); }
IMP(d3d12, CommandList_ResourceBarrier) {}
IMP(d3d12, CommandList_SetDescriptorHeaps) {} // descriptor tables are addresses in the heaps' storage
static void root_sig(Cpu *c, u32 bp) { LIST; u32 *p = emit(self, GPU_L_ROOTSIG, 2); p[0] = bp; p[1] = id_of(ARG(1)); }
static void table(Cpu *c, u32 bp) { LIST; u32 *p = emit(self, GPU_L_TABLE, 4); u64 h = ARG(2); p[0] = bp; p[1] = (u32)ARG(1); p[2] = (u32)h; p[3] = (u32)(h >> 32); }
static void constants(Cpu *c, u32 bp, u32 n, u64 data, u32 offset) { LIST; u32 *p = emit(self, GPU_L_CONSTANTS, 4 + n); p[0] = bp; p[1] = (u32)ARG(1); p[2] = offset; p[3] = n; if (data) memcpy(p + 4, (void *)GP(data), 4ull * n); }
static void root_view(Cpu *c, u32 bp, u32 kind) { LIST; u32 *p = emit(self, GPU_L_ROOT_VIEW, 5); u64 va = ARG(2); p[0] = bp; p[1] = (u32)ARG(1); p[2] = kind; p[3] = (u32)va; p[4] = (u32)(va >> 32); }
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
  LIST; u32 *p = emit(self, GPU_L_IB, 4); u64 v = ARG(1);
  if (v) { u64 va = M64(v); p[0] = (u32)va; p[1] = (u32)(va >> 32); p[2] = M32(v + 8); p[3] = M32(v + 12); } else memset(p, 0, 16);
}
IMP(d3d12, CommandList_IASetVertexBuffers) { // (start, count, views {va, size, stride})
  LIST; u32 n = (u32)ARG(2); u32 *p = emit(self, GPU_L_VB, 2 + 4 * n); p[0] = (u32)ARG(1); p[1] = n;
  for (u32 i = 0; i < n; i++) { u64 v = ARG(3) + 16ull * i; if (ARG(3)) { u64 va = M64(v); p[2 + 4 * i] = (u32)va; p[3 + 4 * i] = (u32)(va >> 32); p[4 + 4 * i] = M32(v + 8); p[5 + 4 * i] = M32(v + 12); } }
}
// render targets and clears carry copies of their descriptors: the heap may change before the GPU runs
static void copy_desc(u32 *dst, u64 handle) { if (handle) memcpy(dst, (void *)GP(handle), GPU_DESC_BYTES); else memset(dst, 0, GPU_DESC_BYTES); }
IMP(d3d12, CommandList_OMSetRenderTargets) { // (count, handles, single range, dsv handle*)
  LIST; u32 n = (u32)ARG(1); u64 hs = ARG(2); int single = (int)(u32)ARG(3); u64 dsv = ARG(4);
  u32 *p = emit(self, GPU_L_SET_RTS, 1 + 8 * (n + 1)); p[0] = n;
  for (u32 i = 0; i < n; i++) copy_desc(p + 1 + 8 * i, single ? M64(hs) + GPU_DESC_BYTES * i : M64(hs + 8 * i));
  copy_desc(p + 1 + 8 * n, dsv ? M64(dsv) : 0);
}
IMP(d3d12, CommandList_ClearRenderTargetView) { // (handle, color[4], rect count, rects)
  LIST; u32 *p = emit(self, GPU_L_CLEAR_RTV, 12); copy_desc(p, ARG(1)); memcpy(p + 8, (void *)GP(ARG(2)), 16);
}
IMP(d3d12, CommandList_ClearDepthStencilView) { // (handle, flags, depth (xmm3), stencil, rect count, rects)
  LIST; u32 *p = emit(self, GPU_L_CLEAR_DSV, 11); copy_desc(p, ARG(1)); p[8] = (u32)ARG(2); { float dz = X[3].ps[0]; memcpy(p + 9, &dz, 4); } p[10] = (u8)ARG(4);
}
IMP(d3d12, CommandList_DiscardResource) {}
IMP(d3d12, CommandList_SetMarker) {}
IMP(d3d12, CommandList_BeginEvent) {}
IMP(d3d12, CommandList_EndEvent) {}
static void query(Cpu *c, u32 op) { LIST; u32 *p = emit(self, op, 3); p[0] = id_of(ARG(1)); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); }
IMP(d3d12, CommandList_BeginQuery) { query(c, GPU_L_QUERY_BEGIN); } // (heap, type, index)
IMP(d3d12, CommandList_EndQuery) { query(c, GPU_L_QUERY_END); }
IMP(d3d12, CommandList_ResolveQueryData) { // (heap, type, first, count, destination, offset)
  LIST; u32 *p = emit(self, GPU_L_QUERY_RESOLVE, 7); u64 off = ARG(6);
  p[0] = id_of(ARG(1)); p[1] = (u32)ARG(2); p[2] = (u32)ARG(3); p[3] = (u32)ARG(4); p[4] = id_of(ARG(5)); p[5] = (u32)off; p[6] = (u32)(off >> 32);
}

// ---- the console list's own methods (XCL's slots): memory as the GPU sees it, in order with the list's work ----
static void mem_write(Cpu *c, u64 addr, u64 v, u32 bytes, u32 stamp) { LIST; u32 *p = emit(self, GPU_L_MEM_WRITE, 6); p[0] = (u32)addr; p[1] = (u32)(addr >> 32); p[2] = (u32)v; p[3] = (u32)(v >> 32); p[4] = bytes; p[5] = stamp; }
IMP(d3d12, CommandList_Write32BitValueBottomOfPipeX) { mem_write(c, ARG(1), (u32)ARG(2), 4, 0); } // #69 (address, value, flags, ?)
IMP(d3d12, CommandList_Write64BitValueBottomOfPipeX) { mem_write(c, ARG(1), ARG(2), 8, ARG(4) == 1); } // #70 (address, value, flags, mode: 1 = a GPU timestamp)
IMP(d3d12, CommandList_Write64BitValueTopOfPipeX) { mem_write(c, ARG(1), ARG(2), 8, 0); } // #89 (address, value, flags)
IMP(d3d12, CommandList_FillMemoryWith32BitValueX) { // #61 (address, bytes, value)
  LIST; u32 *p = emit(self, GPU_L_MEM_FILL, 5); u64 a = ARG(1), n = ARG(2); p[0] = (u32)a; p[1] = (u32)(a >> 32); p[2] = (u32)n; p[3] = (u32)(n >> 32); p[4] = (u32)ARG(3);
}
IMP(d3d12, CommandList_CopyMemoryToMemoryX) { // #60 (destination, source, bytes, flags)
  LIST; u32 *p = emit(self, GPU_L_MEM_COPY, 6); u64 d = ARG(1), s = ARG(2), n = ARG(3);
  p[0] = (u32)d; p[1] = (u32)(d >> 32); p[2] = (u32)s; p[3] = (u32)(s >> 32); p[4] = (u32)n; p[5] = (u32)(n >> 32);
}
static void mem_wait(Cpu *c, u64 addr, u32 func, u64 ref, u64 mask, u32 bytes) {
  LIST; u32 *p = emit(self, GPU_L_MEM_WAIT, 8);
  p[0] = (u32)addr; p[1] = (u32)(addr >> 32); p[2] = func; p[3] = (u32)ref; p[4] = (u32)(ref >> 32); p[5] = (u32)mask; p[6] = (u32)(mask >> 32); p[7] = bytes;
}
IMP(d3d12, CommandList_Wait32BitValueX) { mem_wait(c, ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), 4); } // #71 (address, comparison, reference, mask, flags)
IMP(d3d12, CommandList_Wait64BitValueX) { mem_wait(c, ARG(1), (u32)ARG(2), ARG(3), ARG(4), 8); }        // #72
IMP(d3d12, CommandList_DrawIndexedX) { // #81 (index address, format, index count, ?): indices from memory; the bound buffer stays
  LIST; u32 *p = emit(self, GPU_L_DRAW_INDEXED_X, 4); u64 a = ARG(1); p[0] = (u32)a; p[1] = (u32)(a >> 32); p[2] = (u32)ARG(2); p[3] = (u32)ARG(3);
}
IMP(d3d12, CommandList_RSSetDepthBiasX) { // #94 (bias, clamp, slope) in xmm1..3
  LIST; u32 *p = emit(self, GPU_L_DEPTH_BIAS, 3); float v[3] = {X[1].ps[0], X[2].ps[0], X[3].ps[0]}; memcpy(p, v, 12);
}
// #93: the work recorded so far goes to the GPU now; recording goes on in the same list
IMP(d3d12, CommandList_KickoffX) { LIST; execute_words(0, self); self->kick = self->n; __atomic_fetch_add(&ring[GPU_H_SUBMITTED], 1, __ATOMIC_RELAXED); }
IMP(d3d12, CommandList_UavBarrierX) {} // #146: work runs in order here
IMP(d3d12, CommandList_AllocateUploadMemoryX) { // #148 (bytes, alignment): GPU-visible memory the list's work may read, until its Reset
  LIST; u64 n = ARG(1), al = ARG(2) ? ARG(2) : 256;
  u64 at = (self->up + al - 1) & ~(al - 1);
  if (!self->up_base || at + n > self->up_end) { // a new chunk (the old one may still be read by work in flight)
    u64 sz = n + al > (1u << 20) ? n + al : 1u << 20; self->up_base = self->up = rt_gfx_alloc(sz, 65536, 3); self->up_end = self->up + sz; at = (self->up + al - 1) & ~(al - 1); }
  self->up = at + n; RET(at);
}
IMP(d3d12, CommandList_SetPredication) {}

// ---- xg_xs.dll: the console's texture layout library. Our layout is the one the GPU back end reads texels
// in title memory with: subresources one after another as GetCopyableFootprints places them (rows 256-byte
// aligned). It is not the console's tiling: textures the title lays out through XG at run time come out
// right; texels tiled ahead of time by Microsoft's tools would not. ----
typedef struct { u64 vtbl; u32 magic; s32 refs; u8 desc[56]; } XgComputer;
#define XGMAGIC 0x58474343u
IMP(xg_xs, XGCreateTextureComputer) { // (desc, &computer)
  static const char *const names[] = {"AddRef", "Release", 0, "GetResourceSizeBytes", "GetResourceSizeAlignment", "GetMipLevelOffsetBytes", 0, 0, "CopyIntoSubresource", "CopyFromSubresource", 0, 0};
  static u64 vt; if (!vt) vt = rt_com_vtable("xg_xs", "Computer_", names, 12);
  if (!ARG(0) || !ARG(1)) { RET(0x80004003u); return; }
  XgComputer *x = calloc(1, sizeof *x); x->vtbl = vt; x->magic = XGMAGIC; x->refs = 1; memcpy(x->desc, (void *)GP(ARG(0)), 56);
  M64(ARG(1)) = (uptr)x; RET(S_OK);
}
IMP(xg_xs, Computer_AddRef) { XgComputer *x = (XgComputer *)GP(ARG(0)); RET(++x->refs); }
IMP(xg_xs, Computer_Release) { XgComputer *x = (XgComputer *)GP(ARG(0)); s32 r = --x->refs; if (!r) { x->magic = 0; free(x); } RET(r < 0 ? 0 : r); }
IMP(xg_xs, Computer_GetResourceSizeBytes) { RET(alloc_info(((XgComputer *)GP(ARG(0)))->desc)); }
IMP(xg_xs, Computer_GetResourceSizeAlignment) { RET(65536); }
static u32 subresources(const u8 *desc) { u16 mips = *(u16 *)(desc + 30); return (*(u32 *)desc == 4 ? 1 : *(u16 *)(desc + 28)) * (mips ? mips : 1); }
IMP(xg_xs, Computer_GetMipLevelOffsetBytes) { // (plane, mip): of the first slice
  XgComputer *x = (XgComputer *)GP(ARG(0)); u8 fp[32]; u32 mip = (u32)ARG(2);
  if (mip >= subresources(x->desc)) { RET(0); return; }
  footprints(x->desc, mip, 1, 0, (uptr)fp, 0, 0, 0);
  u64 off; memcpy(&off, fp, 8); RET(off);
}
// (base address, plane, subresource, rows, row pitch, slice pitch): `rows` is the source (into) or destination (from)
static void xg_copy(Cpu *c, int into) {
  XgComputer *x = (XgComputer *)GP(ARG(0)); u64 base = ARG(1); u32 sub = (u32)ARG(3); u64 other = ARG(4); u32 pitch = (u32)ARG(5), slice = (u32)ARG(6);
  if (sub >= subresources(x->desc)) { RET(E_INVALIDARG); return; }
  u8 fp[32]; u32 rows; u64 row;
  // the subresource's place in the whole layout: footprints from 0 up to it
  u64 total = (u64)subresources(x->desc); u8 *all = malloc(32 * total); u32 *nrows = malloc(4 * total); u64 *sizes = malloc(8 * total);
  footprints(x->desc, 0, (u32)total, 0, (uptr)all, (uptr)nrows, (uptr)sizes, 0);
  memcpy(fp, all + 32 * sub, 32); rows = nrows[sub]; row = sizes[sub]; free(all); free(nrows); free(sizes);
  u64 off; memcpy(&off, fp, 8); u32 depth, rp; memcpy(&depth, fp + 20, 4); memcpy(&rp, fp + 24, 4);
  for (u32 z = 0; z < depth; z++) for (u32 y = 0; y < rows; y++) {
    u8 *t = (u8 *)GP(base + off + (u64)rp * (rows * z + y)), *o = (u8 *)GP(other + (u64)slice * z + (u64)pitch * y);
    if (into) memcpy(t, o, (size_t)row); else memcpy(o, t, (size_t)row);
  }
  RET(S_OK);
}
IMP(xg_xs, Computer_CopyIntoSubresource) { xg_copy(c, 1); }
IMP(xg_xs, Computer_CopyFromSubresource) { xg_copy(c, 0); }

// ---- d3dcompiler_47.dll: no compiler runs here. D3DCompile looks the source up among shaders compiled ahead
// of time by tools/hlsl-prep.mjs (<title folder>/shaders/hlsl/<key>.cso, DXIL from dxc), keyed by a hash of
// the source, entry point and target. A miss is logged with the source, for hlsl-prep to pick up. ----
static u64 fnv64(u64 h, const void *p, size_t n) { const u8 *b = p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; } return h; }
static void b64(const u8 *p, size_t n, char *out) {
  static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    u32 v = (u32)p[i] << 16 | (i + 1 < n ? (u32)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
    out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63]; out[o++] = i + 1 < n ? t[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? t[v & 63] : '=';
  }
  out[o] = 0;
}
IMP(d3dcompiler_47, D3DCompile) { // (source, size, name, defines, include, entry, target, flags1, flags2, &code, &errors)
  u64 src = ARG(0), n = ARG(1); const char *entry = ARG(5) ? GSTR(ARG(5)) : "main", *target = ARG(6) ? GSTR(ARG(6)) : "";
  u64 h = fnv64(0xcbf29ce484222325ull, (void *)GP(src), (size_t)n); h = fnv64(h, "", 1); h = fnv64(h, entry, strlen(entry) + 1); h = fnv64(h, target, strlen(target));
  char name[64]; snprintf(name, sizeof name, "shaders/hlsl/%016llx.cso", (unsigned long long)h);
  u8 *buf = malloc(1 << 20); u32 got = host_read_file(name, (u32)strlen(name), 0, 1 << 20, (uptr)buf);
  u64 code = ARG(9), errors = ARG(10);
  if (errors) M64(errors) = 0;
  if (got) { if (code) M64(code) = rt_blob_new(buf, got); free(buf); RET(S_OK); return; }
  free(buf);
  char *enc = malloc((size_t)n * 4 / 3 + 8); b64((u8 *)GP(src), (size_t)n, enc);
  rt_log("d3dcompile: no precompiled shader %016llx for %s %s: run tools/hlsl-prep.mjs on this log", (unsigned long long)h, target, entry);
  rt_log("d3dcompile-source %016llx %s %s %s", (unsigned long long)h, target, entry, enc);
  free(enc);
  if (code) M64(code) = 0;
  if (errors) { const char *m = "pisco: this shader was not compiled ahead of time (tools/hlsl-prep.mjs)"; M64(errors) = rt_blob_new(m, strlen(m) + 1); }
  RET(0x80004005u);
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
    u64 out = 0; create_resource(&device(s->console)->o, 1, 0, (uptr)desc, 0, 0, (uptr)&out);
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
  Obj *q = obj(queue);
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
IMP(dxgi, SwapChain_GetBuffer) { SC; u32 i = (u32)ARG(1); if (i >= self->count) { RET(E_INVALIDARG); return; } Obj *b = obj(self->buffers[i]); b->refs++; RET(give(b, ARG(2), ARG(3))); }
// (sync interval, flags): with an interval, the present waits for the next 60 Hz vertical blank (SW_PACE=off: none)
IMP(dxgi, SwapChain_Present) {
  SC; u32 interval = (u32)ARG(1);
  rt_gpu_present(id_of(self->buffers[self->index]), ++self->frames, interval); self->index = (self->index + 1) % self->count;
  if (pace_off < 0) { const char *e = getenv("SW_PACE"); pace_off = e && !strcmp(e, "off"); }
  if (interval && !pace_off) {
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
  for (u32 i = 0; i < 16; i++) if (self->buffers[i]) { Obj *b = obj(self->buffers[i]); if (b && --b->refs <= 0) { u32 id = b->id; rec_words(GPU_R_DESTROY, &id, 1); } self->buffers[i] = 0; }
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
IMP(dxgi, SwapChain_GetDevice) { SC; Device *d = device(self->console); d->o.refs++; RET(give(&d->o, ARG(1), ARG(2))); }
