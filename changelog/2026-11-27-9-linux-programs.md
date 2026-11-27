---
title: Linux runs its programs
---
The lifted Linux kernel now runs programs: busybox, lifted into the same module, runs the initramfs's `/init` to its end on the `microvm-x64` machine in Node. That covers pipes, a shell in a shell, awk, a 4 MiB file written and hashed, a child killed, a signal handled, ten programs executed. It prints the same 15 lines it prints on QEMU, the same on every run, in 2.5 s from Node's start to the exit port. `fork`, `exec`, signals, pipes and files are the kernel's own work, not an imitation of it.

- **`lifter --program <elf>`** (`PROGRAMS` for `tools/build-title.sh`) lifts static programs with a kernel, at the addresses the kernel loads them at.
- **A program's `longjmp`** is its own: an indirect jump to a return address enters the code there.
- **About QEMU's speed:** with its CPU emulated in software on the same machine, QEMU takes 1.15 s to `/init` where the lifted kernel takes 0.83 s; awk 0.78 s against 0.86 s; ten `exec`s 16 ms against 18 ms.
- **`tests/run.sh kernel`** checks the programs' output against QEMU's.

Still missing: typing into the console, `pisco build` of the Linux title, and programs at the same addresses as one another (bash and busybox).
