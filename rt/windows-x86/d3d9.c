// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9 for Windows desktop apps: the Direct3D object (the adapter and what it supports), the device and
// its swap chain, reference counting, and the statistics. Resources are in d3d9_resources.c, the device's state
// and draws in d3d9_state.c, state blocks in d3d9_stateblock.c; what reaches the GPU goes through d3d9_gpu.c.
// The adapter is the oracle's: its identity, modes, format support and capabilities are what Wine answered on
// the machine the reference session was recorded on (d3d9_oracle.gen.h), so an app takes the same decisions.
// Present shows the back buffer (d3d9_gpu.c), counts a frame, moves the virtual clock and lets the session player act.
#include <stdarg.h>
#include "d3d9.h"
#include "win32.h"
#include "d3d9_layout.gen.h"
#include "d3d9_oracle.gen.h"

void rt_clock_advance_frame(void);   // kernel32.c: one frame was presented

// ---- statistics ----
int d3d9_stats;
static Stat9 *stat_list;
void d3d9_stat_add(Stat9 *s) { s->next = stat_list; stat_list = s; }
typedef struct { const char *what; u32 key, value; u64 count; } ArgCount;
enum { ARG_SLOTS = 8192 };
static ArgCount arg_counts[ARG_SLOTS];
void d3d9_arg_count(const char *what, u32 key, u32 value) {
  u32 h = ((u32)(uptr)what * 2654435761u) ^ (key * 40503u) ^ (value * 2246822519u); h ^= h >> 15;
  for (u32 n = 0; n < ARG_SLOTS; n++) {
    ArgCount *a = &arg_counts[(h + n) % ARG_SLOTS];
    if (!a->what) { a->what = what; a->key = key; a->value = value; }
    if (a->what == what && a->key == key && a->value == value) { a->count++; return; }
  }
}
static int by_calls(const void *a, const void *b) {
  const Stat9 *x = *(Stat9 *const *)a, *y = *(Stat9 *const *)b;
  u64 cx = x->calls - x->mark, cy = y->calls - y->mark;
  return cx != cy ? (cx < cy ? 1 : -1) : strcmp(x->name, y->name);
}
static int by_arg(const void *a, const void *b) {
  const ArgCount *x = a, *y = b; int c = strcmp(x->what, y->what);
  if (c) return c;
  if (x->key != y->key) return x->key < y->key ? -1 : 1;
  return x->count != y->count ? (x->count < y->count ? 1 : -1) : (x->value < y->value ? -1 : x->value > y->value);
}
typedef struct { char *p; size_t n, cap; } Text;
static void put(Text *t, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int n = vsnprintf(0, 0, fmt, ap); va_end(ap);
  if (t->n + n + 1 > t->cap) { t->cap = (t->n + n + 1) * 2; t->p = realloc(t->p, t->cap); }
  va_start(ap, fmt); vsnprintf(t->p + t->n, n + 1, fmt, ap); va_end(ap); t->n += n;
}
static void flush(Text *t) { if (t->n) host_log(t->p, (u32)t->n); t->n = 0; }
// "<title>: N methods, M calls", then the methods by count. With `since_mark`: the calls since the last such
// print (a checkpoint), which becomes the new mark. Without: everything, and the argument values after it.
void d3d9_stats_print(const char *title, int since_mark) {
  unsigned n = 0, used = 0; u64 total = 0;
  for (Stat9 *s = stat_list; s; s = s->next) n++;
  Stat9 **v = malloc((n + 1) * sizeof *v); n = 0;
  for (Stat9 *s = stat_list; s; s = s->next) { if (!since_mark) s->mark = 0; v[n++] = s; }
  qsort(v, n, sizeof *v, by_calls);
  for (unsigned i = 0; i < n; i++) if (v[i]->calls > v[i]->mark) { used++; total += v[i]->calls - v[i]->mark; }
  Text t = {0};
  put(&t, "%s: %u methods, %llu calls", app, used, total); flush(&t);
  put(&t, " ");
  for (unsigned i = 0; i < used; i++) put(&t, "%s %s %llu", i ? "," : "", v[i]->name, v[i]->calls - v[i]->mark);
  flush(&t);
  for (unsigned i = 0; i < n; i++) v[i]->mark = since_mark ? v[i]->calls : 0;
  free(v);
  if (since_mark || !d3d9_stats) { free(t.p); return; }
  ArgCount *a = malloc(sizeof arg_counts); unsigned m = 0;
  for (unsigned i = 0; i < ARG_SLOTS; i++) if (arg_counts[i].what) a[m++] = arg_counts[i];
  qsort(a, m, sizeof *a, by_arg);
  put(&t, "== Direct3D 9 argument values (value x count)"); flush(&t);
  for (unsigned i = 0; i < m;) {
    unsigned j = i, shown = 0;
    if (a[i].key == 0xFFFFFFFFu) put(&t, "  %s:", a[i].what); else put(&t, "  %s %u:", a[i].what, a[i].key);
    for (; j < m && !strcmp(a[j].what, a[i].what) && a[j].key == a[i].key; j++, shown++) {
      put(&t, "%s %s%x x%llu", shown ? "," : "", a[j].value ? "0x" : "", a[j].value, a[j].count);   // all of them: d3d9-compare.py sets them against Wine's
    }
    flush(&t); i = j;
  }
  free(a); free(t.p);
}
// SW_D3D9_FRAMES=1: "frames A-B: method count, ..." for each run of frames that made the same calls
static int frame_log; static Text frame_text, frame_last; static u32 frame_first, frame_end;
static int by_name(const void *a, const void *b) { return strcmp((*(Stat9 *const *)a)->name, (*(Stat9 *const *)b)->name); }
static void frame_flush(void) {
  if (!frame_last.n) return;
  Text t = {0}; put(&t, "frames %u-%u:%s", frame_first, frame_end, frame_last.p); flush(&t); free(t.p); frame_last.n = 0;
}
static void frame_stats(u32 frame) {
  Stat9 *v[256]; unsigned n = 0;
  for (Stat9 *s = stat_list; s && n < 256; s = s->next) if (s->calls > s->frame_mark) v[n++] = s;
  qsort(v, n, sizeof *v, by_name);
  frame_text.n = 0; put(&frame_text, "");
  for (unsigned i = 0; i < n; i++) { put(&frame_text, "%s %s %llu", i ? "," : "", v[i]->name, v[i]->calls - v[i]->frame_mark); v[i]->frame_mark = v[i]->calls; }
  if (frame_last.n && frame_last.n == frame_text.n && !memcmp(frame_last.p, frame_text.p, frame_text.n)) { frame_end = frame; return; }
  frame_flush();
  Text swap = frame_last; frame_last = frame_text; frame_text = swap; frame_first = frame_end = frame;
}
static void at_exit(void) { frame_flush(); if (d3d9_stats) { d3d9_stats_print("== Direct3D 9", 0); d3d9_gpu_stats(); } d3d9_gpu_finish(); }

