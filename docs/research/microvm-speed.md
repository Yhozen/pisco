---
date: 2026-11-28
measured:
  pisco: f5ffd7c3  # main; the Alpine image, its harness and the call cache below are not in this pull request
  alpine: Docker Hub alpine:3.24.4, linux/amd64, pulled 2026-11-27
  chrome: 158.0.8127.88
  node: 24.22.2 (the test VM), 24.22.1 (the sandboxes)
  unkernel: https://github.com/pithings/unkernel bc6e5ba6a06f6fd729a240d8774ec84c8e8dca66 (source read); kernel from npm unkernel@0.2.0-261127-3b71fa5, Linux 7.3.4+, kw_emu.h ABI 4
  wasi-gfx: https://github.com/wasi-gfx/wasi-gfx 24615bbdf06f5a55ea6506db4596ed24072d3a6e (v0.2.0), wasi:webgpu https://github.com/WebAssembly/wasi-webgpu 6a776bada0b66d3dbf9da304a49ff2947ce4e1f8
  steamac: https://github.com/fxgl/steamac 38128287531471ad1f166dbb0487b3a43b3b4a7d (v1.9.15)
status: research only; two experiments, none of their code in the repository
---

# The lifted Linux's speed: where the time goes, and the routes to native

The Microvm x64 runtime runs a real Linux, lifted, and its programs, lifted ([linux.md](../linux.md),
[kernel-as-a-title.md](kernel-as-a-title.md)). This page asks whether that road can be fast enough, and how PISCO
keeps "run anything, anywhere" while using native speed when a host has it. It reports one measurement of where the
machine stands, two experiments and a reading of three outside projects. Re-check the numbers on the versions in
the front matter before relying on them.

## Short answer

- **The microvm is not a dead end, but its kernel is the slow part in the browser.** Lifted compute is 2 to 3
  times the same lifter's code on flat memory. Processes, cold start and memory are where the lifted kernel loses:
  a fork costs about 9 ms in Chrome, against 0.12 ms (in Node) for the same lifted program under a kernel compiled to wasm.
- **The machine's runtime is a small part of the cost.** About 75% of a busybox workload's time is in the lifted
  code of busybox and musl. One runtime change is worth keeping: a cache in front of `rt_call` (awk 11% faster in
  Chrome).
- **What is left is the TLB check on every load and store**: 55 to 90% of the overhead over flat memory. Polls are
  27% of call-heavy code. Both are fixed in the lifter, not the runtime, and the fix applies under any software
  MMU.
- **A lifted x86-64 program runs as a task of unkernel's kernel, unchanged.** A 46-line patch of the kernel's wasm
  makes it ask PISCO first. That one change took forks from 9 ms to 0.12 ms, and lifted compute ran 1.5 to 5 times
  faster than unkernel's own aarch64 emulator on 4 of 5 workloads (1.2 times slower on fib).
- **Native speed comes from a different place for each kind of title.** On a host with hardware virtualisation, run
  the image's own x86-64 kernel and files (KVM, or an arm64 VM with FEX on Apple silicon). Windows and console
  titles get wasmtime with the GPU ring run by wgpu. The browser keeps lifted code, and the lifted wasm stays in
  every image as the reference that runs everywhere.
- **Keep the GPU ring as the one GPU contract**; wasi:webgpu does not fit PISCO's modules today.

## 1. Where the lifted Linux stands

Docker Hub's `alpine:3.24.4` (amd64) was turned into an initramfs and booted on the lifted Linux in a tab. Its
programs were lifted ahead of time and carried by the image as side modules. Busybox-sh workloads were timed in the
guest by `/usr/bin/time -f %e`. A compiled benchmark ran too: `fixtures/bench`'s five workloads in C, built
by one compiler for every target, with the same checksums everywhere. The test VM (4 cores, x86-64), Chrome 158,
median of 5:

| busybox `time`, s | native (a `chroot` on the VM) | PISCO, in Chrome |
|---|---|---|
| `echo hi`, to the first output (cold start) | 0.02 | 4.22 |
| `awk` | 0.09 | 2.50 |
| `sha256sum`, 4 MiB | 0.02 | 0.24 |
| `gzip` | 0.18 | 2.24 |
| the compiled benchmark, all five | 0.77 | 4.66 |
| 50 forks, `(:)` | 0.01 | 0.43 |
| 50 fork + exec, `/bin/true` | 0.02 | 0.78 |

