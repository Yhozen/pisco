// SPDX-License-Identifier: GPL-2.0-or-later
// The machine starts: its RAM, the kernel's ELF loaded at its physical addresses, the CPU in long mode on the
// kernel's own top page table (rt_machine_pgt), entered at rt_machine_entry (the lifter names both: Linux's
// common_startup_64 and early_top_pgt). Then lifted code runs in loops that enter it by address.
//
// Each of the kernel's tasks has a host stack of its own (JSPI: the host's co_switch import suspends one wasm
// stack and resumes another), and a C stack of its own. When __switch_to_asm loads the next task's stack
// pointer (rt_switch), the task that was left with that stack pointer goes on where it switched away; one never
// run starts a stack that enters the code after that load. So a task switch is a switch of host stacks, and the
// lifted functions on each go on as if nothing happened.
#include <unistd.h>
#include "machine.h"

HOST("co_switch") void host_co_switch(int from, int to, int start, int dead);   // suspends `from` (or drops it: dead); resumes `to` or starts it
HOST("stat") s64 host_stat(const char *name, u32 len);                 // a file's size, -1 missing

// the C stack pointer (a wasm global), and the entry of a new host stack: its C stack set before any C runs
__asm__(
  ".globaltype __stack_pointer, i32\n"
  ".functype co_stack_top (i32) -> (i32)\n"
  ".functype co_main (i32) -> ()\n"
  ".globl rt_sp_get\n.type rt_sp_get,@function\nrt_sp_get:\n.functype rt_sp_get () -> (i32)\nglobal.get __stack_pointer\nend_function\n"
  ".globl rt_sp_set\n.type rt_sp_set,@function\nrt_sp_set:\n.functype rt_sp_set (i32) -> ()\nlocal.get 0\nglobal.set __stack_pointer\nend_function\n"
  ".globl rt_co_entry\n.type rt_co_entry,@function\n.export_name rt_co_entry, rt_co_entry\nrt_co_entry:\n.functype rt_co_entry (i32) -> ()\n"
  "local.get 0\ncall co_stack_top\nglobal.set __stack_pointer\nlocal.get 0\ncall co_main\nend_function\n");
u32 rt_sp_get(void); void rt_sp_set(u32);

// a static-call trampoline (Linux's __SCT__*), as the kernel has rewritten it: a jump to the function the call goes
// to now (the call's return address is the caller's), or a return (no function)
void rt_static_call(Cpu *c, u64 tramp) {
  u8 b[5]; vread(tramp, b, 5);
  if (b[0] == 0xE9) { s32 rel; memcpy(&rel, b + 1, 4); rt_dispatch(c, tramp + 5 + (u64)(s64)rel); return; }
  if (b[0] == 0xC3) { R[RSP] += 8; return; }
  uart_flush(); rt_fatal("machine: the static-call trampoline at 0x%llx holds %02x %02x %02x %02x %02x", tramp, b[0], b[1], b[2], b[3], b[4]);
}
// a lifted call came back with another stack pointer than it was made with. Fine when the address it returned
// through (just below the stack) is this call's own: the callee moved the stack on purpose (Linux's error_entry
// leaves the registers it saved there). Else the guest switched stacks in a way the machine did not see (only
// __switch_to_asm's is a task switch), and the host's frames no longer match it
void rt_check_return(Cpu *c, u64 call, u64 next, u64 sp) {
  if (vread64(R[RSP] - 8) == next) return;
  uart_flush();
  rt_fatal("machine: the call at 0x%llx came back with the stack at 0x%llx, not 0x%llx, from 0x%llx (a stack switch the machine does not know)", call, R[RSP], sp, vread64(R[RSP] - 8));
}

