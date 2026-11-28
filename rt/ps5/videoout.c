// SPDX-License-Identifier: GPL-2.0-or-later
// libSceVideoOut: the screen. An app registers frame buffers in its own memory (linear RGBA8 rows here: what a
// CPU-drawn app writes) and flips between them. A flip sends the buffer's pixels to the GPU back end as a texture
// (GPU_R_WRITE_TEXTURE, in pieces that fit the ring) and presents it (GPU_R_PRESENT): web/gpu/backend.js draws it into
// the page's canvas. Waiting for vertical blank paces the app at 60 frames a second on the host's clock (with a
// virtual clock, SW_CLOCK, the clock moves by a frame instead).
#include "ps5.h"
HOST("frame_dump") void host_frame_dump(u32 pixels, u32 width, u32 height, u32 frame);

#include "gpu_ring.h"

#define MAX_BUFFERS 16
// what sceVideoOutSetBufferAttribute2 writes into the app's attribute (80 bytes): ours, read back on registration
typedef struct { u64 pixel_format; u32 tiling, width, height, pitch, magic; } Attr;
#define ATTR_MAGIC 0x50534F56u
static struct { u64 data[MAX_BUFFERS]; u32 width, height, pitch, tiling; u64 format; int open, texture; u64 flips; } vo;
static pthread_mutex_t vo_lock = PTHREAD_MUTEX_INITIALIZER;
static u64 next_vblank_us;

