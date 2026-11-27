// SPDX-License-Identifier: GPL-2.0-or-later
// libkernel (memory, time, threads, sysctl), the C library's functions the system exports (libSceLibcInternal),
// and the system and user services a title starts with.
#include "ps5.h"

// ---- memory: direct memory is guest memory; an allocation's "physical address" is where it is mapped ----
#define DIRECT_MEMORY (4ull << 30)   // what the system reports a title may allocate
IMP(libkernel, sceKernelGetDirectMemorySize) { RET(DIRECT_MEMORY); }
// (search start, search end, length, alignment, memory type, &physical address)
IMP(libkernel, sceKernelAllocateDirectMemory) {
  u64 len = A2, align = A3 < 65536 ? 65536 : A3, out = A5;
  u64 p = rt_valloc(len, align);
  if (!p) { RET(SCE_KERNEL_ERROR(ENOMEM)); return; }
  if (out) M64(out) = p;
  RET(0);
}
// (&address, length, protection, flags, physical address, alignment): mapped where it was allocated
IMP(libkernel, sceKernelMapDirectMemory) {
  if (A0) M64(A0) = A4;
  RET(A4 ? 0 : SCE_KERNEL_ERROR(EINVAL));
}
IMP(libkernel, sceKernelReleaseDirectMemory) { RET(0); }

// ---- time ----
IMP(libkernel, sceKernelGetProcessTime) { rt_clock_polled(); RET(rt_uptime_us()); }
IMP(libkernel, sceKernelGetProcessTimeCounter) { rt_clock_polled(); RET(rt_uptime_us()); }
IMP(libkernel, sceKernelGetProcessTimeCounterFrequency) { RET(1000000); }
static void sleep_us(u64 us) {
  if (rt_clock_virtual()) { rt_clock_advance(us); sched_yield(); return; }
  struct timespec t = {(time_t)(us / 1000000), (long)(us % 1000000) * 1000}; nanosleep(&t, 0);
}
IMP(libkernel, sceKernelUsleep) { sleep_us((u32)A0); RET(0); }
IMP(libkernel, usleep) { sleep_us((u32)A0); RET(0); }

// ---- threads ----
// (&thread, attributes, entry, argument, name)
IMP(libkernel, scePthreadCreate) {
  char name[32] = ""; if (A4) snprintf(name, sizeof name, "%s", GSTR(A4));
  u64 t = ps5_thread_create(A2, A3, name);
  if (!t) { RET(SCE_KERNEL_ERROR(EAGAIN)); return; }
  if (A0) M64(A0) = t;
  RET(0);
}
IMP(libkernel, scePthreadJoin) { u64 r = 0; int e = ps5_thread_join(A0, &r); if (!e && A1) M64(A1) = r; RET(e ? SCE_KERNEL_ERROR(ESRCH) : 0); }
IMP(libkernel, scePthreadYield) { sched_yield(); if (rt_clock_virtual()) rt_clock_polled(); RET(0); }
IMP(libkernel, scePthreadSelf) { RET((u64)(uptr)c); }

// ---- the system, as a title asks for it by name ----
static const char model[] = "PISCO WEBASSEMBLY - LIFTED X86-64";
static int sysctl_out(u64 old, u64 lenp, const void *v, size_t n) {
  if (!lenp) return -1;
  size_t have = (size_t)M64(lenp);
  if (old) { if (have < n) n = have; memcpy((void *)GP(old), v, n); }
  M64(lenp) = n;
  return 0;
}
// (name, old value, &old length, new value, new length). No path: a title cannot take the browser for a console
IMP(libkernel, sysctlbyname) {
  const char *n = GSTR(A0); int r = -1;
  if (!strcmp(n, "hw.model")) r = sysctl_out(A1, A2, model, sizeof model);
  else if (!strcmp(n, "hw.ncpu")) { int v = 8; r = sysctl_out(A1, A2, &v, 4); }
  else if (!strcmp(n, "hw.physmem")) { u64 v = 4ull << 30; r = sysctl_out(A1, A2, &v, 8); }
  if (rt_trace) rt_log("sysctlbyname(%s) -> %d", n, r);
  RET(r ? (u64)-1 : 0);
}
// (SceKernelSwVersion*: size, a version string of 28 bytes, the version as a number)
IMP(libkernel, sceKernelGetSystemSwVersion) { if (A0) { memset((void *)GP(A0 + 8), 0, 28); strcpy(GSTR(A0 + 8), "PISCO"); M32(A0 + 36) = 0; } RET(0); }
IMP(libkernel, write) {
  int fd = (int)A0; u64 n = A2;
  if (fd != 1 && fd != 2) { RET((u64)-1); return; }
  RET((u64)write(fd, (const void *)GP(A1), (size_t)n));
}
IMP(libkernel, getpid) { RET(1); }

// ---- libSceLibcInternal: the C library, on guest memory (which is the runtime's) ----
IMP(libscelibcinternal, _init_env) { RET(0); }
IMP(libscelibcinternal, strlen) { RET(strlen(GSTR(A0))); }
IMP(libscelibcinternal, strcmp) { RET((u64)(s64)strcmp(GSTR(A0), GSTR(A1))); }
IMP(libscelibcinternal, strncmp) { RET((u64)(s64)strncmp(GSTR(A0), GSTR(A1), (size_t)A2)); }
IMP(libscelibcinternal, memset) { memset((void *)GP(A0), (int)A1, (size_t)A2); RET(A0); }
IMP(libscelibcinternal, memcpy) { memcpy((void *)GP(A0), (const void *)GP(A1), (size_t)A2); RET(A0); }
IMP(libscelibcinternal, memmove) { memmove((void *)GP(A0), (const void *)GP(A1), (size_t)A2); RET(A0); }
IMP(libscelibcinternal, memcmp) { RET((u64)(s64)memcmp((const void *)GP(A0), (const void *)GP(A1), (size_t)A2)); }
IMP(libscelibcinternal, malloc) { RET((u64)(uptr)malloc((size_t)A0)); }
IMP(libscelibcinternal, calloc) { RET((u64)(uptr)calloc((size_t)A0, (size_t)A1)); }
IMP(libscelibcinternal, realloc) { RET((u64)(uptr)realloc((void *)GP(A0), (size_t)A1)); }
IMP(libscelibcinternal, free) { free((void *)GP(A0)); }
IMP(libscelibcinternal, posix_memalign) {
  void *p = 0; int r = posix_memalign(&p, (size_t)A1, (size_t)A2);
  if (!r && A0) M64(A0) = (u64)(uptr)p;
  RET((u64)r);
}
// atexit: the handlers run at exit, the last registered first
static u64 exit_fns[32]; static int exit_count;
IMP(libscelibcinternal, atexit) { if (exit_count < 32) exit_fns[exit_count++] = A0; RET(0); }
IMP(libscelibcinternal, exit) {
  int status = (int)A0;
  while (exit_count > 0) { u64 fn = exit_fns[--exit_count]; if (fn) ps5_guest_call(c, fn, 0, 0); }
  ps5_exit(c, status);
}
IMP(libkernel, _exit) { ps5_exit(c, (int)A0); }

// ---- services ----
IMP(libscesystemservice, sceSystemServiceHideSplashScreen) { RET(0); }
IMP(libsceuserservice, sceUserServiceInitialize) { RET(0); }
// the one user: the console's first user id
IMP(libsceuserservice, sceUserServiceGetInitialUser) { if (A0) M32(A0) = 0x10000001; RET(0); }
