// SPDX-License-Identifier: GPL-2.0-or-later
// The microvm-x64 runtime: a small x86-64 machine whose software is an app (docs/linux.md). Its kernel is lifted
// ahead of time (lifter --machine) and runs on this machine: one CPU in long mode, its RAM, a software MMU over the
// kernel's own page tables (mmu.c), exceptions, interrupts and system calls as nested runs of lifted code (cpu.c),
// a local APIC and its timer on a virtual clock (apic.c), port devices: a 16550, an exit port (devices.c), and a
// host stack for each of the kernel's tasks (run.c).
#pragma once
#include <setjmp.h>
#include "rt.h"

// ---- the machine's state that lifted code does not hold (Cpu holds the registers) ----
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };
typedef struct {
  u64 cr0, cr2, cr3, cr4, efer;
  u64 gdt_base, idt_base; u16 gdt_limit, idt_limit, tr; u64 tss_base;
  u16 seg[6]; int cpl;
  u64 star, lstar, cstar, fmask, kernel_gs, tsc_aux, apic_base, pat, misc_enable;
  u64 now;        // the virtual clock: nanoseconds since the machine started, which is also its TSC (1 GHz)
} Machine;
extern Machine mc;
#define RFLAGS_SYS 0x3F7200ull   // the bits of RFLAGS mc_rflags keeps: IF, IOPL, NT, RF, VM, AC, VIF, VIP, ID
#define RFLAGS_IF 0x200ull

// ---- memory (mmu.c) ----
extern u8 *ram; extern u64 ram_size;
enum { PF_P = 1, PF_W = 2, PF_U = 4, PF_I = 16 };
// a virtual address's physical one through the page tables: 0, or a page fault's error code; *writable set
int mmu_walk(u64 va, int write, int user, u64 *pa, int *writable);
void mmu_flush(void);
// the runtime's own accesses, with the kernel's rights: a fault there is a fatal error of the machine
u64 vread64(u64 va); void vwrite64(u64 va, u64 v); u16 vread16(u64 va);
void vread(u64 va, void *out, unsigned n);
// physical memory: RAM, the APIC (apic.c), nothing else (reads give ones)
void phys_read(u64 pa, void *out, unsigned n);
void phys_write(u64 pa, const void *in, unsigned n);

// ---- the CPU (cpu.c) ----
// raises an exception or an interrupt: the frame on the kernel's stack, the handler run until it returns to
// `want` (or, for a fault, to the faulting `rip`), then back to the caller, which goes on. Returns 0 when it came
// back, as the interrupted code expects.
void cpu_deliver(Cpu *c, u32 vector, int has_err, u32 err, u64 rip, u64 want);
void cpu_resume_at(Cpu *c, u64 rip, int cpl) __attribute__((noreturn));
extern int mc_trace;

// ---- the APIC and the clock (apic.c) ----
u32 apic_read(u32 reg); void apic_write(u32 reg, u32 v);
int apic_pending(void);           // the vector the CPU would take now (interrupts on), or -1
void apic_take(int vector);       // it takes it: in service
void apic_tick(void);             // the clock moved: timers that are due raise their interrupts
u64 apic_next_deadline(void);     // when the next timer fires (~0: none)
void apic_tsc_deadline(u64 v); u64 apic_tsc_deadline_get(void);

u32 ioapic_read(u32 off); void ioapic_write(u32 off, u32 v);
void ioapic_line(int pin, int level);   // a device's interrupt line, high or low

