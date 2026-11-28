// SPDX-License-Identifier: GPL-2.0-or-later
// D3D12.X, the console's additions to D3D12 (rt/gdk/d3d12.c): the frame clock (device #78-#80) and the command
// list's own methods. Slots and behaviour follow XCL's findings on retail apps (github.com/ivall/xcl, GPL-2.0).
#include "../gdk/d3d12.h"

// ---- the console's frame clock (device #78-#80): origins every interval, tokens count them ----
static u64 frame_interval, frame_base, frame_last;
IMP(d3d12, Device_SetFrameIntervalX) { // (output, interval µs, periodicity, flags)
  frame_interval = (u32)ARG(2); if (!frame_base) frame_base = (u64)host_now_us();
  rt_log("d3d12: frame interval %llu µs", frame_interval); RET(S_OK);
}
IMP(d3d12, Device_ScheduleFrameEventX) { RET(frame_interval ? S_OK : E_INVALIDARG); } // (type, offset µs, ancillary, flags)
IMP(d3d12, Device_WaitFrameEventX) { // (type, timeout ms, ancillary, flags, &token)
  if (!frame_interval) { RET(E_INVALIDARG); return; }
  u64 k;
  if (d3d12_pace_off()) k = ++frame_last;
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


// ---- the console list's own methods (XCL's slots): memory as the GPU sees it, in order with the list's work ----
static void mem_write(Cpu *c, u64 addr, u64 v, u32 bytes, u32 stamp) { LIST; u32 *p = d3d12_emit(self, GPU_L_MEM_WRITE, 6); p[0] = (u32)addr; p[1] = (u32)(addr >> 32); p[2] = (u32)v; p[3] = (u32)(v >> 32); p[4] = bytes; p[5] = stamp; }
IMP(d3d12, CommandList_Write32BitValueBottomOfPipeX) { mem_write(c, ARG(1), (u32)ARG(2), 4, 0); } // #69 (address, value, flags, ?)
IMP(d3d12, CommandList_Write64BitValueBottomOfPipeX) { mem_write(c, ARG(1), ARG(2), 8, ARG(4) == 1); } // #70 (address, value, flags, mode: 1 = a GPU timestamp)
IMP(d3d12, CommandList_Write64BitValueTopOfPipeX) { mem_write(c, ARG(1), ARG(2), 8, 0); } // #89 (address, value, flags)
IMP(d3d12, CommandList_FillMemoryWith32BitValueX) { // #61 (address, bytes, value)
  LIST; u32 *p = d3d12_emit(self, GPU_L_MEM_FILL, 5); u64 a = ARG(1), n = ARG(2); p[0] = (u32)a; p[1] = (u32)(a >> 32); p[2] = (u32)n; p[3] = (u32)(n >> 32); p[4] = (u32)ARG(3);
}
IMP(d3d12, CommandList_CopyMemoryToMemoryX) { // #60 (destination, source, bytes, flags)
  LIST; u32 *p = d3d12_emit(self, GPU_L_MEM_COPY, 6); u64 d = ARG(1), s = ARG(2), n = ARG(3);
  p[0] = (u32)d; p[1] = (u32)(d >> 32); p[2] = (u32)s; p[3] = (u32)(s >> 32); p[4] = (u32)n; p[5] = (u32)(n >> 32);
}
static void mem_wait(Cpu *c, u64 addr, u32 func, u64 ref, u64 mask, u32 bytes) {
  LIST; u32 *p = d3d12_emit(self, GPU_L_MEM_WAIT, 8);
  p[0] = (u32)addr; p[1] = (u32)(addr >> 32); p[2] = func; p[3] = (u32)ref; p[4] = (u32)(ref >> 32); p[5] = (u32)mask; p[6] = (u32)(mask >> 32); p[7] = bytes;
}
IMP(d3d12, CommandList_Wait32BitValueX) { mem_wait(c, ARG(1), (u32)ARG(2), (u32)ARG(3), (u32)ARG(4), 4); } // #71 (address, comparison, reference, mask, flags)
IMP(d3d12, CommandList_Wait64BitValueX) { mem_wait(c, ARG(1), (u32)ARG(2), ARG(3), ARG(4), 8); }        // #72
IMP(d3d12, CommandList_DrawIndexedX) { // #81 (index address, format, index count, ?): indices from memory; the bound buffer stays
  LIST; u32 *p = d3d12_emit(self, GPU_L_DRAW_INDEXED_X, 4); u64 a = ARG(1); p[0] = (u32)a; p[1] = (u32)(a >> 32); p[2] = (u32)ARG(2); p[3] = (u32)ARG(3);
}
IMP(d3d12, CommandList_RSSetDepthBiasX) { // #94 (bias, clamp, slope) in xmm1..3
  LIST; u32 *p = d3d12_emit(self, GPU_L_DEPTH_BIAS, 3); float v[3] = {X[1].ps[0], X[2].ps[0], X[3].ps[0]}; memcpy(p, v, 12);
}
// #93: the work recorded so far goes to the GPU now; recording goes on in the same list
IMP(d3d12, CommandList_KickoffX) { LIST; d3d12_execute_words(0, self); self->kick = self->n; __atomic_fetch_add(&ring[GPU_H_SUBMITTED], 1, __ATOMIC_RELAXED); }
IMP(d3d12, CommandList_UavBarrierX) {} // #146: work runs in order here
IMP(d3d12, CommandList_AllocateUploadMemoryX) { // #148 (bytes, alignment): GPU-visible memory the list's work may read, until its Reset
  LIST; u64 n = ARG(1), al = ARG(2) ? ARG(2) : 256;
  u64 at = (self->up + al - 1) & ~(al - 1);
  if (!self->up_base || at + n > self->up_end) { // a new chunk (the old one may still be read by work in flight)
    u64 sz = n + al > (1u << 20) ? n + al : 1u << 20; self->up_base = self->up = rt_gfx_alloc(sz, 65536, 3); self->up_end = self->up + sz; at = (self->up + al - 1) & ~(al - 1); }
  self->up = at + n; RET(at);
}
IMP(d3d12, CommandList_SetPredication) {}
