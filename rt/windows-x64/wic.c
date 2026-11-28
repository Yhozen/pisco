// SPDX-License-Identifier: GPL-2.0-or-later
// ole32.dll's COM start-up and the Windows Imaging Component (windowscodecs.dll) for Windows desktop titles: PNG
// files read into 32-bit pixels, which is what an app that loads its textures through WIC asks (Desire). The
// objects: the imaging factory (CoCreateInstance with CLSID_WICImagingFactory or ...2), a decoder made from a file
// name, its one frame, and a format converter to 32bppRGBA, 32bppBGRA or 32bppPBGRA. PNG is decoded by stb_image
// (third_party/stb_image.h, Sean Barrett's, MIT or public domain); any other format fails as WIC's
// WINCODEC_ERR_COMPONENTNOTFOUND does.
#include <strings.h>
#include "rt.h"
#include "vfs.h"
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define S_OK 0
#define S_FALSE 1
#define E_NOINTERFACE 0x80004002u
#define E_INVALIDARG 0x80070057u
#define E_OUTOFMEMORY 0x8007000Eu
#define REGDB_E_CLASSNOTREG 0x80040154u
#define WINCODEC_ERR_COMPONENTNOTFOUND 0x88982F50u
#define WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT 0x88982F80u
#define WINCODEC_ERR_INSUFFICIENTBUFFER 0x88982F8Cu

static int guid_is(u64 p, const char *s) { // p: a GUID in guest memory; s: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
  if (!p) return 0;
  const u8 *g = (const u8 *)GP(p); u8 want[16]; unsigned v[11];
  if (sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]) != 11) return 0;
  memcpy(want, &v[0], 4); u16 b = (u16)v[1], c = (u16)v[2]; memcpy(want + 4, &b, 2); memcpy(want + 6, &c, 2);
  for (int i = 0; i < 8; i++) want[8 + i] = (u8)v[3 + i];
  return !memcmp(g, want, 16);
}
static void guid_put(u64 p, const char *s) {
  unsigned v[11]; sscanf(s, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10]);
  u8 *g = (u8 *)GP(p); memcpy(g, &v[0], 4); u16 b = (u16)v[1], c = (u16)v[2]; memcpy(g + 4, &b, 2); memcpy(g + 6, &c, 2);
  for (int i = 0; i < 8; i++) g[8 + i] = (u8)v[3 + i];
}
#define RGBA "f5c7ad2d-6a8d-43dd-a7a8-a29935261ae9"   // GUID_WICPixelFormat32bppRGBA
#define BGRA "6fddc324-4e03-4bfe-b185-3d77768dc90f"   // 32bppBGRA
#define PBGRA "6fddc324-4e03-4bfe-b185-3d77768dc910"  // 32bppPBGRA
#define PNG_FORMAT "1b7cfaf4-713f-473c-bbcd-6137425faeaf" // GUID_ContainerFormatPng

// ---- ole32 ----
static int com_inits;
IMP(ole32, CoInitializeEx) { RET(com_inits++ ? S_FALSE : S_OK); }
IMP(ole32, CoInitialize) { RET(com_inits++ ? S_FALSE : S_OK); }
IMP(ole32, CoUninitialize) { if (com_inits) com_inits--; }
u64 rt_wic_factory(void);
IMP(ole32, CoCreateInstance) { // (clsid, outer, context, iid, ppv)
  u64 ppv = ARG(4); if (!ppv) { RET(E_INVALIDARG); return; }
  if (guid_is(ARG(0), "cacaf262-9370-4615-a13b-9f5539da4c0a") || guid_is(ARG(0), "317d06e8-5f24-433d-bdf7-79ce68d8abc2")) { M64(ppv) = rt_wic_factory(); RET(S_OK); return; }
  const u8 *g = (const u8 *)GP(ARG(0));
  rt_log("ole32: CoCreateInstance of a class not registered here ({%08x-...})", g[0] | g[1] << 8 | g[2] << 16 | (u32)g[3] << 24);
  M64(ppv) = 0; RET(REGDB_E_CLASSNOTREG);
}

