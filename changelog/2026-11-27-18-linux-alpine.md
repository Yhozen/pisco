---
title: A distribution's program on the lifted Linux: Alpine's busybox
---
The lifted Linux runs Alpine 3.24.4's busybox as Alpine ships it: position-independent and dynamically linked, on musl's loader. Both the loader and the program are lifted when they first run, each into a module of its own, and they print what they print on QEMU.

- **The loader is taken first**: the kernel starts it, at the address its auxiliary vector gives (`AT_BASE`), and the program after it, at its entry point. The `microvm-x64` machine rebuilds each as a program at the addresses it was loaded at, applying the loader's relocations to the copy, as the loader would.
- **Functions found by name**: the loader calls some of its own functions through pointers it looks up in its symbol table, so the machine passes the lifter every function its dynamic symbols name.
- **The same addresses each time** (`norandmaps`), so the images, and the lift cache, are the same from one run to the next.

Still missing: shared libraries the loader opens beside musl's.
