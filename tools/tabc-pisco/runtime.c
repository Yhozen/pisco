// SPDX-License-Identifier: GPL-2.0-or-later
// tabc-pisco's runtime (docs/research/tabc-pisco.md): x86-64 programs lifted by PISCO, run as tasks of unkernel's
// kernel (Linux built for wasm), as tabc ships it. The kernel's one entry into user code, kw_run (include/kw_emu.h,
// ABI 4), is patched (patch-kernel.mjs) to ask this module first: a task whose kw_regs are marked x86-64 runs here,
// on lifted code, instead of on kwemu. Started as experiment 2 of docs/research/microvm-speed.md.
//
// The contract kept from kw_emu.h: kw_run(cpu, regs) runs user code until an event, leaves the task's whole state
// in regs, and returns the event (a system call, a fault it cannot take, the budget, an interrupt). The x86-64
// registers live in kw_regs between runs (layout below), so that fork copies them and exec drops them.
// - memory: lifted code's software TLB (rt/cpu.h MR/MWR, mmu_u); a miss asks the kernel's kw_user_addr, which walks
//   the task's page tables and faults the page in synchronously, as copy_from_user does. The kernel's TLB flushes
//   reach pisco_flush (patched in beside kwemu's).
// - system calls: x86-64's numbers and arguments put in arm64's registers (x8, x0..x5), then kw_run returns
//   KW_EV_SYSCALL; on the next entry x0 is RAX. The lifted code is entered again at the return address, a resume
//   point of its function (each `syscall` is one in a machine's program).
// - polls (each function's start and each jump back): the instruction budget and the kernel's pending interrupts;
//   kw_run returns at a poll's address, which is enterable too.
// - programs: each lifted on its own into a side module, found by the hash of the code page that runs; one met for
//   the first time is rebuilt from memory and handed to the host (program_image), which lifts it or has it already.
#include <setjmp.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "cpu.h"

#define IMPORT(m, n) __attribute__((import_module(m), import_name(n)))
#define EXPORT(n) __attribute__((export_name(n)))
IMPORT("kernel", "kw_user_addr") u64 k_user_addr(u64 uaddr, u64 len, int write);
IMPORT("host", "log") void host_log(const char *s, u32 n);
IMPORT("host", "now_ns") u64 host_now_ns(void);

// ---- kw_emu.h (ABI 4) as the wasm64-built kernel lays it out: pointers are 8 bytes ----
struct kw_event { u32 kind, access; u64 addr, insn; };
struct kw_cpu { u64 phys_base, pgd, irq_pending, budget, icount; struct kw_event ev; u32 jit_busy, flush_all; };
struct kw_regs { u64 x[31]; u64 sp, pc; u32 nzcv, fpcr, fpsr, pad; u64 tpidr; u64 v[32][2]; };
_Static_assert(offsetof(struct kw_regs, pc) == 256 && offsetof(struct kw_regs, tpidr) == 280 && offsetof(struct kw_regs, v) == 288, "kw_regs");
_Static_assert(offsetof(struct kw_cpu, ev) == 40 && offsetof(struct kw_cpu, jit_busy) == 64, "kw_cpu");
enum { EV_BUDGET, EV_SYSCALL, EV_FAULT, EV_ILLEGAL, EV_BREAKPOINT, EV_MISALIGNED, EV_INTERRUPT };

// ---- an x86-64 task's registers in kw_regs ----
// pc: MARK | rip (no aarch64 code can be above 2^48: kwemu faults there). x[9 + i]: r[i] (RSP is sp). x[25]: RFLAGS'
// arithmetic bits; x[26]: MXCSR | FCW << 32; x[27]: FSW | ftop << 16; x[28]: gs_base; x[29]: the system call this task
// waits for, + 1 (0: none). tpidr_el0: fs_base (so clone's CLONE_SETTLS sets it). v[0..15]: xmm; v[16..23]: the x87
// registers; v[24..27]: MMX. x0..x5, x8: a system call's arguments and number, as arm64's.
#define MARK (1ull << 63)
#define XR(i) (9 + (i))
enum { X_TAG = 13, X_FLAGS = 25, X_FP = 26, X_X87 = 27, X_GS = 28, X_PEND = 29, X_NEXT = 30 };
// x[13] (RSP's slot, unused): X64_TAG for every x86-64 task. Signal delivery keeps x9..x28 (arm64 sets x0..x2, x29,
// x30, sp and pc), fork copies them, exec clears them: so the tag says which CPU a task runs on. x[30]: the address a
// pending system call returns to (a restart backs pc up by 4 from it).
#define X64_TAG 0x5049534358363400ull

typedef struct Ctx {
  Cpu cpu; jmp_buf exit, root; volatile u64 root_pc, exit_rip; volatile u32 exit_ev;
  struct kw_cpu *kc; struct kw_regs *kr; u64 rflags; int budget_polls; int depth;
} Ctx;
static Ctx *cur;
static const ProgramDef *progs[64]; static unsigned n_progs;

static void logf_(const char *fmt, ...) { char b[512]; va_list ap; va_start(ap, fmt); int n = vsnprintf(b, sizeof b, fmt, ap); va_end(ap); if (n > (int)sizeof b - 1) n = sizeof b - 1; host_log(b, (u32)n); }
static int trace;
EXPORT("pisco_trace") void pisco_trace(int t) { trace = t; }

// statistics (pisco_stats)
static u64 st_unwound, st_runs, st_syscalls, st_polls_out, st_misses, st_faults_in, st_flushes, st_reenter;

static void load(Cpu *c, const struct kw_regs *k) {
  memset(c, 0, sizeof *c);
  for (int i = 0; i < 16; i++) c->r[i] = i == RSP ? k->sp : k->x[XR(i)];
  memcpy(c->x, k->v, 16 * 16);
  for (int i = 0; i < 8; i++) memcpy(&c->st[i], k->v[16 + i], 16);
  memcpy(c->mm, k->v[24], 64);
  c->mxcsr = (u32)k->x[X_FP]; c->fcw = (u16)(k->x[X_FP] >> 32);
  c->fsw = (u16)k->x[X_X87]; c->ftop = (u32)(k->x[X_X87] >> 16) & 7;
  c->gs_base = k->x[X_GS]; c->fs_base = k->tpidr;
}
static void store(const Cpu *c, struct kw_regs *k, u64 rip, u64 rflags) {
  for (int i = 0; i < 16; i++) if (i != RSP) k->x[XR(i)] = c->r[i];
  k->sp = c->r[RSP]; k->pc = MARK | rip;
  memcpy(k->v, c->x, 16 * 16);
  for (int i = 0; i < 8; i++) memcpy(k->v[16 + i], &c->st[i], 16);
  memcpy(k->v[24], c->mm, 64);
  k->x[X_FLAGS] = rflags; k->x[X_FP] = c->mxcsr | (u64)c->fcw << 32; k->x[X_X87] = c->fsw | (u64)c->ftop << 16;
  k->x[X_GS] = c->gs_base; k->tpidr = c->fs_base;
}

__attribute__((noreturn)) static void leave(Ctx *x, u32 ev, u64 rip) { x->exit_ev = ev; x->exit_rip = rip; longjmp(x->exit, 1); }
__attribute__((noreturn)) static void fatal(Ctx *x, u64 rip, const char *what) { logf_("pisco: %s at 0x%llx", what, rip); leave(x, EV_ILLEGAL, rip); }

