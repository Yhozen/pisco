---
date: 2026-11-27
analysed:
  pisco: e59d56e9f1e6561e96729e6da9615a705bc7905b  # main; the lifter's --machine and the three measurements are this pull request's
  linux: https://github.com/torvalds/linux at v6.12 and v6.18 (source read); 6.18.65 built, sha256 c92606f956f09dc50b5a0e2af9e86763fce27ada1647de97f8d1ac8cfc95801d
  qemu: 11.0.0 (qemu-11.0.0-4.amzn2023), its CPU emulated in software
  node: 24.22.2 (V8 13.6.233.17)
  wasi-sdk: 34
  firecracker: https://github.com/firecracker-microvm/firecracker main, src/vmm/src/arch/x86_64 read on 2026-11-27
  unkernel: https://github.com/pithings/unkernel fd204d3130309c45ce51795c753c3b8ec1eaddfd
status: K0 done (docs/linux.md); the design is the plan of K1 and K2, not built yet
---

# The kernel as a title: running Linux itself on PISCO

[wamr-unkernel-tabc.md](wamr-unkernel-tabc.md) ended with a real kernel as the only way to run any amd64 program.
This page answers how PISCO gets one: by lifting the kernel, as it lifts every other title. Everything measured
below was measured on the 4-core x86-64 test machine, on the versions in the front matter.

## Short answer

- **The kernel becomes a title.** A new runtime, `microvm-x64`, imitates the smallest x86-64 machine a kernel
  accepts, and the lifter recompiles a real `vmlinux` ahead of time. Its programs come later the same way.
- **A microVM is that runtime, not another idea.** Firecracker hands the CPU to KVM and has none to port. What can be
  taken from it is the machine it shows a guest: a 64-bit entry, a local APIC, an IOAPIC, a serial port.
- **The devices are small. The depth is in the CPU:** page faults that are come back from, interrupts, the kernel
  switching stacks, and the kernel rewriting its own code while it boots.
- **Three gates were passed before anything was built on them** (below): stacks switch, memory with faults costs
  1.3 to 2.0 times, and Linux 6.18 with our configuration boots on the planned hardware.
- **No one seems to have done it.** A search found kernels compiled from source (linux-wasm, unkernel, LKL) and
  emulators (v86, QEMU, Bochs in container2wasm, JSLinux), and no operating-system kernel recompiled ahead of time.

## The routes

| Route | The kernel | What PISCO must add | Why not |
|---|---|---|---|
| **The kernel as a title** (taken) | a real `vmlinux`, lifted | a machine runtime, ring-0 instructions, a software MMU | |
| Compiled to wasm, as unkernel | built from source for `arch/wasm` | an x86-64 user ABI in that kernel, PISCO as its user CPU | a kernel port to keep up, wasm64, and PISCO does not run the kernel |
| User-Mode Linux as a Linux title | a program for Linux | ptrace or seccomp, signal frames, shared file mappings | imitating Linux to run Linux; UML without an MMU only arrives in 7.4 |

The second route stays the fallback. Its user programs would need the same software MMU, so that work carries over.

## The three gates

### Stacks: each kernel task on its own (JSPI)

unkernel runs each kernel task on its own wasm stack: its switch is a `WebAssembly.Suspending` import, with the C
stack pointer saved across it. The same was built as a title is (wasi-sdk 34, wasm `setjmp`, shared imported
memory): four stacks, each with its own C stack, switching inside 20 to 47 nested C frames, a `longjmp` in each.

- Every frame came back intact, and each `longjmp` landed in its own stack.
- 200,008 switches took 54.5 ms in Node 24: **0.27 µs a switch**. JSPI is on by default there.
- A JSPI stack is 984 KB by default where a title's thread has 64 MB today: deep recursion in a program needs an
  answer (below).

### Memory: a software TLB that a fault can come back through

