---
title: Signals anywhere, and thousands of processes, on the lifted Linux
---
Two gaps in the `microvm-x64` machine are closed. A program can now take a signal at any point, not only in a system call. And processes that end give back their stacks, so a shell running 500 programs one after another is part of the tests; 4,000 took 5.2 s.

- **Handlers run on top:** when the kernel sends a program elsewhere (a signal's handler) on the way back from a fault, an interrupt or a system call, the handler runs above the interrupted code, and the interrupted code goes on when `sigreturn` comes back to it. A copy-on-write fault that returned with SIGCHLD pending used to stop the machine there.
- **Interrupts in loops are taken on the jump back, at its target:** an interrupted loop goes on at its next iteration.
- **Tasks that end** (`do_task_dead`) give back their C stack, their slot and their wasm stack. A new task whose kernel stack had a dead task's stack pointer used to resume the dead one.
- **`/init`** catches a signal sent into a busy shell loop, as on QEMU.
