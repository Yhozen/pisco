---
title: Programs from anywhere, lifted from memory, on the lifted Linux
---
A program met for the first time is now lifted from its image in memory, not from a file the host has to find. At a program's entry point, its stack's auxiliary vector says where its headers are, and its segments are as the kernel loaded them. The `microvm-x64` machine rebuilds the ELF from that and hands it to the host, which lifts it, caches it and loads it. A static program can therefore run from the initramfs, the disk or anywhere: `/init` copies a program to the disk and runs it from there.

- **An image from memory has no symbols**, so the lifter now also takes function addresses from immediates (`mov $f, %eax`, as musl's start does).
- **`tests/run.sh kernel`** checks a program run from the disk.

Still missing: position-independent and dynamically linked programs, which are somewhere else each time.
