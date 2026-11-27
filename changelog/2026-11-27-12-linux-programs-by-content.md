---
title: Programs at the same addresses on the lifted Linux
---
The lifted Linux kernel can now run several programs linked at the same addresses. busybox and our own `fixtures/linux-hello`, both static at `0x400000`, run interleaved in `/init` and print the same 22 lines as on QEMU.

- **`lifter --user <name>`** lifts a program on its own, its functions and tables under its name, with the hash of each page of its code. `tools/build-title.sh` (`PROGRAMS`) and `pisco build` (`[lifter] programs`) lift each and link them with their list.
- **The machine finds which program runs by its bytes:** after a change of CR3 it hashes the code page it is about to run, reading it first as an instruction fetch would, so the kernel maps it. Then it takes the program whose page matches.
- **A machine's code runs the x87 at its full precision**, so musl's `printf` of a `double` prints what it prints on a CPU.
- **`pisco build`** compiles a cached lift again when the compiler's flags change.

Still missing: bash, whose licence (GPL-3.0) keeps it out of a module linked with the GPL-2.0-only kernel; that needs a module per program.