// ---- the programs the kernel runs, lifted on their own (lifter --user; their list: programs.gen.c) ----
// Several may be at the same addresses (bash and busybox both at 0x400000): the one in memory is the one whose code
// page at the address that runs hashes as its file's. Found again after each write of CR3 (another address space,
// or a page table's address used again). A page of code the kernel has not mapped yet is fetched first, as the CPU
// fetches an instruction: the kernel's page fault handler maps it.
__attribute__((weak)) extern const ProgramDef *const rt_programs[];
__attribute__((weak)) extern const unsigned rt_program_count;
// programs in modules of their own (a side module each: the host loads it into this memory and this table, then
// registers it here), beside those linked into this module
static const ProgramDef *side[256]; static unsigned n_side;
__attribute__((export_name("rt_program_register"))) void rt_program_register(const ProgramDef *p) {
  if (n_side == 256) rt_fatal("machine: more than 256 programs in modules of their own");
  side[n_side++] = p; if (mc_trace) rt_log("machine: program %s, a module of its own", p->name);
}
// the memory a side module's data and C stack frames need: the host asks for it before it instantiates the module
__attribute__((export_name("rt_side_alloc"))) u32 rt_side_alloc(u32 size, u32 align) { void *p = aligned_alloc(align < 16 ? 16 : align, (size + 15) & ~15u); if (!p) rt_fatal("machine: no memory for a program's module"); memset(p, 0, size); return (u32)(uptr)p; }
static const ProgramDef *prog_cur;
void machine_programs_forget(void) { prog_cur = 0; }
static u64 code_hash(Cpu *c, u64 pc, u64 lo, u64 hi) {
  u64 page = pc & ~0xFFFull, from = page > lo ? page : lo, to = page + 4096 < hi ? page + 4096 : hi; u8 b[4096];
  u64 pa; int w;
  if (mmu_walk(page, 0, 1, &pa, &w) >= 0) mmu_read(c, mmu_u, pc, 1, pc, b);   // (a fetch: the kernel maps the page)
  if (mmu_walk(page, 0, 1, &pa, &w) >= 0) return 0;
  phys_read(pa + (from - page), b, (unsigned)(to - from));
  u64 h = 0xcbf29ce484222325ull; for (u64 k = 0; k < to - from; k++) h = (h ^ b[k]) * 0x100000001b3ull; return h;
}
// No program holds the code there: the program that does is in memory, as the kernel loaded it. A static program
// that is not position-independent has its ELF header at the start of its first segment, below its code: its
// headers and its segments' bytes, read from memory (its pages faulted in as reads would), are its file again,
// as the file has them when it starts (the machine meets a program first at its entry point). The host lifts that
// into a module of its own (web/lift.mjs, cached by the image's sha256), loads it and registers it; 1 when it did.
HOST("program_image") int host_program_image(u32 image, u32 len, u32 fns, u32 n_fns);   // and its functions' addresses (u64s)
static const ProgramDef *program_at(Cpu *c, u64 pc);
// fault: a page not mapped yet is faulted in (only inside the program's segments: there the kernel maps it); else
// 0 when a page is not there (the stack, which the kernel has just written at a program's start)
static int user_read(Cpu *c, u64 va, void *out, unsigned n, int fault) {
  for (unsigned k = 0; k < n;) {
    u64 a = va + k, pa; int w; unsigned m = 4096 - (unsigned)(a & 4095); if (m > n - k) m = n - k;
    if (fault && mmu_walk(a, 0, 1, &pa, &w) >= 0) { u8 b; mmu_read(c, mmu_u, a, 1, a, &b); }
    if (mmu_walk(a, 0, 1, &pa, &w) >= 0) return 0;
    phys_read(pa, (u8 *)out + k, m); k += m;
  }
  return 1;
}
// the file offset of a virtual address of an image (its PT_LOAD headers), or ~0
static u64 off_of(const u8 *ph, unsigned phnum, u64 va) {
  for (unsigned i = 0; i < phnum; i++) { u32 t; u64 off, v, fs; memcpy(&t, ph + 56 * i, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&v, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8);
    if (t == 1 && va >= v && va < v + fs) return off + (va - v); }
  return ~0ull;
}
// an image's functions, by its dynamic symbols (a loader calls its own through pointers it finds by name: musl's
// __dls2b, __dls3), into fns (their addresses as loaded): the host tells the lifter of them
static u64 fns[4096]; static unsigned n_fns;
static void dynamic_functions(const u8 *img, u64 size, const u8 *ph, unsigned phnum, u64 bias, u64 symtab, u64 strtab) {
  u64 so = symtab ? off_of(ph, phnum, symtab) : ~0ull; if (so == ~0ull || strtab <= symtab) return;
  for (u64 k = 0; k < (strtab - symtab) / 24 && so + 24 * (k + 1) <= size && n_fns < 4096; k++) {
    u8 info = img[so + 24 * k + 4]; u16 shndx; u64 value; memcpy(&shndx, img + so + 24 * k + 6, 2); memcpy(&value, img + so + 24 * k + 8, 8);
    if ((info & 15) == 2 && shndx && value) fns[n_fns++] = bias + value;   // STT_FUNC, defined
  }
}
// its relocations (RELA's and the PLT's, RELR), applied to the copy as its loader applies them to memory, when apply
// (a program its loader has relocated already has them): so that the code addresses its data holds are addresses
// (what the lifter finds functions by). Its functions found by its dynamic symbols either way
static void relocate(u8 *img, u64 size, const u8 *ph, unsigned phnum, u64 bias, int apply) {
  u64 dyn = ~0ull, dynsz = 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t; memcpy(&t, ph + 56 * i, 4); if (t == 2) { memcpy(&dyn, ph + 56 * i + 8, 8); memcpy(&dynsz, ph + 56 * i + 32, 8); } }
  if (dyn == ~0ull || dyn + dynsz > size) return;
  u64 rela = 0, relasz = 0, relr = 0, relrsz = 0, jmprel = 0, pltsz = 0, symtab = 0, strtab = 0;
  for (u64 k = 0; k + 16 <= dynsz; k += 16) { u64 tag, v; memcpy(&tag, img + dyn + k, 8); memcpy(&v, img + dyn + k + 8, 8);
    if (tag == 0) break; if (tag == 7) rela = v; if (tag == 8) relasz = v; if (tag == 35) relrsz = v; if (tag == 36) relr = v;   // DT_RELRSZ, DT_RELR
    if (tag == 23) jmprel = v; if (tag == 2) pltsz = v; if (tag == 6) symtab = v; if (tag == 5) strtab = v; }
  dynamic_functions(img, size, ph, phnum, bias, symtab, strtab);
  if (!apply) return;
  // RELA and the PLT's: relative ones, and those to a symbol the image defines itself (its own functions, called
  // through its PLT or its GOT: R_X86_64_64, GLOB_DAT, JUMP_SLOT)
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
static int ask_host(Cpu *c, u64 pc) {
  // its auxiliary vector, on its stack where it starts: its program headers (AT_PHDR), its entry, its interpreter's
  // base (AT_BASE: a dynamically linked program's loader, which runs first)
  u64 sp = R[RSP], argc, v, aphdr = 0, aphnum = 0, entry = 0, ibase = 0;
  if (!user_read(c, sp, &argc, 8, 0) || argc > 4096) return 0;
  u64 at = sp + 8 * (argc + 2);
  for (int k = 0; k < 4096; k++, at += 8) { if (!user_read(c, at, &v, 8, 0)) return 0; if (!v) break; }   // the environment
  for (int k = 0; k < 64; k++) {
    u64 kv[2]; at += 8; if (!user_read(c, at, kv, 16, 0)) return 0; at += 8;
    if (kv[0] == 0) break; if (kv[0] == 3) aphdr = kv[1]; if (kv[0] == 5) aphnum = kv[1]; if (kv[0] == 7) ibase = kv[1]; if (kv[0] == 9) entry = kv[1];
  }
  // which image: the program at its entry, or its interpreter where it starts (its ELF header at its base)
  u8 h[64], ph[64 * 56]; u64 phdr, phnum, bias = 0; int loader = 0;
  if (pc == entry && aphdr && aphnum && aphnum <= 64) { phdr = aphdr; phnum = aphnum; }
  else if (ibase) { if (!user_read(c, ibase, h, 64, 1) || memcmp(h, "\x7f" "ELF\x02\x01", 6)) return 0; u64 po; u16 pn; memcpy(&po, h + 32, 8); memcpy(&pn, h + 56, 2); phdr = ibase + po; phnum = pn; bias = ibase; loader = 1; if (phnum > 64) return 0; }
  else return 0;
  if (!user_read(c, phdr, ph, 56u * (unsigned)phnum, 1)) return 0;
  if (!loader) for (unsigned i = 0; i < phnum; i++) { u32 t; u64 va; memcpy(&t, ph + 56 * i, 4); memcpy(&va, ph + 56 * i + 16, 8); if (t == 6) bias = phdr - va; }   // PT_PHDR: a position-independent program's load bias
  // its first segment (file offset 0) holds its ELF header; the image is its segments' bytes at their offsets
  u64 size = 0, base = ~0ull; int holds = 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t, fl; u64 off, va, fs, ms; memcpy(&t, ph + 56 * i, 4); memcpy(&fl, ph + 56 * i + 4, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&va, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8); memcpy(&ms, ph + 56 * i + 40, 8);
    if (t != 1) continue; if (off + fs > size) size = off + fs; if (off == 0) base = va + bias; if ((fl & 1) && pc >= va + bias && pc < va + bias + ms) holds = 1; }
  if (!holds || base == ~0ull || size > (256u << 20)) return 0;
  if (!user_read(c, base, h, 64, 1) || memcmp(h, "\x7f" "ELF\x02\x01", 6)) return 0;
  u8 *img = calloc(1, size); if (!img) return 0;
  for (unsigned i = 0; i < phnum; i++) { u32 t; u64 off, va, fs; memcpy(&t, ph + 56 * i, 4); memcpy(&off, ph + 56 * i + 8, 8); memcpy(&va, ph + 56 * i + 16, 8); memcpy(&fs, ph + 56 * i + 32, 8);
    if (t == 1 && fs && !user_read(c, va + bias, img + off, (unsigned)fs, 1)) { free(img); return 0; } }
  // a loader, or a program that relocates itself, has not run its relocations yet where it starts: done in the copy
  n_fns = 0; relocate(img, size, ph, (unsigned)phnum, bias, loader || !ibase);
  // as a program at the addresses it was loaded at: no dynamic headers, its addresses moved by its bias, no sections
  u64 po; memcpy(&po, h + 32, 8); u8 *p = img + po; u16 pn; memcpy(&pn, h + 56, 2);
  if (po + 56u * pn <= size) for (unsigned i = 0; i < pn; i++) {
    u32 t; memcpy(&t, p + 56 * i, 4);
    if (t == 2 || t == 3 || t == 0x6474e550 || t == 0x6474e552) { t = 0; memcpy(p + 56 * i, &t, 4); continue; }   // DYNAMIC, INTERP, GNU_EH_FRAME, GNU_RELRO
    u64 va; memcpy(&va, p + 56 * i + 16, 8); va += bias; memcpy(p + 56 * i + 16, &va, 8); memcpy(p + 56 * i + 24, &va, 8);
  }
  u16 exec = 2; memcpy(img + 16, &exec, 2); u64 e; memcpy(&e, h + 24, 8); e += bias; memcpy(img + 24, &e, 8);
  memset(img + 40, 0, 8); memset(img + 60, 0, 4);
  uart_flush();
  int r = host_program_image((u32)(uptr)img, (u32)size, (u32)(uptr)fns, n_fns); free(img); return r;
}
static const ProgramDef *program_at(Cpu *c, u64 pc) {
  unsigned linked = &rt_program_count ? rt_program_count : 0;
  if (!linked && !n_side) { if (ask_host(c, pc)) return program_at(c, pc); return 0; }
  if (prog_cur && pc >= prog_cur->lo && pc < prog_cur->hi) return prog_cur;
  prog_cur = 0;
  for (unsigned i = 0; i < linked + n_side; i++) {
    const ProgramDef *p = i < linked ? rt_programs[i] : side[i - linked];
    if (pc < p->lo || pc >= p->hi) continue;
    if (code_hash(c, pc, p->lo, p->hi) == p->page_hashes[((pc & ~0xFFFull) - (p->lo & ~0xFFFull)) >> 12]) { prog_cur = p; break; }
  }
  if (mc_trace && prog_cur) rt_log("machine: user code at 0x%llx is %s's", pc, prog_cur->name);
  if (!prog_cur) { static u64 asked; if (asked != pc) { asked = pc; if (ask_host(c, pc)) return program_at(c, pc); } }
  if (mc_trace && !prog_cur) for (unsigned i = 0; i < linked + n_side; i++) { const ProgramDef *p = i < linked ? rt_programs[i] : side[i - linked];
    rt_log("machine: user code at 0x%llx: not %s's (0x%llx to 0x%llx, page hash 0x%llx here, 0x%llx there)", pc, p->name, p->lo, p->hi, code_hash(c, pc, p->lo, p->hi), pc >= p->lo && pc < p->hi ? p->page_hashes[((pc & ~0xFFFull) - (p->lo & ~0xFFFull)) >> 12] : 0); }
  return prog_cur;
}

