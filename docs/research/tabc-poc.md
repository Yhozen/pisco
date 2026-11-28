---
date: 2026-11-27
measured:
  pisco: branch conductor/tabc-poc-bench on Yhozen/microvm-k13 (aec78d3), the commit that adds this page; image out/images/alpine, manifest sha256:d2ba3abde756168c015133395c6275018549f99480d196087f420fae4e382bac
  tabc: https://github.com/fzakaria/tabc v1.0.0 (8b7e7920606d5fc20197c19da92a1f308504dbe2), the release tabc-linux-amd64.tar.gz, sha256 fadac2ead553a0e627a5369569a6a03c79c600c13cbb45428e7b398c821ebf1d
  unkernel: 0.2.0-261125-cec0a49 (npm), as tabc 1.0.0 carries it
  alpine: alpine:3.24.4 on Docker Hub; linux/amd64 sha256:0f49f331d9212aff11b932863be4eb01cdf962969aa86dab7d1ee21ac6f5f52b, linux/arm64 sha256:1829893486be257df5049ab3b7fc95afbd0fc09d89803cbc580bda1ba5386b29 (the one tabc pulls)
  qemu-user: Alpine 3.24's qemu-x86_64 11.0.3-r0 for aarch64, sha256 3ccc698b81da078425115d531c23d328e9dd1c094fb1ac4dc9417d1687f4363f
  linux: 6.18.65, recipes/linux/kernel.config, built by gcc 13.3 (Ubuntu 24.04)
  compiler: zig 0.17.1 (zig cc, -O2, static musl) for tools/tabc-poc/bench.c
  host: the 4-vCPU x86-64 VM of the earlier pages (Xeon @ 2.50 GHz, KVM), Linux 6.18.56, Google Chrome 158.0.8127.88, Node 24.22.2
status: research only; a proof of concept, not a feature
---

# tabc the PISCO way: a container on the lifted Linux, against tabc

[tabc](https://github.com/fzakaria/tabc) runs a Docker image in a browser tab: headless Chrome, unkernel (Linux built
for wasm), and the image's aarch64 programs on kwemu, unkernel's aarch64 CPU. An amd64 image would need an emulator
in the guest. PISCO lifts x86-64 code to wasm ahead of time (AOT), kernel included ([linux.md](../linux.md)). This page
builds the smallest container that makes the two comparable: the same Alpine image, run the same way, in the same
browser, on the same machine. It then measures whether lifting beats tabc. The earlier analysis is
[wamr-unkernel-tabc.md](wamr-unkernel-tabc.md).

## Short answer

| | PISCO, lifted Linux, amd64 | tabc, arm64 image | tabc, amd64 image (qemu-user) |
|---|---|---|---|
| CPU-bound programs | level with tabc arm64: from 3 times faster to 3 times slower | the reference | 1.4 to 90 times slower than PISCO (6 to 8 on busybox's workloads) |
| fork, exec | 4 to 15 times slower than tabc arm64 | the reference | 18 to 50 times slower than PISCO |
| tab to first output | 4.2 s | 1.3 s | 1.3 s plus the emulator's set-up |
| memory, idle tab | 1,041 MiB | 554 MiB | 555 MiB |
| download | 92 MB (18 MB gzipped) | 13 MB of unkernel, then the image's files as read | the same |
| ahead of time | the kernel lifted: 72 s; the image's programs: 60 s (8 vCPU) | nothing | nothing |

**The verdict.** For an amd64 image AOT wins clearly: 6 to 50 times faster than tabc's only route today (qemu-user in the
guest). Against tabc's native case, arm64 programs on its aarch64 CPU, it does not win. Compute is level, ahead on
some workloads and behind on others. PISCO loses on process creation, cold start and memory. The lifted code is
fast; the cost is the machine around it: a software MMU on every access, indirect calls resolved at run time, and a
kernel that is itself lifted code. Closing the gap is mostly machine work, not lifter work.

## What was built

The container (`tools/tabc-poc/build.sh`, `recipes/alpine`):

1. **The image**: `pull.py` pulls `alpine:3.24.4` for linux/amd64 from Docker Hub, anonymously (manifest, config,
   layer, each checked by digest). It is the smallest pull there is, and the arm64 one is the image tabc pulls (the
   same diff_id).
