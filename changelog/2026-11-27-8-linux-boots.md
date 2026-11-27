---
title: Linux boots, lifted
---
Linux 6.18 runs on PISCO: the whole kernel, lifted ahead of time into one 42 MB module, boots on the `microvm-x64` machine in Node to `Run /init as init process` in 0.9 s, printing the same boot log on every run. No program runs yet: `/init` is busybox, which is not lifted with the kernel yet; that is next.

- **The code the kernel rewrites while it boots:** its runtime constants (the dentry hash table's address and shift, `USER_PTR_MAX`, listed in `runtime_ptr_*` and `runtime_shift_*`) are read from memory, where the kernel wrote them. Its static-call trampolines are decoded as it has rewritten them. Its alternatives stay as the file has them, which is correct on any CPU.
- **Where the kernel goes back into its code:** the fix-ups of its exception table, its entry stubs at computed addresses, the byte after an `int3` and five bytes on (its self-test emulates a call there). A call may come back on a stack the callee moved on purpose (`error_entry`), as long as it returns through its own address.
- **The machine grew** the boot protocol (the zero page, its memory map, the command line, the initramfs), an MP table, the IOAPIC, the 16550's interrupt, a CMOS clock on the virtual clock's date, SERIALIZE in CPUID, and `fxsave`, `fxrstor` and `cmpxchg16b` in the lifter.
- **`tests/run.sh kernel`** builds the lifted kernel and checks its boot against `recipes/linux/traces/boot.txt`, twice.