// ---- entering lifted code by address: a function's start, or a return address in one (c->resume) ----
extern const unsigned rt_resume_count; extern const u64 rt_resume_at[]; extern const unsigned rt_resume_fn[];
typedef struct { unsigned fn_count; const u64 *fn_addrs; void (*const *fn_ptrs)(Cpu *); unsigned resume_count; const u64 *resume_at; const unsigned *resume_fn; } Code;
static Code code_of(Cpu *c, u64 pc) {
  const ProgramDef *p = (pc >> 47) ? 0 : program_at(c, pc);
  if (p) return (Code){ p->fn_count, p->fn_addrs, p->fn_ptrs, p->resume_count, p->resume_at, p->resume_fn };
  return (Code){ fn_count, fn_addrs, fn_ptrs, rt_resume_count, rt_resume_at, rt_resume_fn };   // the kernel's (and programs lifted with it)
}
static int find(const u64 *v, unsigned n, u64 pc) { unsigned lo = 0, hi = n; while (lo < hi) { unsigned m = (lo + hi) / 2; if (v[m] < pc) lo = m + 1; else hi = m; } return lo < n && v[lo] == pc ? (int)lo : -1; }
static void enter(Cpu *c, u64 pc) {
  Code k = code_of(c, pc); int i;
  if ((i = find(k.fn_addrs, k.fn_count, pc)) >= 0) { k.fn_ptrs[i](c); return; }
  if ((i = find(k.resume_at, k.resume_count, pc)) >= 0) { c->resume = pc; k.fn_ptrs[k.resume_fn[i]](c); return; }
  uart_flush();
  rt_fatal("machine: the CPU goes on at 0x%llx, which is no lifted function and no return address", pc);
}
// an indirect call or jump: a function's start, or a return address in one (a program's longjmp jumps to the
// return address of its setjmp): then what was running is abandoned and the code is entered there, in the loop
// that runs it (the innermost level's, or the task's root)
void rt_call(Cpu *c, u64 t) {
  Code k = code_of(c, t); int i;
  if ((i = find(k.fn_addrs, k.fn_count, t)) >= 0) { k.fn_ptrs[i](c); return; }
  if (co_cur->levels) { co_cur->levels->loop_pc = t; longjmp(co_cur->levels->loop, 1); }
  co_cur->root_pc = t; longjmp(co_cur->root, 1);
}
// a level's loop: its handler, then wherever it goes (a function that returns: on at the address it popped)
void run_level(Cpu *c, Level *l, u64 pc) {
  l->loop_pc = pc;
  setjmp(l->loop);
  u64 p = l->loop_pc;
  for (;;) { c->landing = 0; enter(c, p); p = vread64(R[RSP] - 8); }
}
static void root_loop(Cpu *c, Co *co) {
  setjmp(co->root);
  u64 p = co->root_pc;
  for (;;) { c->landing = 0; enter(c, p); p = vread64(R[RSP] - 8); }
}

