---
title: Linux itself as a title, the groundwork
---
The plan to run a real Linux kernel on PISCO: lifted ahead of time like any other title, on a runtime that imitates a small x86-64 machine. Nothing of the kernel runs under PISCO yet. This first step builds the kernel, makes the lifter read it, sets up QEMU as the reference, and measures the three things the design stands on. The reasoning and the numbers are in `docs/research/kernel-as-a-title.md`; the title's page is `docs/linux.md`.

- **`tools/build-linux.sh`** fetches Linux 6.18.65 from kernel.org (checked by SHA-256) and builds it with `recipes/linux/kernel.config` on top of the kernel's `tinyconfig`: one CPU, no jump labels, no mitigations, no modules, no ACPI, no PCI. It stops at a line the kernel did not take. With busybox (`tools/build-busybox.sh`, shared with neofetch's build) and `recipes/linux/init` it packs an initramfs. Two builds give the same files.
- **The lifter reads a kernel** (`--machine`, or any image in the upper half of the address space): 10,853 functions and 583,233 instructions. It does not lift 3,958 of them yet (0.68 %), all instructions of the system: `sti`, `cli`, `pushfq`, `wrmsr` and about fifty rarer kinds. A kernel's code is taken from its sections, since its executable segment holds its strings too.
- **A fix for every 64-bit title:** an operand that is a control, debug or segment register was taken for the general register of the same number (`mov cr3, rax` wrote RBX). A segment register now reads as its selector in a program; the others are counted as not lifted.
- **QEMU is the oracle** (`tools/oracle-qemu`): it boots the same kernel and initramfs, and what `/init`'s programs print is recorded in `recipes/linux/traces/programs.txt` (pipes, `fork`, `exec`, a file written and hashed, a child killed, a signal handled). `tests/run.sh kernel` checks it, also on a machine without HPET, PIT and 8259.
- **Three measurements:** four JSPI stacks, each with its own C stack, switch in 0.27 µs; a software TLB that a page fault can come back through costs 1.3 to 2.0 times today's lifted code on the benchmark title; and the kernel boots with only a local APIC, an IOAPIC and a serial port.

Still missing: the machine itself (`rt/microvm-x64`), and so everything that runs. Next is a small test kernel of our own on it, then this one.