// ---- the objects: one structure for all of them; a decoder, its frame and a converter share the pixels ----
enum { K_FACTORY, K_DECODER, K_FRAME, K_CONVERTER, K_STREAM };
// rgba: w * h * 4, R G B A; a stream's bytes are in data (size n, read position at)
typedef struct { u64 vtbl; u32 magic, refs, kind, w, h, bgra, premultiply; u8 *rgba, *data; u64 n, at; } Wic;
#define WMAGIC 0x57494331u
static Wic *self(Cpu *c) { Wic *o = (Wic *)GP(ARG(0)); return o && o->magic == WMAGIC ? o : 0; }
static const char *const UNKNOWN[] = {"QueryInterface", "AddRef", "Release"};
static u64 vt(const char *prefix, const char *const *names, unsigned n) {
  const char *all[40]; for (unsigned i = 0; i < 3; i++) all[i] = UNKNOWN[i]; for (unsigned i = 0; i < n; i++) all[3 + i] = names[i];
  return rt_com_vtable("windowscodecs", prefix, all, 3 + n);
}
static Wic *make(u32 kind) {
  static u64 vts[5];
  static const char *const factory[] = {"CreateDecoderFromFilename", "CreateDecoderFromStream", "CreateDecoderFromFileHandle", "CreateComponentInfo", "CreateDecoder", "CreateEncoder", "CreatePalette", "CreateFormatConverter", "CreateBitmapScaler", "CreateBitmapClipper", "CreateBitmapFlipRotator", "CreateStream", "CreateColorContext", "CreateColorTransformer", "CreateBitmap", "CreateBitmapFromSource", "CreateBitmapFromSourceRect", "CreateBitmapFromMemory", "CreateBitmapFromHBITMAP", "CreateBitmapFromHICON", "CreateComponentEnumerator", "CreateFastMetadataEncoderFromDecoder", "CreateFastMetadataEncoderFromFrameDecode", "CreateQueryWriter", "CreateQueryWriterFromReader", "CreateImageEncoder"};
  static const char *const decoder[] = {"QueryCapability", "Initialize", "GetContainerFormat", "GetDecoderInfo", "CopyPalette", "GetMetadataQueryReader", "GetPreview", "GetColorContexts", "GetThumbnail", "GetFrameCount", "GetFrame"};
  static const char *const source[] = {"GetSize", "GetPixelFormat", "GetResolution", "CopyPalette", "CopyPixels", "GetMetadataQueryReader", "GetColorContexts", "GetThumbnail"};
  static const char *const converter[] = {"GetSize", "GetPixelFormat", "GetResolution", "CopyPalette", "CopyPixels", "Initialize", "CanConvert"};
  static const char *const stream[] = {"Read", "Write", "Seek", "SetSize", "CopyTo", "Commit", "Revert", "LockRegion", "UnlockRegion", "Stat", "Clone", "InitializeFromIStream", "InitializeFromFilename", "InitializeFromMemory", "InitializeFromIStreamRegion"};
  if (!vts[kind]) vts[kind] = kind == K_FACTORY ? vt("Factory_", factory, 26) : kind == K_DECODER ? vt("Decoder_", decoder, 11) : kind == K_FRAME ? vt("Source_", source, 8) : kind == K_STREAM ? vt("Stream_", stream, 15) : vt("Converter_", converter, 7);
  Wic *o = calloc(1, sizeof *o); o->vtbl = vts[kind]; o->magic = WMAGIC; o->refs = 1; o->kind = kind;
  return o;
}
u64 rt_wic_factory(void) { return (uptr)make(K_FACTORY); }
static void drop(Wic *o) { if (--o->refs) return; free(o->rgba); free(o->data); o->magic = 0; free(o); }
static void share(Wic *to, const Wic *from) { to->w = from->w; to->h = from->h; size_t n = (size_t)from->w * from->h * 4; to->rgba = malloc(n ? n : 1); if (from->rgba) memcpy(to->rgba, from->rgba, n); }
// every interface an object is asked for is the object (the factory's IWICImagingFactory2, a frame's IWICBitmapSource, ...).
// (Written out for each kind: tools/gen-imports.sh finds the methods by their IMP lines.)
static void query(Cpu *c) { Wic *o = self(c); if (!ARG(2)) { RET(E_INVALIDARG); return; } if (!o) { M64(ARG(2)) = 0; RET(E_NOINTERFACE); return; } o->refs++; M64(ARG(2)) = ARG(0); RET(S_OK); }
static void add_ref(Cpu *c) { Wic *o = self(c); RET(o ? ++o->refs : 0); }
static void release(Cpu *c) { Wic *o = self(c); if (!o) { RET(0); return; } u32 r = o->refs - 1; drop(o); RET(r); }
IMP(windowscodecs, Factory_QueryInterface) { query(c); }
IMP(windowscodecs, Factory_AddRef) { add_ref(c); }
IMP(windowscodecs, Factory_Release) { release(c); }
IMP(windowscodecs, Decoder_QueryInterface) { query(c); }
IMP(windowscodecs, Decoder_AddRef) { add_ref(c); }
IMP(windowscodecs, Decoder_Release) { release(c); }
IMP(windowscodecs, Source_QueryInterface) { query(c); }
IMP(windowscodecs, Source_AddRef) { add_ref(c); }
IMP(windowscodecs, Source_Release) { release(c); }
IMP(windowscodecs, Converter_QueryInterface) { query(c); }
IMP(windowscodecs, Converter_AddRef) { add_ref(c); }
IMP(windowscodecs, Converter_Release) { release(c); }
IMP(windowscodecs, Stream_QueryInterface) { query(c); }
IMP(windowscodecs, Stream_AddRef) { add_ref(c); }
IMP(windowscodecs, Stream_Release) { release(c); }

