---
title: Libraries on the lifted Linux, linked and opened
---
A program's own shared libraries now run on the lifted Linux: one it is linked against, and one it opens itself with `dlopen` and calls by name. Each is lifted into a module of its own when its code is first met, and prints what it prints on QEMU (`fixtures/linux-dyn`, on Alpine's musl).

- **Found from the code**: a library's code is met in the middle of a run (its constructor, or a call into it), where no auxiliary vector says what it is. The `microvm-x64` machine takes the nearest ELF header below the code whose executable segments hold it, and the image as its loader has relocated it.
- **Where it was met is a function**: a constructor no symbol names is a function start the lifter is told of.

Still missing: a network, snapshots.
