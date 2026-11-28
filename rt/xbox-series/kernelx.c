// SPDX-License-Identifier: GPL-2.0-or-later
// kernelx, the console's kernel library: what it adds to kernel32 (console type, OS version, memory status,
// thread names). XMem is xmem.c. Behaviour follows XCL's findings on retail apps (github.com/ivall/xcl, GPL-2.0).
#include "../gdk/gdk.h"

// ---- kernelx ----
IMP(kernelx, GetConsoleType) { RET(6); }                       // Xbox Series X
IMP(kernelx, GetSystemOSVersion) { u64 p = ARG(0); M8(p) = 10; M8(p + 1) = 0; M16(p + 2) = 25398; M16(p + 4) = 0; }
IMP(kernelx, TitleMemoryStatus) {
  u64 p = ARG(0), total = 13824ull << 20;
  // dwLength, dwReserved, then ullTotalMem, ullAvailMem, legacy..., app, ...
  for (int i = 0; i < 9; i++) M64(p + 8 + 8 * i) = total / (i ? 2 : 1);
  RET(1);
}

IMP(kernelx, SetThreadName) {
  Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_THREAD) { RET(0); return; }
  free(o->thread->name); o->thread->name = rt_narrow(GWSTR(ARG(1)));
  RET(1);
}
IMP(kernelx, GetThreadName) { // (thread, buffer, length, &returned)
  Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0));
  if (!o || o->type != OBJ_THREAD) { RET(0); return; }
  const char *n = o->thread->name ? o->thread->name : "";
  u64 l = strlen(n), cap = ARG(2);
  for (u64 i = 0; i < l && i + 1 < cap; i++) GWSTR(ARG(1))[i] = (u8)n[i];
  if (cap) GWSTR(ARG(1))[l < cap ? l : cap - 1] = 0;
  if (ARG(3)) M64(ARG(3)) = l;
  RET(1);
}
