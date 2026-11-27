# Linux, the kernel as a title (2026-11-27)

The plan to run Linux itself: a real [Linux](https://kernel.org) kernel (6.18.65, GPL-2.0-only), built from its
source with our own configuration, lifted ahead of time like any other title, on a runtime that imitates a small
x86-64 machine (`rt/microvm-x64`, `base/microvm-x64`). The Linux x64 runtime ([fastfetch.md](fastfetch.md)) imitates the
kernel one system call at a time; here `fork`, signals, pipes and files are the kernel's own work. Why this route,
what was measured before taking it and how the machine is designed: [research/kernel-as-a-title.md](research/kernel-as-a-title.md).

**So far (K4) Linux runs, lifted, with its programs, as an image, and can be typed into.** The kernel and the initramfs's busybox, lifted into one
module, boot on the machine in Node and run `/init` to its end: pipes, a shell in a shell, awk, a 4 MiB file written
and hashed, a child killed, a signal handled, ten programs executed. They print the 15 lines they print on QEMU,
the same on every run, in 2.5 s from the start of Node to the exit port. `fork`, `exec`, signals, pipes and files
are the kernel's own work. `pisco build recipes/linux` makes it an image like any other title's, and with `/bin/sh`
as its first program its shell can be typed into, in a terminal or in the page. The machine was first checked on `fixtures/microvm-hello`, a kernel of about 300 lines
that does what Linux asks of it, and prints what QEMU prints.

## The files

| | |
|---|---|
| Source | `linux-6.18.65.tar.xz` from kernel.org, sha256 `c92606f9…801d` (in full in `tools/build-linux.sh`) |
| Configuration | the kernel's `tinyconfig` and `recipes/linux/kernel.config` on top: one CPU, no jump labels, no mitigations, no branch tracking, no modules, tracing or JITs, no ACPI, no PCI, only 64-bit programs; a 16550 console, an initramfs, `/proc`, `/sys`, a tmpfs |
| Built with | the machine's gcc, without jump tables (`KCFLAGS=-fno-jump-tables`), naming no user, host or time |
| `vmlinux` | what the lifter reads: 10.4 MB, 4,078 K of code, linked at `0xffffffff81000000`, its symbols kept |
| `bzImage` | the same kernel as a boot loader takes it: what QEMU boots |
| `initramfs.cpio` | `/init` (`recipes/linux/init`), busybox 1.36.1 (static, `tools/build-busybox.sh`) and a link for each of its 401 applets; uncompressed, written by the kernel's own `gen_init_cpio` |
| Command line | `recipes/linux/cmdline`: the serial console, no vDSO, no check of the PIT's interrupt |
| The oracle | QEMU 11.0.0, machine `q35` without its HPET, CPU `Westmere` without AES, emulated in software (`tools/oracle-qemu`) |

Nothing of the kernel or busybox is in the repository: `tools/build-linux.sh` fetches the sources and builds them
into `out/linux`. A lifted kernel will be GPL-2.0-only code linked with the runtime, which is GPL-2.0-or-later: the
module is then under GPL-2.0 ([LICENSING.md](../LICENSING.md)).

## The machine (K1)

`rt/microvm-x64` is one x86-64 CPU in long mode and what a kernel needs around it:

| | |
|---|---|
| `mmu.c` | RAM (256 MiB, `SW_MEMORY` in MiB) and the APIC's page, reached through the kernel's page tables (4 KiB, 2 MiB and 1 GiB pages, accessed and dirty bits, user and write rights); a software TLB for kernel code and one for user code |
| `cpu.c` | exceptions, interrupts and system calls as nested runs of lifted code, their return (`iretq`, `sysretq`), MSRs, control registers, the GDT, IDT and TSS |
| `apic.c` | the local APIC (xAPIC): its timer (one-shot, periodic, TSC-deadline), priorities and EOI; the virtual clock; CPUID (an Intel CPU, the TSC at 1 GHz) |
| `devices.c` | a 16550 at `0x3f8` (its output is the title's standard output), the exit port at `0xf4` (as QEMU's `isa-debug-exit`), 8259s that answer as masked |
| `run.c` | the loader (the ELF at its physical addresses, CR3 on `early_top_pgt`, entered at `common_startup_64`), the loops that enter lifted code by address, a host stack for each of the kernel's tasks |

How it works:

- **Memory.** In a machine's image the lifter makes every load a read through the TLB and every store a write
  (`MW64(a, v)` for `M64(a) = v`), each with the instruction's address. A miss walks the page tables; a fault runs
  the kernel's handler and the access is tried again. Device memory is never cached.
- **A fault, an interrupt or a system call is a call.** The frame goes on the kernel's stack as the CPU puts it
  there, the handler runs in a loop of its own, and when its `iretq` returns where it was raised, the lifted
  function that raised it simply goes on, its registers and flags intact. When the handler returns elsewhere (a
  fix-up, a preempted program sent to another address), what was interrupted is abandoned and the code there is
  entered by its address.
- **Interrupts are taken at polls**: at each function's start and each jump back. Each poll moves the virtual clock
  by 4 ns; `hlt` moves it to the next timer. So a run is the same every time.
- **Each kernel task has a host stack of its own** (JSPI, on by default in Node 24 and Chrome). The stack switch in
  `__switch_to_asm` parks one and resumes or starts the other. A call that comes back on another stack than it was
  made on is a fatal error, so a stack switch the machine missed cannot go unnoticed.

## Build it and run it

The image, and a shell to type into:

```sh
tools/setup-toolchain.sh wasi musl linux-headers kernel   # (kernel says what to install; it installs nothing)
tools/build-linux.sh                                      # out/linux: the kernel, the initramfs, out/linux/root
target/release/pisco build recipes/linux                  # out/images/linux (the kernel and busybox lifted: minutes)
target/release/pisco run out/images/linux                 # Node: the boot, then /init's programs
target/release/pisco run out/images/linux --chrome        # the same in headless Chrome
SW_CMDLINE="$(cat recipes/linux/cmdline) rdinit=/bin/sh" target/release/pisco run out/images/linux
                                                          # a shell, in this terminal (Ctrl-] ends it)
node tools/serve.mjs 8000 web out/images/linux            # the page, in a browser of your own:
# http://127.0.0.1:8000/browser/index.html?image=/files/0/&SW_CMDLINE=console%3DttyS0%20rdinit%3D/bin/sh
```

In a terminal or in the page the machine's clock is the host's and it waits for keys when idle (`SW_CONSOLE=tty`,
which both set); fed from a pipe it keeps its virtual clock. The pieces one at a time:

```sh
tools/setup-toolchain.sh musl linux-headers kernel qemu   # the last two say what is missing; they install nothing
tools/build-linux.sh                                      # out/linux: vmlinux, bzImage, initramfs.cpio (minutes)
tools/oracle-qemu/oracle.sh run linux                     # QEMU: the boot log, then what /init's programs print
tools/oracle-qemu/oracle.sh programs linux                # only the programs' lines of that run
tools/census.sh out/linux/vmlinux                         # what the lifter does not lift yet (it knows a kernel)
PROGRAMS=out/linux/build/busybox-1.36.1/busybox_unstripped SPLIT=16 \
  tools/build-title.sh out/linux/vmlinux out/linux/wasm   # Linux and busybox lifted into one module (minutes the first time)
(cd out/linux && SW_INITRD=initramfs.cpio SW_CMDLINE="$(cat ../../recipes/linux/cmdline)" node ../../web/run-node.mjs wasm/title.wasm vmlinux)
fixtures/microvm-hello/build.sh out/microvm-hello         # the machine's test kernel
tools/build-title.sh out/microvm-hello/hello out/microvm-hello/wasm      # lifted (base/microvm-x64, from OS=microvm)
node web/run-node.mjs out/microvm-hello/wasm/title.wasm out/microvm-hello/hello   # its serial port, in Node
node tools/run-chrome.mjs out/microvm-hello/wasm/title.wasm out/microvm-hello/hello   # the same in headless Chrome
target/release/lifter out/linux/vmlinux out/linux/census/title.c --split 16 --machine   # the same, by hand
```

`tests/run.sh kernel` checks the kernel: the lifter must read `vmlinux`; on QEMU the programs must print
`recipes/linux/traces/programs.txt`, also on a machine without HPET, PIT and 8259; and its image, run by `pisco run`
in Node, must boot twice the same to `/init` (`traces/boot.txt`), its programs must print what they print on QEMU,
and a shell typed into through the console (`recipes/linux/console-input.txt`) must answer as `traces/console.txt`
(`KERNEL_RECORD=1` records the traces again). It is skipped until `out/linux` is built, and says so when QEMU is missing. `tests/run.sh microvm` (a
default step, in CI) builds and runs the test kernel: it must print `fixtures/microvm-hello/expected.txt`, what QEMU
prints, and does under QEMU too when QEMU is there; `tests/run.sh chrome` runs it in the page.

## What it took (K0)

- **Three measurements before any design was trusted**: stacks that switch (JSPI), memory through a software TLB a
  fault can come back through, and this kernel on the hardware the machine will have. All three are in the research
  page, with their numbers.
- **The lifter reads a kernel** (`--machine`): an image above 4 GiB that stays at its link addresses (`OS=microvm`
  in `title.env`), whose code is what its sections say is code. A kernel's executable segment holds its read-only
  data too, and taking that for code made 52,729 instructions of strings.
- **Operands the 64-bit emitters took for general registers.** `mov cr3, rax` was lifted as a write to RBX, and
  `mov ax, ds` as a read of RBX. In a program a segment register now reads as its selector and takes a write
  without effect; control, debug, MMX and mask registers are counted as not lifted.
- **The census**: 10,853 functions, 583,233 instructions, 3,958 of them not lifted (0.68 %), all instructions of the
  system: `sti`, `cli`, `pushfq`, `wrmsr`, `rdmsr`, `in`, `out`, `swapgs`, `iretq` and about fifty rarer kinds.
- **What the kernel forces**: `CONFIG_HPET_TIMER` and `CONFIG_MICROCODE` cannot be turned off on x86-64, so the build
  checks every line of the recipe's configuration against the kernel's own and stops at a line it did not take.
- **An oracle that prints the same every time.** The shell's own "Terminated" for a killed child came or not with
  the machine's load; `/init` now hides it, and silences the kernel's console before its checks.

## What it took (K1)

- **The test kernel first** (`fixtures/microvm-hello`): built with gcc as Linux is, linked at `0xffffffff81000000`,
  with a 32-bit start for QEMU (a PVH note) that PISCO skips. Its 20 lines check paging and `invlpg`, a page fault
  returned from, one fixed up, `int3`, `ud2` as a warning, the timer while halted, two tasks switching stacks, and a
  ring-3 task that makes system calls and is preempted. QEMU printed them on the first run; in Node it took two
  fixes, and Chrome printed them as Node did.
- **The lifter's machine mode**: the system's instructions as calls of the machine (`cli`, `sti`, `hlt`, `pushfq`,
  `iretq`, `sysretq`, `syscall`, `swapgs`, MSRs, control and segment registers, the descriptor tables, `in` and
  `out`, `invlpg`); stores as writes; interrupt polls; decoding that goes on past an `int3`, `int n` or `ud2` (a
  handler comes back after it), but not into padding: a 2 MiB alignment of `int3`s once made a function of two million
  of them.
- **A stack switch the machine does not see is caught.** `__switch_to_asm`, written in assembly, has no symbol
  size, so the machine's hook covered only its first instruction: the plain `mov rsp` "returned" onto the other
  task's stack, and the boot task went on there with nothing noticed. Labels now reach to the next function, and a
  call in a machine's image checks that it comes back with the stack it was made with.
- **Shared code made safe for it**: `rt/cpu.h`'s helpers store through `MW` forms, and `f80_load` and `f80_store`
  take the CPU state, for every title; `rt_cpuid` and `rt_rdtsc` in `rt/core.c` are weak, so a machine has its own.
- **Linux's census with the machine mode**: 602,001 instructions, 56 not lifted (`xsave` and `fxsave` forms,
  `cmpxchg16b`, `lkgs`, a few AVX-512 and AMX ones, string I/O).