`fixtures/bench`, Node 24, ms, best of seven. Every load and store of the lifted code checks a 4,096-entry table
(the page of the access's last byte, so one that crosses a page misses). In the last column the slow path is given
the CPU state, as one that runs a fault handler in lifted code and returns must be:

| workload | native | today | TLB | TLB, slow path given the CPU |
|---|---|---|---|---|
| calls (fib 32) | 5 | 23 | 34 | 39 |
| bytes (FNV, 64 MB) | 103 | 131 | 174 | 166 |
| sort (heapsort, 2M u32) | 259 | 648 | 1,141 | 1,232 |
| float (4x4 transform) | 29 | 59 | 113 | 115 |
| memory (20M random loads) | 136 | 163 | 279 | 283 |

- **1.3 to 2.0 times today's lifted code**, and giving the slow path the CPU adds at most 8 %. The limit set
  beforehand was 3 times.
- Not in it: separate read and write entries (the same check), and a counter for interrupts at function entries.
- The same table needs no wasm64: guest addresses are translated, so a kernel at `0xffffffff81000000` fits wasm32.

### The kernel, and the hardware it needs

`tools/build-linux.sh` builds Linux 6.18.65 from kernel.org's source with `recipes/linux/kernel.config` on top of
the kernel's `tinyconfig`, in about three minutes: one CPU, no jump labels, no mitigations, no modules, no ACPI, no
PCI, no jump tables (`KCFLAGS`).

| | |
|---|---|
| `vmlinux` | 10.4 MB; 4,078 K of code; 15,132 symbols |
| Lifted (`lifter --machine`, 0.7 s) | 10,853 functions, 583,233 instructions |
| Not lifted yet | 3,958 instructions (0.68 %), all of the system: `sti` 1,288, `cli` 1,083, `pushfq` 716, `wrmsr` 221, `mov` to and from control and segment registers 163, `rdmsr` 155, `out` 95, `in` 61, `swapgs` 24, `iretq` 17, then about fifty kinds under ten each |
| Indirect jumps through tables | none |
| Code it rewrites | 1,361 alternative entries, 249 static-call sites; no `lock` prefixes, no jump labels |
| Instructions that may fault and go on elsewhere | 656 exception-table entries |

On QEMU (`tools/oracle-qemu/oracle.sh run linux`) it boots to `/init` in under three seconds and the script of
`recipes/linux/init` runs to its end: pipes, `fork`, `exec`, a file of 4 MiB written and hashed, a child killed, a
signal handled. Its 15 lines are `recipes/linux/traces/programs.txt`, the same over eight runs.

- **Without HPET, PIT and 8259 it boots and prints the same** (`ORACLE_QEMU_MACHINE=q35,hpet=off,pit=off,pic=off`,
  with the TSC's rate on the command line). The serial port's interrupt comes through the IOAPIC, found in the MP
  table. So the machine is a local APIC, an IOAPIC, a 16550, the CMOS clock's two ports and an exit port.
- **One thing QEMU could not show:** its emulated CPU has no TSC-deadline timer, so there the kernel ran without a
  timer interrupt. From the source (`calibrate_APIC_clock`, `native_calibrate_tsc`, `native_calibrate_cpu_early`), an
  Intel CPU with CPUID leaves 0x15 and 0x16 and the TSC-deadline bit needs neither a PIT nor a calibration.

## The design

1. **Memory.** Guest RAM is a block of the module's memory. In machine mode the lifter emits loads and stores as
   functions of the address, each with its instruction's address, through the table measured above. A miss walks the
   kernel's page tables, serves a device's memory or raises a page fault. Kernel images and user images use
   separate tables, picked when lifting.
2. **Faults, interrupts and system calls are nested calls.** The slow path pushes the exception frame and runs the
   kernel's handler until its `iretq` returns to the interrupted instruction, and the lifted function goes on, its
   flags still in its C locals. A return elsewhere either nests a new run (a signal handler) or leaves for the
   task's root loop, which enters lifted code by label as `linux_run` does today (`exec`, a fix-up address).
3. **Each kernel task has its own host stack.** At the stack switch inside `__switch_to_asm` the current stack is
   parked and the next resumed or started; `do_task_dead` ends one. Idle (`hlt`) suspends until the next timer
   deadline or key.
4. **Code the kernel rewrites.** Each alternative site is lifted with every variant, behind a selection. Static-call
   sites and their trampolines decode the five or six bytes then in guest memory (a call, a jump, a return, a
   no-op, a zeroed result, a conditional jump).
5. **Boot.** The loader puts `vmlinux` at its physical addresses, writes the boot parameters, an MP table and the
   initramfs, and enters `common_startup_64` at its link address with CR3 on the kernel's own `early_top_pgt` and
   `%r15` on the boot parameters. `startup_64` before it runs at physical addresses, which lifted code cannot; with
   the kernel loaded where it was linked, nothing it does is missed (read in v6.18's `head_64.S` and `head64.c`).
6. **Programs.** Static programs from the initramfs, lifted in the same mode. The kernel maps them; the dispatcher
   finds a program's code by address, later by the content of its pages.

## What the design review added

A second pass over the design, against the lifter's code, found these. They are K1's and K2's to do.

- **A read-modify-write is one write access**, not a read then a write: a fault that sleeps between the two would
  lose another thread's update. The same for `xchg`, `cmpxchg` and `xadd`.
- **String instructions go page by page** in the runtime: a fix-up needs exact counts in RCX, RSI and RDI.
- **`iretq` is not always the end of a handler.** `sync_core()` returns to its own next instruction with one, for
  every site patched. Reporting SERIALIZE (CPUID 7, EDX bit 14) makes it one instruction, lifted as nothing.
- **A level of nesting has more than one way back:** a restarted system call returns two bytes earlier, a warning
  (`ud2`) two bytes later, and the boot's `int3` self-test at a label of its own.
- **The selection of alternatives needs no watch on writes:** every site runs its original until
  `apply_alternatives` returns, then all are set from the kernel's own feature bits, the last matching entry
  winning. The bits' address comes from the `testb` of any `static_cpu_has` site in `.altinstr_aux`.
- **Jumps between functions become wasm tail calls** (`-mtail-call`): 31 `return_call` in `fixtures/linux-hello`,
  its output unchanged. Without them a function and its `.cold` part can call each other without end. The kernel
  is also built without that split.
- **Recursion deeper than a JSPI stack:** the entry counter counts depth too; past a limit the run leaves for the
  root loop and enters the function again at its entry, where nothing but the CPU state is live.
- **The local APIC needs its in-service priority**, or the timer nests in itself.
- **Entry stubs at computed addresses** (`irq_entries_start`, `early_idt_handler_array`) are swept whole.
- **One module per program from K3:** the kernel is GPL-2.0-only and some programs are GPL-3.0-or-later (bash), so
  they cannot be one linked program (`LICENSING.md`). Busybox, GPL-2.0-only, can share the kernel's.
- **Not solved by this design:** a program that returns into an arbitrary instruction (Go's signal preemption) or
  makes code while it runs. Those wait for the interpreter of K8.

## To re-check

- The kernel: a newer long-term release (6.18.65 here), and whether `CONFIG_SMP`, `CONFIG_JUMP_LABEL` and
  `CONFIG_CPU_MITIGATIONS` can still be turned off.
- JSPI's stack size (984 KB in V8 13.6) and growable stacks, behind `--experimental-wasm-growable-stacks` here.
- QEMU's emulated CPU and the TSC-deadline timer: with it, the oracle could run the machine's own timer.
- unkernel: an x86-64 user ABI in its kernel would change the second route's cost.
