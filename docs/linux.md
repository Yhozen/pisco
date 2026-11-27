# Linux, the kernel as a title (2026-11-27)

The plan to run Linux itself: a real [Linux](https://kernel.org) kernel (6.18.65, GPL-2.0-only), built from its
source with our own configuration, lifted ahead of time like any other title, on a runtime that imitates a small
x86-64 machine (`microvm-x64`, not written yet). The Linux x64 runtime ([fastfetch.md](fastfetch.md)) imitates the
kernel one system call at a time; here `fork`, signals, pipes and files are the kernel's own work. Why this route,
what was measured before taking it and how the machine is designed: [research/kernel-as-a-title.md](research/kernel-as-a-title.md).

**So far (K0) nothing of the kernel runs under PISCO.** It is built, the lifter reads it and says what it does not
lift yet, and QEMU boots it as the reference: its programs print the 15 lines the lifted kernel will have to print.

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

## Build it and run it

```sh
tools/setup-toolchain.sh musl linux-headers kernel qemu   # the last two say what is missing; they install nothing
tools/build-linux.sh                                      # out/linux: vmlinux, bzImage, initramfs.cpio (minutes)
tools/oracle-qemu/oracle.sh run linux                     # QEMU: the boot log, then what /init's programs print
tools/oracle-qemu/oracle.sh programs linux                # only the programs' lines of that run
tools/census.sh out/linux/vmlinux                         # what the lifter does not lift yet (it knows a kernel)
target/release/lifter out/linux/vmlinux out/linux/census/title.c --split 16 --machine   # the same, by hand
```

`tests/run.sh kernel` does the last four: the lifter must read `vmlinux`, and on QEMU the programs must print
`recipes/linux/traces/programs.txt`, also on a machine without HPET, PIT and 8259 (`KERNEL_RECORD=1` records the
file again). It is skipped until `out/linux` is built, and says so when QEMU is missing.

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

## Limits

- The kernel does not run under PISCO yet: K1 builds the machine on a test kernel of our own, K2 boots this one.
- On QEMU without the PIT the kernel has no timer interrupt (its emulated CPU has no TSC-deadline timer): `/init`
  ends all the same, but that boot says nothing of the timer the machine will have.
- One CPU, no network, no disk: the initramfs is the whole file system.
- Only what was lifted before the kernel starts can run: here the kernel and busybox.

## Progress

| # | Milestone | State |
|---|---|---|
| K0 | the kernel built from a pinned source with our configuration; the lifter reads it and counts what it does not lift; QEMU as the oracle, `/init`'s programs recorded; stacks, memory and hardware measured | done |
| K1 | the machine (`rt/microvm-x64`) on a test kernel of our own: paging, a fault fixed up, the timer, two tasks, a ring-3 task, the serial port, equal under QEMU and PISCO | next |
| K2 | this kernel lifted: its boot log up to `Run /init as init process` | |
| K3 | busybox lifted: `/init`'s programs print `recipes/linux/traces/programs.txt` | |
| K4 | a console typed into, in Node and in the page; `pisco build recipes/linux` | |