## What it took (K2)

The lifted kernel (11,612 functions, 636,773 instructions; a 42 MB module, compiled in under six minutes on four
cores) printed most of its boot on the first run. What it needed after that:

| Stop | What the kernel did | What changed |
|---|---|---|
| `error_entry` came back on another stack | it saves the registers above its own return address, on purpose | a call may come back on a moved stack through its own return address; only another address is a missed switch |
| the CPU went on at `int3_selftest_asm`+5 | the boot's `int3` self-test emulates a 5-byte call at the `int3` | a label after each `int3`, and 5 bytes on |
| an address `0x0123456789abcdef`-based | the dentry hash's address and shift and `USER_PTR_MAX` are runtime constants, patched into the code at boot | the lifter reads `runtime_ptr_*` and `runtime_shift_*`; those immediates are read from memory |

Done before the first run, from the kernel's source: the static-call trampolines (`__SCT__*`) decode the jump the
kernel has written into them; the exception table's fix-ups and the entry stubs at computed addresses
(`early_idt_handler_array`, `irq_entries_start`) are functions; CPUID reports SERIALIZE, so `sync_core()` needs
no `iretq` to itself; `fxsave`, `fxrstor` and `cmpxchg16b`; the boot protocol (the zero page with its memory map,
the command line, the initramfs at the top of RAM), an MP table with the IOAPIC, the IOAPIC, the 16550's
interrupt, a CMOS clock on the virtual clock's date, and the microcode revision MSR (high, so the TSC-deadline
timer is not turned off for an erratum). The alternatives were left as the file has them: the code before patching
is correct on any CPU, and the boot showed nothing that needed more.

