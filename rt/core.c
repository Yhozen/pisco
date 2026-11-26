// SPDX-License-Identifier: GPL-2.0-or-later
// What every runtime needs whatever system it imitates: the log, guest memory, the virtual CPU (CPUID, the time
// stamp counter), the search of the lifted functions, random numbers and the end of the process.
#include <stdarg.h>
#include "rt.h"

void rt_log(const char *fmt, ...) {
  char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  host_log(buf, (u32)n);
}
void rt_fatal(const char *fmt, ...) {
  char buf[1024]; va_list ap; va_start(ap, fmt); int n = vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  host_log(buf, (u32)n);
  host_fault(5, 0);
}
void rt_trap(u64 a) { rt_fatal("guest trap (int3 / ud2 / divide error) at 0x%llx", a); }
void rt_unimpl(u64 a) { rt_fatal("instruction at 0x%llx is not lifted yet", a); }

// ---- memory: guest regions come from the top of linear memory; malloc shares it through sbrk ----
static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
u64 rt_valloc(u64 size, u64 align) {
  if (align < 4096) align = 4096;
  size = (size + 4095) & ~4095ull;
  pthread_mutex_lock(&mem_lock);
  void *p = aligned_alloc(align, size); // dlmalloc takes big blocks straight from sbrk
  pthread_mutex_unlock(&mem_lock);
  if (!p) return 0;
#if GUEST_BITS == 32
  // a 32-bit program that is not large-address-aware expects every pointer below 2 GiB
  if ((u64)(uptr)p + size > 0x80000000ull) { free(p); return 0; }
#endif
  memset(p, 0, size);
  return (u64)(uptr)p;
}
void rt_vfree(u64 a) { free((void *)GP(a)); }

// ---- lifted functions: an indirect call or jump finds its target by address ----
void rt_dispatch(Cpu *c, u64 t) {
  unsigned lo = 0, hi = fn_count;
  while (lo < hi) { unsigned m = (lo + hi) / 2; if (fn_addrs[m] < t) lo = m + 1; else hi = m; }
  if (lo < fn_count && fn_addrs[lo] == t) { fn_ptrs[lo](c); return; }
  rt_fatal("indirect call or jump to 0x%llx, which is no lifted function (return address 0x%llx)", t, (u64)MP(R[RSP]));
}

volatile u32 rt_last_pc;
// --trace-pc: the last 256 blocks entered, and SW_BREAK=<hex address>: when that block is entered, the registers,
// the top of the stack and the blocks before it are logged (the run goes on)
static u32 pc_ring[256], pc_at; static s64 break_at = -2;
void rt_pc(Cpu *c, u32 pc) {
  rt_last_pc = pc; pc_ring[pc_at++ & 255] = pc;
  if (break_at == -2) { const char *e = getenv("SW_BREAK"); break_at = e ? (s64)strtoull(e, 0, 16) : -1; }
  if ((s64)pc != break_at) return;
  rt_log("break at 0x%x: eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x", pc, (u32)R[0], (u32)R[1], (u32)R[2], (u32)R[3], (u32)R[4], (u32)R[5], (u32)R[6], (u32)R[7]);
  char line[200]; int n = 0;
  for (int i = 0; i < 8; i++) n += snprintf(line + n, sizeof line - n, " %08x", M32(R[4] + 4 * i));
  rt_log("  stack:%s", line);
  if (R[5] >= 0x1000) { n = 0; for (int i = 0; i < 8; i++) n += snprintf(line + n, sizeof line - n, " %08x", M32(R[5] + 4 * i)); rt_log("  [ebp]:%s", line); }
  n = 0; for (int i = 32; i >= 1; i--) n += snprintf(line + n, sizeof line - n, " %x", pc_ring[(pc_at - 1 - i) & 255]);
  rt_log("  blocks before:%s", line);
}

// ---- the virtual CPU: what CPUID reports decides which code paths a title takes ----
void rt_cpuid(Cpu *c) {
  u32 leaf = (u32)R[RAX], sub = (u32)R[RCX], a = 0, b = 0, d = 0, cx = 0;
  (void)sub;
  switch (leaf) {
  case 0: a = 0xD; b = 0x756E6547; d = 0x49656E69; cx = 0x6C65746E; break; // GenuineIntel
  case 1:
    a = 0x000906EA; b = (8u << 16) | (8u << 8);
    { unsigned f = rt_cpu_features; // the title's recipe may leave features out (an old title must not pick MMX paths)
      cx = (f & CPU_SSE3 ? 1u << 0 : 0) | (f & CPU_PCLMUL ? 1u << 1 : 0) | (f & CPU_SSSE3 ? 1u << 9 : 0) | (f & CPU_CX16 ? 1u << 13 : 0)
         | (f & CPU_SSE41 ? 1u << 19 : 0) | (f & CPU_SSE42 ? 1u << 20 : 0) | (f & CPU_POPCNT ? 1u << 23 : 0);
      d = (f & CPU_FPU ? 1u << 0 : 0) | (f & CPU_TSC ? 1u << 4 : 0) | (f & CPU_CX8 ? 1u << 8 : 0) | (f & CPU_CMOV ? 1u << 15 : 0)
        | (f & CPU_MMX ? 1u << 23 : 0) | (f & CPU_FXSR ? 1u << 24 : 0) | (f & CPU_SSE ? 1u << 25 : 0) | (f & CPU_SSE2 ? 1u << 26 : 0); }
    break;
  case 0x80000000: a = 0x80000004; break;
  case 0x80000001: cx = 1u << 0; d = (1u << 11) | (1u << 20) | (1u << 29); break; // LAHF; SYSCALL NX LM
  case 0x80000002: case 0x80000003: case 0x80000004: {
    static const char brand[48] = "pisco virtual CPU (lifted x86-64)";
    const u32 *w = (const u32 *)(brand + 16 * (leaf - 0x80000002)); a = w[0]; b = w[1]; cx = w[2]; d = w[3]; break;
  }
  }
  R[RAX] = a; R[RBX] = b; R[RCX] = cx; R[RDX] = d;
}
#if GUEST_BITS == 32
u64 rt_rdtsc(void) { return rt_uptime_us() * 3000; } // a 3 GHz counter on the title's clock
#else
u64 rt_rdtsc(void) { return (u64)(host_now_us() * 3000.0); }
#endif

// ---- the end of the process ----
HOST("exit") __attribute__((noreturn)) void host_exit(u32 code);
void (*rt_exit_hook)(void);
void rt_exit_process(u32 code) { void (*hook)(void) = rt_exit_hook; rt_exit_hook = 0; if (hook) hook(); host_exit(code); }

u64 rt_random64(void) { u64 v = 0; arc4random_buf(&v, sizeof v); return v; }