// ---- the tasks' host stacks ----
static Cpu cpu;
enum { MAX_CO = 1024, C_STACK = 512 * 1024 };
static Co *tasks[MAX_CO]; static int n_co;
Co *co_cur;
static Co *co_new(void) {
  for (int i = 0; i < n_co; i++) if (tasks[i]->free) { Co *co = tasks[i]; int id = co->id; memset(co, 0, sizeof *co); co->id = id; return co; }   // a dead task's slot
  if (n_co == MAX_CO) rt_fatal("machine: more than %d kernel tasks at once", MAX_CO);
  Co *co = calloc(1, sizeof *co); co->id = n_co; tasks[n_co++] = co; return co;
}
static Co *co_dead;   // a task that died and switched away: its C stack is freed by the next to run
static void reap(void) { if (co_dead) { free(co_dead->cstack); co_dead->cstack = 0; co_dead->free = 1; co_dead = 0; } }
void rt_task_dying(Cpu *c) { (void)c; co_cur->dying = 1; }
u32 co_stack_top(int id) { char *s = malloc(C_STACK); if (!s) rt_fatal("machine: no memory for a task's C stack"); tasks[id]->cstack = s; return (u32)(uptr)(s + C_STACK); }
void co_main(int id) {
  Co *co = tasks[id]; co_cur = co; reap();
  co->root_pc = co->start_pc;
  root_loop(&cpu, co);
}
void rt_switch(Cpu *c, u64 rsp, u64 next) {
  Co *me = co_cur, *to = 0;
  me->key = R[RSP];
  for (int i = 0; i < n_co; i++) if (tasks[i] != me && tasks[i]->parked && !tasks[i]->free && tasks[i]->key == rsp) { to = tasks[i]; break; }
  R[RSP] = rsp;
  int start = 0;
  if (!to) { to = co_new(); to->start_pc = next; start = 1; }
  if (mc_trace > 1) rt_log("machine: task switch from %d (stack 0x%llx%s) to %d (0x%llx, %s)", me->id, me->key, me->dying ? ", ending" : "", to->id, rsp, start ? "a new task" : "it goes on");
  int dead = me->dying && me->cstack;   // (the boot task's C stack is the module's own: never freed)
  me->parked = !dead; to->parked = 0; me->sp = rt_sp_get();
  if (dead) co_dead = me;
  host_co_switch(me->id, to->id, start, dead);
  rt_sp_set(me->sp); me->parked = 0; co_cur = me; reap();
}

