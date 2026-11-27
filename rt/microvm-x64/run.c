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

// an indirect call or jump: a function's start, or a return address in one (a program's longjmp jumps to the
// return address of its setjmp): then what was running is abandoned and the code is entered there, in the loop
// that runs it (the innermost level's, or the task's root)
static int is_function(u64 pc) { unsigned lo = 0, hi = fn_count; while (lo < hi) { unsigned m = (lo + hi) / 2; if (fn_addrs[m] < pc) lo = m + 1; else hi = m; } return lo < fn_count && fn_addrs[lo] == pc; }
void rt_call(Cpu *c, u64 t) {
  if (is_function(t)) { rt_dispatch(c, t); return; }
  if (co_cur->levels) { co_cur->levels->loop_pc = t; longjmp(co_cur->levels->loop, 1); }
  co_cur->root_pc = t; longjmp(co_cur->root, 1);
}
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
