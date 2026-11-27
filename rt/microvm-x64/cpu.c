// SPDX-License-Identifier: GPL-2.0-or-later
// The machine's CPU, where lifted code cannot be it: exceptions, interrupts and system calls, their return
// (iretq, sysretq), the model-specific and control registers, the descriptor tables.
//
// An exception, an interrupt or a system call is a nested run: the frame goes on the kernel's stack as the CPU
// puts it there, and the kernel's handler runs (a Level, run.c) until it returns. When it returns to where it was
// raised, as almost always, the lifted function that raised it simply goes on, its registers and flags intact.
// When it returns elsewhere (a fix-up address, a signal handler, a new program), the code that raised it is
// abandoned, and the code there is entered by its address (run.c).
#include "machine.h"

Machine mc;
u64 mc_rflags = 2;
int mc_trace;

static int has_error_code(u32 v) { return v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30; }

void cpu_deliver(Cpu *c, u32 vector, int has_err, u32 err, u64 rip, u64 want) {
  if ((vector + 1) * 16 > (u32)mc.idt_limit + 1) rt_fatal("machine: vector %u is past the IDT (at 0x%llx)", vector, rip);
  u8 g[16]; vread(mc.idt_base + 16ull * vector, g, 16);
  u64 handler = (u64)(g[0] | g[1] << 8) | (u64)(g[6] | g[7] << 8) << 16 | (u64)(g[8] | g[9] << 8 | g[10] << 16 | (u32)g[11] << 24) << 32;
  unsigned ist = g[4] & 7, type = g[5] & 15;
  if (!(g[5] & 0x80)) rt_fatal("machine: vector %u has no handler (at 0x%llx)", vector, rip);
  if (mc_trace > 1) rt_log("machine: task %d: vector %u at 0x%llx (cpl %d) -> 0x%llx", co_cur->id, vector, rip, mc.cpl, handler);
  int old_cpl = mc.cpl; u64 old_rsp = R[RSP];
  u64 rsp = old_rsp;
  if (old_cpl == 3) rsp = vread64(mc.tss_base + 4);
  if (ist) rsp = vread64(mc.tss_base + 36 + 8ull * (ist - 1));
  rsp &= ~15ull;
  u64 frame[6]; int n = 0;
  frame[n++] = mc.seg[S_SS]; frame[n++] = old_rsp; frame[n++] = (mc_rflags & RFLAGS_SYS) | 2; frame[n++] = mc.seg[S_CS]; frame[n++] = rip;
  if (has_err) frame[n++] = err;
  for (int k = 0; k < n; k++) { rsp -= 8; vwrite64(rsp, frame[k]); }
  R[RSP] = rsp;
  mc.cpl = 0; mc.seg[S_CS] = (u16)(g[2] | g[3] << 8);
  if (old_cpl != 0) mc.seg[S_SS] = 0;
  if (type == 0xE) mc_rflags &= ~RFLAGS_IF;   // an interrupt gate
  mc_rflags &= ~0x14100ull;                   // TF, NT, RF
  Level l = { .want_rip = want, .alt_rip = rip, .want_cpl = old_cpl, .prev = co_cur->levels };
  co_cur->levels = &l;
  if (!setjmp(l.jb)) run_level(c, &l, handler);
  co_cur->levels = l.prev;
}

// an exception the code raises itself: int3 (a trap: the frame has the next instruction), int n, ud2 (a fault)
void rt_exc(Cpu *c, u32 vector, u32 err, u64 rip, u64 next) { cpu_deliver(c, vector, has_error_code(vector), err, rip, next); }

// a return to (rip, cpl): to the Level that waits for it (its raiser goes on), or else the innermost one is
// abandoned and the code there is entered by address in the loop below it
void cpu_resume_at(Cpu *c, u64 rip, int cpl) {
  if (mc_trace > 2) { rt_log("machine: task %d: back to 0x%llx (cpl %d); waiting:", co_cur->id, rip, cpl); for (Level *l = co_cur->levels; l; l = l->prev) rt_log("    0x%llx (cpl %d)", l->want_rip, l->want_cpl); }
  for (Level *l = co_cur->levels; l; l = l->prev)
    if (l->want_cpl == cpl && (l->want_rip == rip || l->alt_rip == rip)) {
      if (l->alt_rip == rip && l->want_rip != rip) rt_fatal("machine: a handler went back to 0x%llx, which cannot be done again", rip);
      co_cur->levels = l; longjmp(l->jb, 1);
    }
  Level *l = co_cur->levels;
  if (mc_trace) rt_log("machine: on at 0x%llx (cpl %d), not where it was interrupted", rip, cpl);
  // a program sent elsewhere while code of its waits below (a signal's handler, on the way back from a fault, an
  // interrupt or a system call): the code there runs on top, and the code that waits goes on when the kernel comes
  // back to it (sigreturn). (A new program after execve runs on top too; its old frames go with the task.)
  if (cpl == 3 && l && l->want_cpl == 3) {
    Level n = { .want_rip = ~0ull, .alt_rip = ~0ull, .want_cpl = -1, .prev = l };
    co_cur->levels = &n;
    run_level(c, &n, rip);
  }
  // else (a fix-up in the kernel, or nothing waits): what was interrupted is abandoned
  if (l) { co_cur->levels = l->prev; if (l->prev) { l->prev->loop_pc = rip; longjmp(l->prev->loop, 1); } }
  co_cur->root_pc = rip; longjmp(co_cur->root, 1);
}

