// SPDX-License-Identifier: GPL-2.0-or-later
// fixtures/microvm-hello: a small x86-64 kernel that checks the machine rt/microvm-x64 imitates, one thing at a
// time, on the serial port (docs/linux.md, K1). What it prints is the same on every machine that runs it (QEMU or
// PISCO): no times, no addresses, no counts that depend on speed. It ends at the exit port (0xf4).
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
#define KOFF 0xffffffff80000000ull
#define PHYS(p) ((u64)(p) - KOFF)

static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void wrmsr(u32 msr, u64 v) { __asm__ volatile("wrmsr" :: "c"(msr), "a"((u32)v), "d"((u32)(v >> 32))); }
static inline u64 rdmsr(u32 msr) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr)); return (u64)hi << 32 | lo; }
static inline void invlpg(u64 va) { __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory"); }
static inline u64 read_cr3(void) { u64 v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }

// ---- the serial port ----
static void putc_(char ch) { while (!(inb(0x3fd) & 0x20)) {} outb(0x3f8, (u8)ch); }
static void puts_(const char *s) { while (*s) { if (*s == '\n') putc_('\r'); putc_(*s++); } }
static void putx(u64 v) { char b[19]; int n = 0; b[n++] = '0'; b[n++] = 'x'; int i = 60; while (i > 0 && !((v >> i) & 15)) i -= 4;
  for (; i >= 0; i -= 4) b[n++] = "0123456789abcdef"[(v >> i) & 15]; b[n] = 0; puts_(b); }
static void putd(u64 v) { char b[21]; int n = 20; b[n] = 0; do { b[--n] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + n); }
static void say(const char *a, u64 v, const char *b) { puts_(a); putd(v); puts_(b); }

// ---- interrupts ----
struct regs { u64 r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax, vec, err, rip, cs, rflags, rsp, ss; };
struct gate { u16 lo; u16 sel; u8 ist, type; u16 mid; u32 hi; u32 zero; } __attribute__((packed));
static struct gate idt[256] __attribute__((aligned(16)));
static struct { u16 limit; u64 base; } __attribute__((packed)) idt_ptr;
static struct { u32 r0; u64 rsp0, rsp1, rsp2, r1, ist[7], r2; u16 r3, iomap; } __attribute__((packed)) tss;
extern u64 tss_desc[2];
extern void isr3(void), isr6(void), isr13(void), isr14(void), isr32(void), syscall_entry(void), enter_user(void), user_preempted(void);
extern u64 probe_read(u64 *p); extern char probe_read_insn[], probe_read_fixup[];
static void gate(int n, void (*f)(void), int dpl) { u64 a = (u64)f; idt[n] = (struct gate){ (u16)a, 0x08, 0, (u8)(0x8E | dpl << 5), (u16)(a >> 16), (u32)(a >> 32), 0 }; }

#define APIC 0xfee00000ull
static inline u32 apic_r(u32 r) { return *(volatile u32 *)(APIC + r); }
static inline void apic_w(u32 r, u32 v) { *(volatile u32 *)(APIC + r) = v; }

// ---- page tables of the checks: 4 KiB pages at 0xffffc90000000000 (the 402nd entry of the top table) ----
extern u64 early_top_pgt[512], dyn_slot[1];
static u64 dyn_pdpt[512] __attribute__((aligned(4096))), dyn_pd[512] __attribute__((aligned(4096))), dyn_pt[512] __attribute__((aligned(4096)));
static u8 pool[8][4096] __attribute__((aligned(4096)));
#define DYN 0xffffc90000000000ull
static void map(unsigned slot, void *page) { dyn_pt[slot] = page ? PHYS(page) | 3 : 0; invlpg(DYN + 4096ull * slot); }

static volatile u64 ticks, faults, last_cr2, int3_rip, ud_count, preempted;
static int preempt_user;

void trap(struct regs *r) {
  switch (r->vec) {
  case 3: int3_rip = r->rip; return;                      // a trap: rip is already past the int3
  case 6: ud_count++; r->rip += 2; return;                // a warning: go on past the ud2, as Linux's WARN()
  case 14:
    last_cr2 = read_cr2();
    if (r->rip == (u64)probe_read_insn) { r->rip = (u64)probe_read_fixup; return; }   // the exception table
    if (last_cr2 >> 12 == (DYN >> 12) + 5) { faults++; map(5, pool[5]); return; }     // demand paging: map, go on
    break;
  case 32:
    ticks++; apic_w(0xb0, 0);
    if (preempt_user && (r->cs & 3) == 3) { preempt_user = 0; preempted = 1; r->rip = (u64)user_preempted; r->rsp = 0x800000; }
    return;
  }
  puts_("trap: unexpected vector "); putd(r->vec); puts_(" at "); putx(r->rip); puts_(", cr2 "); putx(read_cr2()); puts_("\n");
  outb(0xf4, 2);
  for (;;) __asm__ volatile("hlt");
}

// ---- tasks ----
struct task { u64 sp; u64 stack[1024]; };
static struct task boot_task, task_a, task_b, task_c;
extern void __switch_to_asm(struct task *prev, struct task *next);
static void task_init(struct task *t, void (*entry)(void)) {
  u64 *sp = &t->stack[1024];
  *--sp = (u64)entry;                 // where __switch_to_asm returns the first time
  for (int i = 0; i < 6; i++) *--sp = 0;   // rbp rbx r12 r13 r14 r15
  t->sp = (u64)sp;
}
static void task_a_main(void) {
  puts_("task A: first run\n"); __switch_to_asm(&task_a, &task_b);
  puts_("task A: resumed\n"); __switch_to_asm(&task_a, &task_b);
  for (;;) {}
}
static void task_b_main(void) {
  puts_("task B: first run\n"); __switch_to_asm(&task_b, &task_a);
  puts_("task B: resumed\n"); __switch_to_asm(&task_b, &boot_task);
  for (;;) {}
}

u64 kernel_rsp0, user_rsp;
void syscall_c(struct regs *r) {
  if (r->rax == 1) { const char *s = (const char *)r->rsi; for (u64 i = 0; i < r->rdx; i++) { if (s[i] == '\n') putc_('\r'); putc_(s[i]); } r->rax = r->rdx; return; }
  if (r->rax == 60) {
    say("exit: ring 3 exited with ", r->rdi, "\n");
    apic_w(0x320, 1 << 16);           // the timer, masked
    __switch_to_asm(&task_c, &boot_task);
  }
  r->rax = (u64)-38;
}

static void timer_periodic(u32 count) { apic_w(0x3e0, 0xb); apic_w(0x320, 32 | 1 << 17); apic_w(0x380, count); }

void kmain(void) {
  puts_("microvm-hello: long mode, paging on\n");
  outb(0x21, 0xff); outb(0xa1, 0xff);   // the 8259s (if any), masked
  for (int i = 0; i < 256; i++) idt[i] = (struct gate){0};
  gate(3, isr3, 3); gate(6, isr6, 0); gate(13, isr13, 0); gate(14, isr14, 0); gate(32, isr32, 0);
  idt_ptr.limit = sizeof idt - 1; idt_ptr.base = (u64)idt;
  __asm__ volatile("lidt %0" :: "m"(idt_ptr));

  // paging: one page seen at two addresses, then the second address moved to another page
  dyn_slot[0] = PHYS(dyn_pdpt) | 3; dyn_pdpt[0] = PHYS(dyn_pd) | 3; dyn_pd[0] = PHYS(dyn_pt) | 3;
  *(volatile u64 *)pool[0] = 0x1234; *(volatile u64 *)pool[1] = 0x5678;
  map(0, pool[0]);
  say("paging: a page read through a second address: ", *(volatile u64 *)DYN, "\n");
  *(volatile u64 *)DYN = 0x4321;
  say("paging: written there, read at the first: ", *(volatile u64 *)pool[0], "\n");
  map(0, pool[1]);
  say("paging: the address moved to another page (invlpg): ", *(volatile u64 *)DYN, "\n");
  say("paging: cr3 is the top table's physical address: ", read_cr3() == PHYS(early_top_pgt), "\n");

  // a page fault handled and returned from: the access goes on once the page is mapped
  *(volatile u64 *)pool[5] = 77;
  say("fault: a page not mapped read as ", *(volatile u64 *)(DYN + 5 * 4096), "");
  say(" after ", faults, " fault"); puts_(last_cr2 == DYN + 5 * 4096 ? " at the right address\n" : " at a wrong address\n");
  // a page fault fixed up: the faulting instruction does not go on, its fix-up does
  say("fixup: a read of an address not mapped returned -", -probe_read((u64 *)(DYN + 7 * 4096)), "\n");

  // a breakpoint and a warning, each returned from
  __asm__ volatile("int3");
  puts_(int3_rip > (u64)kmain && int3_rip < (u64)kmain + 65536 ? "int3: handled, back in kmain\n" : "int3: came back somewhere else\n");
  __asm__ volatile("ud2");
  say("ud2: handled as a warning, ", ud_count, " so far\n");

  // the local APIC's timer, while the CPU halts
  apic_w(0xf0, 0x1ff);                  // enabled, spurious vector 0xff
  timer_periodic(1000000);
  __asm__ volatile("sti");
  while (ticks < 3) __asm__ volatile("hlt");
  __asm__ volatile("cli");
  apic_w(0x320, 1 << 16);
  say("timer: ", ticks, " ticks while halted\n");

  // tasks: each on its own stack, switched as Linux switches them
  task_init(&task_a, task_a_main); task_init(&task_b, task_b_main);
  __switch_to_asm(&boot_task, &task_a);
  puts_("tasks: back in the boot task\n");

  // ring 3: a task that enters user mode, makes system calls and is preempted by the timer
  u64 t = (u64)&tss; tss.iomap = sizeof tss;
  task_init(&task_c, enter_user); tss.rsp0 = (u64)&task_c.stack[1024]; kernel_rsp0 = tss.rsp0;
  tss_desc[0] = (sizeof tss - 1) | (t & 0xffffff) << 16 | 0x89ull << 40 | ((t >> 24) & 0xff) << 56; tss_desc[1] = t >> 32;
  __asm__ volatile("ltr %w0" :: "r"(0x28));
  wrmsr(0xc0000081, 0x0010000800000000ull);   // STAR: kernel CS 0x08, user CS 0x23 and SS 0x1b
  wrmsr(0xc0000082, (u64)syscall_entry);       // LSTAR
  wrmsr(0xc0000084, 0x200);                    // SFMASK: interrupts off in the kernel
  wrmsr(0xc0000080, rdmsr(0xc0000080) | 1);    // EFER.SCE
  preempt_user = 1; timer_periodic(1000000);
  __switch_to_asm(&boot_task, &task_c);
  say("ring 3: back in the boot task, preempted: ", preempted, "\n");

  puts_("microvm-hello: done\n");
  outb(0xf4, 0);
  for (;;) __asm__ volatile("hlt");
}