// ---- memory: the TLB rt/cpu.h's MR / MWR check, filled by the kernel's kw_user_addr ----
TlbEntry mmu_k[4096], mmu_u[4096];
int mc_budget; u64 mc_rflags = 2;
static void programs_forget(void);
static int code_in(u64 a, u64 b);
static u64 cur_pgd;
static const struct ProgramDef *prog_cur;
// kind 1: a range (a, b); 2: all; 3: a new address space (kw_set_pgd)
static u64 st_fk[4];
EXPORT("pisco_flush") void pisco_flush(u32 kind, u64 a, u64 b) {
  if (kind < 4) st_fk[kind]++;
  if (kind == 1 && b > a && b - a <= 64 * 4096) {
    if (code_in(a, b)) programs_forget();
    for (u64 p = a & ~0xFFFull; p < b; p += 4096) { TlbEntry *e = &mmu_u[(p >> 12) & 4095]; u64 tag = (p >> 12) + 1; if (e->rtag == tag || e->wtag == tag) *e = (TlbEntry){0}; }
    return;
  }
  memset(mmu_u, 0, sizeof mmu_u); st_flushes++;
  if (kind == 3) { cur_pgd = a; prog_cur = 0; } else programs_forget();
}
// the page of a for a read or a write, made present (and dirty) by the kernel; a fault it cannot take ends the run
static void fill(Cpu *c, TlbEntry *t, u64 a, int write, u64 ip) {
  Ctx *x = cur; int polls = mc_budget; st_misses++;
  u64 page = a & ~0xFFFull;
  u64 p = k_user_addr(page, 1, write);   // (may fault the page in, and may sleep: another task can run meanwhile)
  cur = x; mc_budget = polls;
  if (!p) {
    if (trace) logf_("pisco: fault at 0x%llx (%s) at 0x%llx", a, write ? "write" : "read", ip);
    x->kc->ev.kind = EV_FAULT; x->kc->ev.access = write ? 1 : 0; x->kc->ev.addr = a;
    leave(x, EV_FAULT, ip);   // (the kernel's fault path decides: a signal; the instruction is not run again)
  }
  TlbEntry *e = &t[(a >> 12) & 4095]; u64 tag = (a >> 12) + 1;
  if (e->rtag != tag) e->wtag = 0;
  e->rtag = tag; if (write) e->wtag = tag;
  e->add = (u32)p - (u32)page;
  (void)c;
}
static inline u8 *hptr(Cpu *c, TlbEntry *t, u64 a, int write, u64 ip) {
  TlbEntry *e = &t[(a >> 12) & 4095]; u64 tag = (a >> 12) + 1;
  if (!(write ? e->wtag == tag : e->rtag == tag)) fill(c, t, a, write, ip);
  return (u8 *)(uptr)((u32)a + e->add);
}
void mmu_read(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip, void *out) {
  unsigned first = 4096 - (unsigned)(a & 4095);
  if (n <= first) { memcpy(out, hptr(c, t, a, 0, ip), n); return; }
  u8 *p0 = hptr(c, t, a, 0, ip), *p1 = hptr(c, t, a + first, 0, ip);
  memcpy(out, p0, first); memcpy((u8 *)out + first, p1, n - first);
}
void mmu_write(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip, const void *in) {
  unsigned first = 4096 - (unsigned)(a & 4095);
  if (n <= first) { memcpy(hptr(c, t, a, 1, ip), in, n); return; }
  u8 *p1 = hptr(c, t, a + first, 1, ip), *p0 = hptr(c, t, a, 1, ip);   // (both pages first: a fault leaves memory as it was)
  p1 = hptr(c, t, a + first, 1, ip);
  memcpy(p0, in, first); memcpy(p1, (const u8 *)in + first, n - first);
}
uptr mmu_ptr(Cpu *c, TlbEntry *t, u64 a, unsigned n, u64 ip) {
  if ((a & 4095) + n > 4096) fatal(cur, ip, "an atomic access across pages");
  return (uptr)hptr(c, t, a, 1, ip);
}
static u64 rd64(Cpu *c, u64 a) { u64 v; mmu_read(c, mmu_u, a, 8, a, &v); return v; }

