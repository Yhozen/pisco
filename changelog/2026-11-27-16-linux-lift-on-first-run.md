---
title: Programs lifted when they first run, on the lifted Linux
---
A program that no build lifted can now run on the lifted Linux kernel, in Node. When the `microvm-x64` machine meets user code it has no lifted program for, it hands the page to the host. The host finds the initramfs program whose code page there is the same, lifts it into a module of its own, caches it by its sha256, loads it, and the machine goes on. `/init` runs `fixtures/linux-proc` that way: lifted in 2.7 s the first time, from the cache after, printing what it prints on QEMU.

- **`web/lift.mjs`** (Node) reads the initramfs, compares code pages, and runs **`tools/build-side.sh`** (the lifter, wasi-sdk, `wasm-ld -shared`).
- **A machine's title exports what any lifted program may import** (`rt/microvm-x64/side-exports.txt`), not only what its build's programs import.

Still missing: the page cannot lift (no lifter there), and programs on the disk or written while the machine runs are not found yet.