void rt_iret(Cpu *c) {
  u64 f[5]; vread(R[RSP], f, sizeof f);   // rip, cs, rflags, rsp, ss
  mc.seg[S_CS] = (u16)f[1]; mc.seg[S_SS] = (u16)f[4]; mc.cpl = (int)(f[1] & 3);
  u64 was = mc_rflags; mc_rflags = (f[2] & RFLAGS_SYS) | 2;
  if (!(was & RFLAGS_IF) && (mc_rflags & RFLAGS_IF)) mc_budget = 0;
  R[RSP] = f[3];
  cpu_resume_at(c, f[0], mc.cpl);
}

static u64 sysret_flags;   // what the last sysret loaded into RFLAGS, arithmetic bits too
void rt_sysret(Cpu *c) {
  u64 rip = R[RCX]; sysret_flags = R[11];
  u64 was = mc_rflags; mc_rflags = (R[11] & RFLAGS_SYS) | 2;
  if (!(was & RFLAGS_IF) && (mc_rflags & RFLAGS_IF)) mc_budget = 0;
  mc.cpl = 3; mc.seg[S_CS] = (u16)(((mc.star >> 48) + 16) | 3); mc.seg[S_SS] = (u16)(((mc.star >> 48) + 8) | 3);
  cpu_resume_at(c, rip, 3);
}

void rt_syscall_m(Cpu *c, u64 next, Flags *f) {
  if (!(mc.efer & 1)) { cpu_deliver(c, 6, 0, 0, next - 2, next); return; }
  if (mc_trace > 2) rt_log("machine: task %d: system call %llu at 0x%llx", co_cur->id, R[RAX], next);
  R[RCX] = next; R[11] = flags_get64(f) | (mc_rflags & RFLAGS_SYS);
  mc.cpl = 0; mc.seg[S_CS] = (u16)((mc.star >> 32) & 0xFFFC); mc.seg[S_SS] = (u16)(((mc.star >> 32) & 0xFFFC) + 8);
  mc_rflags = ((mc_rflags & ~mc.fmask) & RFLAGS_SYS) | 2;
  Level l = { .want_rip = next, .alt_rip = next, .want_cpl = 3, .prev = co_cur->levels };
  co_cur->levels = &l;
  if (!setjmp(l.jb)) run_level(c, &l, mc.lstar);
  co_cur->levels = l.prev;
  u64 v = sysret_flags;   // the program's flags, as sysret put them back
  f->cf = v & 1; f->pf = (v >> 2) & 1; f->af = (v >> 4) & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->df = (v >> 10) & 1; f->of = (v >> 11) & 1;
}

void flags_set64(Cpu *c, Flags *f, u64 v) {
  (void)c;
  f->cf = v & 1; f->pf = (v >> 2) & 1; f->af = (v >> 4) & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->df = (v >> 10) & 1; f->of = (v >> 11) & 1;
  u64 was = mc_rflags; mc_rflags = (v & RFLAGS_SYS) | 2;
  if (!(was & RFLAGS_IF) && (mc_rflags & RFLAGS_IF)) mc_budget = 0;
}
// sti: interrupts are taken after the next instruction (so `sti; hlt` waits): at the next poll
void rt_sti(Cpu *c) { (void)c; if (!(mc_rflags & RFLAGS_IF)) { mc_rflags |= RFLAGS_IF; mc_budget = 0; } }

void rt_swapgs(Cpu *c) { u64 t = c->gs_base; c->gs_base = mc.kernel_gs; mc.kernel_gs = t; }