// ---- the programs: lifted on their own (lifter --user), a side module each, found by the hash of the code page ----
// Several may be at the same addresses (every position-independent program at 0x555555554000): the one in memory is
// the one whose code page at the address that runs hashes as its file's (rt/microvm-x64/run.c). Found again after
// each change of address space. A program no module holds is rebuilt from memory and handed to the host
// (program_image), which lifts it (Node) or has it lifted already (the page), and registers it.
IMPORT("host", "program_image") int host_program_image(u32 image, u32 len, u32 fns, u32 n_fns);
static u64 calls_gen = 1;   // the generation of identities: a program registered, a flush of all or of code, a fork
static void programs_forget(void) { prog_cur = 0; calls_gen++; }
static int code_in(u64 a, u64 b) { for (unsigned i = 0; i < n_progs; i++) if (a < progs[i]->hi && b > progs[i]->lo) return 1; return 0; }
typedef struct { u64 page, pgd, gen; const ProgramDef *p; } IdSlot;
static IdSlot ids[1024];
static u64 prog_cur_gen, prog_cur_pgd;
// A page of code is the same page of the same file as long as its frame is the one checked and holds the same
// bytes: a program's text is the page cache's, shared by every process that maps it. So a page checked once (its
// whole hash) is known again by its frame, its address and 16 of its words, after a fork or a flush.
typedef struct { uptr frame; u64 page, sample; const ProgramDef *p; } FrameSlot;
static FrameSlot frames[4096];
static u64 sample_of(const u8 *b) { u64 h = 0xcbf29ce484222325ull; for (int k = 0; k < 16; k++) { u64 w; memcpy(&w, b + 256 * k + 8 * k, 8); h = (h ^ w) * 0x100000001b3ull; } return h; }
EXPORT("rt_program_register") void rt_program_register(const ProgramDef *p) {
  if (n_progs < 64) progs[n_progs++] = p; else logf_("pisco: more than 64 programs");
  programs_forget();
  if (trace) logf_("pisco: program %s registered (0x%llx to 0x%llx, %u functions, %u resume points)", p->name, p->lo, p->hi, p->fn_count, p->resume_count);
}
EXPORT("rt_side_alloc") u32 rt_side_alloc(u32 size, u32 align) { void *p = aligned_alloc(align < 16 ? 16 : align, (size + 15) & ~15u); if (p) memset(p, 0, size); return (u32)(uptr)p; }
// the host address of a user page (made present by the kernel), or 0
static u8 *upage(u64 a, int write) { Ctx *x = cur; int polls = mc_budget; u64 p = k_user_addr(a & ~0xFFFull, 1, write); cur = x; mc_budget = polls; return p ? (u8 *)(uptr)p : 0; }
static int user_read(Cpu *c, u64 va, void *out, unsigned n, int fault) {
  (void)c; (void)fault;
  for (unsigned k = 0; k < n;) {
    u64 a = va + k; unsigned m = 4096 - (unsigned)(a & 4095); if (m > n - k) m = n - k;
    u8 *p = upage(a, 0); if (!p) return 0;
    memcpy((u8 *)out + k, p + (a & 4095), m); k += m;
  }
  return 1;
}
static u64 st_hash;
static u64 code_hash(u64 pc, u64 lo, u64 hi) {
  st_hash++;
  u64 page = pc & ~0xFFFull, from = page > lo ? page : lo, to = page + 4096 < hi ? page + 4096 : hi;
  u8 *b = upage(page, 0); if (!b) return 0;
  u64 h = 0xcbf29ce484222325ull; for (u64 k = from - page; k < to - page; k++) h = (h ^ b[k]) * 0x100000001b3ull; return h;
}
static u64 off_of(const u8 *ph, unsigned phnum, u64 va) {
  for (unsigned i = 0; i < phnum; i++) { u32 t; u64 off, v, fs; memcpy(&t, ph + 56 * i, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&v, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8);
    if (t == 1 && va >= v && va < v + fs) return off + (va - v); }
  return ~0ull;
}
enum { MAX_FNS = 16384 };
static u64 fns[MAX_FNS]; static unsigned n_fns;
static void dynamic_functions(const u8 *img, u64 size, const u8 *ph, unsigned phnum, u64 bias, u64 symtab, u64 strtab) {
  u64 so = symtab ? off_of(ph, phnum, symtab) : ~0ull; if (so == ~0ull || strtab <= symtab) return;
  for (u64 k = 0; k < (strtab - symtab) / 24 && so + 24 * (k + 1) <= size && n_fns < MAX_FNS; k++) {
    u8 info = img[so + 24 * k + 4]; u16 shndx; u64 value; memcpy(&shndx, img + so + 24 * k + 6, 2); memcpy(&value, img + so + 24 * k + 8, 8);
    if ((info & 15) == 2 && shndx && value) fns[n_fns++] = bias + value;
  }
}
static void relocate(u8 *img, u64 size, const u8 *ph, unsigned phnum, u64 bias, int apply) {
  u64 dyn = ~0ull, dynsz = 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t; memcpy(&t, ph + 56 * i, 4); if (t == 2) { memcpy(&dyn, ph + 56 * i + 8, 8); memcpy(&dynsz, ph + 56 * i + 32, 8); } }
  if (dyn == ~0ull || dyn + dynsz > size) return;
  u64 rela = 0, relasz = 0, relr = 0, relrsz = 0, jmprel = 0, pltsz = 0, symtab = 0, strtab = 0;
  for (u64 k = 0; k + 16 <= dynsz; k += 16) { u64 tag, v; memcpy(&tag, img + dyn + k, 8); memcpy(&v, img + dyn + k + 8, 8);
    if (tag == 0) break; if (tag == 7) rela = v; if (tag == 8) relasz = v; if (tag == 35) relrsz = v; if (tag == 36) relr = v;
    if (tag == 23) jmprel = v; if (tag == 2) pltsz = v; if (tag == 6) symtab = v; if (tag == 5) strtab = v;
    if ((tag == 12 || tag == 13) && v && n_fns < MAX_FNS) fns[n_fns++] = bias + v; }   // DT_INIT, DT_FINI: the loader calls them
  dynamic_functions(img, size, ph, phnum, bias, symtab, strtab);
  if (!apply) return;
  u64 so = symtab ? off_of(ph, phnum, symtab) : ~0ull;
  for (int t = 0; t < 2; t++) {
    u64 o = t ? (jmprel ? off_of(ph, phnum, jmprel) : ~0ull) : (rela ? off_of(ph, phnum, rela) : ~0ull), n = t ? pltsz : relasz;
    for (u64 k = 0; o != ~0ull && k + 24 <= n && o + k + 24 <= size; k += 24) {
      u64 where, info, add; memcpy(&where, img + o + k, 8); memcpy(&info, img + o + k + 8, 8); memcpy(&add, img + o + k + 16, 8);
      u64 w = off_of(ph, phnum, where); if (w == ~0ull || w + 8 > size) continue;
      u32 type = (u32)info; u64 sym = info >> 32, val;
      if (type == 8) { val = bias + add; memcpy(img + w, &val, 8); continue; }
      if ((type == 1 || type == 6 || type == 7) && so != ~0ull && so + 24 * (sym + 1) <= size) {
        u16 shndx; u64 value; memcpy(&shndx, img + so + 24 * sym + 6, 2); memcpy(&value, img + so + 24 * sym + 8, 8);
        if (shndx) { val = bias + value + (type == 1 ? add : 0); memcpy(img + w, &val, 8); }
      }
    }
  }
  u64 o = relr ? off_of(ph, phnum, relr) : ~0ull, at = 0;
  for (u64 k = 0; o != ~0ull && k + 8 <= relrsz && o + k + 8 <= size; k += 8) {
    u64 e; memcpy(&e, img + o + k, 8);
    if (!(e & 1)) { at = e; u64 w = off_of(ph, phnum, at); if (w != ~0ull && w + 8 <= size) { u64 v; memcpy(&v, img + w, 8); v += bias; memcpy(img + w, &v, 8); } at += 8; }
    else { for (int bit = 1; bit < 64; bit++) if (e >> bit & 1) { u64 w = off_of(ph, phnum, at + 8ull * (bit - 1)); if (w != ~0ull && w + 8 <= size) { u64 v; memcpy(&v, img + w, 8); v += bias; memcpy(img + w, &v, 8); } } at += 63 * 8; }
  }
}
static int holds_pc(const u8 *ph, unsigned phnum, u64 bias, u64 pc) {
  for (unsigned i = 0; i < phnum; i++) { u32 t, fl; u64 va, ms; memcpy(&t, ph + 56 * i, 4); memcpy(&fl, ph + 56 * i + 4, 4); memcpy(&va, ph + 56 * i + 16, 8); memcpy(&ms, ph + 56 * i + 40, 8);
    if (t == 1 && (fl & 1) && pc >= va + bias && pc < va + bias + ms) return 1; }
  return 0;
}
static u64 bias_of(const u8 *ph, unsigned phnum, u64 base) {
  for (unsigned i = 0; i < phnum; i++) { u32 t; u64 off, va; memcpy(&t, ph + 56 * i, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&va, ph + 56 * i + 16, 8);
    if (t == 1 && off == 0) return base - va; }
  return base;
}
static u64 image_below(Cpu *c, u64 pc, u8 h[64], u8 *ph, u64 *phnum) {
  u64 a = pc & ~0xFFFull;
  for (unsigned k = 0; k < 65536 && a; k++, a -= 4096) {
    if (!user_read(c, a, h, 64, 0) || memcmp(h, "\x7f" "ELF\x02\x01", 6)) continue;
    u64 po; u16 pn; memcpy(&po, h + 32, 8); memcpy(&pn, h + 56, 2);
    if (!pn || pn > 64 || po > 4096 || !user_read(c, a + po, ph, 56u * pn, 0)) continue;
    if (holds_pc(ph, pn, bias_of(ph, pn, a), pc)) { *phnum = pn; return a; }
  }
  return ~0ull;
}
static int auxv(Cpu *c, u64 *aphdr, u64 *aphnum, u64 *entry, u64 *ibase) {
  u64 sp = R[RSP], argc, v;
  if (!user_read(c, sp, &argc, 8, 0) || argc > 4096) return 0;
  u64 at = sp + 8 * (argc + 2);
  for (int k = 0; k < 4096; k++, at += 8) { if (!user_read(c, at, &v, 8, 0)) return 0; if (!v) break; }
  for (int k = 0; k < 64; k++) {
    u64 kv[2]; at += 8; if (!user_read(c, at, kv, 16, 0)) return 0; at += 8;
    if (kv[0] == 0) return 1; if (kv[0] == 3) *aphdr = kv[1]; if (kv[0] == 5) *aphnum = kv[1]; if (kv[0] == 7) *ibase = kv[1]; if (kv[0] == 9) *entry = kv[1];
  }
  return 0;
}
static u64 st_asked;
static int ask_host(Cpu *c, u64 pc) {
  u64 aphdr = 0, aphnum = 0, entry = 0, ibase = 0; int start = auxv(c, &aphdr, &aphnum, &entry, &ibase);
  u8 h[64], ph[64 * 56]; u64 phdr, phnum = 0, bias = 0; int apply = 0, found = 0;
  st_asked++;
  if (start && pc == entry && aphdr && aphnum && aphnum <= 64) {
    phdr = aphdr; phnum = aphnum; apply = !ibase;
    if (!user_read(c, phdr, ph, 56u * (unsigned)phnum, 1)) return 0;
    int has_phdr = 0;
    for (unsigned i = 0; i < phnum; i++) { u32 t; u64 va; memcpy(&t, ph + 56 * i, 4); memcpy(&va, ph + 56 * i + 16, 8); if (t == 6) { bias = phdr - va; has_phdr = 1; } }
    if (!has_phdr) {   // (a static position-independent program has no PT_PHDR: its ELF header says where its headers are)
      u64 hb = phdr & ~0xFFFull, po;
      if (user_read(c, hb, h, 64, 1) && !memcmp(h, "\x7f" "ELF\x02\x01", 6) && (memcpy(&po, h + 32, 8), hb + po == phdr)) bias = bias_of(ph, (unsigned)phnum, hb);
    }
    found = 1;
  } else if (start && ibase && user_read(c, ibase, h, 64, 1) && !memcmp(h, "\x7f" "ELF\x02\x01", 6)) {
    u64 po, e; u16 pn; memcpy(&po, h + 32, 8); memcpy(&pn, h + 56, 2); memcpy(&e, h + 24, 8);
    if (pc == ibase + e && pn && pn <= 64) { phdr = ibase + po; phnum = pn; bias = ibase; apply = 1; if (!user_read(c, phdr, ph, 56u * (unsigned)phnum, 1)) return 0; found = 1; }
  }
  if (!found) { u64 b = image_below(c, pc, h, ph, &phnum); if (b == ~0ull) return 0; bias = bias_of(ph, (unsigned)phnum, b); }
  u64 size = 0, base = ~0ull; int holds = 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t, fl; u64 off, va, fs, ms; memcpy(&t, ph + 56 * i, 4); memcpy(&fl, ph + 56 * i + 4, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&va, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8); memcpy(&ms, ph + 56 * i + 40, 8);
    if (t != 1) continue; if (off + fs > size) size = off + fs; if (off == 0) base = va + bias; if ((fl & 1) && pc >= va + bias && pc < va + bias + ms) holds = 1; }
  if (!holds || base == ~0ull || size > (256u << 20)) return 0;
  if (!user_read(c, base, h, 64, 1) || memcmp(h, "\x7f" "ELF\x02\x01", 6)) return 0;
  u8 *img = calloc(1, size); if (!img) return 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t; u64 off, va, fs; memcpy(&t, ph + 56 * i, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&va, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8);
    if (t == 1 && fs && !user_read(c, va + bias, img + off, (unsigned)fs, 1)) { free(img); return 0; } }
  n_fns = 0; relocate(img, size, ph, (unsigned)phnum, bias, apply);
  if (n_fns < MAX_FNS) fns[n_fns++] = pc;
  u64 po; memcpy(&po, h + 32, 8); u8 *p = img + po; u16 pn; memcpy(&pn, h + 56, 2);
  if (po + 56u * pn <= size) for (unsigned i = 0; i < pn; i++) {
    u32 t; memcpy(&t, p + 56 * i, 4);
    if (t == 2 || t == 3 || t == 0x6474e550 || t == 0x6474e552) { t = 0; memcpy(p + 56 * i, &t, 4); continue; }
    u64 va; memcpy(&va, p + 56 * i + 16, 8); va += bias; memcpy(p + 56 * i + 16, &va, 8); memcpy(p + 56 * i + 24, &va, 8);
  }
  u16 exec = 2; memcpy(img + 16, &exec, 2); u64 e; memcpy(&e, h + 24, 8); e += bias; memcpy(img + 24, &e, 8);
  memset(img + 40, 0, 8); memset(img + 60, 0, 4);
  Ctx *x = cur; int polls = mc_budget;
  int r = host_program_image((u32)(uptr)img, (u32)size, (u32)(uptr)fns, n_fns); free(img);
  cur = x; mc_budget = polls;
  return r;
}
static const ProgramDef *program_at(Cpu *c, u64 pc) {
  if (prog_cur && pc >= prog_cur->lo && pc < prog_cur->hi && prog_cur_gen == calls_gen && prog_cur_pgd == cur_pgd) return prog_cur;
  u64 page = pc & ~0xFFFull; IdSlot *slot = &ids[((page >> 12) ^ (cur_pgd >> 12)) & 1023];
  if (slot->page == page && slot->pgd == cur_pgd && slot->gen == calls_gen) { prog_cur = slot->p; prog_cur_gen = calls_gen; prog_cur_pgd = cur_pgd; return prog_cur; }
  prog_cur = 0;
  u8 *host = upage(page, 0); FrameSlot *fs = host ? &frames[((uptr)host >> 12) & 4095] : 0;
  if (fs && fs->frame == (uptr)host && fs->page == page && fs->p && pc >= fs->p->lo && pc < fs->p->hi && fs->sample == sample_of(host)) prog_cur = fs->p;
  else for (unsigned i = 0; i < n_progs; i++) {
    const ProgramDef *p = progs[i];
    if (pc < p->lo || pc >= p->hi) continue;
    if (code_hash(pc, p->lo, p->hi) == p->page_hashes[((pc & ~0xFFFull) - (p->lo & ~0xFFFull)) >> 12]) {
      prog_cur = p; if (fs) *fs = (FrameSlot){ (uptr)host, page, sample_of(host), p }; break; }
  }
  if (!prog_cur) { static u64 asked; if (asked != pc) { asked = pc; if (ask_host(c, pc)) return program_at(c, pc); } return 0; }
  *slot = (IdSlot){ page, cur_pgd, calls_gen, prog_cur }; prog_cur_gen = calls_gen; prog_cur_pgd = cur_pgd;
  return prog_cur;
}
static int find(const u64 *v, unsigned n, u64 pc) { unsigned lo = 0, hi = n; while (lo < hi) { unsigned m = (lo + hi) / 2; if (v[m] < pc) lo = m + 1; else hi = m; } return lo < n && v[lo] == pc ? (int)lo : -1; }
// where a poll may leave lifted code for the kernel (its budget, an interrupt) and come back to later: a function's
// start only. A poll at a jump back may also be a return address, which the function can be entered at; but the
// loop's state there (its flags: a carry kept from one turn to the next in libcrypto's bignum loops) is not what a
// return from a call has, and re-entering there broke one TLS handshake in three. At a function's start nothing
// but the registers is live.
static int enterable(u64 pc) {
  const ProgramDef *p = program_at(&cur->cpu, pc); if (!p) return 0;
  return find(p->fn_addrs, p->fn_count, pc) >= 0;
}
static void enter(Cpu *c, u64 pc) {
  const ProgramDef *p = program_at(c, pc); int i;
  if (!p) fatal(cur, pc, "no lifted program holds the code");
  if ((i = find(p->fn_addrs, p->fn_count, pc)) >= 0) { p->fn_ptrs[i](c); return; }
  if ((i = find(p->resume_at, p->resume_count, pc)) >= 0) { st_reenter++; c->resume = pc; p->fn_ptrs[p->resume_fn[i]](c); return; }
  fatal(cur, pc, "the code goes on at an address that is no lifted function and no return address");
}
// indirect calls and jumps by address, cached for one generation of the address space (rt/microvm-x64/run.c)
typedef struct { u64 pc, gen, pgd; void (*fn)(Cpu *); } CallSlot;
static CallSlot calls[4096];
// An indirect jump the lifter did not resolve is a tail call here, one host frame deeper: an interpreter whose
// dispatch is such a jump (Lua's computed goto, merged by GCC into one `jmp rax`) would go deeper with every opcode
// until the browser's stack runs out. Past MAX_DEPTH nested indirect calls, what runs is abandoned and the code is
// entered at the target from the run loop: returns are found by their return addresses, as after a system call.
enum { MAX_DEPTH = 256 };
void rt_call(Cpu *c, u64 t) {
  Ctx *x = cur;
  if (x->depth >= MAX_DEPTH) { st_unwound++; x->root_pc = t; longjmp(x->root, 1); }
  CallSlot *s = &calls[((t >> 4) ^ (t >> 16)) & 4095];
  void (*fn)(Cpu *) = 0;
  if (s->pc == t && s->gen == calls_gen && s->pgd == cur_pgd) fn = s->fn;
  else {
    const ProgramDef *p = program_at(c, t); int i;
    if (p && (i = find(p->fn_addrs, p->fn_count, t)) >= 0) { fn = p->fn_ptrs[i]; *s = (CallSlot){ t, calls_gen, cur_pgd, fn }; }
  }
  if (!fn) { x->root_pc = t; longjmp(x->root, 1); }   // (a return address: what runs is abandoned, the code entered there)
  x->depth++; fn(c); x->depth--;
}
void rt_dispatch(Cpu *c, u64 t) { rt_call(c, t); }
void rt_check_return(Cpu *c, u64 call, u64 next, u64 sp) { (void)call; (void)next; (void)sp; (void)c; }