2. **The root**: `mkinitramfs.py` turns the layers, applied in order (OCI whiteouts honoured), into the kernel's
   initramfs. It adds `/init` (`recipes/alpine/init`), `/usr/local/bin/bench` (the compiled benchmark) and
   `/usr/local/bin/workload` (the workloads, below). The busybox in it is Alpine's, byte for byte (sha256
   `01a989eb…`).
3. **The kernel**: Linux 6.18, alone (`KERNEL_ONLY=1 tools/build-linux.sh`), lifted by `pisco build recipes/alpine`
   with no programs of its own.
4. **The programs, ahead of time**: Alpine's programs are dynamically linked and position-independent, so the
   machine only knows their image once the kernel has loaded them (K12). The build runs the image once in Node with
   every workload (`workload all`). Each program the machine meets is lifted then (`web/lift.mjs`, into
   `out/tabc-poc/lifted` by `PISCO_LIFT_CACHE`): musl's loader, busybox and the benchmark.
5. **The image again**, with those modules as layers (`[hints.pisco] lifted`, new). The page now runs them: when the
   machine meets a program, the worker hashes the image the machine took (sha256) and loads the module of that
   hash, which the page compiled beside the kernel's (`web/browser/worker.js`, [linux.md](../linux.md#what-it-took-k12-a-distributions-program-dynamically-linked)).
   Nothing is lifted in the page. A program outside the training run would stop the machine in the page, as before.
6. **Run like `tabc run`**: the command follows ` -- ` on the kernel's command line (`?SW_CMDLINE=` in the page).
   `/init` mounts `/proc`, `/sys`, `/dev` and `/tmp`, runs it with the image's `sh -c`, prints its exit status and
   ends the machine.

```sh
tools/tabc-poc/build.sh                 # out/images/alpine (and the one-time costs, out/tabc-poc/build-times.txt)
node tools/serve.mjs 8000 web out/images/alpine
# http://127.0.0.1:8000/browser/index.html?image=/files/0/&SW_CMDLINE=console%3DttyS0%2C115200%20panic%3D-1%20no_timer_check%20vdso%3D0%20tsc%3Dreliable%208250.nr_uarts%3D1%20norandmaps%20quiet%20--%20workload%20awk
node tools/tabc-poc/bench.mjs pisco 5   # and tabc-arm64, tabc-x86, native; then: bench.mjs report
```

The build ran on a boat.dev `large` sandbox ([dev-machines.md](../dev-machines.md)) and the image was copied back;
everything below was measured on the test VM.

## Method

- **One host, one browser.** The same VM, the same Chrome 158 binary, started with tabc's own flags
  (`internal/supervisor/chrome.go`, repeated in `bench.mjs`), a fresh profile per container, headless. PISCO's side
  adds only `--remote-debugging-port`, to read the page's console.
- **One container per sample, from nothing.** Each run starts the browser, boots the kernel, runs one workload and
  ends. 5 runs per workload, median (best). Benchmarks ran one at a time, nothing else on the machine.
- **The same guest**: one CPU, 256 MiB (tabc with `--cpus 1 -m 256m`; its defaults are 2 CPUs and 1 GiB).
- **The same image contents**: `alpine:3.24.4`, its amd64 layer for PISCO and native, its arm64 layer for tabc. For
  tabc's amd64 case, the amd64 layer is copied into the arm64 container and entered with `chroot`. A
  `binfmt_misc` entry (the `F` flag) runs every x86-64 ELF there through Alpine's `qemu-x86_64`. That is what an
  amd64 image costs on tabc today: every process, the shell included, is x86-64 under emulation. The set-up
  (`prelude.sh`, the files from a volume) is outside the times.
