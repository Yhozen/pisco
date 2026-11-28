# Linux, the kernel as an app (2026-11-27)

The plan to run Linux itself: a real [Linux](https://kernel.org) kernel (6.18.65, GPL-2.0-only), built from its
source with our own configuration, lifted ahead of time like any other app, on a runtime that imitates a small
x86-64 machine (`rt/microvm-x64`, `base/microvm-x64`). The Linux x64 runtime ([fastfetch.md](fastfetch.md)) imitates the
kernel one system call at a time; here `fork`, signals, pipes and files are the kernel's own work. Why this route,
what was measured before taking it and how the machine is designed: [research/kernel-as-an-app.md](research/kernel-as-an-app.md).

**So far (K4) Linux runs, lifted, with its programs, as an image, and can be typed into.** The kernel and the initramfs's busybox, lifted into one
module, boot on the machine in Node and run `/init` to its end: pipes, a shell in a shell, awk, a 4 MiB file written
and hashed, a child killed, a signal handled, ten programs executed. They print the 15 lines they print on QEMU,
the same on every run, in 2.5 s from the start of Node to the exit port. `fork`, `exec`, signals, pipes and files
are the kernel's own work. `pisco build recipes/linux` makes it an image like any other app's, and with `/bin/sh`
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
| `devices.c` | a 16550 at `0x3f8` (the console, both ways), the exit port at `0xf4` (as QEMU's `isa-debug-exit`), the CMOS clock, resets through `0x64` and `0xcf9` |
| `virtio.c` | virtio's MMIO transport, a page from `0xd0000000` and a pin from 5 for each device, announced on the kernel's command line; its block device: a disk (`SW_DISK=<file>` beside the kernel, or `SW_DISK=<n>M` of zeros), its writes kept in memory |
| `net.c` | virtio's network device (`SW_NET=1`): frames to the host as they are sent, and from a ring the host writes (K14) |
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
PROGRAMS="out/linux/root/busybox out/linux/root/hello" SPLIT=16 \
  tools/build-app.sh out/linux/vmlinux out/linux/wasm   # Linux and its programs lifted into one module (minutes the first time)
(cd out/linux && SW_INITRD=initramfs.cpio SW_CMDLINE="$(cat ../../recipes/linux/cmdline)" node ../../web/run-node.mjs wasm/app.wasm vmlinux)
fixtures/microvm-hello/build.sh out/microvm-hello         # the machine's test kernel
tools/build-app.sh out/microvm-hello/hello out/microvm-hello/wasm      # lifted (base/microvm-x64, from OS=microvm)
node web/run-node.mjs out/microvm-hello/wasm/app.wasm out/microvm-hello/hello   # its serial port, in Node
node tools/run-chrome.mjs out/microvm-hello/wasm/app.wasm out/microvm-hello/hello   # the same in headless Chrome
target/release/lifter out/linux/vmlinux out/linux/census/app.c --split 16 --machine   # the same, by hand
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
  in `app.env`), whose code is what its sections say is code. A kernel's executable segment holds its read-only
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
  take the CPU state, for every app; `rt_cpuid` and `rt_rdtsc` in `rt/core.c` are weak, so a machine has its own.
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

- **The programs in the kernel's module**: `lifter --program <elf>` (`PROGRAMS` for `tools/build-app.sh`) lifts
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
- **The image**: `pisco build` knows a machine's base: `[hints.pisco] programs` are lifted with the kernel, `[run]
  initrd` and `cmdline` reach the runtime (`SW_INITRD`, `SW_CMDLINE`) through the image's config. Two builds with
  empty caches give the same manifest (`sha256:72e36c24…`); a build takes eight minutes on four cores.
- **`tools/run-chrome.mjs` types**: `TYPE=<text>` sends key presses to the page, which is how typing into the page
  was checked.

## What it took (K5)

- **A disk**: virtio's block device (virtio 1.2, MMIO transport, one queue of 256, reads, writes, flush, its id), in
  `rt/microvm-x64/virtio.c`; the kernel gains `CONFIG_VIRTIO_MMIO`, `VIRTIO_BLK` and ext2. With `SW_DISK` the runtime
  adds `virtio_mmio.device=4K@0xd0000000:5` to the command line. It worked on its first run: `/init` formats it with
  busybox's `mke2fs`, writes 1,000 lines, mounts it again and reads them back (`tests/run.sh kernel`). QEMU's machine
  here has no virtio-mmio bus, so that check has no oracle; without a disk `/init` prints what it prints on QEMU.

## What it took (K6): programs at the same addresses

Static programs are linked at the same addresses (busybox and `fixtures/linux-hello` both from `0x400000`), so a
module cannot tell them apart by address. Each is now lifted on its own (`lifter --user <name>`: its functions and
tables under its name), and the machine finds which one runs by its bytes: the lifter records the hash of each page
of a program's code, and when the CPU enters user code after a change of CR3 the machine hashes the page there and
takes the program whose page matches. Lifted code never fetches its own instructions, so a code page the kernel
has not mapped yet is read first, as a fetch would: the kernel's page fault handler maps it.

- `/init` runs busybox and linux-hello, interleaved; its 22 lines are QEMU's (`traces/programs.txt`).
- A machine's code is built with the x87 at its full precision (`-DGUEST_LINUX` too): musl's `printf` of a
  `double` works in `long double`, and printed `6.019999e+23` for `6.020000e+23` without it.
- `pisco build` records the flags each lift's objects were compiled with and compiles them again when they change:
  they are not in the lift's key, and cached objects of the old flags beside a runtime of the new ones hung the
  kernel at boot.
- bash is GPL-3.0-or-later and cannot be linked into one module with the GPL-2.0-only kernel
  ([LICENSING.md](../LICENSING.md)); that needs a module per program, next.

## What it took (K7): signals anywhere, and many processes

- **A signal on the way back from a fault**: a shell's copy-on-write fault came back with SIGCHLD pending, so the
  kernel went to the handler, and after `sigreturn` back to the faulting instruction, which no label marks. Now when
  the kernel sends a program elsewhere while code of its waits below (on a fault, an interrupt or a system call), the
  code there runs on top, and the waiting code goes on when the kernel comes back to it: the access is done again. A
  fix-up in the kernel still abandons what faulted.
- **Interrupts in loops are taken on the jump back**, at its target, which is a label: an interrupted loop the
  kernel resumes later (after a handler) goes on at its next iteration with its flags as they are.
- **Tasks that end**: `do_task_dead` marks the task; when it switches away, its C stack is freed, its slot used again
  and its wasm stack dropped by the host. A task that ended was found again by a new task whose kernel stack had its
  stack pointer, and went on as the process that had exited.
- Checked: `/init` catches a signal sent into a busy loop, as on QEMU; a shell runs 500 programs one after another
  (`tests/run.sh kernel`). 4,000 programs took 5.2 s, the host's memory 635 MB at the end against 595 MB after 2,000.

## What it took (K8): real programs

The initramfs has four programs now, all static, all at `0x400000`, each lifted on its own into the kernel's module:
busybox, `fixtures/linux-hello`, fastfetch 2.71.0 (MIT) and perl 5.44.0 with cowsay 3.8.4 (perl's licence is
GPL-1.0-or-later or Artistic, so it can be linked with the GPL-2.0 kernel). `/init` runs fastfetch and cowsay, and
their lines are QEMU's. `tools/build-linux.sh` builds them (`tools/build-fastfetch.sh`, `tools/build-cowsay.sh`) and
copies only the perl modules cowsay loads: perl itself names them.

fastfetch reads the kernel's own `/proc` and `/sys` now, not the Linux runtime's imitation of them:

```
Kernel: Linux 6.18.65
CPU: pisco microvm-x64 (lifted) @ 1.00 GHz
Memory: 17.16 MiB / 244.25 MiB (7%)
Disk (/): 3.57 MiB / 118.45 MiB (3%) - rootfs
```

## What it took (K9): a program in a module of its own

bash is GPL-3.0-or-later and cannot be linked into the GPL-2.0-only kernel's module, so it is a module of its own:

- **A wasm side module**: the lifted program compiled position-independent (`-fPIC`) and linked by `wasm-ld -shared`
  (the `dylink.0` convention). It imports the app's memory, its function table, its C stack pointer, the machine's
  functions (`mmu_read`, `rt_poll`, `rt_syscall_m`...) and compiler-rt's, and two of its data's addresses; it exports
  its `ProgramDef`.
- **Loaded by the host** (`web/side.mjs`, in Node and the page's worker), as a dynamic linker loads a library: its
  memory taken from the machine (`rt_side_alloc`), its functions added to the table, its relocations applied, then
  registered with the machine (`rt_program_register`); from there it is found by its code pages like any program.
- **`pisco build`**: `[hints.pisco] modules = ["/bash"]` makes it a layer of the image, named in the config's `side`; the
  app's module exports what the side modules import (`pisco` reads their import sections).
- `/init` runs bash as it runs on QEMU, in Node and in headless Chrome.

## What it took (K10, K11): programs lifted when they first run

A program no build lifted can run now, in Node, from wherever its file is: the initramfs, the disk, a pipe. When the
machine meets a program no module of its holds, it is at the program's entry point, and the program is in memory as
the kernel loaded it. Its stack's auxiliary vector says where its program headers are (`AT_PHDR`); its segments'
bytes, read from memory (its pages faulted in as reads would), are its file again, as the file has them when it
starts. The machine hands that image to the host (`program_image`). `web/lift.mjs` lifts it into a module of its
own (`tools/build-side.sh`: the lifter, wasi-sdk, `wasm-ld -shared`), caches it by the image's sha256 in
`out/lift-cache`, and loads it, and the machine goes on. (K10 found the file in the initramfs by its code page; K11
needs no file.)

An image from memory has no symbols, so the lifter now also takes a function's address from an immediate (`mov
$f, %eax`, as musl's start does for `libc_start_main_stage2`). A machine's app therefore exports whatever a lifted program may import
(`rt/microvm-x64/side-exports.txt`), not only what the programs of its build import.

`/init` runs `/bin/proc` (`fixtures/linux-proc`), which is in the initramfs but in no recipe list: it is lifted in
under 3 s the first time and taken from the cache after, and prints what it prints on QEMU; with a disk, `/init`
copies it there and runs it from the disk. The page cannot lift (it
has no lifter): there such a program stops the machine.

## What it took (K12): a distribution's program, dynamically linked

`/init` runs Alpine 3.24.4's busybox, as Alpine ships it: position-independent and dynamically linked against musl,
whose loader (`/lib/ld-musl-x86_64.so.1`) is also its C library. Both are lifted when they first run, each into a
module of its own, and print what they print on QEMU.

- **The loader runs first.** The kernel starts it, not the program: at the loader's first instruction the auxiliary
  vector's `AT_BASE` says where it is, and its ELF header there gives its program headers. The machine takes its
  image from there, and at the program's entry point (`AT_ENTRY`) the program's, its load bias found from its
  `PT_PHDR`.
- **Each image as it is loaded.** The machine moves the image's addresses by its bias and makes it an `ET_EXEC` (no
  dynamic headers, no sections), so the lifter reads it as a static program at those addresses. The loader relocates
  itself when it starts, after the image was taken, so the machine applies its relocations to the copy first
  (`R_X86_64_RELATIVE`, RELR, and `64`, `GLOB_DAT` and `JUMP_SLOT` to symbols it defines itself): code addresses in
  its data are then addresses, which is how the lifter finds functions. A program the loader has relocated already
  is taken as it is.
- **Functions found by name.** The loader calls some of its own functions through pointers it finds by name in its
  symbol table (musl's `__dls2b`, `__dls3`), so no instruction names them. The machine reads the image's dynamic
  symbols and gives the host the address of each function among them; the lifter takes them as function starts
  (`--entry`).
- **The same addresses each time**: `norandmaps` on the kernel's command line places the loader and the program
  where they were the last time, so the images, and the lift cache's keys, are the same from one run to the next.
- **The x87's maths** (`expm1`, `log2`... which the lifter emits for `f2xm1` or `fyl2x`) is exported by the app for
  such a module too (`rt/microvm-x64/side-exports.txt`): musl's maths has x87 code.

The loader (669 KB) lifts in about a minute and busybox (803 KB) in two, on four cores; from the cache, nothing.

## What it took (K13): libraries

`/init` runs `/bin/dyn` (`fixtures/linux-dyn`, linked against Alpine's musl), which has libraries of its own:
`libgreet.so`, which it is linked against (a constructor, a table of function pointers, a call through its PLT), and
`libplug.so`, which it opens itself (`dlopen`) and calls by name (`dlsym`). Each is lifted into a module of its own
when its code is first met, and they print what they print on QEMU.

A library's code is met in the middle of a run, not where a program starts: the loader calls its constructor, or the
program calls into it. There the stack holds no auxiliary vector, so the machine finds the image another way: the
nearest ELF header below the code, on a page that is there (the loader has read the library's headers and dynamic
tables), whose executable segments hold it. Its bias is where its first segment is. The loader has relocated it
already, so its image is taken as it is, and the address where it was met is a function start the lifter is told of
(a constructor no symbol names).

## What it took (K14): a network

`SW_NET=1` gives the machine virtio's network device, and in Node the guest reaches the network as QEMU's user
networking would let it: the guest is 10.0.2.15 behind a gateway at 10.0.2.2 and a name server at 10.0.2.3, and its
TCP connections are made with the host's sockets (TCP to the gateway reaches the host's own 127.0.0.1).

- **The kernel** gains IPv4, Unix and packet sockets and `CONFIG_VIRTIO_NET` (no IPv6, no other network device).
- **The device** (`rt/microvm-x64/net.c`): two queues, the MAC in its configuration, no offloads. `virtio.c` is now
  virtio's MMIO transport for several devices, a page and an IOAPIC pin each (the disk first when there is one), each
  announced on the command line.
- **The frames** go to the host as they are sent (`net_send`); the host writes those for the guest into a ring in the
  module's memory (`NetRing`, rt/microvm-x64/machine.h), which the machine empties into the driver's buffers when it
  polls. An idle machine waits on the ring's bell, which the host rings for a frame and for a key typed, and with a
  network the machine is on the host's clock: the other end of a connection is.
- **Not ours: unkernel's network stack** (MIT; `web/third_party/unkernel/`, its types stripped), which answers ARP,
  DHCP, DNS and pings and terminates TCP, on the main thread of Node's host (`web/net-node.mjs`). Unkernel's kernel has a
  device of its own (kwnet) where this machine has virtio's, but both speak Ethernet frames, and the stack needed no
  change.

Two things the network brought out, which were not the network's:

- **A system call restarted.** `ping` waits in a system call that its own `SIGALRM` interrupts; after the handler the
  kernel restarts the call by sending the program back to the `syscall` instruction itself, which was no address the
  machine could enter. In a machine's program each `syscall` is one now, as the instruction after it already was.
- **The lift cache's key** (`web/lift.mjs`) is the image's sha256 and the tools' (the lifter, `rt/cpu.h`): with the
  image's alone, a program lifted before that change would have been taken from the cache as it was.

`tests/run.sh net` types into a shell: `udhcpc` gets its lease, `nslookup localhost` an answer from the host's resolver,
and `wget` a page from a web server on the host's loopback. The page has no network yet: it needs a host that
terminates the frames there (unkernel's own does it through a Wisp server over a WebSocket).

## What it took (S0): the machine's state as one file

A running machine is snapshotted into one file, an [AgentFS](https://github.com/tursodatabase/agentfs) database
(SQLite), and goes on from it in another process: in Node, in the page, or one after the other. Why this design, and
what unkernel and AgentFS taught: [research/state-as-a-file.md](research/state-as-a-file.md).

- **All the machine is in the module's memory**: its RAM, its registers (`Cpu`, `Machine`), the APIC, the 16550, virtio's
  queues, the disk's bytes, the runtime's own heap. What is not is the host stack of each kernel task (JSPI). A snapshot
  is taken where those hold nothing the guest's stacks do not (`rt/microvm-x64/snapshot.c`): at a poll at a function's
  start, or at a halt, once no task waits in a fault's handler (each level says whether a fresh host stack could go
  back where it returns: `resumable`).
- **Going on** (`rt_restore`): the memory is put back into a new instance of the same module, its programs' modules
  loaded first as they were (bash: the same table slots, the same addresses). Every host stack is dropped. The CPU
  goes on where the snapshot was taken, and every other task starts afresh on a new host stack when it is switched to,
  as a task never run does, entering the code after `__switch_to_asm`'s stack switch, its registers and return
  addresses on its kernel stack. The clock goes on from the machine's time.
- **The lifter** makes the instruction after a `hlt` an entry of lifted code, as it already did for return addresses,
  the instructions after `syscall`s and loops' heads.
- **The file** (`web/state.mjs`): `/machine/snapshots/<id>.json` (where the CPU goes on, the memory's size, its 64 KiB
  chunks by sha256, the console's last screen, the host that took it, its parent), `/machine/chunks/<sha256>` (each
  chunk gzipped, chunks of zeros left out, one file per chunk however many snapshots hold it), `machine:head` in the
  key-value store, and a log of every snapshot and restore (`tool_calls`: `agentfs timeline <file>`). It is written
  through `agentfs-sdk`: Turso's engine in Node, Turso's wasm build in the page, the file in OPFS.
- **Hosts**: Node with `SW_STATE=<file>` goes on from the file's last snapshot, and Ctrl-] in a terminal takes one and
  ends the run. The page keeps `pisco-<image>.db` in OPFS, with a panel to take a snapshot, go back to any snapshot,
  push the file to the server or pull one from it (`tools/serve.mjs --states <dir>`, the folder Node's runs read), and
  download or open the file. With `SW_STATE_URL` (Node) or `&turso=<url>` (the page) the file is a replica of a
  database in Turso's cloud; that is wired but not checked yet.

```sh
tools/setup-agentfs.sh                                   # out/npm: agentfs-sdk, Turso's engines, the page's bundle
SW_STATE=out/states/pisco-linux.db SW_CMDLINE="$(cat recipes/linux/cmdline) rdinit=/bin/sh" target/release/pisco run out/images/linux
                                                         # a shell; Ctrl-] keeps it in the file; the same again goes on from it
node tools/serve.mjs 8000 web out/images/linux --states out/states   # the page: its panel pushes and pulls that same file
# http://127.0.0.1:8000/browser/index.html?image=/files/0/&SW_CMDLINE=console%3DttyS0%20rdinit%3D/bin/sh&pull=1
```

`tests/run.sh snapshot` checks it: the test kernel snapshotted in ring 3 goes on to its end in another process, as an
unbroken run prints it, and a Linux shell's file in its tmpfs, its variable, its working folder and a counter in the
background all go on in a second process.

## Limits

- In the page only programs lifted when the image is built run; in Node any program or library is lifted when its
  code is first met, static or dynamically linked.
- A library's image is taken when its code is first met, with its data as it is then: a library whose data the
  program has written before calling it (no constructor) has another image, and another lift, when that data
  differs.
- Model-specific registers the machine does not know read as 0 where a CPU raises #GP, which some of the kernel's
  `rdmsr_safe` callers would want.
- A store that faults between its read and its write (an add to memory whose page must be copied) reads again after
  the fault; with one CPU and no other task in between, the same.
- On QEMU without the PIT the kernel has no timer interrupt (its emulated CPU has no TSC-deadline timer): `/init`
  ends all the same, but that boot says nothing of the timer the machine will have.
- One CPU. The network only in Node, IPv4 only, and of UDP only DHCP and DNS (unkernel's stack has no more). The
  disk's writes stay in memory: the file it was read from does not change.
- Only what was lifted before the kernel starts can run: here the kernel and busybox.
- Snapshots: none of a machine with a network (its peers' connections are the host's), none after a program was
  lifted while the machine ran (its module is not in the image). A snapshot holds the whole memory but its zeros; the
  kernel's caches are not dropped first.

## Progress

| # | Milestone | State |
|---|---|---|
| K0 | the kernel built from a pinned source with our configuration; the lifter reads it and counts what it does not lift; QEMU as the oracle, `/init`'s programs recorded; stacks, memory and hardware measured | done |
| K1 | the machine (`rt/microvm-x64`) on a test kernel of our own: paging, a fault fixed up, the timer, two tasks, a ring-3 task, the serial port, equal under QEMU and PISCO, in Node and Chrome | done |
| K2 | this kernel lifted: its boot log up to `Run /init as init process`, in Node, the same on every run | done |
| K3 | busybox lifted with the kernel: `/init`'s programs print `recipes/linux/traces/programs.txt`, what they print on QEMU | done |
| K4 | a console typed into, in Node and in the page; `pisco build recipes/linux`, two builds the same | done |
| K5 | a virtio disk: formatted, written and read again by `/init` | done |
| K6 | programs at the same addresses, found by the hash of their code pages: busybox and linux-hello | done |
| K7 | signals delivered anywhere (on the way back from a fault or an interrupt); tasks' stacks freed when they end | done |
| K8 | real programs on it: fastfetch, and cowsay on perl, as on QEMU | done |
| K9 | a program in a module of its own: bash (GPL-3.0) beside the kernel, as on QEMU | done |
| K10 | programs lifted when they first run (Node: from the initramfs, cached by sha256) | done |
| K11 | programs lifted from their image in memory: from the disk, a pipe, anywhere | done |
| K12 | a distribution's program, position-independent and dynamically linked: Alpine's busybox on musl's loader | done |
| K13 | libraries: one a program is linked against, one it opens (`dlopen`), each lifted when its code is first met | done |
| K14 | a network: virtio's network device, unkernel's network stack in Node; a lease, a name and a page fetched | done |
| S0 | the machine's state as one file: a running Linux snapshotted into an AgentFS database and gone on from it in another process, Node or the page | done (a proof of concept) |