// ---- references ----
static void device_destroy(Device9 *d);
static void device_unref(Device9 *d) { if (--d->o.refs == 0) device_destroy(d); }
void d3d9_object_init(Object9 *o, u32 kind, Device9 *device, Object9 *container) {
  o->kind = kind; o->device = device; o->container = container;
  if (container) return;                  // a level: counted by its texture
  o->refs = 1; if (device) device->o.refs++;
}
u32 d3d9_addref(Object9 *o) {
  if (o->container) o = o->container;
  if (++o->refs == 1 && o->device) o->device->o.refs++;   // back from zero while the device still used it
  return (u32)o->refs;
}
u32 d3d9_release(Object9 *o) {
  if (o->container) o = o->container;
  if (o->refs <= 0) { com_unsure("Direct3D 9: Release of an object (kind %u) that has no reference left", o->kind); return 0; }
  u32 n = (u32)--o->refs;
  if (!n) { Device9 *d = o->device; if (!o->held) d3d9_destroy(o); if (d) device_unref(d); }
  return n;
}
void d3d9_hold(Object9 *o) { if (!o) return; if (o->container) o = o->container; o->held++; }
void d3d9_unhold(Object9 *o) {
  if (!o) return;
  if (o->container) o = o->container;
  if (--o->held == 0 && o->refs == 0) d3d9_destroy(o);
}