- **The same workloads** (`tools/tabc-poc/workload`, Alpine's busybox `sh`). Each is timed inside the guest by
  busybox's `time` (10 ms steps), the same program on every side:
  - `awk`: `BEGIN { for (i = 0; i < 100000; i++) s += i % 7 }`
  - `sha256`: `sha256sum` of a 4 MiB file in `/tmp`
  - `gzip`: `gzip -6` of `seq 1 500000` (3.4 MB to 1,066,374 bytes)
  - `fork`: 50 subshells (`(:)`); `exec`: 50 runs of `/bin/true` (fork, exec, the dynamic loader)
  - `bench`: `tools/tabc-poc/bench.c`, `fixtures/bench`'s five workloads in C, built by one compiler (zig cc 0.17.1,
    `-O2`, static musl) for x86-64 (baseline ISA) and aarch64. The checksums are the same on every side.
- **The guest's clock is the host's** on PISCO in the page (`SW_CONSOLE=tty`, the page's default), so `time` in the
  guest means wall time there too.
- **Cold start**: from starting Chrome (or `tabc run`) to the line `hi` of `echo hi`. **Memory**: the PSS of every
  process of the container (browser, renderers, GPU and utility processes; for tabc, its supervisor too) 12 s after
  the start of an idle container (`sleep 20`), as tabc's `tools/bench/memory.py` measures it.
- **Native**: the amd64 layer in a `chroot` on the VM.

## The numbers

Median (best) of 5, in seconds unless marked. Raw results: `out/tabc-poc/results/*.json`, the tables from `bench.mjs report`.

### One-time costs and download

| | PISCO | tabc |
|---|---|---|
| kernel | built by gcc in 48 s; lifted in 6.4 s and its C compiled in 64.5 s (8 vCPU): a 49.5 MB module | prebuilt, in tabc's binary |
| the image's programs | lifted during the first run in Node, 65 s in all: busybox (803 KB) 39.7 s into 14.6 MB, musl's loader (669 KB) 17.9 s into 6.3 MB, the benchmark (25 KB) 2.0 s into 284 KB | nothing: run as they are |
| what one container fetches | 91.8 MB: the kernel's module 49.5, the lifted programs 21.1, `vmlinux` and the initramfs 21.1 (17.7 MB gzipped, 11.8 MB brotli) | unkernel's kernel and SDK 12.6 MB (4.8 MB gzipped), then each file of the image as the guest reads it, out of its layer (4.2 MB compressed for the whole arm64 image) |

### Cold start

| `echo hi` | native | PISCO | tabc arm64 |
|---|---|---|---|
| first output | 0.02 (0.02) | 4.22 (4.17) | 1.34 (1.30) |
| container ended | 0.03 (0.02) | 4.33 (4.26) | 1.37 (1.33) |

PISCO's 4.2 s, from one run's console: Chrome ready to navigate at 0.37 s; the image fetched and its four modules
compiled by 1.65 s; then 2.2 s to `container: up` (the worker, the kernel's boot, unpacking the 8.3 MB initramfs,
the first `sh`). In Node the same boot is 2.0 s, with 0.5 s of it unpacking the initramfs in the lifted kernel.
tabc's amd64 case starts the same arm64 container (1.37 s); its emulator's set-up comes on top.

### Steady state

| busybox `time`, s | native | PISCO | tabc arm64 | tabc amd64 (qemu-user) |
|---|---|---|---|---|
| awk | 0.09 (0.09) | 2.50 (2.32) | 2.09 (1.98) | 20.93 (20.09) |
| sha256sum, 4 MiB | 0.02 (0.02) | 0.24 (0.24) | 0.15 (0.15) | 1.82 (1.62) |
| gzip | 0.18 (0.15) | 2.24 (2.16) | 5.21 (5.03) | 13.71 (13.40) |
| bench (all five) | 0.77 (0.73) | 4.66 (4.39) | 4.38 (4.35) | 39.32 (36.70) |

| the compiled benchmark, ms | native | PISCO | PISCO, in the kernel's module | tabc arm64 | tabc amd64 |
|---|---|---|---|---|---|
| calls, fib(32) | 8 (8) | 247 (225) | 241 (210) | 77 (74) | 1,387 (1,345) |
| bytes, FNV of 64 MB | 140 (120) | 787 (693) | 752 (591) | 608 (592) | 1,127 (1,108) |
| sort, heapsort of 2M u32 | 387 (370) | 2,454 (2,387) | 2,447 (2,113) | 1,566 (1,534) | 7,050 (6,847) |
| float, 4x4 transform | 37 (35) | 294 (266) | 252 (218) | 895 (889) | 27,485 (25,011) |
| memory, 20M random loads | 188 (184) | 598 (524) | 500 (465) | 1,177 (1,137) | 1,856 (1,785) |

"In the kernel's module" is `recipes/alpine-k` (`INKERNEL=1`): the static benchmark also lifted into the kernel's
module, not position-independent. The machine takes that one, so the comparison isolates what a side module's PIC
code costs: 0 to 16% here.

### Processes

| 50 of them, s | native | PISCO | tabc arm64 | tabc amd64 |
|---|---|---|---|---|
| fork, `(:)` | 0.01 (0.01) | 0.43 (0.32) | 0.03 (0.02) | 7.77 (7.44) |
| fork + exec, `/bin/true` | 0.02 (0.02) | 0.78 (0.70) | 0.20 (0.18) | 38.94 (37.82) |

That is about 9 ms a fork and 16 ms a fork and exec on PISCO, against 0.6 ms and 4 ms on tabc's arm64. The
Linux runtime, which copies the whole memory, took 105 ms a process ([wamr-unkernel-tabc.md](wamr-unkernel-tabc.md)).

### Memory

| idle container at 12 s, MiB PSS | PISCO | tabc arm64 | tabc amd64 |
|---|---|---|---|
| one CPU, 256 MiB guest | 1,041 | 554 | 555 |
| PISCO with a 128 MiB guest (`SW_MEMORY=128`) | 914 | | |
| tabc's defaults (2 CPUs, 1 GiB guest) | | 585, 588 (two runs) | |

tabc's post says about 540 MiB a container, which is confirmed here. PISCO's extra 490 MiB is all in the page's
renderer (626 against 122 MiB). The guest's RAM is all committed (128 MiB less guest, 127 MiB less PSS), where
unkernel's grows as it is used. The rest is the code of 70 MB of modules compiled by V8, and the bytes those modules
and the drive's files were fetched as. It has not been broken down further.

### Against the earlier figures

| | earlier: unkernel arm64 / unkernel amd64 / PISCO Linux runtime | now: tabc arm64 / tabc amd64 / PISCO lifted Linux |
|---|---|---|
| awk 100k loop | 2.62 / 14.24 / 0.48 | 2.09 / 20.93 / 2.50 |
| sha256sum 4 MiB | 0.14 / 1.82 / 0.06 | 0.15 / 1.82 / 0.24 |
| 20 processes | 0.03 / 10.4 / 2.1 (`true` is a builtin: no process) | 50 forks 0.03, 50 execs 0.20 / 7.8, 38.9 / 0.43, 0.78 |

The guess on the earlier page, "1.3 to 2x on compute", was too kind. The lifted Linux is 4 to 5 times slower than
the Linux runtime on awk and sha256sum, and 3 to 30 times native on the benchmark.

## Where the time goes

A CPU profile of `workload awk` and `gzip` in Node (`node --cpu-prof`, with the guest clock on the host's): busybox's
module 33%, musl's loader (the C library) 25%, the kernel's module and the machine 32%. Within the machine:
`rt_call` (the lookup of an indirect call or jump, through musl's PLT and every function pointer) 7.4%;
`mmu_walk`, `access` and `phys_read` (TLB misses: walks of the page tables) about 7%; `code_of` 1%. Inside the
lifted functions, every load and store goes through the TLB check (`MR`, `MWR` in `rt/cpu.h`), every `push` and
`pop` included. Every function entry and jump back also polls for interrupts.

- Compiled code is 3 to 30 times native here. The same lifter's code without the machine (`fixtures/bench` on the
  Linux runtime) is 1.2 to 4 times native, and the earlier software-TLB prototype 1.6 to 6 times. The machine's
  full TLB (permissions, two TLBs, a fault path), the polls and the resume check at each function's entry cost
  more than the prototype measured.
- Chrome is slower than Node on the same image: awk 2.3 to 2.8 s in the page against 1.8 to 2.1 s in Node, and
  fib(32) 225 to 247 ms against 104 to 125 ms. Lazy compilation and V8's tiers were ruled out: `--no-liftoff`
  doubled the time, `--no-wasm-lazy-compilation` changed nothing. A profile of the page's worker is the next step
  (AGENTS.md).
- Runs vary by 10 to 30% on PISCO and by under 5% on tabc and native. One boot running `bench` three times gave
  sort 1.4 to 3.1 s in Node.

## Where AOT wins, where it loses

**Wins**:
- Any amd64 image. tabc has no way to run one but emulation inside the guest: 1.4 to 90 times slower than lifted
  code (6 to 8 times on busybox's workloads), and 50 times slower to run a program.
- Floating point and memory-bound code, even against tabc's native arm64: float 3 times, random loads 2 times, gzip
  2.3 times faster. kwemu's JIT is weakest there.
- No emulator to install, nothing translated at run time: the code the tab runs was compiled once, by clang.

**Loses**:
- Integer and call-heavy code against tabc's native arm64: fib 3 times, sort 1.6 times, awk 1.2 times slower.
- Processes, 4 to 15 times slower: fork and exec are the kernel's own, but run as lifted code through the software
  MMU, where unkernel's kernel is wasm compiled from C, with its MMU in wasm too.
- Cold start, 3 times slower: 92 MB to fetch and compile, a lifted kernel to boot, an initramfs to unpack.
- Memory: twice as much.
- Only what was lifted runs in the page. A program the training run did not meet stops the machine; tabc runs any
  arm64 file it is given.
- A build: minutes of lifting and compiling for the kernel once, and for each new program file. That is cacheable
  by sha256 and the same for everyone; tabc needs none.

## What it would take to close the gaps

In the order of what they would buy:

1. **The machine's cost on user code.** An inline cache for indirect calls (the PLT's `jmp *GOT` resolves to the same
   function every time), a larger or set-associative user TLB, and stack accesses that check the TLB once per
   function instead of per `push`. Polls could be cheaper too: a counter in a register, not in memory. The target is
   the prototype's 1.3 to 2.2 times the Linux runtime, which would put compute at 2 to 3 times tabc's arm64.
2. **Snapshots** (K14, next on linux.md): boot once at build time and ship the machine's memory after `/init` has
   started. That removes the kernel's boot and the initramfs's unpacking from every cold start (2 s in the page).
3. **Fewer bytes and less compiling**: serve the modules compressed (49.5 MB to 4.9 MB brotli for the kernel's);
   ship `vmlinux`'s loadable segments only; use `WebAssembly.compileStreaming` from the HTTP cache, so that Chrome
   caches the compiled code (a second container starts from it). Read the image's files out of its layer on demand
   (a virtio disk over the layer, as tabc does) instead of an initramfs.
4. **Memory**: guest RAM that grows as it is used (a memory that starts small and grows, or pages handed out
   lazily), and not keeping the fetched bytes of modules once they are compiled.
5. **fork and exec** follow from 1 and 2: they are the kernel's code, running through the same MMU.
6. **Programs met for the first time in the page** need the lifter in the page (step 2 of
   [wamr-unkernel-tabc.md](wamr-unkernel-tabc.md#could-pisco-run-any-docker-image)), or an interpreter as a fallback.

The side module's PIC code is not where the time goes (0 to 16%), and neither is V8's tiering.

## Reproduce

```sh
tools/tabc-poc/build.sh                      # INKERNEL=1 also builds recipes/alpine-k
# tabc 1.0.0's release on PATH, TABC_CHROMIUM=$(command -v google-chrome); TABC_VOL with bench-aarch64, workload,
# qemu-x86_64 (Alpine's apk), prelude.sh and x86root/ (the amd64 layer, with bench and workload in usr/local/bin)
for s in native pisco tabc-arm64 tabc-x86; do node tools/tabc-poc/bench.mjs $s 5; done
node tools/tabc-poc/bench.mjs pisco-k 5 bench
node tools/tabc-poc/bench.mjs report
```

`TABC_VOL` is put together by hand: `tools/tabc-poc/tabc-x86-prelude.sh` as `prelude.sh`, `workload`,
`bench-aarch64`, `qemu-x86_64` out of Alpine's `qemu-x86_64-11.0.3-r0.apk` (aarch64, community), and `x86root/`, the
amd64 layer unpacked with `bench` (x86-64) and `workload` in `usr/local/bin`.
