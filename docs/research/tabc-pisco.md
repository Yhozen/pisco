---
date: 2026-11-28
measured:
  pisco: this branch (conductor/tabc-poc-bench), main at 320526a merged
  tabc: https://github.com/fzakaria/tabc 8b7e7920606d5fc20197c19da92a1f308504dbe2 (1.0.0); the release binary, and the same commit built from source with and without tools/tabc-pisco/tabc.patch
  unkernel: npm unkernel@0.2.0-261125-cec0a49 (the version tabc 1.0.0 pins), its one-CPU kernel
  alpine: Docker Hub alpine:3.24.4, linux/amd64 for PISCO and native, linux/arm64 for tabc
  chrome: 158.0.8127.88, started by tabc with its own flags
  node: 24.22.2
  wasi-sdk: 34
  host: the 4-core x86-64 test VM, nothing else running
status: research; a proof of concept (tools/tabc-pisco), nothing of it in any app or base
---

# tabc with PISCO: amd64 containers in a tab, their programs lifted

[tabc](https://github.com/fzakaria/tabc) runs a Docker image in a browser tab. Its guest is unkernel, Linux built
for wasm, and its programs are aarch64, run by unkernel's emulator kwemu. This page puts PISCO inside tabc. The
image is amd64, and its x86-64 programs run as wasm lifted ahead of time, as ordinary tasks of the same kernel. It
then measures the result against tabc as released, on the same machine and browser.

## Short answer

TODO

## How it works

```
tabc run alpine:3.24.4 (amd64)
 supervisor (Go, the host) ── page (Chrome) ── worker: unkernel's kernel (wasm), patched: kw_run asks PISCO first
   pisco/ route: runtime,      │                  ├─ aarch64 tasks (tabc's init, the SDK's shell, x86load): kwemu
   lifted modules by sha256,   │                  └─ x86-64 tasks (every program of the image): PISCO's runtime,
   lift on the host            │                     lifted code, a software TLB over the kernel's page tables
```

- **The kernel is tabc's, patched as a binary.** `patch-kernel.mjs` (binaryen) moves `kw_run`'s body to
  `kw_run_a64` and gives `kw_run` a new one: a task whose registers are marked x86-64 runs on `pisco.run`, any other
  on kwemu as before. kwemu's three TLB flushes also tell PISCO. Nothing of unkernel is rebuilt, and its custom
  sections come through unchanged. The patched kernel is made from the release by `build.sh` and is never in the
  repository.
- **PISCO's runtime** (`runtime.c`, 350 KB of wasm) lives in the kernel's memory above its RAM, from 1 GiB.
  - Lifted code checks every access against a software TLB (`rt/cpu.h`); a miss asks the kernel's `kw_user_addr`,
    which walks the task's page tables and faults the page in.
  - System calls are translated to arm64's numbers and layouts (about 150) and handed back to the kernel.
  - Polls charge the kernel's instruction budget and see its interrupts. Signals are delivered the x86-64 way.
  - Each task runs on a C stack of its own, since a task may sleep in a page fault while another runs.
- **x86load** (`x86load.c`, an aarch64 program of 2 KB) is binfmt_misc's interpreter for every x86-64 ELF file. It
  maps the program and its interpreter (musl's loader) from the file, at the addresses x86-64 Linux uses without
  randomisation, builds the x86-64 stack and hands the task to PISCO. The page registers it before the container
  starts (flag `F`, so it is found inside the container's root, and `P`, so busybox's applets keep their names).
- **A program is a module of its own, by the sha256 of its image.** Where a program starts, the runtime rebuilds its
  image from memory, as `rt/microvm-x64` does ([linux.md](../linux.md), K11 to K13). The page
  (`page/pisco.js`) instantiates the module of that hash:
  - one this image's containers ran before is in the supervisor's list for the image (`pisco/index.json`), and is
    compiled while the kernel boots;
  - one met for the first time is posted to the supervisor, which lifts it on the host (`TABC_PISCO_LIFT`, the
    lifter and wasi-sdk), once, keeps it by its sha256 for every image, and sends it back. The guest waits meanwhile
    (JSPI).
- **Identity is cheap after the first time.** Code is found by the hash of its page, as on the microvm. A page
  checked once is known again by its address space and address, and, after a fork or a flush, by its frame (a
  program's text is the page cache's, shared by every process that maps it) and 16 of its words. Indirect calls go
  through a cache of targets for each address space.
- **tabc's changes** (`tabc.patch`, 400 lines): an amd64 image allowed (`TABC_IMAGE_ARCH`), `page/pisco.js` and
  ten lines in `guest.js`, the supervisor's `pisco/` route (`internal/supervisor/pisco.go`), and its log on stderr
  for experiments (`TABC_LOG_STDERR`).

## Method

The method of [tabc-poc.md](tabc-poc.md#method), with two sides more:

- **The same host, browser and flags.** The 4-core test VM, Chrome 158, started by tabc itself with its own flags,
  a fresh profile per container, headless. Every side is `tabc run --cpus 1 -m 256m`, one container per sample from
  nothing, 5 samples, median (best).
- **Sides:**
  - `tabc-arm64`: tabc 1.0.0's release binary on `alpine:3.24.4`'s arm64 image: kwemu runs every program.
  - `tabc-src`: the same commit built from source here, with its own page and kernel: shows that building from
    source changes nothing (it does not).
  - `tabc-pisco`: that build with `tabc.patch`, on the amd64 image of the same tag: PISCO runs every program but
    tabc's init and x86load.
  - `tabc-x86` and `native`: from [tabc-poc.md](tabc-poc.md) (the amd64 layer in tabc under qemu-user, and in a
    `chroot` on the VM), not measured again.
- **The same workloads** (`tools/tabc-poc/workload`), from a volume, timed by busybox's `time` in the guest; the
  compiled benchmark is one source built for both architectures, with the same checksums on every side.
- **What tabc-pisco had already lifted**: busybox and musl's loader (the image's), and the benchmark, from one earlier
  run of each. A program met for the first time is lifted then (below), once.

## The numbers

TODO

## What it took

Most of the night went to three places where the first version lost, each found by a profile, not guessed:

1. **Finding the program, after every flush.** The first version re-checked which program holds the code (a hash
   of its 4 KB page) after every TLB flush. A 500-fork loop made 300,000 range flushes (copy-on-write faults on
   data pages) and 4,800 full ones, and `program_at` was the top function of the profile. Now a range flush forgets
   only when it touches code; identities and the call cache are kept per address space; and a page checked once
   is known again by its frame and 16 of its words. Page hashes went from 28,826 to 47 for 1,000 processes.
2. **Registering binfmt_misc** through the SDK's `exec` cost 300 ms of every start. tabc's init (Go, which runs
   anyway) does it now, before it enters the container's root, and turns address randomisation off and the
   bottom-up mmap layout on: a library musl's loader maps is then at the same address on every run (so its module
   is found again), and below 2^47, where x86-64 code can be (the guest's own layout is aarch64's, near 2^48).
3. **System calls the translation lacked.** `rt_sigsuspend` was missing, so `sleep 1 & wait` spun on ENOSYS and the
   child never ran. The table now has 221 calls, with `poll`, `select`, `pause` and `alarm` turned into the
   calls arm64 has, and epoll's packed x86-64 events converted.

And from the experiment it grew out of ([microvm-speed.md](microvm-speed.md), experiment 2): programs that are
position-independent and dynamically linked (Alpine's busybox on musl's loader, both lifted, at the addresses
x86-64 Linux uses without randomisation), and a C stack per task, since a task can sleep in a page fault (a file
page fetched from the supervisor) while another runs.

## Limits

- **One CPU.** The runtime is built for unkernel's one-CPU kernel (its memory is not shared), so the page asks for
  one CPU whatever `--cpus` says. tabc's default is two. Several would need the runtime in every CPU's worker,
  with its state per CPU.
- **A program met for the first time is lifted then**, on the host: seconds for a small one, minutes for a large
  one on this VM (busybox 1.38, 1.2 MB: 166 s), and the container waits. After that it is kept by its sha256, for
  every image. A supervisor could lift an image's programs when it pulls the image instead.
- **What the runtime does not do yet**: x86-64 signal frames are arm64's (a handler that reads `uc_mcontext` sees
  arm64's registers); `ptrace`; a fault is not taken again at its instruction (a handler cannot resume a faulting
  access); 221 system calls are translated, the rest return ENOSYS and are logged once.
- **The kernel is patched as a binary**, against one release of unkernel. A hook in `kw_emu.h` (a second CPU behind
  `kw_run`) would make the patch unnecessary.
- **Identity by sampled words.** After a fork or a flush, a page of code is known again by its frame, its address and
  16 of its words, not its whole hash. A different program whose page has the same frame, address and those words
  would be taken for the first one.

## Reproduce

```sh
tools/tabc-poc/build.sh                  # the benchmark's files (bench-x86_64, the native root), once
SHARE_STOCK=1 tools/tabc-pisco/setup.sh  # out/tabc-pisco: tabc 1.0.0 from source, with PISCO and without
export TABC_CHROMIUM=$(command -v google-chrome) TABC_PISCO_LIFT=$PWD/tools/tabc-pisco/lift-side.sh
TABC_SHARE=out/tabc-pisco/share TABC_IMAGE_ARCH=amd64 out/tabc-pisco/tabc run alpine:3.24.4 uname -m
for s in tabc-arm64 tabc-src tabc-pisco; do node tools/tabc-poc/bench.mjs $s 5; done
node tools/tabc-poc/bench.mjs report
```
