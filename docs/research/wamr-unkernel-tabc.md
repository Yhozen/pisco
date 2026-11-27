---
date: 2026-11-27
analysed:
  pisco: c418f90b7ae0cc96425455b78fea486c27024f21  # main, re-read on 2026-11-27 (cowsay, PS5, AVX in the lifter); the busybox experiment on aeb1f22, re-lifted on c418f90 with the same counts; the TLB bench and WAMR's fastfetch on d5ffbf5
  unkernel: https://github.com/pithings/unkernel fd204d3130309c45ce51795c753c3b8ec1eaddfd (2026-11-26); npm unkernel 0.2.0-261126-fd204d3
  tabc: https://github.com/fzakaria/tabc 8b7e7920606d5fc20197c19da92a1f308504dbe2 (2026-11-26)
  wamr: https://github.com/bytecodealliance/wasm-micro-runtime WAMR-2.4.6 (0fa421193212b3fb45c7f59ec53662cc2851fcc7)
  post: https://fzakaria.com/2026/11/25/browser-tabs-as-a-container-runtime
  alpine: alpine-minirootfs-3.24.4-x86_64.tar.gz, sha256 cdaa38cb8de3537967ee5b6c9b6cd00cc962da2b018e6eea48696bb274923012
status: research only; no workstream started
---

# WAMR, unkernel and tabc: can they help PISCO, and could PISCO run any Docker image?

Three projects were looked at, with one question: does any of them help PISCO or let it do something it could not?
Then a second one: could PISCO run any amd64 Docker image, as tabc runs arm64 ones? Everything below was measured on
the 4-core x86-64 test VM with Node 24, on the commits in the front matter; re-check them before relying on this.

## Short answer

| | Helps today | Could expand PISCO | Why |
|---|---|---|---|
| WAMR | no | no | its AOT compiler rejects every module PISCO builds now |
| tabc | no | as a user of the result | a Docker runtime around unkernel; it lists "x86-64 images" as missing |
| unkernel | no | yes, for the Linux runtime | a real Linux kernel in wasm; its design admits lifted user code, with limits |

None of them touches the Windows, Xbox Series or PS5 runtimes.

## WAMR 2.4.6