// a file's bytes, read whole (0 if it cannot be read; the Win32 error in *err)
static u8 *read_file(const char *name, u64 *n, u32 *err) {
  *err = 0; VFile *f = vfs_open(name, 0x80000000u, 3, err); // GENERIC_READ, OPEN_EXISTING
  if (!f) { if (!*err) *err = 2; return 0; }
  *n = vfs_size(f); u8 *data = malloc(*n ? *n : 1); s64 got = vfs_read(f, data, *n); vfs_close(f);
  if (got != (s64)*n) { free(data); *err = 30; return 0; } // ERROR_READ_FAULT
  return data;
}
// a decoder of the PNG in data (what is being read is named for the log); WINCODEC_ERR_COMPONENTNOTFOUND otherwise
static u32 decode(const u8 *data, u64 n, const char *what, u64 ppv) {
  int w = 0, h = 0, ch = 0; u8 *px = stbi_load_from_memory(data, (int)n, &w, &h, &ch, 4);
  if (!px) { rt_log("wic: %s: not a PNG this decoder reads (%s)", what, stbi_failure_reason()); M64(ppv) = 0; return WINCODEC_ERR_COMPONENTNOTFOUND; }
  if (rt_trace) rt_log("wic: %s: %dx%d, %d channels", what, w, h, ch);
  Wic *d = make(K_DECODER); d->w = (u32)w; d->h = (u32)h; size_t sz = (size_t)w * h * 4; d->rgba = malloc(sz); memcpy(d->rgba, px, sz); stbi_image_free(px);
  M64(ppv) = (uptr)d; return S_OK;
}
IMP(windowscodecs, Factory_CreateStream) { if (!ARG(1)) { RET(E_INVALIDARG); return; } M64(ARG(1)) = (uptr)make(K_STREAM); RET(S_OK); }
IMP(windowscodecs, Stream_InitializeFromMemory) { // (buffer, size): the bytes are copied (the title keeps its buffer)
  Wic *o = self(c); if (!o || !ARG(1)) { RET(E_INVALIDARG); return; }
  free(o->data); o->n = (u32)ARG(2); o->data = malloc(o->n ? o->n : 1); memcpy(o->data, (void *)GP(ARG(1)), o->n); o->at = 0; RET(S_OK);
}
IMP(windowscodecs, Stream_InitializeFromFilename) { // (name, access)
  Wic *o = self(c); if (!o || !ARG(1)) { RET(E_INVALIDARG); return; }
  char *name = rt_narrow(GWSTR(ARG(1))); u32 err; u64 n = 0; u8 *d = read_file(name, &n, &err); free(name);
  if (!d) { RET(0x80070000u | err); return; }
  free(o->data); o->data = d; o->n = n; o->at = 0; RET(S_OK);
}
IMP(windowscodecs, Stream_Read) { // (buffer, count, &read)
  Wic *o = self(c); if (!o || !ARG(1)) { RET(E_INVALIDARG); return; }
  u64 k = o->at < o->n ? o->n - o->at : 0; if (k > (u32)ARG(2)) k = (u32)ARG(2);
  memcpy((void *)GP(ARG(1)), o->data + o->at, k); o->at += k; if (ARG(3)) M32(ARG(3)) = (u32)k; RET(k == (u32)ARG(2) ? S_OK : S_FALSE);
}
IMP(windowscodecs, Stream_Seek) { // (move, origin: set, current, end, &new position)
  Wic *o = self(c); if (!o) { RET(E_INVALIDARG); return; }
  s64 base = (u32)ARG(2) == 0 ? 0 : (u32)ARG(2) == 1 ? (s64)o->at : (s64)o->n, to = base + (s64)ARG(1);
  if (to < 0) { RET(0x80030019u); return; } // STG_E_INVALIDFUNCTION
  o->at = (u64)to; if (ARG(3)) M64(ARG(3)) = o->at; RET(S_OK);
}
IMP(windowscodecs, Stream_Stat) { if (!ARG(1)) { RET(E_INVALIDARG); return; } Wic *o = self(c); memset((void *)GP(ARG(1)), 0, 80); M32(ARG(1) + 8) = 2; M64(ARG(1) + 16) = o ? o->n : 0; RET(S_OK); } // STATSTG: type STGTY_STREAM, size
IMP(windowscodecs, Factory_CreateDecoderFromStream) { // (stream, vendor, options, &decoder)
  Wic *st = (Wic *)GP(ARG(1)); u64 ppv = ARG(4);
  if (!ppv) { RET(E_INVALIDARG); return; }
  if (!st || st->magic != WMAGIC || st->kind != K_STREAM) { rt_log("wic: CreateDecoderFromStream on a stream not made here"); M64(ppv) = 0; RET(E_INVALIDARG); return; }
  RET(decode(st->data + (st->at < st->n ? st->at : st->n), st->at < st->n ? st->n - st->at : 0, "a stream", ppv));
}