// ---- virtio's devices on its MMIO transport (virtio.c): the block device (virtio.c), the network device (net.c) ----
typedef struct { u32 num, ready; u64 desc, avail, used; u16 last; } VQueue;
typedef struct { u64 addr; u32 len; u16 flags, next; } VDesc;   // (flags: 1 next, 2 the device writes it)
typedef struct VDev {
  u32 id; u64 features; unsigned nq; VQueue q[2];                 // the device's: its virtio ID, features, queues
  u32 (*config)(struct VDev *d, u32 off);                         // its configuration space, a word at a time
  void (*notify)(struct VDev *d, unsigned q);                     // the driver made buffers available on queue q
  u64 base; u32 pin, status, isr, dev_sel, drv_sel, qsel, gen; u64 drv;   // the transport's
} VDev;
void virtio_add(VDev *d);                 // a device, at the next page and pin
unsigned virtio_count(void); VDev *virtio_dev(unsigned i);
VDev *virtio_at(u64 pa);                  // the device whose registers are at pa's page, if any
u32 virtio_read(VDev *d, u32 off); void virtio_write(VDev *d, u32 off, u32 v);
int virtio_pop(VQueue *q, u16 *head);     // the next chain the driver made available
unsigned virtio_chain(VQueue *q, u16 head, VDesc *d, unsigned max);
void virtio_push(VQueue *q, u16 head, u32 len);
void virtio_interrupt(VDev *d);
void virtio_init(void);                   // the block device, with SW_DISK
void net_init(void);                      // the network device, with SW_NET
void net_poll(void);                      // frames that arrived: into the driver's buffers
int net_present(void);
volatile u32 *net_bell(void);           // what an idle machine waits on with a network (0: none)

// ---- devices (devices.c) ----
void uart_flush(void);
// the console's input: a ring in this memory that the host writes (keys typed, or a pipe) and the 16550 reads
typedef struct { volatile u32 w, r, cap, eof; u8 data[4096]; } ConsoleRing;
extern ConsoleRing *console;
void console_init(void);
void console_poll(void);          // new bytes: the 16550's receive interrupt
int console_pending(void);
extern int mc_paced;              // someone types (SW_CONSOLE=tty): the clock is the host's, and idle waits for keys
// the network's frames for the guest (net.c): a ring in this memory that the host writes. Records: a u32 length, then
// the frame, to a multiple of 4 bytes; w and r count bytes (the host moves w, the machine r); the host adds 1 to
// bell and wakes it for each frame and each key typed (what an idle machine with a network waits on)
typedef struct { volatile u32 w, r, cap, bell; u8 data[1 << 18]; } NetRing;

// ---- the tasks' host stacks and the loops that run lifted code (run.c) ----
typedef struct Level {
  jmp_buf jb;          // back to the code that raised it (cpu_deliver's caller), when the handler returns there
  jmp_buf loop;        // the loop that runs its handler, entered again at loop_pc when the handler goes elsewhere
  volatile u64 loop_pc;
  u64 want_rip, alt_rip; int want_cpl;
  int resumable;       // its handler can come back to want_rip on a host stack started afresh (a snapshot's: snapshot.c)
  struct Level *prev;
} Level;
typedef struct Co {
  jmp_buf root; volatile u64 root_pc;
  Level *levels;       // innermost first
  u32 sp;              // its C stack pointer while it is parked
  u64 key;             // parked: the kernel stack pointer it was left with (what switch_to loads to go back)
  int id, started, parked, dying, free;
  char *cstack;        // its C stack (malloc'd), freed when it has died
  u64 start_pc;
} Co;
extern Co *co_cur;
void run_level(Cpu *c, Level *l, u64 pc) __attribute__((noreturn));
void machine_programs_forget(void);   // the address space changed: which program runs is found again
extern const u64 rt_machine_entry, rt_machine_pgt;
int machine_enterable_kernel(u64 pc);   // the kernel's lifted code can be entered there: a function, or a return address
int machine_is_function(Cpu *c, u64 pc);   // a lifted function starts there (in the address space that runs)
int machine_resumable(void);          // every task's host stack could be started afresh from the guest's own stack
void machine_restart(u64 pc) __attribute__((noreturn));   // the tasks' host stacks dropped; the CPU goes on at pc

// ---- snapshots (snapshot.c): the machine's state is its memory, taken where no host stack holds any of it ----
int machine_snapshot_wanted(void);
void machine_snapshot(Cpu *c, u64 pc);   // at a safe point (a poll at a function's start, a halt): pc is where it goes on
void apic_clock_restored(void);          // the host's clock goes on from the machine's
