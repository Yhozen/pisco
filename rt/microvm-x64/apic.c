// SPDX-License-Identifier: GPL-2.0-or-later
// The local APIC (xAPIC, its registers in the page at APIC_BASE): its timer (one-shot, periodic, TSC-deadline),
// the interrupts it raises and holds in service until EOI, its priority. And the machine's clock: virtual, the
// same on every run. It moves by a few nanoseconds at each poll of lifted code (function entries and jumps back:
// rt/cpu.h POLL) and jumps to the next timer when the CPU halts. The TSC counts it at 1 GHz, and so does the
// APIC timer before its divider.
#include <time.h>
#include "machine.h"

int mc_budget;
enum { CHUNK = 256 };    // polls between two looks at the clock and the interrupts
#define POLL_NS 4        // the virtual time of a poll

static u32 irr[8], isr[8], tpr, svr = 0xFF, lvt_timer = 0x10000, lvt_lint0 = 0x10000, lvt_lint1 = 0x10000, lvt_error = 0x10000, lvt_pmc = 0x10000, lvt_thermal = 0x10000;
static u32 timer_initial, timer_divide, ldr, dfr = ~0u, esr, icr_lo, icr_hi;
static u64 timer_deadline = ~0ull, timer_period, tsc_deadline;

int mc_paced;
static u64 host_ns(void) { static u64 t0; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); u64 v = (u64)t.tv_sec * 1000000000ull + (u64)t.tv_nsec; if (!t0) t0 = v; return v - t0; }
u64 rt_rdtsc(void) {
  if (mc_paced) { u64 h = host_ns(); if (h > mc.now) mc.now = h; return mc.now; }   // someone types: the host's time
  int used = CHUNK - mc_budget; if (used < 0) used = 0; if (used > CHUNK) used = CHUNK; return mc.now + (u64)used * POLL_NS;
}

static int highest(const u32 *v) { for (int k = 7; k >= 0; k--) if (v[k]) return k * 32 + 31 - __builtin_clz(v[k]); return -1; }
static void raise_(int vec) { if (vec >= 16) irr[vec >> 5] |= 1u << (vec & 31); }
int apic_pending(void) {
  if (!(svr & 0x100)) return -1;
  int v = highest(irr); if (v < 0) return -1;
  int s = highest(isr);
  if (s >= 0 && (v >> 4) <= (s >> 4)) return -1;
  if ((v >> 4) <= (int)(tpr >> 4)) return -1;
  return v;
}
void apic_take(int v) { irr[v >> 5] &= ~(1u << (v & 31)); isr[v >> 5] |= 1u << (v & 31); }

static u64 divider(void) { u32 d = (timer_divide & 3) | ((timer_divide >> 1) & 4); return d == 7 ? 1 : 2ull << d; }
static void timer_arm(void) {
  u32 mode = (lvt_timer >> 17) & 3;
  if (mode == 2) { timer_deadline = tsc_deadline ? tsc_deadline : ~0ull; timer_period = 0; return; }
  if (!timer_initial) { timer_deadline = ~0ull; return; }
  timer_period = mode == 1 ? (u64)timer_initial * divider() : 0;
  timer_deadline = rt_rdtsc() + (u64)timer_initial * divider();
}
void apic_tick(void) {
  u64 now = rt_rdtsc();
  if (timer_deadline != ~0ull && now >= timer_deadline) {
    if (!(lvt_timer & 0x10000)) raise_(lvt_timer & 0xFF);
    if (((lvt_timer >> 17) & 3) == 2) { tsc_deadline = 0; timer_deadline = ~0ull; }
    else if (timer_period) { timer_deadline += timer_period; if (timer_deadline <= now) timer_deadline = now + timer_period; }
    else timer_deadline = ~0ull;
  }
}
u64 apic_next_deadline(void) { return timer_deadline; }
void apic_tsc_deadline(u64 v) { tsc_deadline = v; if (((lvt_timer >> 17) & 3) == 2) timer_deadline = v ? v : ~0ull; }
u64 apic_tsc_deadline_get(void) { return tsc_deadline; }