| the compiled benchmark, ms | native | PISCO |
|---|---|---|
| calls, fib(32) | 8 | 247 |
| bytes, FNV of 64 MB | 140 | 787 |
| sort, heapsort of 2M u32 | 387 | 2,454 |
| float, 4x4 transform | 37 | 294 |
| memory, 20M random loads | 188 | 598 |

- **Cold start**: the image is fetched and its four modules compiled by 1.65 s; then 2.2 s to the first `sh` (the
  worker, the kernel's boot, unpacking the 8.3 MB initramfs). In Node the same boot is 2.0 s.
- **Memory**: an idle tab is 1,041 MiB.
- **Download**: 91.8 MB (17.7 MB gzipped), most of it the lifted kernel (49.5 MB).
- **One-time costs**: lifting the kernel took 72 s and the programs 65 s, once per image, not per visit.
- **Side modules are cheap**: the position-independent code of a side module costs 0 to 16% against the same code
  in the title's module.

Compiled code is 3 to 30 times native, processes about 40 times. Section 2 finds where compute's time goes;
processes, cold start and memory are the lifted kernel's.

## 2. Experiment 1: what the machine costs lifted user code

Scope: lifted **user** code on the Microvm x64 runtime (busybox, musl's loader, the benchmark), not the kernel.
Measured on an 8-vCPU sandbox about 3 times faster than the test VM, so only ratios carry over. Every A/B was run
interleaved, five rounds, in Chrome and in Node.

**The profile.** `workload awk gzip` in Node, self time, boot subtracted:
- lifted busybox and musl: 75%;
- `rt_call` (indirect calls and jumps): 6%;
- TLB misses (`mmu_walk`, `access`, `phys_*`): 4 to 5%;
- the rest of the runtime: under 3%.

**Three changes, measured in turn:**

| Change | Result | Kept |
|---|---|---|
| A 4,096-slot cache of indirect-call targets in front of `rt_call`. A user address's entry holds for one generation of the address space. A CR0, CR3 or CR4 write, an `invlpg` of a user page, or a program registered starts a new one. Kernel entries hold for ever | awk 0.81 → 0.72 s in Chrome (−11%), −15% in Node; `rt_call` 153 → 53 ms in the profile | yes, not in this pull request |
| TLB entries found with a shift instead of a multiply by 24 (three arrays, or 32-byte entries) | `memory` 3 to 10% faster, but `gzip`, `bytes` and `sort` about 10% slower in Chrome (Node level or better) | no |
| No entry poll in leaf functions | no gain; it moves the virtual clock | no |

**Where the overhead over flat memory goes.** The compiled benchmark in Node, in ms. "Flat" is the same binary
lifted for the Linux x64 runtime: no MMU and no polls. The diagnostic columns take parts out of the lifted C. They
are not correct machines; they measure what a part costs.

| | native | flat | machine | no polls | no polls, no resume or return check |
|---|---|---|---|---|---|
| calls | 3 | 13 | 37 | 27 | 26 |
| bytes | 64 | 70 | 202 | 182 | 166 |
| sort | 168 | 239 | 841 | 763 | 716 |
| float | 10 | 28 | 89 | 77 | 77 |
| memory | 96 | 97 | 208 | 197 | 198 |

- Polls and the checks at function entry and after calls are 46% of the overhead on `calls`, 20 to 27% on `bytes`,
  `sort` and `float`, 9% on `memory`.
- **The other 54 to 91% is the inline TLB check on every access**, together with the guest registers stored to
  `Cpu` before each access because its slow path may run the kernel. Each access is about 16 wasm operations before
  the load.
- Where a poll's counter lives made no difference. The number of polls is what costs.

**What would win it back, in the lifter:**
1. **Fewer TLB checks.** Translate RSP's page once for a run of pushes, pops and `[rsp+k]`, re-checking when RSP
   leaves the page. Check nearby accesses through the same base register once when they stay in one page. Only a
   slow path, a call or a poll can change the TLB between two accesses, and the lifter sees all three.
2. **Registers in locals**: the slow path stores the registers itself, so LLVM can keep them in wasm locals across
   hot loops. Larger, and worth less than item 1.
3. **Fewer polls**: none at the entry of a leaf that every caller polls around, or one every n-th back edge with an
   exact count. Each moves the virtual clock, so the recorded traces would be checked again.

**Correctness.** The five checksums, the output of every workload and `tests/run.sh microvm` were the same for
every variant. Chrome was 1.3 to 1.6 times slower than Node on the same image, which is as large as anything above.
The next step there is a Performance trace of the page's worker.

## 3. Experiment 2: a lifted x86-64 program as a task of unkernel's kernel

unkernel builds Linux for wasm (`arch/wasm`) and runs user code with an aarch64 emulator, kwemu, linked into the
kernel's module behind one call: `kw_run(cpu, regs)` runs user code until a system call, fault, budget or
interrupt (`include/kw_emu.h`). The experiment put PISCO behind that call, without rebuilding the kernel.

**How it plugs in:**
- **The patch**: binaryen moves `kw_run`'s body to `kw_run_a64` and gives `kw_run` a new one that runs PISCO's lifted
  code when the task is x86-64. The three TLB-flush functions also tell PISCO. The release exports every symbol
  needed (`kw_run`, `kw_user_addr`, `kw_flush_tlb_*`, `kw_set_pgd`). kwemu runs at the same speed after the patch.
- **Memory**: the kernel and its processes share one linear memory. PISCO's runtime and the lifted modules are
  instantiated above the kernel's RAM after boot. Every call between them is wasm-to-wasm, which matters because
  `kw_user_addr` may sleep and a JSPI suspension cannot cross a JS frame.
- **The task's state**: the x86-64 registers live in `kw_regs` between runs, so fork copies them and exec clears
  them. A tag marks the task as x86-64.
- **Events**: system calls are translated to arm64's numbers and layouts (about 150; `struct stat`, clone's
  argument order, `O_*` bits). A TLB miss calls the kernel's `kw_user_addr`, which walks the tables and runs
  `handle_mm_fault`. Polls charge the kernel's budget and see its interrupts. Signals are delivered x86-64's way and
  return through `rt_sigreturn`.
- **Starting a program**: a 61-line aarch64 loader maps the ELF and builds an x86-64 stack. Registered with
  binfmt_misc, it makes x86-64 files executable directly.

**Results**, same sandbox, Node, 5 boots per side, median, ms:

| | native x86-64 | kwemu (aarch64 build) | lifted x86-64 on unkernel | lifted, flat memory |
|---|---|---|---|---|
| calls, fib(32) | 3 | 25 | 30 (1.2× slower than kwemu) | 14 |
| bytes, FNV 64 MB | 69 | 210 | 107 (2.0× faster) | 75 |
| sort, heapsort 2M | 230 | 900 | 611 (1.5× faster) | 473 |
| float, 4x4 transform | 14 | 325 | 66 (4.9× faster) | 31 |
| memory, 20M loads | 96 | 417 | 179 (2.3× faster) | 102 |

| 500 of each, per process | kwemu, aarch64 busybox sh | lifted x86-64 busybox sh |
|---|---|---|
| fork | 0.14 ms | 0.12 ms |
| fork + exec of an aarch64 `/bin/true` | 0.24 ms | 0.24 ms |
| fork + exec of an x86-64 busybox `true` | | 0.88 ms (the loader reads the whole file) |

- The lifted code runs 1.3 to 2.1 times slower than on flat memory: the software TLB, as experiment 1 found.
- fib is the one workload where it loses to kwemu: calls, pushes and pops through the TLB, which is experiment 1's
  item 1.
- Boot is 140 to 190 ms in Node.

**What did not work.** `$((6*7))` in busybox sh stops at `0x47cfc8`, a `jmp reg` into `arith_apply`'s jump table.
It is one of 22 jump tables the lifter does not recover in that busybox (1.36.1, built by Ubuntu's gcc 13). PISCO's
own machine stops there too (`tests/run.sh kernel` fails at the shell's first fork on a gcc 13 build, at baseline).
This is a lifter gap, and every route needs it fixed.

**What a full integration would take** (about 6 to 8 weeks):

| | Work | Estimate |
|---|---|---|
| 1 | Program identity (several static programs at 0x400000, `run.c`'s page hash), lifting on first run, then position-independent and dynamically linked programs | 1 to 1.5 weeks |
| 2 | A C stack per x86-64 task, for faults that sleep | 3 days |
| 3 | The rest of x86-64's user ABI: in PISCO's runtime (signal frames, the rest of the system calls), or a second system call table in the kernel | 1 to 2 weeks, or 3 to 4 in the kernel |
| 4 | A TLB tagged by address space (now flushed on every switch); one per CPU for SMP | 2 days; SMP 1 to 2 weeks |
| 5 | The same in Chrome, with cold start and memory measured | 3 to 5 days |
| 6 | A stable hook instead of the binary patch: a second CPU behind `kw_run`, proposed upstream | a conversation, then about a week |
| 7 | The lifter: the jump tables above and experiment 1's items | as experiment 1 |

Licences: the patched module is a modified GPL-2.0 kernel, built from a release at run time. None of unkernel's code
would be in the repository.

## 4. Native paths

**The principle**: the wasm realisation is the reference and is always in the image. A native realisation is an
acceleration a host may choose, checked against the reference's outputs, not its traces.

**Linux images on a host with hardware virtualisation.** The microvm image already carries the originals: its
`vmlinux` and the distribution's x86-64 files. On x86-64 Linux with `/dev/kvm`, the same kernel and files can run
under a VMM (Firecracker, or QEMU with KVM), near the native column of §1 instead of 6 to 40 times slower. On Apple silicon,
an x86-64 kernel cannot run under Hypervisor.framework: the route is an arm64 kernel with FEX for x86-64 user code.
PR #43 measured FEX in a libkrun guest 1.1 to 3 times faster than the lift. What it costs: a VMM per host OS, a
capability probe and a policy in `pisco run`, and determinism (a KVM run has no virtual clock). Rosetta is no
foundation: Apple's notice of 2026-09-01 makes macOS 27 its last release, except for older games.

**Windows and console titles.** Their originals need Windows, which PISCO imitates, so KVM gains them nothing. Their
native path is the wasm module compiled ahead of time by wasmtime, with the GPU ring executed by wgpu on the host's
own API ([hosts.md](../hosts.md)). A first step with no port: `web/gpu/backend.js` unchanged in Node on dawn.node,
for real-GPU frame times and headless pixel checks.

**wasi-gfx and wasi:webgpu.** WIT packages for the Component Model: a surface, a frame buffer, and wasi:webgpu
(Phase 2, one interface that mirrors WebGPU on WASI P3 async). A wasmtime runtime and a browser shim exist; no
browser implements it natively, and wasi-gfx has no licence file. **Not a fit today:**
- components only, with cooperative threads until shared-everything-threads lands, and the ring needs the title
  writing while another thread executes;
- copies per call (`write-buffer-with-copy`), on the upload paths that dominated Tag and Desire;
- WebGPU's feature ceiling (no binding arrays, 64-bit integers or mesh shaders), the gaps hosts.md gives for a
  native host.

Re-check when shared-everything-threads ships in wasmtime and Chrome.

**steamac**, which runs ARM64 SteamOS in a libkrun VM on Apple silicon, with DirectX through DXVK or vkd3d-proton,
Mesa's Venus and virglrenderer on MoltenVK or KosmicKrisp, confirms the ring's design. Its experimental `wmtr`
protocol is close to `rt/gpu_ring.h`:
- a request ring in shared memory, references as (blob, offset), handles the client assigns;
- a reply slot per thread for synchronous calls;
- an executor that spins before it sleeps.

Three things to borrow:
- **A higher-level stream gets closer to the native API.** Their Direct3D 11-level path beat their Vulkan-level one
  2.8 times on one title (their figure). PISCO's ring is at Direct3D 12's level, so a native executor can target the
  most direct API a host has.
- **Pick the back end at run time**, with an override, and log the choice.
- **Keep a validating layer** in a native executor (wgpu-core), since the stream is the title's.

## 5. What to do

1. **Browser, the kernel**: move Linux images to a kernel compiled to wasm with PISCO's lifted user code, after the
   Chrome numbers of experiment 2's item 5. Stop tuning the lifted kernel's own speed; it stays as the reference
   path and the oracle's twin.
2. **Lifter, first**: recover the jump tables (`0x47cfc8`), then fewer TLB checks for stack and same-base accesses.
   Both count under any kernel.
3. **Runtime**: land the `rt_call` cache.
4. **Native hosts**: a KVM column in the benchmark (the image's own `vmlinux` under Firecracker or QEMU with KVM),
   then a host may run an image's kernel on hardware of its architecture, written in
   [spec/platforms/microvm-x64.md](../spec/platforms/microvm-x64.md).
5. **GPU**: keep the ring; dawn.node first, wgpu when a title needs it.
