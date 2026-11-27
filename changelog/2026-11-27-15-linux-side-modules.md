---
title: bash on the lifted Linux, in a module of its own
---
A program can now be a wasm module of its own, loaded by the host beside the kernel's. bash, whose GPL-3.0 licence keeps it out of a module linked with the GPL-2.0-only kernel, runs that way on the lifted Linux: `/init` prints what it prints on QEMU, in Node and in headless Chrome.

- **A side module:** the lifted program compiled position-independent and linked by `wasm-ld -shared`. It imports the title's memory, table, C stack pointer and the machine's functions.
- **`web/side.mjs` loads it** as a dynamic linker loads a library: its memory taken from the machine, its functions added to the table, its relocations applied, then it is registered with the machine, which finds it by its code pages like any program.
- **`pisco build`:** `[lifter] modules` makes each one a layer of the image; the title's module exports what they import.
- **`LICENSING.md`** says how a machine's programs are licensed, and what this does and does not settle.