// ---- IDirect3D9 ----
#define SELF ((Object9 *)GP(ARG(0)))
static u32 query(Cpu *c, Object9 *o, const char *iid_a, const char *iid_b, const char *iid_c) {
  u64 riid = ARG(1), out = ARG(2);
  if (!out) return E_POINTER;
  if (com_is_iid(riid, IID_IUnknown_text) || (iid_a && com_is_iid(riid, iid_a)) || (iid_b && com_is_iid(riid, iid_b)) || (iid_c && com_is_iid(riid, iid_c))) { d3d9_addref(o); M32(out) = (u32)(uptr)o; return D3D_OK; }
  M32(out) = 0; return E_NOINTERFACE;
}
IMP(d3d9, Direct3DCreate9) {
  if (rt_trace) rt_log("Direct3DCreate9(SDK version %u)", (u32)ARG(0));
  if (!rt_exit_hook) { const char *e = getenv("SW_D3D9_STATS"); d3d9_stats = e && *e == '1'; e = getenv("SW_D3D9_FRAMES"); frame_log = e && *e == '1'; rt_exit_hook = at_exit; }
  Object9 *o = (Object9 *)GP(com_new("d3d9", &IDirect3D9_interface, sizeof(Object9)));
  o->kind = K_DIRECT3D; o->refs = 1;
  RET((uptr)o);
}
// d3d9.dll!DebugSetMute: D3DX finds it with GetProcAddress and calls it around its own work; it silences the
// debug runtime's messages, of which we have none
IMP(d3d9, DebugSetMute) { }
IMP(d3d9, IDirect3D9_QueryInterface) { STAT(QueryInterface); RET(query(c, SELF, IDirect3D9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3D9_AddRef) { STAT(AddRef); RET(++SELF->refs); }
IMP(d3d9, IDirect3D9_Release) { STAT(Release); Object9 *o = SELF; u32 n = (u32)--o->refs; if (!n) { o->vtable = 0; free(o); } RET(n); }
IMP(d3d9, IDirect3D9_GetAdapterCount) { STAT(GetAdapterCount); RET(oracle_adapters); }
IMP(d3d9, IDirect3D9_GetAdapterIdentifier) { // D3DADAPTER_IDENTIFIER9: 1100 bytes
  STAT(GetAdapterIdentifier); u64 p = ARG(3);
  if (ARG(1) >= oracle_adapters || !p) { RET(D3DERR_INVALIDCALL); return; }
  memset((void *)GP(p), 0, 1100);
  strcpy(GSTR(p), oracle_driver); strcpy(GSTR(p + 512), oracle_description); strcpy(GSTR(p + 1024), oracle_device_name);
  M32(p + 1056) = oracle_driver_version_low; M32(p + 1060) = oracle_driver_version_high;
  M32(p + 1064) = oracle_vendor; M32(p + 1068) = oracle_device; M32(p + 1072) = oracle_subsys; M32(p + 1076) = oracle_revision;
  memcpy((void *)GP(p + 1080), oracle_identifier, 16); M32(p + 1096) = oracle_whql;
  RET(D3D_OK);
}
IMP(d3d9, IDirect3D9_GetAdapterModeCount) {
  STAT(GetAdapterModeCount); ARGV("GetAdapterModeCount format", ARG(2)); u32 n = 0;
  if (ARG(1) < oracle_adapters) for (unsigned i = 0; i < sizeof oracle_modes / sizeof oracle_modes[0]; i++) n += oracle_modes[i].asked == (u32)ARG(2);
  RET(n);
}
IMP(d3d9, IDirect3D9_EnumAdapterModes) { // D3DDISPLAYMODE: width, height, refresh rate, format
  STAT(EnumAdapterModes); ARGV("EnumAdapterModes format", ARG(2)); u64 p = ARG(4);
  if (ARG(1) < oracle_adapters && p) for (unsigned i = 0; i < sizeof oracle_modes / sizeof oracle_modes[0]; i++) if (oracle_modes[i].asked == (u32)ARG(2) && oracle_modes[i].index == (u32)ARG(3)) {
    M32(p) = oracle_modes[i].width; M32(p + 4) = oracle_modes[i].height; M32(p + 8) = oracle_modes[i].refresh; M32(p + 12) = oracle_modes[i].format; RET(D3D_OK); return;
  }
  RET(D3DERR_INVALIDCALL);
}
static void display_mode(u64 p) { M32(p) = oracle_display.width; M32(p + 4) = oracle_display.height; M32(p + 8) = oracle_display.refresh; M32(p + 12) = oracle_display.format; }
IMP(d3d9, IDirect3D9_GetAdapterDisplayMode) {
  STAT(GetAdapterDisplayMode);
  if (ARG(1) >= oracle_adapters || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  display_mode(ARG(2)); RET(D3D_OK);
}
// The oracle was asked about the usual formats and uses against its X8R8G8B8 display (tools/oracle-wine/d3d9probe.c);
// a question it was not asked is not answered by guessing.
static u32 format_support(u32 adapter_format, u32 usage, u32 type, u32 format) {
  if (adapter_format == oracle_display.format) for (unsigned i = 0; i < sizeof oracle_formats / sizeof oracle_formats[0]; i++)
    if (oracle_formats[i].format == format && oracle_formats[i].usage == usage && oracle_formats[i].type == type) return oracle_formats[i].hr;
  com_unsure("Direct3D 9: CheckDeviceFormat(adapter format %u, usage 0x%x, resource type %u, format %u) was not asked of the oracle: answering that it is not available", adapter_format, usage, type, format);
  return D3DERR_NOTAVAILABLE;
}
IMP(d3d9, IDirect3D9_CheckDeviceFormat) {
  STAT(CheckDeviceFormat);
  ARGV("CheckDeviceFormat device_type", ARG(2)); ARGV("CheckDeviceFormat adapter_format", ARG(3)); ARGV("CheckDeviceFormat usage", ARG(4)); ARGV("CheckDeviceFormat resource_type", ARG(5)); ARGV("CheckDeviceFormat format", ARG(6));
  if (ARG(1) >= oracle_adapters) { RET(D3DERR_INVALIDCALL); return; }
  RET(format_support((u32)ARG(3), (u32)ARG(4), (u32)ARG(5), (u32)ARG(6)));
}
IMP(d3d9, IDirect3D9_CheckDepthStencilMatch) {
  STAT(CheckDepthStencilMatch);
  if (ARG(1) < oracle_adapters && (u32)ARG(3) == oracle_display.format) for (unsigned i = 0; i < sizeof oracle_depth_match / sizeof oracle_depth_match[0]; i++)
    if (oracle_depth_match[i].target == (u32)ARG(4) && oracle_depth_match[i].depth == (u32)ARG(5)) { RET(oracle_depth_match[i].hr); return; }
  com_unsure("Direct3D 9: CheckDepthStencilMatch(adapter format %u, target %u, depth %u) was not asked of the oracle", (u32)ARG(3), (u32)ARG(4), (u32)ARG(5));
  RET(D3DERR_NOTAVAILABLE);
}
IMP(d3d9, IDirect3D9_CheckDeviceType) {
  STAT(CheckDeviceType);
  if (ARG(1) < oracle_adapters && (u32)ARG(3) == oracle_display.format) for (unsigned i = 0; i < sizeof oracle_device_types / sizeof oracle_device_types[0]; i++)
    if (oracle_device_types[i].format == (u32)ARG(4) && oracle_device_types[i].windowed == (ARG(5) != 0)) { RET(oracle_device_types[i].hr); return; }
  com_unsure("Direct3D 9: CheckDeviceType(display format %u, back buffer format %u, windowed %u) was not asked of the oracle", (u32)ARG(3), (u32)ARG(4), (u32)ARG(5));
  RET(D3DERR_NOTAVAILABLE);
}
// D3DCAPS9 of the oracle's HAL device; its reference device answers with the same capabilities under its own type
static void device_caps(u64 p, u32 type) { memcpy((void *)GP(p), oracle_caps, sizeof oracle_caps); M32(p) = type; }
IMP(d3d9, IDirect3D9_GetDeviceCaps) {
  STAT(GetDeviceCaps); ARGV("GetDeviceCaps device_type", ARG(2));
  if (ARG(1) >= oracle_adapters || !ARG(3)) { RET(D3DERR_INVALIDCALL); return; }
  if (ARG(2) != 1 && ARG(2) != 2) { com_unsure("Direct3D 9: GetDeviceCaps of device type %u: only the HAL (1) and the reference device (2) were asked of the oracle", (u32)ARG(2)); RET(D3DERR_INVALIDCALL); return; }
  device_caps(ARG(3), (u32)ARG(2)); RET(D3D_OK);
}
IMP(d3d9, IDirect3D9_GetAdapterMonitor) { STAT(GetAdapterMonitor); RET(ARG(1) < oracle_adapters ? 0x00010001u : 0); } // an HMONITOR: the one screen

// ---- the device ----
// CreateDevice(adapter, device type, focus window, behaviour flags, D3DPRESENT_PARAMETERS *, IDirect3DDevice9 **).
// Flags: FPU_PRESERVE 2, MULTITHREADED 4, PUREDEVICE 0x10, SOFTWARE / HARDWARE / MIXED vertex processing 0x20 / 0x40 / 0x80.
IMP(d3d9, IDirect3D9_CreateDevice) {
  STAT(CreateDevice); ARGV("CreateDevice device_type", ARG(2)); ARGV("CreateDevice flags", ARG(4));
  Object9 *d3d = SELF; u32 flags = (u32)ARG(4); Present9 *pp = (Present9 *)GP(ARG(5)); u64 out = ARG(6);
  if (out) M32(out) = 0;
  if (ARG(1) >= oracle_adapters || !ARG(5) || !out || !(flags & 0xE0)) { RET(D3DERR_INVALIDCALL); return; }
  if (ARG(2) != 1) { com_unsure("Direct3D 9: CreateDevice of device type %u: only the HAL device (1) exists", (u32)ARG(2)); RET(D3DERR_NOTAVAILABLE); return; }
  u32 window = pp->window ? pp->window : (u32)ARG(3); s32 cw = 0, ch = 0;
  if (!pp->windowed) {
    // full screen: the page's canvas is the display, and the mode the app asks for is its back buffer's size (the
    // refresh rate is the host's). The mode must be named, as on Windows.
    if (!pp->width || !pp->height || !pp->format || !window) { RET(D3DERR_INVALIDCALL); return; }
  } else {
    if (!win32_client_size(window, &cw, &ch)) { RET(D3DERR_INVALIDCALL); return; }
    // what Direct3D fills in: the window's client area, the display's format, one back buffer
    if (!pp->width) pp->width = (u32)cw; if (!pp->height) pp->height = (u32)ch;
  }
  if (!pp->format) pp->format = oracle_display.format;
  if (!pp->count) pp->count = 1;
  if (!d3d9_format(pp->format) || (pp->auto_depth && !d3d9_format(pp->depth_format)) || pp->count != 1 || pp->multisample) {
    com_unsure("Direct3D 9: CreateDevice with back buffer format %u x%u, multisampling %u, depth format %u is not implemented", pp->format, pp->count, pp->multisample, pp->depth_format);
    RET(D3DERR_NOTAVAILABLE); return;
  }
  Device9 *d = (Device9 *)GP(com_new("d3d9", &IDirect3DDevice9_interface, sizeof(Device9)));
  d->o.kind = K_DEVICE; d->o.refs = 1; d->d3d = d3d; d3d->refs++;
  d->adapter = (u32)ARG(1); d->device_type = (u32)ARG(2); d->focus_window = (u32)ARG(3); d->behavior = flags; d->pp = *pp; d->pp.window = window;
  d->swapchain = (SwapChain9 *)GP(com_new("d3d9", &IDirect3DSwapChain9_interface, sizeof(SwapChain9)));
  d->swapchain->o.kind = K_SWAPCHAIN; d->swapchain->o.device = d; d->swapchain->o.held = 1;
  // the back buffer and the depth buffer belong to the device: no reference of the app's until it asks for them
  d->back_buffer = d3d9_surface_new(d, 0, 0, pp->width, pp->height, pp->format, USAGE_RENDERTARGET, POOL_DEFAULT, pp->multisample, pp->quality);
  d3d9_hold(&d->back_buffer->o); d->back_buffer->lockable = pp->flags & 1;   // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
  if (pp->auto_depth) { d->auto_depth = d3d9_surface_new(d, 0, 0, pp->width, pp->height, pp->depth_format, USAGE_DEPTHSTENCIL, POOL_DEFAULT, pp->multisample, pp->quality); d3d9_hold(&d->auto_depth->o); }
  d->targets[0] = d->back_buffer; d3d9_hold(&d->back_buffer->o);
  d->depth = d->auto_depth; d3d9_hold((Object9 *)d->auto_depth);
  d3d9_state_init(d);
  // Direct3D runs the x87 in single precision unless it is told to leave it alone
  if (!(flags & 2)) c->fcw &= ~0x300;
  if (rt_trace) rt_log("Direct3D 9 device: %ux%u format %u, depth %u (%s), window 0x%x, flags 0x%x%s", pp->width, pp->height, pp->format, pp->depth_format, pp->auto_depth ? "automatic" : "none", window, flags, (flags & 2) ? "" : ", x87 to 24-bit precision");
  M32(out) = (u32)(uptr)d; RET(D3D_OK);
}
static void device_destroy(Device9 *d) {
  if (d->recording) { d3d9_stateblock_destroy(d->recording); d->recording = 0; }
  d3d9_state_free(&d->state, 1);
  for (Declaration9 *v = d->fvf_declarations, *next; v; v = next) { next = v->next_fvf; d3d9_unhold(&v->o); }
  for (int i = 0; i < 2; i++) if (d->up[i]) { d3d9_gpu_release(&d->up[i]->o); free(d->up[i]->bits); d->up[i]->o.vtable = 0; free(d->up[i]); d->up[i] = 0; }   // the UP draws' buffers
  for (unsigned i = 0; i < 4; i++) d3d9_unhold((Object9 *)d->targets[i]);
  d3d9_unhold((Object9 *)d->depth); d3d9_unhold((Object9 *)d->auto_depth); d3d9_unhold((Object9 *)d->back_buffer);
  d->swapchain->o.vtable = 0; free(d->swapchain);
  Object9 *d3d = d->d3d; if (--d3d->refs == 0) { d3d->vtable = 0; free(d3d); }
  d->o.vtable = 0; free(d);
}
#define DEV ((Device9 *)GP(ARG(0)))
IMP(d3d9, IDirect3DDevice9_QueryInterface) { STAT(device_QueryInterface); RET(query(c, SELF, IDirect3DDevice9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DDevice9_AddRef) { STAT(device_AddRef); RET(++SELF->refs); }
IMP(d3d9, IDirect3DDevice9_Release) { STAT(device_Release); Device9 *d = DEV; u32 n = (u32)d->o.refs - 1; device_unref(d); RET(n); }
IMP(d3d9, IDirect3DDevice9_TestCooperativeLevel) { STAT(device_TestCooperativeLevel); RET(D3D_OK); }   // never lost
IMP(d3d9, IDirect3DDevice9_GetAvailableTextureMem) { STAT(device_GetAvailableTextureMem); RET(oracle_texture_mem); }
IMP(d3d9, IDirect3DDevice9_EvictManagedResources) { STAT(device_EvictManagedResources); RET(D3D_OK); }
IMP(d3d9, IDirect3DDevice9_GetDirect3D) {
  STAT(device_GetDirect3D);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  DEV->d3d->refs++; M32(ARG(1)) = (u32)(uptr)DEV->d3d; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetDeviceCaps) {
  STAT(device_GetDeviceCaps);
  if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; }
  device_caps(ARG(1), DEV->device_type); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetDisplayMode) {
  STAT(device_GetDisplayMode);
  if (ARG(1) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  display_mode(ARG(2)); RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_GetCreationParameters) { // D3DDEVICE_CREATION_PARAMETERS: adapter, device type, focus window, behaviour flags
  STAT(device_GetCreationParameters); Device9 *d = DEV; u64 p = ARG(1);
  if (!p) { RET(D3DERR_INVALIDCALL); return; }
  M32(p) = d->adapter; M32(p + 4) = d->device_type; M32(p + 8) = d->focus_window; M32(p + 12) = d->behavior; RET(D3D_OK);
}
IMP(d3d9, IDirect3DDevice9_SetCursorPosition) { STAT(device_SetCursorPosition); DEV->cursor_x = (s32)ARG(1); DEV->cursor_y = (s32)ARG(2); }
IMP(d3d9, IDirect3DDevice9_ShowCursor) { STAT(device_ShowCursor); Device9 *d = DEV; u32 was = d->cursor_shown; d->cursor_shown = ARG(1) != 0; RET(was); }
IMP(d3d9, IDirect3DDevice9_GetNumberOfSwapChains) { STAT(device_GetNumberOfSwapChains); RET(1); }
IMP(d3d9, IDirect3DDevice9_GetSwapChain) {
  STAT(device_GetSwapChain); Device9 *d = DEV;
  if (ARG(1) || !ARG(2)) { RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&d->swapchain->o); M32(ARG(2)) = (u32)(uptr)d->swapchain; RET(D3D_OK);
}
// Present(source rectangle, destination rectangle, window, dirty region): the end of a frame
static u32 present(Device9 *d) {
  d->frames++;
  d3d9_gpu_present(d);
  if (frame_log) frame_stats(d->frames);
  rt_clock_advance_frame();
  session_frame(d->frames);
  return D3D_OK;
}
IMP(d3d9, IDirect3DDevice9_Present) { STAT(device_Present); RET(present(DEV)); }
IMP(d3d9, IDirect3DDevice9_GetBackBuffer) { // (swap chain, back buffer, type, IDirect3DSurface9 **)
  STAT(device_GetBackBuffer); Device9 *d = DEV;
  if (ARG(1) || ARG(2) || !ARG(4)) { if (ARG(4)) M32(ARG(4)) = 0; RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&d->back_buffer->o); M32(ARG(4)) = (u32)(uptr)d->back_buffer; RET(D3D_OK);
}
// Queries do not exist on this device: an app that asks takes the path it has for cards without them
IMP(d3d9, IDirect3DDevice9_CreateQuery) { STAT(device_CreateQuery); ARGV("device_CreateQuery type", ARG(1)); if (ARG(2)) M32(ARG(2)) = 0; RET(D3DERR_NOTAVAILABLE); }

// ---- IDirect3DSwapChain9: the device's own ----
IMP(d3d9, IDirect3DSwapChain9_QueryInterface) { STAT(swapchain_QueryInterface); RET(query(c, SELF, IDirect3DSwapChain9_interface.iid, 0, 0)); }
IMP(d3d9, IDirect3DSwapChain9_AddRef) { STAT(swapchain_AddRef); RET(d3d9_addref(SELF)); }
IMP(d3d9, IDirect3DSwapChain9_Release) { STAT(swapchain_Release); RET(d3d9_release(SELF)); }
IMP(d3d9, IDirect3DSwapChain9_Present) { STAT(swapchain_Present); RET(present(SELF->device)); }
IMP(d3d9, IDirect3DSwapChain9_GetBackBuffer) {
  STAT(swapchain_GetBackBuffer); Device9 *d = SELF->device;
  if (ARG(1) || !ARG(3)) { if (ARG(3)) M32(ARG(3)) = 0; RET(D3DERR_INVALIDCALL); return; }
  d3d9_addref(&d->back_buffer->o); M32(ARG(3)) = (u32)(uptr)d->back_buffer; RET(D3D_OK);
}
IMP(d3d9, IDirect3DSwapChain9_GetDisplayMode) { STAT(swapchain_GetDisplayMode); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } display_mode(ARG(1)); RET(D3D_OK); }
IMP(d3d9, IDirect3DSwapChain9_GetDevice) { STAT(swapchain_GetDevice); Device9 *d = SELF->device; d->o.refs++; M32(ARG(1)) = (u32)(uptr)d; RET(D3D_OK); }
IMP(d3d9, IDirect3DSwapChain9_GetPresentParameters) { STAT(swapchain_GetPresentParameters); if (!ARG(1)) { RET(D3DERR_INVALIDCALL); return; } *(Present9 *)GP(ARG(1)) = SELF->device->pp; RET(D3D_OK); }
