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

HOST("co_switch") void host_co_switch(int from, int to, int start);   // suspends `from`; resumes `to` or starts it

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

void rt_call(Cpu *c, u64 t) { rt_dispatch(c, t); }
// a lifted call came back with another stack pointer than it was made with: the guest switched stacks in a way
// the machine did not see (only __switch_to_asm's is a task switch), and the host's frames no longer match it
void rt_bad_return(Cpu *c, u64 call, u64 sp) {
  uart_flush();
  rt_fatal("machine: the call at 0x%llx came back with the stack at 0x%llx, not 0x%llx (a stack switch the machine does not know)", call, R[RSP], sp);
}

// ---- entering lifted code by address: a function's start, or a return address in one (c->resume) ----
extern const unsigned rt_resume_count; extern const u64 rt_resume_at[]; extern const unsigned rt_resume_fn[];
static void enter(Cpu *c, u64 pc) {
  unsigned lo = 0, hi = fn_count;
  while (lo < hi) { unsigned m = (lo + hi) / 2; if (fn_addrs[m] < pc) lo = m + 1; else hi = m; }
  if (lo < fn_count && fn_addrs[lo] == pc) { fn_ptrs[lo](c); return; }
  lo = 0; hi = rt_resume_count;
  while (lo < hi) { unsigned m = (lo + hi) / 2; if (rt_resume_at[m] < pc) lo = m + 1; else hi = m; }
  if (lo < rt_resume_count && rt_resume_at[lo] == pc) { c->resume = pc; fn_ptrs[rt_resume_fn[lo]](c); return; }
  uart_flush();
  rt_fatal("machine: the CPU goes on at 0x%llx, which is no lifted function and no return address", pc);
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
  if (n_co == MAX_CO) rt_fatal("machine: more than %d kernel tasks", MAX_CO);
  Co *co = calloc(1, sizeof *co); co->id = n_co; tasks[n_co++] = co; return co;
}
u32 co_stack_top(int id) { (void)id; char *s = malloc(C_STACK); if (!s) rt_fatal("machine: no memory for a task's C stack"); return (u32)(uptr)(s + C_STACK); }
void co_main(int id) {
  Co *co = tasks[id]; co_cur = co;
  co->root_pc = co->start_pc;
  root_loop(&cpu, co);
}
void rt_switch(Cpu *c, u64 rsp, u64 next) {
  Co *me = co_cur, *to = 0;
  me->key = R[RSP];
  for (int i = 0; i < n_co; i++) if (tasks[i] != me && tasks[i]->parked && tasks[i]->key == rsp) { to = tasks[i]; break; }
  R[RSP] = rsp;
  int start = 0;
  if (!to) { to = co_new(); to->start_pc = next; start = 1; }
  if (mc_trace > 1) rt_log("machine: task switch from stack 0x%llx to 0x%llx (%s)", me->key, rsp, start ? "a new task" : "it goes on");
  me->parked = 1; to->parked = 0; me->sp = rt_sp_get();
  host_co_switch(me->id, to->id, start);
  rt_sp_set(me->sp); me->parked = 0; co_cur = me;
}

// ---- the start ----
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
  Cpu *c = &cpu; c->mxcsr = 0x1F80; c->fcw = 0x037F;
  Co *boot = co_new(); boot->started = 1; co_cur = boot;
  boot->root_pc = rt_machine_entry;
  root_loop(c, boot);
}