// (name, vendor, access, options, &decoder): the file read and decoded at once
IMP(windowscodecs, Factory_CreateDecoderFromFilename) {
  u64 ppv = ARG(5); if (!ppv) { RET(E_INVALIDARG); return; }
  char *name = rt_narrow(GWSTR(ARG(1))); u32 err; u64 n = 0; u8 *d = read_file(name, &n, &err);
  if (!d) { rt_log("wic: %s: not found", name); free(name); M64(ppv) = 0; RET(0x80070000u | err); return; }
  RET(decode(d, n, name, ppv)); free(d); free(name);
}
IMP(windowscodecs, Factory_CreateFormatConverter) { if (!ARG(1)) { RET(E_INVALIDARG); return; } M64(ARG(1)) = (uptr)make(K_CONVERTER); RET(S_OK); }
IMP(windowscodecs, Decoder_GetContainerFormat) { if (!ARG(1)) { RET(E_INVALIDARG); return; } guid_put(ARG(1), PNG_FORMAT); RET(S_OK); }
IMP(windowscodecs, Decoder_GetFrameCount) { if (!ARG(1)) { RET(E_INVALIDARG); return; } M32(ARG(1)) = 1; RET(S_OK); }
IMP(windowscodecs, Decoder_GetFrame) { // (index, &frame)
  Wic *d = self(c); if (!d || !ARG(2)) { RET(E_INVALIDARG); return; }
  if ((u32)ARG(1)) { M64(ARG(2)) = 0; RET(E_INVALIDARG); return; }
  Wic *f = make(K_FRAME); share(f, d); M64(ARG(2)) = (uptr)f; RET(S_OK);
}
// IWICBitmapSource, for frames and converters
static void get_size(Cpu *c) { Wic *o = self(c); if (!o || !ARG(1) || !ARG(2)) { RET(E_INVALIDARG); return; } M32(ARG(1)) = o->w; M32(ARG(2)) = o->h; RET(S_OK); }
static void get_format(Cpu *c) { Wic *o = self(c); if (!o || !ARG(1)) { RET(E_INVALIDARG); return; } guid_put(ARG(1), !o->bgra ? RGBA : o->premultiply ? PBGRA : BGRA); RET(S_OK); }
static void get_resolution(Cpu *c) { if (!ARG(1) || !ARG(2)) { RET(E_INVALIDARG); return; } MF64(ARG(1)) = 96.0; MF64(ARG(2)) = 96.0; RET(S_OK); }
static void copy_pixels(Cpu *c) { // (rect or 0, stride, size, buffer)
  Wic *o = self(c); u64 r = ARG(1), buf = ARG(4); u32 stride = (u32)ARG(2), size = (u32)ARG(3);
  if (!o || !buf || !o->rgba) { RET(E_INVALIDARG); return; }
  s32 x = 0, y = 0, w = (s32)o->w, h = (s32)o->h;
  if (r) { x = (s32)M32(r); y = (s32)M32(r + 4); w = (s32)M32(r + 8); h = (s32)M32(r + 12); }
  if (x < 0 || y < 0 || w < 0 || h < 0 || (u32)(x + w) > o->w || (u32)(y + h) > o->h) { RET(E_INVALIDARG); return; }
  if (!h || !w) { RET(S_OK); return; }
  if (stride < (u32)w * 4 || (u64)stride * (u64)(h - 1) + (u64)w * 4 > size) { RET(WINCODEC_ERR_INSUFFICIENTBUFFER); return; }
  for (s32 j = 0; j < h; j++) {
    const u8 *s = o->rgba + ((size_t)(y + j) * o->w + (size_t)x) * 4; u8 *d = (u8 *)GP(buf + (u64)stride * (u64)j);
    for (s32 i = 0; i < w; i++, s += 4, d += 4) {
      u8 r = s[0], g = s[1], b = s[2], a = s[3];
      if (o->premultiply) { r = (u8)((r * a + 127) / 255); g = (u8)((g * a + 127) / 255); b = (u8)((b * a + 127) / 255); }
      if (o->bgra) { d[0] = b; d[1] = g; d[2] = r; d[3] = a; } else { d[0] = r; d[1] = g; d[2] = b; d[3] = a; }
    }
  }
  RET(S_OK);
}
IMP(windowscodecs, Source_GetSize) { get_size(c); }
IMP(windowscodecs, Source_GetPixelFormat) { get_format(c); }
IMP(windowscodecs, Source_GetResolution) { get_resolution(c); }
IMP(windowscodecs, Source_CopyPixels) { copy_pixels(c); }
IMP(windowscodecs, Converter_GetSize) { get_size(c); }
IMP(windowscodecs, Converter_GetPixelFormat) { get_format(c); }
IMP(windowscodecs, Converter_GetResolution) { get_resolution(c); }
IMP(windowscodecs, Converter_CopyPixels) { copy_pixels(c); }
// (source, format, dither, palette, alpha threshold, palette type): the source's pixels, in the format asked for
IMP(windowscodecs, Converter_Initialize) {
  Wic *o = self(c), *src = (Wic *)GP(ARG(1)); u64 fmt = ARG(2);
  if (!o || !src || src->magic != WMAGIC || !src->rgba) { RET(E_INVALIDARG); return; }
  int rgba = guid_is(fmt, RGBA), bgra = guid_is(fmt, BGRA), pbgra = guid_is(fmt, PBGRA);
  if (!rgba && !bgra && !pbgra) { const u8 *g = (const u8 *)GP(fmt); rt_log("wic: conversion to pixel format {%08x-...} is not done here", g[0] | g[1] << 8 | g[2] << 16 | (u32)g[3] << 24); RET(WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT); return; }
  free(o->rgba); share(o, src); o->bgra = !rgba; o->premultiply = pbgra;
  RET(S_OK);
}
IMP(windowscodecs, Converter_CanConvert) { if (ARG(3)) M32(ARG(3)) = guid_is(ARG(2), RGBA) || guid_is(ARG(2), BGRA) || guid_is(ARG(2), PBGRA); RET(S_OK); }