`recipes/linux/traces/boot.txt` is the boot up to `/init`. It differs from QEMU's where the machines differ: the
CPU's name and features, the TSC at 1 GHz, the MSRs the machine leaves at 0 (it says which, on its standard error).

## What it took (K3)

- **The programs in the kernel's module**: `lifter --program <elf>` (`PROGRAMS` for `tools/build-title.sh`) lifts
  static programs with a machine's image, at their own addresses, which are where the kernel loads them. One
  module, 15,310 functions; the kernel finds a program's code by address when it returns to user mode.
- **A program's own `longjmp`**: no hook of setjmp and longjmp in a machine (the Linux runtime's are for programs
  without a kernel); an indirect jump to a return address abandons what was running and enters there.
- That was all: busybox's `fork`, `exec`, pipes, signals and page faults went through the kernel, on the nested
  runs and task stacks K1 built, and printed QEMU's lines on the first run.

Timed from the host, each line of the console stamped as it arrives (Node 24 on the 4-core test machine; QEMU
11.0 with its CPU emulated in software on the same machine, the same kernel and initramfs):

| | lifted, in Node | QEMU |
|---|---|---|
| from the start to `Run /init` | 0.83 s | 1.15 s |
| `awk`: 100,000 iterations | 0.86 s | 0.78 s |
| 4 MiB written to a file and hashed | 0.44 s | 0.71 s |
| ten programs executed (`/bin/true`) | 18 ms | 16 ms |