// ---- the start ----
// ---- what a boot loader hands the kernel (Linux's 64-bit boot protocol): the boot parameters ("zero page") with
// the memory map, the command line and the initramfs; an MP table with the CPU, the IOAPIC and the ISA interrupts ----
enum { ZERO_PAGE = 0x7000, CMDLINE = 0x20000, MPTABLE = 0x9FC00 };
static void put8(u64 pa, u8 v) { ram[pa] = v; }
static void put16(u64 pa, u16 v) { memcpy(ram + pa, &v, 2); }
static void put32(u64 pa, u32 v) { memcpy(ram + pa, &v, 4); }
static void put64(u64 pa, u64 v) { memcpy(ram + pa, &v, 8); }
static u8 sum(u64 pa, unsigned n) { u8 s = 0; for (unsigned i = 0; i < n; i++) s += ram[pa + i]; return s; }
static void mptable(void) {
  u64 fp = MPTABLE, t = MPTABLE + 16, e = t + 44; unsigned count = 0;
  #define ENTRY(...) do { u8 b_[] = { __VA_ARGS__ }; memcpy(ram + e, b_, sizeof b_); e += sizeof b_; count++; } while (0)
  ENTRY(0, 0, 0x14, 3, 0x00, 0x06, 0, 0, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);   // the CPU: local APIC 0, enabled, the boot CPU
  ENTRY(1, 0, 'I', 'S', 'A', ' ', ' ', ' ');                                          // bus 0: ISA
  ENTRY(2, 1, 0x11, 1, 0x00, 0x00, 0xC0, 0xFE);                                       // IOAPIC 1 at 0xfec00000
  for (u8 irq = 0; irq < 16; irq++) ENTRY(3, 0, 0, 0, 0, irq, 1, irq);                // ISA interrupt n on pin n
  ENTRY(4, 3, 0, 0, 0, 0, 0xFF, 0);                                                   // LINT0: ExtINT
  ENTRY(4, 1, 0, 0, 0, 0, 0xFF, 1);                                                   // LINT1: NMI
  #undef ENTRY
  memcpy(ram + t, "PCMP", 4); put16(t + 4, (u16)(e - t)); put8(t + 6, 4); memcpy(ram + t + 8, "PISCO   ", 8); memcpy(ram + t + 16, "microvm-x64 ", 12);
  put16(t + 34, (u16)count); put32(t + 36, 0xFEE00000); put8(t + 7, (u8)-sum(t, (unsigned)(e - t)));
  memcpy(ram + fp, "_MP_", 4); put32(fp + 4, (u32)t); put8(fp + 8, 1); put8(fp + 9, 4); put8(fp + 10, (u8)-sum(fp, 16));
}
static void boot_params(void) {
  u64 z = ZERO_PAGE;
  const char *line = getenv("SW_CMDLINE");
  if (!line) line = "console=ttyS0,115200 earlyprintk=serial,ttyS0,115200 panic=-1 no_timer_check vdso=0 tsc=reliable 8250.nr_uarts=1";
  char with[4096];
  if (virtio_present()) { snprintf(with, sizeof with, "%s virtio_mmio.device=4K@0x%llx:5", line, (unsigned long long)virtio_base()); line = with; }   // the disk, announced
  size_t n = strlen(line); if (n > 4095) n = 4095;
  memcpy(ram + CMDLINE, line, n); ram[CMDLINE + n] = 0;
  put16(z + 0x1FE, 0xAA55); put32(z + 0x202, 0x53726448); put16(z + 0x206, 0x020F);   // the setup header: "HdrS", 2.15
  put8(z + 0x210, 0xFF); put8(z + 0x211, 0x01); put32(z + 0x228, CMDLINE); put32(z + 0x238, 4095);
  put32(z + 0x22C, 0x7FFFFFFF); put32(z + 0x230, 0x200000);
  // the memory map: low memory but the MP table, then everything from 1 MiB
  struct { u64 a, n; u32 type; } map[] = { { 0, MPTABLE, 1 }, { MPTABLE, 0x100000 - MPTABLE, 2 }, { 0x100000, ram_size - 0x100000, 1 } };
  for (unsigned i = 0; i < 3; i++) { u64 at = z + 0x2D0 + 20 * i; put64(at, map[i].a); put64(at + 8, map[i].n); put32(at + 16, map[i].type); }
  put8(z + 0x1E8, 3);
  // the initramfs (SW_INITRD, a file beside the kernel), at the top of RAM
  const char *rd = getenv("SW_INITRD");
  if (rd && *rd) {
    s64 size = host_stat(rd, (u32)strlen(rd));
    if (size <= 0) rt_fatal("machine: no initramfs %s", rd);
    u64 at = (ram_size - (u64)size) & ~0xFFFFFull;
    if (host_read_file(rd, (u32)strlen(rd), 0, (u64)size, (u64)(uptr)(ram + at)) != (u64)size) rt_fatal("machine: cannot read %s", rd);
    put32(z + 0x218, (u32)at); put32(z + 0x21C, (u32)size);
  }
  mptable();
}