// (user id, bus type, index, parameters): one screen, handle 1
IMP(libscevideoout, sceVideoOutOpen) { vo.open = 1; RET(1); }
IMP(libscevideoout, sceVideoOutClose) { vo.open = 0; RET(0); }
IMP(libscevideoout, sceVideoOutSetFlipRate) { RET(0); }
// (attribute, pixel format, tiling mode, width, height, option, DCC control, DCC clear colour)
IMP(libscevideoout, sceVideoOutSetBufferAttribute2) {
  Attr a = { A1, (u32)A2, (u32)A3, (u32)A4, (u32)A3, ATTR_MAGIC };
  memset((void *)GP(A0), 0, 80); memcpy((void *)GP(A0), &a, sizeof a);
  RET(0);
}
IMP(libscevideoout, sceVideoOutSetBufferAttribute) {   // (attribute, pixel format, tiling, aspect, width, height, pitch)
  Attr a = { A1, (u32)A2, (u32)A4, (u32)A5, (u32)SA(0), ATTR_MAGIC };
  memset((void *)GP(A0), 0, 80); memcpy((void *)GP(A0), &a, sizeof a);
  RET(0);
}
// (handle, set index, first buffer index, buffers: {data, metadata, reserved, reserved} each, count, attribute, category, option)
IMP(libscevideoout, sceVideoOutRegisterBuffers2) {
  u32 first = (u32)A2, n = (u32)A4; Attr a; memcpy(&a, (void *)GP(A5), sizeof a);
  if (a.magic != ATTR_MAGIC || first + n > MAX_BUFFERS) { RET(SCE_KERNEL_ERROR(EINVAL)); return; }
  pthread_mutex_lock(&vo_lock);
  for (u32 i = 0; i < n; i++) vo.data[first + i] = M64(A3 + 32ull * i);
  vo.width = a.width; vo.height = a.height; vo.pitch = a.pitch ? a.pitch : a.width; vo.tiling = a.tiling; vo.format = a.pixel_format;
  pthread_mutex_unlock(&vo_lock);
  if (rt_trace) rt_log("videoout: %u buffers of %ux%u (format 0x%llx, tiling %u) from index %u", n, a.width, a.height, (u64)a.pixel_format, a.tiling, first);
  RET(0);
}
IMP(libscevideoout, sceVideoOutRegisterBuffers) {   // (handle, first index, addresses, count, attribute)
  u32 first = (u32)A1, n = (u32)A3; Attr a; memcpy(&a, (void *)GP(A4), sizeof a);
  if (a.magic != ATTR_MAGIC || first + n > MAX_BUFFERS) { RET(SCE_KERNEL_ERROR(EINVAL)); return; }
  for (u32 i = 0; i < n; i++) vo.data[first + i] = M64(A2 + 8ull * i);
  vo.width = a.width; vo.height = a.height; vo.pitch = a.pitch ? a.pitch : a.width; vo.tiling = a.tiling; vo.format = a.pixel_format;
  RET(0);
}
// the buffer to the GPU back end, and presented
static void show(u32 index) {
  if (!vo.width || !vo.height || index >= MAX_BUFFERS || !vo.data[index]) return;
  if (!vo.texture) {
    // a 2D RGBA8 texture (DXGI_FORMAT_R8G8B8A8_UNORM, 28): the bytes as the app wrote them; an sRGB format's
    // values are already encoded, and the canvas shows them as they are
    vo.texture = 0x7001;
    u32 w[14] = { (u32)vo.texture, 2, 28, vo.width, 0, vo.height, 1, 1, 1, 0, 0, 0, 0, 0 };
    rt_gpu_rec_words(GPU_R_RESOURCE, w, 14);
  }
  u32 pitch = vo.pitch * 4, rows = (2u << 20) / pitch; if (!rows) rows = 1;
  const u8 *src = (const u8 *)GP(vo.data[index]);
  for (u32 y = 0; y < vo.height; y += rows) {
    u32 n = vo.height - y < rows ? vo.height - y : rows;
    u8 *p = rt_gpu_rec_begin(GPU_R_WRITE_TEXTURE, 36 + n * pitch); u32 *w = (u32 *)p;
    w[0] = (u32)vo.texture; w[1] = 0; w[2] = 0; w[3] = y; w[4] = 0; w[5] = vo.width; w[6] = n; w[7] = 1; w[8] = pitch;
    memcpy(p + 36, src + (size_t)y * pitch, (size_t)n * pitch);
    rt_gpu_rec_end_quiet();
  }
  rt_gpu_present((u32)vo.texture, ++vo.flips, 1);
  rt_clock_advance_frame();
  static int dump_raw = -1, max_frames;
  if (dump_raw < 0) {
    const char *d = getenv("SW_DUMP_RAW"), *m = getenv("SW_MAX_FRAMES");
    dump_raw = d && *d; max_frames = m && *m ? atoi(m) : 0;
  }
  if (dump_raw) host_frame_dump((u32)(uptr)src, vo.width, vo.height, (u32)vo.flips);
  if (max_frames && vo.flips >= (u32)max_frames) ps5_exit(0, 0);  /* Cpu unused */
}
// SW_SHOT_EVERY=<n>: every nth flip is saved by the page as shots/flip-<n>.png (a progress picture)
static u32 shot_every = ~0u;
// (handle, buffer index (-1: blank), flip mode, flip argument)
IMP(libscevideoout, sceVideoOutSubmitFlip) {
  s32 index = (s32)A1;
  if (index >= 0) {
    pthread_mutex_lock(&vo_lock); show((u32)index); pthread_mutex_unlock(&vo_lock);
    if (shot_every == ~0u) { const char *e = getenv("SW_SHOT_EVERY"); shot_every = e ? (u32)atoi(e) : 0; }
    if (shot_every && vo.flips % shot_every == 0) {
      char text[64]; int n = snprintf(text, sizeof text, "shot flip-%05llu", vo.flips);
      u8 *p = rt_gpu_rec_begin(GPU_R_MARK, (u32)n + 1); memcpy(p, text, (size_t)n); rt_gpu_rec_end();
    }
  }
  RET(0);
}
IMP(libscevideoout, sceVideoOutWaitVblank) {
  if (!rt_clock_virtual()) {
    const char *pace = getenv("SW_PACE");
    if (!pace || strcmp(pace, "off")) {
      u64 now = rt_uptime_us();
      if (next_vblank_us < now) next_vblank_us = now;
      next_vblank_us += 16667;
      u64 wait = next_vblank_us - now;
      if (wait > 0 && wait < 100000) { struct timespec t = {0, (long)wait * 1000}; nanosleep(&t, 0); }
    }
  }
  RET(0);
}
IMP(libscevideoout, sceVideoOutGetResolutionStatus) { if (A1) { memset((void *)GP(A1), 0, 32); M32(A1) = 1920; M32(A1 + 4) = 1080; M32(A1 + 8) = 1920; M32(A1 + 12) = 1080; } RET(0); }
void ps5_video_flush(void) { rt_gpu_drain(); }
