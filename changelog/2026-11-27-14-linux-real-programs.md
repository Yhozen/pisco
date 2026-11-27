---
title: fastfetch and cowsay on the lifted Linux
---
Real programs run on the lifted Linux kernel. fastfetch 2.71.0 and cowsay 3.8.4 on perl 5.44.0 are in its initramfs beside busybox, all static at the same addresses, each lifted on its own into the kernel's module. `/init` prints fastfetch's lines and cowsay's cow exactly as on QEMU.

- **fastfetch reads the kernel's own `/proc` and `/sys`**: the machine's CPU (`pisco microvm-x64 (lifted) @ 1.00 GHz`), its 244 MiB of memory, the root file system's use.
- **`tools/build-linux.sh`** builds both programs and copies only the perl modules cowsay loads, as perl itself names them.

Still missing: bash (its GPL-3.0 keeps it out of a module linked with the kernel), a network.