// ---- polls: the budget (instructions, as kwemu counts them; a poll stands for INSN_PER_POLL) and interrupts ----
enum { POLLS = 4096, INSN_PER_POLL = 8 };
static int polls_for(u64 budget) { u64 n = budget / INSN_PER_POLL; return n > POLLS ? POLLS : (int)n; }
void rt_poll(Cpu *c, u64 ip) {
  Ctx *x = cur; struct kw_cpu *kc = x->kc;
  u64 used = (u64)(x->budget_polls - mc_budget) * INSN_PER_POLL; if (used > kc->budget) used = kc->budget;
  kc->budget -= used; kc->icount += used;
  int irq = kc->irq_pending && *(volatile u32 *)(uptr)kc->irq_pending;
  if ((irq || kc->budget < INSN_PER_POLL) && enterable(ip)) {
    if (!irq) { kc->icount += kc->budget; kc->budget = 0; }
    st_polls_out++; x->budget_polls = mc_budget = 0;
    leave(x, irq ? EV_INTERRUPT : EV_BUDGET, ip);
  }
  x->budget_polls = mc_budget = kc->budget < INSN_PER_POLL ? 64 : polls_for(kc->budget);
  (void)c;
}

// ---- system calls: x86-64's, as arm64's ----
enum { AT_FDCWD = -100 };
static u64 fl_x2a(u64 f) {   // the O_* bits that differ (x86-64 against asm-generic's arm64)
  u64 r = f & ~(u64)(0200000 | 0400000 | 040000 | 0100000);
  if (f & 0200000) r |= 040000; if (f & 0400000) r |= 0100000; if (f & 040000) r |= 0200000; if (f & 0100000) r |= 0400000;
  return r;
}
static u64 fl_a2x(u64 f) {
  u64 r = f & ~(u64)(0200000 | 0400000 | 040000 | 0100000);
  if (f & 040000) r |= 0200000; if (f & 0100000) r |= 0400000; if (f & 0200000) r |= 040000; if (f & 0400000) r |= 0100000;
  return r;
}
// sets arm64's number and arguments; 0 when it is done here (RAX holds the result), 1 when the kernel does it
static int translate(Ctx *x, Cpu *c, u64 *a) {
  u64 nr = R[RAX], p[6] = { R[RDI], R[RSI], R[RDX], R[10], R[8], R[9] };
  long n = -1;
  #define AS(num) do { n = (num); for (int k_ = 0; k_ < 6; k_++) a[k_] = p[k_]; } while (0)
  #define SH(num) do { n = (num); a[0] = (u64)(s64)AT_FDCWD; for (int k_ = 0; k_ < 5; k_++) a[k_ + 1] = p[k_]; } while (0)
  switch (nr) {
  case 0: AS(63); break;   case 1: AS(64); break;   case 3: AS(57); break;
  case 2: SH(56); a[2] = fl_x2a(a[2]); break;                       // open -> openat
  case 257: AS(56); a[2] = fl_x2a(a[2]); break;                     // openat
  case 4: SH(79); a[3] = 0; break;                                  // stat -> newfstatat
  case 6: SH(79); a[3] = 0x100; break;                              // lstat
  case 5: AS(80); break;                                            // fstat
  case 262: AS(79); break;                                          // newfstatat
  case 8: AS(62); break;   case 9: AS(222); break;  case 10: AS(226); break; case 11: AS(215); break; case 12: AS(214); break;
  case 13: AS(134); break; case 14: AS(135); break; case 15: AS(139); break; case 16: AS(29); break;  case 17: AS(67); break;  case 18: AS(68); break;
  case 19: AS(65); break;  case 20: AS(66); break;
  case 21: SH(48); a[3] = 0; break;                                 // access -> faccessat
  case 22: AS(59); a[1] = 0; break;                                 // pipe -> pipe2
  case 24: AS(124); break; case 25: AS(216); break; case 28: AS(233); break; case 32: AS(23); break;
  case 33: if (p[0] == p[1]) { R[RAX] = p[1]; return 0; } AS(24); a[2] = 0; break;   // dup2 -> dup3
  case 35: AS(101); break; case 38: AS(103); break; case 39: AS(172); break; case 40: AS(71); break;
  case 56: AS(220); a[3] = p[4]; a[4] = p[3]; break;                // clone: arm64's order (CLONE_BACKWARDS)
  case 57: n = 220; a[0] = 17; a[1] = a[2] = a[3] = a[4] = 0; break; // fork
  case 58: n = 220; a[0] = 0x4111; a[1] = a[2] = a[3] = a[4] = 0; break; // vfork
  case 59: AS(221); break; case 60: AS(93); break; case 61: AS(260); break; case 62: AS(129); break; case 63: AS(160); break;
  case 72: AS(25); if (p[1] == 4) a[2] = fl_x2a(p[2]); break;      // fcntl (F_SETFL's flags)
  case 74: AS(82); break;  case 77: AS(46); break;  case 79: AS(17); break;  case 80: AS(49); break;  case 81: AS(50); break;
  case 82: n = 38; a[0] = (u64)(s64)AT_FDCWD; a[1] = p[0]; a[2] = (u64)(s64)AT_FDCWD; a[3] = p[1]; break;   // rename -> renameat
  case 83: SH(34); break;                                           // mkdir -> mkdirat
  case 84: SH(35); a[2] = 0x200; break;                             // rmdir -> unlinkat(AT_REMOVEDIR)
  case 86: n = 37; a[0] = (u64)(s64)AT_FDCWD; a[1] = p[0]; a[2] = (u64)(s64)AT_FDCWD; a[3] = p[1]; a[4] = 0; break;   // link
  case 87: SH(35); a[2] = 0; break;                                 // unlink
  case 88: n = 36; a[0] = p[0]; a[1] = (u64)(s64)AT_FDCWD; a[2] = p[1]; break;   // symlink
  case 89: SH(78); break;                                           // readlink
  case 90: SH(53); break;  case 91: AS(52); break;                  // chmod, fchmod
  case 92: SH(54); a[4] = 0; break;                                 // chown
  case 95: AS(166); break; case 96: AS(169); break; case 97: AS(163); break; case 99: AS(179); break; case 100: AS(153); break;
  case 102: AS(174); break; case 104: AS(176); break; case 105: AS(146); break; case 106: AS(144); break; case 107: AS(175); break;
  case 108: AS(177); break; case 109: AS(154); break; case 110: AS(173); break; case 111: n = 155; a[0] = 0; break;
  case 112: AS(157); break; case 121: AS(155); break; case 124: AS(156); break; case 131: AS(132); break;
  case 158:                                                         // arch_prctl
    if (p[0] == 0x1002) { c->fs_base = p[1]; R[RAX] = 0; return 0; }
    if (p[0] == 0x1001) { c->gs_base = p[1]; R[RAX] = 0; return 0; }
    R[RAX] = (u64)-22; return 0;
  case 160: AS(164); break; case 186: AS(178); break; case 200: AS(130); break; case 202: AS(98); break; case 204: AS(123); break;
  case 217: AS(61); break; case 218: AS(96); break; case 228: AS(113); break; case 229: AS(114); break; case 230: AS(115); break;
  case 231: AS(94); break; case 234: AS(131); break;
  case 258: AS(34); break; case 260: AS(54); break; case 263: AS(35); break; case 264: AS(38); break; case 265: AS(37); break;
  case 266: AS(36); break; case 267: AS(78); break; case 268: AS(53); break; case 269: AS(48); break; case 270: AS(72); break;
  case 271: AS(73); break; case 273: AS(99); break; case 280: AS(88); break; case 292: AS(24); break;
  case 293: AS(59); a[1] = fl_x2a(p[1]); break; case 302: AS(261); break; case 318: AS(278); break; case 332: AS(291); break;
  case 334: R[RAX] = (u64)-38; return 0;                            // rseq: no
  // signals
  case 127: AS(136); break; case 128: AS(137); break; case 129: AS(138); break; case 130: AS(133); break;   // rt_sigpending, timedwait, queueinfo, suspend
  case 297: AS(240); break;                                         // rt_tgsigqueueinfo
  // what waits: x86-64's older calls, as the ones arm64 has (their structures put below the stack's red zone)
  case 7: {                                                         // poll -> ppoll
    u64 scr = (R[RSP] - 1024) & ~15ull; s64 ms = (s64)(int)p[2];
    if (ms >= 0) { u64 ts[2] = { (u64)(ms / 1000), (u64)(ms % 1000) * 1000000 }; mmu_write(c, mmu_u, scr, 16, scr, ts); }
    n = 73; a[0] = p[0]; a[1] = p[1]; a[2] = ms >= 0 ? scr : 0; a[3] = 0; a[4] = 8; break; }
  case 23: {                                                        // select -> pselect6 (the time left written back)
    u64 scr = (R[RSP] - 1024) & ~15ull;
    if (p[4]) { u64 tv[2]; mmu_read(c, mmu_u, p[4], 16, p[4], tv); tv[1] *= 1000; mmu_write(c, mmu_u, scr, 16, scr, tv); }
    n = 72; a[0] = p[0]; a[1] = p[1]; a[2] = p[2]; a[3] = p[3]; a[4] = p[4] ? scr : 0; a[5] = 0; break; }
  case 34: n = 73; a[0] = a[1] = a[2] = a[3] = 0; a[4] = 8; break;  // pause -> ppoll(nothing, no timeout)
  case 37: {                                                        // alarm -> setitimer(ITIMER_REAL), the old one's seconds returned
    u64 scr = (R[RSP] - 1024) & ~15ull, it[4] = { 0, 0, p[0], 0 }; mmu_write(c, mmu_u, scr, 32, scr, it);
    n = 103; a[0] = 0; a[1] = scr; a[2] = scr + 32; break; }
  // sockets: the same structures, other numbers
  case 41: AS(198); break; case 42: AS(203); break; case 43: AS(202); break; case 44: AS(206); break; case 45: AS(207); break;
  case 46: AS(211); break; case 47: AS(212); break; case 48: AS(210); break; case 49: AS(200); break; case 50: AS(201); break;
  case 51: AS(204); break; case 52: AS(205); break; case 53: AS(199); break; case 54: AS(208); break; case 55: AS(209); break;
  case 288: AS(242); break; case 299: AS(243); break; case 307: AS(269); break;
  // epoll: x86-64's struct epoll_event is packed (12 bytes), arm64's is not (16)
  case 213: n = 20; a[0] = 0; break; case 291: AS(20); break;      // epoll_create, epoll_create1
  case 233: {                                                       // epoll_ctl
    u64 scr = (R[RSP] - 1024) & ~15ull; AS(21);
    if (p[3]) { u8 e[12]; u64 g[2] = {0}; mmu_read(c, mmu_u, p[3], 12, p[3], e); memcpy(g, e, 4); memcpy(&g[1], e + 4, 8); mmu_write(c, mmu_u, scr, 16, scr, g); a[3] = scr; }
    break; }
  case 232: case 281:                                               // epoll_wait, epoll_pwait: fewer events, so that 16 bytes each fit
    AS(22); a[2] = p[2] * 12 / 16; if (!a[2] && p[2]) a[2] = 1; if (nr == 232) { a[4] = 0; a[5] = 8; } break;
  // the rest that are only renumbered
  case 73: AS(32); break; case 75: AS(83); break; case 76: AS(45); break; case 93: AS(55); break;
  case 94: SH(54); a[4] = 0x100; break;                             // lchown -> fchownat(AT_SYMLINK_NOFOLLOW)
  case 85: SH(56); a[3] = p[1]; a[2] = 01 | 0100 | 01000; break;    // creat -> openat(O_WRONLY|O_CREAT|O_TRUNC)
  case 98: AS(165); break; case 101: AS(117); break; case 103: AS(116); break;
  case 113: AS(145); break; case 114: AS(143); break; case 115: AS(158); break; case 116: AS(159); break;
  case 117: AS(147); break; case 118: AS(148); break; case 119: AS(149); break; case 120: AS(150); break;
  case 122: AS(151); break; case 123: AS(152); break; case 125: AS(90); break; case 126: AS(91); break;
  case 133: SH(33); break;                                          // mknod -> mknodat
  case 137: AS(43); break; case 138: AS(44); break; case 140: AS(141); break; case 141: AS(140); break;
  case 142: AS(118); break; case 143: AS(121); break; case 144: AS(119); break; case 145: AS(120); break;
  case 146: AS(125); break; case 147: AS(126); break; case 148: AS(127); break;
  case 149: AS(228); break; case 150: AS(229); break; case 151: AS(230); break; case 152: AS(231); break;
  case 157: AS(167); break; case 161: AS(51); break; case 162: AS(81); break; case 165: AS(40); break; case 166: AS(39); break;
  case 170: AS(161); break; case 203: AS(122); break; case 221: AS(223); break;
  case 222: AS(107); break; case 223: AS(110); break; case 224: AS(108); break; case 225: AS(109); break; case 226: AS(111); break;
  case 227: AS(112); break; case 247: AS(95); break;
  case 253: n = 26; a[0] = 0; break; case 254: AS(27); break; case 255: AS(28); break; case 294: AS(26); break;
  case 272: AS(97); break; case 275: AS(76); break; case 276: AS(77); break; case 277: AS(84); break;
  case 282: AS(74); a[3] = 0; break; case 289: AS(74); break;      // signalfd, signalfd4
  case 283: AS(85); break; case 286: AS(86); break; case 287: AS(87); break;
  case 284: n = 19; a[0] = p[0]; a[1] = 0; break; case 290: AS(19); break;   // eventfd, eventfd2
  case 285: AS(47); break; case 295: AS(69); break; case 296: AS(70); break; case 306: AS(267); break; case 309: AS(168); break;
  case 316: AS(276); break; case 317: AS(277); break; case 319: AS(279); break; case 322: AS(281); break; case 324: AS(283); break;
  case 326: AS(285); break; case 435: AS(435); break; case 436: AS(436); break; case 437: AS(437); break; case 439: AS(439); break;
  default: { static u64 told[32]; static unsigned k; int seen = 0; for (unsigned i = 0; i < k; i++) seen |= told[i] == nr;
    if (!seen) { if (k < 32) told[k++] = nr; logf_("pisco: x86-64 system call %llu is not translated (ENOSYS)", nr); }
    R[RAX] = (u64)-38; return 0; }
  }
  (void)x;
  return n < 0 ? 0 : (a[6] = (u64)n, 1);
  #undef AS
  #undef SH
}
// arm64's struct stat (asm-generic, 128 bytes) as x86-64's (144), in place
static void stat_a2x(Cpu *c, u64 buf) {
  u8 g[128], s[144]; mmu_read(c, mmu_u, buf, 128, buf, g); memset(s, 0, sizeof s);
  #define G(T, o) (*(T *)(g + (o)))
  #define S(T, o) (*(T *)(s + (o)))
  S(u64, 0) = G(u64, 0); S(u64, 8) = G(u64, 8); S(u64, 16) = G(u32, 20); S(u32, 24) = G(u32, 16); S(u32, 28) = G(u32, 24); S(u32, 32) = G(u32, 28);
  S(u64, 40) = G(u64, 32); S(s64, 48) = G(s64, 48); S(s64, 56) = G(s32, 56); S(s64, 64) = G(s64, 64);
  memcpy(s + 72, g + 72, 48);   // the three timespecs
  #undef G
  #undef S
  mmu_write(c, mmu_u, buf, 144, buf, s);
}
// what a system call returned (x0), as x86-64's result, its structures converted
static void syscall_done(Cpu *c, struct kw_regs *k) {
  u64 nr = k->x[X_PEND] - 1; s64 r = (s64)k->x[0]; k->x[X_PEND] = 0;
  R[RAX] = (u64)r;
  if (r < 0 && r > -4096) return;
  switch (nr) {
  case 4: case 6: stat_a2x(c, R[RSI]); break;
  case 5: stat_a2x(c, R[RSI]); break;
  case 262: stat_a2x(c, R[RDX]); break;
  case 63: { static const char m[] = "x86_64"; u8 z[65] = {0}; memcpy(z, m, sizeof m); mmu_write(c, mmu_u, R[RDI] + 4 * 65, 65, R[RDI], z); break; }
  case 72: if (R[RSI] == 3) R[RAX] = fl_a2x((u64)r); break;        // F_GETFL
  case 23: if (R[8]) { u64 scr = (R[RSP] - 1024) & ~15ull, ts[2]; mmu_read(c, mmu_u, scr, 16, scr, ts); ts[1] /= 1000; mmu_write(c, mmu_u, R[8], 16, R[8], ts); } break;
  case 37: { u64 scr = (R[RSP] - 1024) & ~15ull, old[4]; mmu_read(c, mmu_u, scr + 32, 32, scr + 32, old); R[RAX] = old[2] + (old[3] ? 1 : 0); break; }
  case 232: case 281:                                               // 16-byte events as 12-byte ones, in place
    for (s64 i = 0; i < r; i++) { u8 g[16]; mmu_read(c, mmu_u, R[RSI] + 16 * i, 16, R[RSI], g); u8 e[12]; memcpy(e, g, 4); memcpy(e + 4, g + 8, 8); mmu_write(c, mmu_u, R[RSI] + 12 * i, 12, R[RSI], e); }
    break;
  }
}
void rt_syscall_m(Cpu *c, u64 next, Flags *f) {
  Ctx *x = cur; u64 a[7] = {0};
  u64 rflags = flags_get64(f);
  if (trace > 1) logf_("pisco: system call %llu at 0x%llx", R[RAX], next);
  if (!translate(x, c, a)) return;   // done here: the code goes on
  st_syscalls++;
  u64 nr = R[RAX];
  if (nr == 56 || nr == 57 || nr == 58 || nr == 435) programs_forget();   // clone, fork, vfork, clone3
  R[RCX] = next; R[11] = rflags;
  struct kw_regs *k = x->kr;
  for (int i = 0; i < 6; i++) k->x[i] = a[i];
  k->x[8] = a[6]; k->x[X_PEND] = nr + 1; k->x[X_NEXT] = next;
  x->rflags = rflags;
  leave(x, EV_SYSCALL, next);
}
void rt_syscall(Cpu *c, u64 next) { Flags f = {0}; rt_syscall_m(c, next, &f); }