- **Blocked.** `wamrc` fails with `WASM module load failed: invalid section id` on every module with a Wasm exception
  tag section. PISCO's runtime uses `setjmp` / `longjmp` (thread exit; since neofetch, `linux_run` on Linux), which
  wasi-sdk 34 compiles to legacy exceptions. Binaryen's `--translate-to-exnref` does not help: WAMR 2.4.6 has legacy
  exceptions in its interpreter only and standard exceptions not at all
  ([proposal status](https://github.com/bytecodealliance/wasm-micro-runtime/blob/WAMR-2.4.6/doc/stability_wasm_proposals.md)).
  Checked: `fixtures/bench`, `hello64`, `linux-hello`, `linux-proc` (all rejected).
- **What it showed before neofetch:** fastfetch built on `d5ffbf5`, which had no tag section, ran under WAMR AOT
  with the same 13 lines as in Node: 20 ms against 162 ms in Node, start-up included. Compiling it took 275 s and
  1.8 GB of memory, for a 14 MB `.aot` from a 5.8 MB module.
- **Memory model:** one linear memory per instance; on x86-64 a 32-bit memory reserves 8 GiB so that bounds checks
  are hardware traps. memory64 works in the classic interpreter and AOT only. Lifted FNV at 5 GiB gave the native
  checksum in plain and shared memory (`tests/memory64/run.sh`, not in main).
- Wasmtime 51 compiles the same modules after `--translate-to-exnref` (`docs/hosts.md`); it remains the engine for a
  native host.

## tabc

Go code around unkernel's SDK: a supervisor per container, a page that boots unkernel in headless Chromium, Docker's
OCI runtime interface, and `tocker` (a small Docker daemon) for Compose. It has no CPU or kernel of its own.

- About 0.9 s from command to exit for `echo hi`, about 540 MiB of host memory per idle container, 11 to 36 times
  native on compute (the post's figures).
- Worth borrowing for PISCO's Linux runtime: files read straight out of an image's layer tars (it answers the same
  three questions PISCO's host answers: attributes, entries, bytes), and the terminal and TCP connections carried
  over WebSockets (`docs/design.md` there).
- `docs/design.md` ends with "x86-64 images, since the guest runs aarch64" among what is missing.

## unkernel

Linux 7.3 built as one wasm64 module (`arch/wasm`, from linux-wasm), with a real MMU (Sv48 page tables), JSPI for
task switches, and user space run by kwemu, an aarch64 CPU in Zig (an interpreter, a decoded-op tier, a JIT to
wasm). Runs in Node 24, Chrome, Firefox 153+ and Safari 27+.

What matters for PISCO, from its source:

- **One entry into user code** (`kw_run` in `user_mode_loop`) behind an ISA-neutral contract,
  `include/kw_emu.h` (ABI 4); the guest CPU has been swapped once already (riscv64 to aarch64).
- **Kernel and all processes share one linear memory.** User addresses are virtual: kwemu walks the kernel's page
  tables and keeps its own TLB. PISCO's lifted code uses the guest address as the wasm address (`GP(a)` in
  `rt/cpu.h`), so in this kernel every access would need a lookup.
- **Faults can be taken inline.** The kernel's `copy_from_user` already walks the tables in software and faults pages
  in synchronously, which is what a lifted program's TLB miss would call.
- **Foreign code can be linked in:** its JIT instantiates wasm modules on the kernel's memory at run time
  (`kw_host_jit_compile`).
- **Only arm64's user ABI.** x86-64 binaries run through qemu-user (Alpine) or box64 (Debian) inside the guest.
  `.agents/x64.md` there sketches a native x86-64 front end and lists the kernel work: a second syscall table,
  `struct stat`, packed `epoll_event`, `arch_prctl`, clone's argument order, x86 signal frames.
- fork + exit + wait about 60 µs (its README); PISCO's fork copies the whole memory, about 20 ms for bash.
- Licences: the kernel and `kw_emu.h` are GPL-2.0(-or-later), the SDK MIT; compatible with PISCO.

### What a software TLB costs lifted code

`fixtures/bench`, Node 24, ms, best of 3. The TLB variant puts a 1024-entry direct-mapped lookup (identity fill) in
front of every guest load and store, through a copy of `rt/cpu.h` (`GP`), checksums unchanged:

| workload | native | wasm32 | wasm64 | wasm64 + software TLB |
|---|---|---|---|---|
| calls (fib 32) | 6 | 23 | 23 | 34 |
| bytes (FNV, 64 MB) | 99 | 127 | 129 | 171 |
| sort (heapsort, 2M u32) | 251 | 550 | 530 | 998 |
| float (4x4 transform) | 20 | 57 | 57 | 124 |
| memory (20M random loads) | 135 | 160 | 161 | 223 |

So 1.3 to 2.2 times slower than PISCO now, 1.6 to 6 times native. It leaves out permission checks and misses.

## Alpine's own busybox on PISCO (an experiment)

To see how far PISCO is from real distribution binaries, Alpine 3.24.4's x86-64 `bin/busybox` (stripped,
position-independent, dynamically linked) and `lib/ld-musl-x86_64.so.1` (musl's dynamic linker, also the C
library) were run on the Linux runtime of `aeb1f22`, without changing the repository (main's lifter, `c418f90`, still
refuses both files as they are, and lifts the merged ELF below to the same counts):

1. A throwaway script put both files at fixed addresses in one ELF (busybox at 0x400000, ld-musl at 0x600000): one
   copy with the relocations applied (RELA and RELR) and the exported functions as a `.symtab`, for the lifter; one
   with the files' own bytes, for the runtime to load (the dynamic linker relocates them itself).
2. In a scratch copy of `rt/`, the auxiliary vector's `AT_BASE` and `AT_ENTRY` came from the environment, so the
   process starts in musl's `_dlstart`, which relocates both and calls busybox.
3. `tools/build-title.sh`: 4,166 functions, 284,285 instructions, 18 left out (x87 `fcmov`, `fpatan`, `fldenv`,
   `fnstenv`); 55 s; a 6.8 MB module.
4. The root's 304 symlinks to `/bin/busybox` became `#!/bin/busybox` scripts (the host does not follow a root's
   absolute symlinks).

`sh -c` with pipes, `$(...)`, `ls`, `wc`, `tr`, `sort`, `awk`, `grep`, `sed`, `id` and `sha256sum` printed what the
same commands print natively (the sha256 checked). Strict mode stopped at `sendfile`, which `cat` tries first.

### Against unkernel on the same machine

unkernel from npm, its Alpine guest on one CPU, Alpine's x86-64 busybox started through its dynamic linker (qemu-x86_64,
installed by the guest on first use); times are the guest's `time`, boot left out. PISCO's are wall time in Node less
the 0.14 s `busybox true` takes.