// ---- the IOAPIC at 0xfec00000: 24 pins, each a vector for the local APIC (edge or level, masked or not) ----
static u32 io_sel, io_id; static u64 redir[24]; static u8 line[24];
static void io_try(int pin) {
  u64 e = redir[pin];
  if ((e & 0x10000) || !line[pin]) return;                    // masked, or the line is low
  if (e & 0x8000) { if (e & 0x4000) return; redir[pin] |= 0x4000; }   // level: once until EOI (remote IRR)
  raise_((int)(e & 0xFF)); mc_budget = 0;
}
void ioapic_line(int pin, int level) {
  int was = line[pin]; line[pin] = (u8)level;
  if (level && (!was || (redir[pin] & 0x8000))) io_try(pin);
}
static void io_eoi(int vector) {
  for (int p = 0; p < 24; p++) if ((redir[p] & 0x8000) && (redir[p] & 0x4000) && (int)(redir[p] & 0xFF) == vector) { redir[p] &= ~0x4000ull; io_try(p); }
}
u32 ioapic_read(u32 off) {
  if (off == 0) return io_sel;
  if (off != 0x10) return 0;
  if (io_sel == 0) return io_id; if (io_sel == 1) return 0x00170011; if (io_sel == 2) return 0;
  if (io_sel >= 0x10 && io_sel < 0x10 + 48) { u64 e = redir[(io_sel - 0x10) / 2]; return (io_sel & 1) ? (u32)(e >> 32) : (u32)e; }
  return 0;
}
void ioapic_write(u32 off, u32 v) {
  if (off == 0) { io_sel = v & 0xFF; return; }
  if (off != 0x10) return;
  if (io_sel == 0) { io_id = v & 0x0F000000; return; }
  if (io_sel >= 0x10 && io_sel < 0x10 + 48) {
    int p = (int)(io_sel - 0x10) / 2;
    if (io_sel & 1) redir[p] = (redir[p] & 0xFFFFFFFFull) | (u64)v << 32;
    else { redir[p] = (redir[p] & ~0xFFFFFFFFull) | (v & ~0x5000u) | (redir[p] & 0x5000); io_try(p); }
  }
}

u32 apic_read(u32 r) {
  switch (r) {
  case 0x20: return 0;                // ID
  case 0x30: return 0x00050014;       // version: 6 LVT entries
  case 0x80: return tpr;
  case 0xA0: { int s = highest(isr); return s < 0 ? tpr : (u32)(s & 0xF0) > tpr ? (u32)(s & 0xF0) : tpr; }   // processor priority
  case 0xD0: return ldr; case 0xE0: return dfr; case 0xF0: return svr;
  case 0x280: return esr;
  case 0x300: return icr_lo & ~0x1000u; case 0x310: return icr_hi;
  case 0x320: return lvt_timer; case 0x330: return lvt_thermal; case 0x340: return lvt_pmc;
  case 0x350: return lvt_lint0; case 0x360: return lvt_lint1; case 0x370: return lvt_error;
  case 0x380: return timer_initial;
  case 0x390: { u64 now = rt_rdtsc(); if (timer_deadline == ~0ull || now >= timer_deadline || ((lvt_timer >> 17) & 3) == 2) return 0; return (u32)((timer_deadline - now) / divider()); }
  case 0x3E0: return timer_divide;
  }
  if (r >= 0x100 && r < 0x180) return isr[(r - 0x100) >> 4];
  if (r >= 0x200 && r < 0x280) return irr[(r - 0x200) >> 4];
  if (r >= 0x180 && r < 0x200) return 0;   // TMR
  return 0;
}
void apic_write(u32 r, u32 v) {
  switch (r) {
  case 0x80: tpr = v & 0xFF; mc_budget = 0; break;
  case 0xB0: { int s = highest(isr); if (s >= 0) { isr[s >> 5] &= ~(1u << (s & 31)); io_eoi(s); } mc_budget = 0; break; }   // EOI
  case 0xD0: ldr = v; break; case 0xE0: dfr = v; break;
  case 0xF0: svr = v; break;
  case 0x280: esr = 0; break;
  case 0x300: icr_lo = v;
    if (((v >> 18) & 3) == 1 || (((v >> 18) & 3) == 0 && (icr_hi >> 24) == 0)) { if (((v >> 8) & 7) == 0) { raise_(v & 0xFF); mc_budget = 0; } }   // to itself: fixed
    break;
  case 0x310: icr_hi = v; break;
  case 0x320: { u32 was = (lvt_timer >> 17) & 3; lvt_timer = v; if (((v >> 17) & 3) != was) timer_arm(); break; }
  case 0x330: lvt_thermal = v; break; case 0x340: lvt_pmc = v; break;
  case 0x350: lvt_lint0 = v; break; case 0x360: lvt_lint1 = v; break; case 0x370: lvt_error = v; break;
  case 0x380: timer_initial = v; timer_arm(); break;
  case 0x3E0: timer_divide = v; break;
  }
}