static void load(const char *name) {
  u8 h[64];
  if (host_read_file(name, (u32)strlen(name), 0, 64, (u64)(uptr)h) != 64 || memcmp(h, "\x7f" "ELF\x02\x01", 6)) rt_fatal("machine: %s is no 64-bit ELF", name);
  u64 phoff; u16 phnum; memcpy(&phoff, h + 32, 8); memcpy(&phnum, h + 56, 2);
  u8 *ph = malloc(56u * phnum);
  if (host_read_file(name, (u32)strlen(name), phoff, 56u * phnum, (u64)(uptr)ph) != 56u * phnum) rt_fatal("machine: cannot read the program headers of %s", name);
  u64 pgt = 0;
  for (unsigned i = 0; i < phnum; i++) {
    u8 *p = ph + 56 * i; u32 type; u64 off, va, pa, filesz, memsz;
    memcpy(&type, p, 4); memcpy(&off, p + 8, 8); memcpy(&va, p + 16, 8); memcpy(&pa, p + 24, 8); memcpy(&filesz, p + 32, 8); memcpy(&memsz, p + 40, 8);
    if (type != 1) continue;
    if (pa + memsz > ram_size) rt_fatal("machine: %s's segment at 0x%llx does not fit in %llu MiB of RAM", name, pa, ram_size >> 20);
    if (filesz && host_read_file(name, (u32)strlen(name), off, filesz, (u64)(uptr)(ram + pa)) != filesz) rt_fatal("machine: cannot read a segment of %s", name);
    if (rt_machine_pgt >= va && rt_machine_pgt < va + memsz) pgt = rt_machine_pgt - va + pa;
  }
  free(ph);
  if (!pgt) rt_fatal("machine: %s has no early_top_pgt in a loaded segment", name);
  mc.cr3 = pgt;
}
int main(void) {
  const char *e = getenv("SW_TRACE"); mc_trace = e ? atoi(e) : 0;
  u64 mib = (e = getenv("SW_MEMORY")) ? strtoull(e, 0, 10) : 256;
  ram_size = mib << 20; ram = aligned_alloc(4096, ram_size);
  if (!ram) rt_fatal("machine: no room for %llu MiB of RAM", mib);
  memset(ram, 0, ram_size);
  // the CPU as the 64-bit boot protocol leaves it: long mode, paging, interrupts off, the kernel's segments
  mc.cr0 = 0x80050033; mc.cr4 = 0x20; mc.efer = 0xD01; mc.cpl = 0; mc.seg[S_CS] = 0x08; mc.seg[S_SS] = mc.seg[S_DS] = mc.seg[S_ES] = 0x10;
  mc.apic_base = 0xFEE00900; mc.pat = 0x0007040600070406ull;
  load(rt_modules[0].name);
  virtio_init();
  boot_params();
  console_init(); mc_paced = (e = getenv("SW_CONSOLE")) && !strcmp(e, "tty");
  Cpu *c = &cpu; c->mxcsr = 0x1F80; c->fcw = 0x037F;
  R[15] = ZERO_PAGE;   // where Linux's common_startup_64 finds the boot parameters (physical)
  Co *boot = co_new(); boot->started = 1; co_cur = boot;
  boot->root_pc = rt_machine_entry;
  root_loop(c, boot);
}