The same awk on the Linux runtime took 0.48 s, and twenty processes started there 2.1 s
([research/wamr-unkernel-tabc.md](research/wamr-unkernel-tabc.md)): memory through the MMU costs here, and a
process costs about a hundredth of what it costs there.

## What it took (K4)

- **The serial port's input**: a ring in the module's memory, written by the host (Node's standard input, raw in a
  terminal; the page's keys as a terminal sends them, Enter as CR, arrows as escape sequences, Ctrl-C as 0x03) and
  read by the 16550, with its receive interrupt. A byte is there only while the driver has that interrupt on: Linux's
  8250 reads the receiver while it probes, and took the first three bytes of a pipe.
- **Idle that waits**: with someone typing, `hlt` waits on the ring (`memory.atomic.wait32`) until a key or the next
  timer, on the host's clock.
- **A bug the shell found**: a signal's frame is written to the user's stack with `fxsave`, and its copy-on-write
  fault reported `rip 0`, so the kernel found no fix-up and oopsed. The helpers of `rt/cpu.h` that reach memory
  (`fxsave`, the x87's) saw the file's fallback `ip_` and `tlb_`, not the instruction's; they take them now.
- **The image**: `pisco build` knows a machine's base: `[lifter] programs` are lifted with the kernel, `[run]
  initrd` and `cmdline` reach the runtime (`SW_INITRD`, `SW_CMDLINE`) through the image's config. Two builds with
  empty caches give the same manifest (`sha256:72e36c24…`); a build takes eight minutes on four cores.
- **`tools/run-chrome.mjs` types**: `TYPE=<text>` sends key presses to the page, which is how typing into the page
  was checked.

## Limits

- Only programs lifted with the kernel run, at the addresses they were lifted at: one busybox. Two programs at
  the same addresses (bash and busybox are both at 0x400000) need modules found by content (K7).
- A signal that interrupts a program between system calls returns, after its handler, to an instruction that is
  no label: such a program stops the machine (shells and busybox wait in system calls, where it works).
- Model-specific registers the machine does not know read as 0 where a CPU raises #GP, which some of the kernel's
  `rdmsr_safe` callers would want.
- A store that faults between its read and its write (an add to memory whose page must be copied) reads again after
  the fault; with one CPU and no other task in between, the same.
- On QEMU without the PIT the kernel has no timer interrupt (its emulated CPU has no TSC-deadline timer): `/init`
  ends all the same, but that boot says nothing of the timer the machine will have.
- One CPU, no network, no disk: the initramfs is the whole file system.
- Only what was lifted before the kernel starts can run: here the kernel and busybox.

## Progress

| # | Milestone | State |
|---|---|---|
| K0 | the kernel built from a pinned source with our configuration; the lifter reads it and counts what it does not lift; QEMU as the oracle, `/init`'s programs recorded; stacks, memory and hardware measured | done |
| K1 | the machine (`rt/microvm-x64`) on a test kernel of our own: paging, a fault fixed up, the timer, two tasks, a ring-3 task, the serial port, equal under QEMU and PISCO, in Node and Chrome | done |
| K2 | this kernel lifted: its boot log up to `Run /init as init process`, in Node, the same on every run | done |
| K3 | busybox lifted with the kernel: `/init`'s programs print `recipes/linux/traces/programs.txt`, what they print on QEMU | done |
| K4 | a console typed into, in Node and in the page; `pisco build recipes/linux`, two builds the same | done |
| K5 | virtio block and network devices; a writable disk | next |
