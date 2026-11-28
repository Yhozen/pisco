// SPDX-License-Identifier: GPL-2.0-or-later
// Snapshots of the machine: everything it is, its RAM, its CPU's registers, its devices, the runtime's own state, is
// in the module's memory, but for what the host's wasm stacks hold (each kernel task's, run.c). A snapshot is taken
// where those hold nothing the guest's own stacks do not: at a poll at a function's start or at a halt, when no task
// waits in a level that a host stack started afresh could not go back to (run.c machine_resumable). The host keeps
// the memory (host_snapshot); a host that goes on from it puts the memory back into a new instance of the same
// module and calls rt_restore, which starts the CPU again where the snapshot was taken, every other task on a new
// host stack when it is switched to, as a task never run starts (docs/linux.md, snapshots).
#include "machine.h"

HOST("snapshot") s32 host_snapshot(u64 pc);              // the machine's memory, as it is now, kept; it goes on at pc
HOST("console_open") void host_console_open(u32 ring);

// the host asks for a snapshot here (any value but 0), and wakes a halted machine as a key typed does
static volatile u32 wanted;
__attribute__((export_name("rt_snapshot_word"))) u32 rt_snapshot_word(void) { return (u32)(uptr)&wanted; }
int machine_snapshot_wanted(void) { return __atomic_load_n(&wanted, __ATOMIC_ACQUIRE) != 0; }

// the C library's thread pointer (a wasm global, not memory): what it was is put back on restore
__asm__(
  ".globaltype __tls_base, i32\n"
  ".globl rt_tls_get\n.type rt_tls_get,@function\nrt_tls_get:\n.functype rt_tls_get () -> (i32)\nglobal.get __tls_base\nend_function\n"
  ".globl rt_tls_set\n.type rt_tls_set,@function\nrt_tls_set:\n.functype rt_tls_set (i32) -> ()\nlocal.get 0\nglobal.set __tls_base\nend_function\n");
u32 rt_tls_get(void); void rt_tls_set(u32);
static u32 tls;

void machine_snapshot(Cpu *c, u64 pc) {
  (void)c;
  if (net_present()) { __atomic_store_n(&wanted, 0, __ATOMIC_RELEASE); rt_log("machine: no snapshot of a machine with a network (its peers' connections are not in it)"); return; }
  if (!machine_resumable()) return;   // (a task waits in a fault's handler: at the next poll)
  uart_flush();
  tls = rt_tls_get();
  __atomic_store_n(&wanted, 0, __ATOMIC_RELEASE);   // (cleared first: the snapshot does not ask for itself again)
  host_snapshot(pc);
}

// a snapshot's memory is in place (the host put it there, in a new instance of this module, its programs' modules
// loaded as they were): the CPU goes on at pc
__attribute__((export_name("rt_restore"))) void rt_restore(u64 pc) {
  rt_tls_set(tls);
  mmu_flush();
  apic_clock_restored();
  if (console) host_console_open((u32)(uptr)console);
  if (mc_trace) rt_log("machine: a snapshot goes on at 0x%llx, the clock at %llu ns", pc, mc.now);
  machine_restart(pc);
}
