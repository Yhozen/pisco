# neofetch (2026-11-26)

The second Linux app, and the first made of several programs: [neofetch](https://github.com/dylanaraps/neofetch)
7.1.0 (MIT), a bash script of 10,572 lines. Running it means running bash, the subshells bash forks for every
`$(...)`, and the programs it starts (`uname`, `awk`, `grep`, `hostname`, `realpath`). Here they are bash 5.2.37 and
busybox 1.36.1, each one static x86-64 program, lifted into a module of its own; each process is an instance of
its program's module with memory of its own, in a worker of its own. Nothing of the three is in the repository:
`tools/build-neofetch.sh` fetches their sources (checked by sha256) and builds them.

What it shows is the machine the runtime describes, as fastfetch does ([fastfetch.md](fastfetch.md)): no logo of
its own for "PISCO", so neofetch's Linux penguin.

## The files

| | |
|---|---|
| Sources | `bash-5.2.37.tar.gz` sha256 `9599b22e…21ff`, `busybox-1.36.1.tar.bz2` `b8cc24c9…e314`, neofetch `7.1.0.tar.gz` `58a95e6b…27e7` (in full in `tools/build-neofetch.sh`) |
| Built with | musl 1.2.5 and the kernel's headers (`tools/setup-toolchain.sh musl linux-headers`), `-static -no-pie`, symbols kept |
| bash | no readline, no job control (neofetch runs it without a terminal of its own), no bash malloc; GPL-3.0-or-later |
| busybox | its default configuration but `tc`; every applet is a script in `/bin` (`#!/bin/busybox`: busybox takes the applet from the script's path), and so is `/usr/bin/env`, which neofetch's `#!` names; GPL-2.0-only |
| The root | `/bin/bash`, `/bin/sh` (bash again), `/bin/busybox` and its 400 applets, `/usr/bin/env`, `/usr/bin/neofetch`: `out/neofetch/root`, the recipe's folder (`recipes/neofetch`) |
| Image | one module per program (bash 3.7 MB, busybox 6.4 MB; `/bin/sh` is the same file as `/bin/bash`, so one module); the config's `programs` says which path is which module |

The image's licences are the programs': the image is built on the user's machine from their sources and holds
their code (GPL-3.0 and GPL-2.0 programs side by side, not linked together); the runtime linked into each module is
ours (GPL-2.0-or-later).

## Build it and run it

```sh
tools/setup-toolchain.sh wasi musl linux-headers
tools/build-neofetch.sh                        # out/neofetch/root
target/release/pisco build recipes/neofetch    # out/images/neofetch (two programs lifted: about two minutes)
target/release/pisco run out/images/neofetch                # Node, as for a pipe
SW_TTY=100x30 target/release/pisco run out/images/neofetch  # Node, as for a terminal
SCREENSHOT=out/neofetch.png node tools/run-chrome.mjs out/images/neofetch   # the page, as a PNG
```

`tests/run.sh neofetch` runs it in strict mode on the virtual clock in Node and in headless Chrome; what it prints
must be `recipes/neofetch/traces/pipe.txt`. A run is 83 processes (82 forks, mostly `$(...)` subshells, and 10
programs executed), about 7 seconds in Node.

## What it took

- **Processes.** `fork` asks the host for a new instance of the same module, given a copy of this process's memory;
  the copy goes on where the parent called fork, with 0 in RAX. Everything the runtime knows (its descriptors, the
  guest's registers, its heap) is in that memory; what is not is the runtime's own stack, so the guest is entered
  again at the return addresses on its stack instead (below). `execve` asks the host for the module of the program
  the path names; arguments, environment, working directory and descriptors go with it (`rt/linux-x64/process.c`).
  Scripts are run through the program their `#!` names, as the kernel does.
- **Entering a function at a return address.** A lifted function returns natively where the guest returns, so the
  guest can go on from any return address with nothing below it: every function of a Linux app can be entered
  at the return address of each call it makes (`c->resume`, a `switch` the lifter puts at its start), and the
  runtime enters the guest in a loop that goes on at the address each function returned to (`linux_run`). A
  forked child and a `longjmp` both use it: `longjmp` unwinds the runtime's stack to that loop, which goes on at
  the address `setjmp` saved (the landing pads of the first version are gone).
- **The host's part** (`web/kernel.mjs`, the same in Node and in the page): one SharedArrayBuffer with the process
  table and the pipes (a 64 KiB ring each, and how many ends of each kind are open in all processes together), read
  and written with Atomics from every process's worker; `wait4` and a read from an empty pipe wait as a system call
  waits. Starting a worker is the supervisor's (Node's main thread, the page): a worker asks with a message.
- **Switches of a static program.** gcc compiles a switch of code that is not position-independent to
  `jmp [table+index*8]`, the table's absolute addresses in data. The lifter took each address as a function of its
  own, so bash's parser loop (`yyparse`) became a recursion a thousand calls deep, more than a browser worker's
  stack holds. Those tables are read now and their slots are no function entries: bash went from 2,730 functions
  and 274,970 lifted instructions to 1,955 and 160,921, busybox from 4,959 to 3,698.
- **The page is a terminal**: lines of cells and a cursor that moves (neofetch draws its logo, goes up twelve lines
  and writes beside it).
- **The login shell** is the shell the file system has: `/etc/passwd` names `/bin/bash`, so bash sets `SHELL` and
  neofetch's Shell line says bash.

## Limits

- No signals are delivered (no `SIGCHLD`, `SIGPIPE`, `kill` of another process does nothing); bash, without job
  control, waits for its children in `wait4` and does not need them.
- Threads (`clone` with `CLONE_VM`) are still refused.
- The file system is read-only: neofetch runs with `--no_config`, which keeps it from writing its configuration
  file into the home folder.
- A fork copies all of the process's memory (bash's is 64 MiB, the module's initial memory): about 20 ms each.
- No keyboard reaches a process's standard input in the page.

## Progress

| # | Milestone | State |
|---|---|---|
| N0 | processes: `fork`, `execve` (and `#!`), `wait4`, pipes, the working directory; the host's process table and pipes in Node; `fixtures/linux-proc` against its native run | done |
| N1 | bash and busybox built and lifted; `bash -c` with `$(...)`, pipelines and busybox's applets | done |
| N2 | neofetch in Node, strict; an image of several programs (`pisco build`, the config's `programs`) | done |
| N3 | jump tables of static programs (the browser's stack); the page as a terminal; the processes in the page; neofetch in headless Chrome | done |