| | unkernel, arm64 busybox | unkernel, x86-64 busybox (qemu-user) | PISCO, x86-64 busybox lifted |
|---|---|---|---|
| `awk 'BEGIN{for(i=0;i<100000;i++)s+=i%7; print s}'` | 2.62 s | 14.24 s | 0.48 s |
| `sha256sum` of 4 MiB | 0.14 s | 1.82 s | 0.06 s |
| `sh -c 'for i in $(seq 20); do true; done'` | 0.03 s | 10.44 s | 2.1 s |

- Running x86-64 at all is not the hard part: it already runs on unkernel, slowly. The work is making it fast.
- Lifted code was about 30 times faster than unkernel's x86-64 route, and should stay above unkernel's own arm64 after
  the TLB's cost (an estimate). Not measured: box64, which unkernel's notes put at 2 to 3 times faster than qemu-user.
- PISCO's processes were 70 times slower to start than a real kernel's. Each side has what the other lacks.

## Could PISCO run any Docker image?

Not as it is built: the kernel is imitated one system call at a time (never complete), and only code lifted before the
program starts can run (no JITs: Node, Java, LuaJIT). The machine that could:

1. **The real Linux kernel in wasm**: a fork of unkernel's kernel patches (GPL-2.0, like Linux), given x86-64's user
   ABI (Linux's `arch/x86` has it). Threads, signals, sockets, writable files and fast fork come with it.
2. **Memory through page tables**: a lookup on every access (measured above), which is what makes fork, shared memory
   and large reservations work.
3. **Three ways to run x86-64 code, on one register file:** the lifter ahead of time, per file, cached by sha256
   (distribution files are the same for everyone, so the cache can be shared); the lifter writing wasm at run time,
   in the page, for code that appears while a program runs; an interpreter, for single steps and exact faults. Main's
   "enter a function at any return address" (`linux_run`) is what lets a process move between them.
4. **Docker on top**: if the fork keeps unkernel's SDK interface, tabc should run on it with amd64 images.

Steps, none started:

0. Any amd64 image, slowly: unkernel and tabc with qemu-user or box64 available in any image. No PISCO in it; stays
   as the fallback. Untested beyond unkernel's own images.
1. Lifted programs as tasks in that kernel, falling back to the emulator per program. The smallest proof:
   `fixtures/linux-hello` lifted without the runtime, run as an unkernel task.
2. The run-time translator, replacing the emulator fallback: the largest new piece, months.
3. A shared cache of lifted files.

What main already has toward step 1 (`c418f90`): processes, `execve` of `#!` scripts and pipes (neofetch, cowsay);
"enter a function at any return address" (`linux_run`); relative relocations read for PS5 images (`ps5.rs`), the
start of what a position-independent Linux program needs. Not yet: threads, signals, writes, sockets
(`docs/status.md`, Linux x64, Next).

What stays hard: exact faults (Java and Go handle their own SIGSEGV); CPUID (since `77f869e` the lifter lifts AVX,
AVX2, FMA and BMI, `tests/avx.sh`, but CPUID does not report them, so a program that checks, as RHEL 10's x86-64-v3
builds do at start, would refuse to run until it does); module size (perl's is 34 MB, `docs/cowsay.md`, so a large
image is minutes to lift on first use); x86's memory ordering for threads on ARM hosts, about 540 MiB per idle container and under
4 GiB of guest memory, and a kernel fork to keep unless unkernel takes the changes. The kernel is Pooya Parsa's
(unkernel); tabc is Farid Zakaria's.

## To re-check

- WAMR: standard exception handling in AOT (then `wamrc` on any module of `tests/run.sh`).
- unkernel: an x86-64 front end, or a contract change for user code that keeps state outside `kw_regs`; its ABI
  version (4 here).
- tabc: amd64 images.