// ---- what user code may meet otherwise ----
void rt_trap(u64 a) { leave(cur, EV_ILLEGAL, a); }
void rt_unimpl(u64 a) { fatal(cur, a, "an instruction that is not lifted"); }
void rt_hlt(Cpu *c, u64 next) { (void)c; leave(cur, EV_ILLEGAL, next); }
void rt_exc(Cpu *c, u32 vector, u32 err, u64 rip, u64 next) { (void)c; (void)err; (void)next; leave(cur, vector == 3 ? EV_BREAKPOINT : EV_ILLEGAL, rip); }
void flags_set64(Cpu *c, Flags *f, u64 v) { (void)c; f->cf = v & 1; f->pf = (v >> 2) & 1; f->af = (v >> 4) & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->df = (v >> 10) & 1; f->of = (v >> 11) & 1; mc_rflags = (v & 0x240000) | 2; }
void rt_cpuid(Cpu *c) {
  u32 leaf = (u32)R[RAX], a = 0, b = 0, cx = 0, d = 0;
  switch (leaf) {   // rt/microvm-x64/apic.c's CPU, as a program sees it
  case 0: a = 0x16; b = 0x756E6547; d = 0x49656E69; cx = 0x6C65746E; break;
  case 1: a = 0x000906EA; b = 0x00010800;
    cx = 1u << 0 | 1u << 9 | 1u << 13 | 1u << 19 | 1u << 20 | 1u << 23;   // (no PCLMULQDQ: the lifter has no carry-less multiply)
    d = 1u << 0 | 1u << 4 | 1u << 5 | 1u << 6 | 1u << 8 | 1u << 11 | 1u << 13 | 1u << 15 | 1u << 16 | 1u << 19 | 1u << 23 | 1u << 24 | 1u << 25 | 1u << 26; break;
  case 0x80000000: a = 0x80000008; break;
  case 0x80000001: cx = 1u << 0; d = 1u << 11 | 1u << 20 | 1u << 29; break;
  case 0x80000008: a = 0x3028; break;
  }
  R[RAX] = a; R[RBX] = b; R[RCX] = cx; R[RDX] = d;
}
u64 rt_rdtsc(void) { return host_now_ns(); }
u64 rt_random64(void) { static u64 s = 0x9E3779B97F4A7C15ull; s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
// a kernel's instructions, which user code cannot run (#GP: SIGILL here)
#define PRIV(name, ...) void name(__VA_ARGS__) { leave(cur, EV_ILLEGAL, 0); }
PRIV(rt_sti, Cpu *c) PRIV(rt_iret, Cpu *c) PRIV(rt_sysret, Cpu *c) PRIV(rt_swapgs, Cpu *c) PRIV(rt_wrmsr, Cpu *c, u64 ip) PRIV(rt_rdmsr, Cpu *c, u64 ip)
PRIV(rt_wcr, Cpu *c, unsigned n, u64 v) PRIV(rt_lgdt, Cpu *c, u64 a) PRIV(rt_lidt, Cpu *c, u64 a) PRIV(rt_sgdt, Cpu *c, u64 a) PRIV(rt_sidt, Cpu *c, u64 a)
PRIV(rt_ltr, Cpu *c, u16 s) PRIV(rt_wseg, Cpu *c, unsigned s, u16 v) PRIV(rt_invlpg, Cpu *c, u64 a) PRIV(rt_out, Cpu *c, u16 p, unsigned s, u32 v)
u64 rt_rcr(Cpu *c, unsigned n) { (void)c; (void)n; leave(cur, EV_ILLEGAL, 0); }
u16 rt_str(Cpu *c) { (void)c; leave(cur, EV_ILLEGAL, 0); }
u16 rt_rseg(Cpu *c, unsigned s) { (void)c; return s == 1 ? 0x33 : 0x2b; }
u32 rt_in(Cpu *c, u16 p, unsigned s) { (void)c; (void)p; (void)s; leave(cur, EV_ILLEGAL, 0); }

// ---- the kernel's side: kw_run for x86-64 tasks ----
static u32 run_task(u64 cpu_, u64 regs_);
// ---- a C stack per task: a task may sleep in the kernel (a page fault that waits for the disk) inside its lifted
// frames, and another task run meanwhile; each runs on a stack of its own, found by its kw_regs (a dead task's are a
// new task's) ----
enum { TASK_STACK = 1 << 20, MAX_STACKS = 1024 };
typedef struct { u64 regs; u8 *stack; } TaskStack;
static TaskStack stacks[MAX_STACKS]; static unsigned n_stacks;
static uptr sp_get(void) { uptr v; __asm__ volatile(".globaltype __stack_pointer, i32\n global.get __stack_pointer\n local.set %0" : "=r"(v)); return v; }
static void sp_set(uptr v) { __asm__ volatile(".globaltype __stack_pointer, i32\n local.get %0\n global.set __stack_pointer" : : "r"(v)); }
static u8 *stack_of(u64 regs) {
  unsigned h = (unsigned)((regs >> 4) * 2654435761u) % MAX_STACKS;
  for (unsigned k = 0; k < MAX_STACKS; k++, h = (h + 1) % MAX_STACKS) {
    if (stacks[h].regs == regs) return stacks[h].stack;
    if (!stacks[h].regs) {
      if (n_stacks >= MAX_STACKS / 2) return 0;
      u8 *s = malloc(TASK_STACK); if (!s) return 0;
      stacks[h] = (TaskStack){ regs, s }; n_stacks++; return s;
    }
  }
  return 0;
}
static u32 run_on_own_stack(u64 cpu_, u64 regs_, u32 (*f)(u64, u64)) {
  u8 *s = stack_of(regs_);
  if (!s) return f(cpu_, regs_);   // (more tasks than stacks: the shared one)
  uptr old = sp_get(); sp_set((uptr)(s + TASK_STACK) & ~(uptr)15);
  u32 r = f(cpu_, regs_);
  sp_set(old); return r;
}
EXPORT("pisco_run") u32 pisco_run(u64 cpu_, u64 regs_) { return run_on_own_stack(cpu_, regs_, run_task); }
EXPORT("pisco_mine") u32 pisco_mine(u64 regs) { return ((struct kw_regs *)(uptr)regs)->x[X_TAG] == X64_TAG; }
static void run_from(Ctx *x, u64 p) {
  Cpu *c = &x->cpu;
  x->root_pc = p;
  setjmp(x->root);
  p = x->root_pc; x->depth = 0;
  for (;;) { c->landing = 0; enter(c, p); p = rd64(c, R[RSP] - 8); }
}
static u32 run_task(u64 cpu_, u64 regs_) {
  Ctx ctx, *x = &ctx, *outer = cur; st_runs++;
  x->kc = (struct kw_cpu *)(uptr)cpu_; x->kr = (struct kw_regs *)(uptr)regs_;
  Cpu *c = &x->cpu; load(c, x->kr);
  struct kw_regs *k = x->kr;
  u64 rip = k->pc & ~MARK, rflags = k->x[X_FLAGS];
  if (!(k->pc & MARK)) {
    // the kernel delivered a signal, as arm64 does: its frame at sp (the interrupted registers, sigreturn's), the
    // handler in pc, its arguments in x0..x2, sa_restorer in x30. Entered as x86-64 enters a handler: the arguments
    // in RDI, RSI, RDX, the restorer as its return address (musl's __restore_rt: rt_sigreturn, 15, here 139)
    u64 sp = k->sp - 8, p = k_user_addr(sp, 8, 1);
    if (!p) { x->kc->ev.kind = EV_FAULT; x->kc->ev.access = 1; x->kc->ev.addr = sp; return EV_FAULT; }
    *(u64 *)(uptr)p = k->x[30];
    R[RDI] = k->x[0]; R[RSI] = k->x[1]; R[RDX] = k->x[2]; R[RAX] = 0; R[RSP] = sp;
    k->x[X_PEND] = 0; rflags = 0x202;
    if (trace) logf_("pisco: signal %llu: handler 0x%llx", k->x[0], rip);
  } else if (k->x[X_PEND]) {
    if (rip + 4 == k->x[X_NEXT]) {   // restarted (pc backed up over arm64's svc): the kernel runs it again
      k->pc = MARK | k->x[X_NEXT];
      if (trace) logf_("pisco: system call %llu restarted", k->x[X_PEND] - 1);
      return EV_SYSCALL;
    }
    syscall_done(c, k);
  }
  struct kw_cpu *kc = x->kc;
  if (!kc->budget) { store(c, x->kr, rip, rflags); return EV_BUDGET; }
  if (kc->irq_pending && *(volatile u32 *)(uptr)kc->irq_pending) { store(c, x->kr, rip, rflags); return EV_INTERRUPT; }
  cur = x;
  x->budget_polls = mc_budget = polls_for(kc->budget);
  if (!x->budget_polls) x->budget_polls = mc_budget = 1;
  if (setjmp(x->exit)) {
    // what the polls since the last check used
    if (x->exit_ev != EV_BUDGET && x->exit_ev != EV_INTERRUPT) { u64 used = (u64)(x->budget_polls - mc_budget) * INSN_PER_POLL; if (used > kc->budget) used = kc->budget; kc->budget -= used; kc->icount += used; }
    store(c, x->kr, x->exit_rip, x->exit_ev == EV_SYSCALL ? x->rflags : rflags);
    if (x->exit_ev == EV_ILLEGAL || x->exit_ev == EV_BREAKPOINT) { kc->ev.kind = x->exit_ev; kc->ev.addr = x->exit_rip; kc->ev.insn = 0; }
    kc->ev.kind = x->exit_ev;
    if (trace > 1) logf_("pisco: out at 0x%llx: event %u", x->exit_rip, x->exit_ev);
    cur = outer;
    return x->exit_ev;
  }
  run_from(x, rip);
  return EV_ILLEGAL;
}
// what kwemu returned: the loader (x86load, aarch64) asks to become x86-64 with `svc` and x8 = 0x7ff0, the entry
// in x0 and the stack in x1. Its registers become an x86-64 task's, and it runs at once.
EXPORT("pisco_after") u32 pisco_after(u64 cpu_, u64 regs_, u32 ev) {
  struct kw_regs *k = (struct kw_regs *)(uptr)regs_;
  if (ev != EV_SYSCALL || k->x[8] != 0x7ff0) return ev;
  u64 entry = k->x[0], sp = k->x[1];
  memset(k->x, 0, sizeof k->x); memset(k->v, 0, sizeof k->v);
  k->sp = sp; k->pc = MARK | entry; k->tpidr = 0; k->x[X_TAG] = X64_TAG;
  k->x[X_FP] = 0x1F80 | (u64)0x037F << 32; k->x[X_FLAGS] = 0x202;
  programs_forget();   // (a new program in this address space)
  if (trace) logf_("pisco: a task becomes x86-64: entry 0x%llx, stack 0x%llx", entry, sp);
  return pisco_run(cpu_, regs_);
}
EXPORT("pisco_stats") void pisco_stats(void) {
  logf_("pisco: %llu runs, %llu system calls to the kernel, %llu exits at polls, %llu TLB misses, %llu full TLB flushes, %llu entries at return addresses, %llu programs asked for, %u registered",
        st_runs, st_syscalls, st_polls_out, st_misses, st_flushes, st_reenter, st_asked, n_progs);
  logf_("pisco: flushes: %llu ranges, %llu all, %llu address spaces; %llu page hashes; %llu indirect calls too deep, unwound", st_fk[1], st_fk[2], st_fk[3], st_hash, st_unwound);
}