// lifted code polled: the clock moves on, and an interrupt that is due and allowed is taken here
void rt_poll(Cpu *c, u64 ip) {
  mc.now = rt_rdtsc(); mc_budget = CHUNK;
  apic_tick(); console_poll();
  if (!(mc_rflags & RFLAGS_IF)) return;
  int v = apic_pending(); if (v < 0) return;
  apic_take(v);
  cpu_deliver(c, (u32)v, 0, 0, ip, ip);
}
// hlt: until an interrupt. The clock jumps to the next timer; when someone types (or a pipe may still bring input),
// the machine waits for it, until the next timer at most (its time on the host's clock when paced)
void rt_hlt(Cpu *c, u64 next) {
  uart_flush();
  for (;;) {
    mc.now = rt_rdtsc(); mc_budget = CHUNK; apic_tick(); console_poll();
    int v = (mc_rflags & RFLAGS_IF) ? apic_pending() : -1;
    if (v >= 0) { apic_take(v); cpu_deliver(c, (u32)v, 0, 0, next, next); return; }
    u64 d = apic_next_deadline();
    int input = console && (!console->eof || console_pending()) && (mc_rflags & RFLAGS_IF);
    if (!(mc_rflags & RFLAGS_IF) || (d == ~0ull && !input)) rt_fatal("machine: the CPU halted at 0x%llx with nothing to wake it (interrupts %s)", next, mc_rflags & RFLAGS_IF ? "on, no timer" : "off");
    if (input && (mc_paced || d == ~0ull)) {
      u32 w = console->w;
      if (w == console->r) __builtin_wasm_memory_atomic_wait32((int *)&console->w, (int)w, d == ~0ull ? -1 : (s64)(d > mc.now ? d - mc.now : 0));
      if (!mc_paced && d != ~0ull && d > mc.now) mc.now = d;
      continue;
    }
    if (d > mc.now) mc.now = d;
  }
}

// ---- CPUID: an Intel CPU with what the lifter lifts (bases/microvm-x64), a TSC-deadline timer, the TSC's rate ----
void rt_cpuid(Cpu *c) {
  u32 leaf = (u32)R[RAX], sub = (u32)R[RCX], a = 0, b = 0, cx = 0, d = 0;
  switch (leaf) {
  case 0: a = 0x16; b = 0x756E6547; d = 0x49656E69; cx = 0x6C65746E; break;   // GenuineIntel
  case 1: a = 0x000906EA; b = 0x00010800;   // family 6; one logical CPU, CLFLUSH 64 bytes
    cx = 1u << 0 | 1u << 1 | 1u << 9 | 1u << 13 | 1u << 19 | 1u << 20 | 1u << 23 | 1u << 24;   // SSE3 PCLMUL SSSE3 CX16 SSE4.1 SSE4.2 POPCNT TSC-deadline
    d = 1u << 0 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 9 | 1u << 11 | 1u << 13 | 1u << 15 | 1u << 16 | 1u << 19 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26;
    break;   // FPU TSC MSR PAE CX8 APIC SEP PGE CMOV PAT CLFLUSH MMX FXSR SSE SSE2
  case 6: a = 1u << 2; break;   // ARAT: the APIC timer runs while the CPU halts
  case 7: if (sub == 0) d = 1u << 14; break;   // SERIALIZE (so that sync_core() needs no iretq to itself)
  case 0x15: a = 1; b = 1; cx = 1000000000; break;   // the TSC: 1 GHz, from a 1 GHz crystal
  case 0x16: a = 1000; b = 1000; cx = 1000; break;   // MHz
  case 0x80000000: a = 0x80000008; break;
  case 0x80000001: cx = 1u << 0; d = 1u << 11 | 1u << 20 | 1u << 29; break;   // LAHF; SYSCALL NX LM
  case 0x80000002: case 0x80000003: case 0x80000004: {
    static const char brand[48] = "pisco microvm-x64 (lifted)";
    const u32 *w = (const u32 *)(brand + 16 * (leaf - 0x80000002)); a = w[0]; b = w[1]; cx = w[2]; d = w[3]; break;
  }
  case 0x80000007: d = 1u << 8; break;   // the TSC never changes rate
  case 0x80000008: a = 0x3028; break;    // 40 physical, 48 virtual address bits
  }
  R[RAX] = a; R[RBX] = b; R[RCX] = cx; R[RDX] = d;
}