// ---- model-specific registers ----
static void unknown_msr(const char *what, u32 n, u64 ip) {
  static u32 told[64]; static unsigned k;
  for (unsigned i = 0; i < k; i++) if (told[i] == n) return;
  if (k < 64) told[k++] = n;
  rt_log("machine: %s of MSR 0x%x at 0x%llx is not imitated (read as 0, written to nothing)", what, n, ip);
}
void rt_wrmsr(Cpu *c, u64 ip) {
  u32 n = (u32)R[RCX]; u64 v = (u64)(u32)R[RDX] << 32 | (u32)R[RAX];
  switch (n) {
  case 0xC0000080: mc.efer = (v & ~0x400ull) | (mc.efer & 0x400); break;   // (LMA stays: the machine is always in long mode)
  case 0xC0000081: mc.star = v; break;
  case 0xC0000082: mc.lstar = v; break;
  case 0xC0000083: mc.cstar = v; break;
  case 0xC0000084: mc.fmask = v; break;
  case 0xC0000100: c->fs_base = v; break;
  case 0xC0000101: c->gs_base = v; break;
  case 0xC0000102: mc.kernel_gs = v; break;
  case 0xC0000103: mc.tsc_aux = v; break;
  case 0x1B: mc.apic_base = (v & ~0xFFFull) | (v & 0xD00); break;
  case 0x6E0: apic_tsc_deadline(v); break;
  case 0x277: mc.pat = v; break;
  case 0x1A0: mc.misc_enable = v; break;
  case 0x10: case 0x8B: break;   // the TSC itself (the virtual clock does not move back); the microcode's revision
  default: unknown_msr("a write", n, ip);
  }
}
void rt_rdmsr(Cpu *c, u64 ip) {
  u32 n = (u32)R[RCX]; u64 v = 0;
  switch (n) {
  case 0xC0000080: v = mc.efer; break;
  case 0xC0000081: v = mc.star; break;
  case 0xC0000082: v = mc.lstar; break;
  case 0xC0000083: v = mc.cstar; break;
  case 0xC0000084: v = mc.fmask; break;
  case 0xC0000100: v = c->fs_base; break;
  case 0xC0000101: v = c->gs_base; break;
  case 0xC0000102: v = mc.kernel_gs; break;
  case 0xC0000103: v = mc.tsc_aux; break;
  case 0x1B: v = mc.apic_base; break;
  case 0x6E0: v = apic_tsc_deadline_get(); break;
  case 0x277: v = mc.pat; break;
  case 0x1A0: v = mc.misc_enable; break;
  case 0x10: v = rt_rdtsc(); break;
  case 0x8B: v = 0xFFFFFFFF00000000ull; break;   // the microcode's revision: newer than any erratum asks
  default: unknown_msr("a read", n, ip);
  }
  R[RAX] = (u32)v; R[RDX] = (u32)(v >> 32);
}

// ---- control registers ----
void rt_wcr(Cpu *c, unsigned n, u64 v) {
  (void)c;
  switch (n) {
  case 0: if (v != mc.cr0) mmu_flush(); mc.cr0 = v | 0x80000001ull; break;   // (paging and protection stay on)
  case 2: mc.cr2 = v; break;
  case 3: mc.cr3 = v; mmu_flush(); machine_programs_forget(); break;
  case 4: if (v != mc.cr4) mmu_flush(); mc.cr4 = v | 0x20; break;
  case 8: apic_write(0x80, (u32)(v & 15) << 4); break;
  default: rt_fatal("machine: a write of CR%u", n);
  }
}
u64 rt_rcr(Cpu *c, unsigned n) {
  (void)c;
  switch (n) { case 0: return mc.cr0; case 2: return mc.cr2; case 3: return mc.cr3; case 4: return mc.cr4; case 8: return apic_read(0x80) >> 4; }
  rt_fatal("machine: a read of CR%u", n);
}

// ---- descriptor tables and segments ----
void rt_lgdt(Cpu *c, u64 a) { (void)c; mc.gdt_limit = vread16(a); mc.gdt_base = vread64(a + 2); }
void rt_lidt(Cpu *c, u64 a) { (void)c; mc.idt_limit = vread16(a); mc.idt_base = vread64(a + 2); }
void rt_sgdt(Cpu *c, u64 a) { u16 l = mc.gdt_limit; u64 b = mc.gdt_base; MW16(a, l); MW64(a + 2, b); }
void rt_sidt(Cpu *c, u64 a) { u16 l = mc.idt_limit; u64 b = mc.idt_base; MW16(a, l); MW64(a + 2, b); }
void rt_ltr(Cpu *c, u16 sel) {
  (void)c; u8 d[16]; vread(mc.gdt_base + (sel & ~7u), d, 16);
  mc.tr = sel;
  mc.tss_base = (u64)(d[2] | d[3] << 8 | d[4] << 16 | (u32)d[7] << 24) | (u64)(d[8] | d[9] << 8 | d[10] << 16 | (u32)d[11] << 24) << 32;
}
u16 rt_str(Cpu *c) { (void)c; return mc.tr; }
void rt_wseg(Cpu *c, unsigned seg, u16 sel) { (void)c; mc.seg[seg] = sel; }
u16 rt_rseg(Cpu *c, unsigned seg) { (void)c; return seg == S_CS ? (u16)(mc.seg[S_CS] ? mc.seg[S_CS] : 0x08) : mc.seg[seg]; }
