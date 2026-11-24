# series-web (working name)

Runs a 64-bit Windows title's x86-64 code in WebAssembly — in a browser tab or in Node — by recompiling
it ahead of time and supplying the operating system underneath it. The target is the Xbox Series build of
Minecraft; until those files are here it is tested on titles we build ourselves and on a real program
(Stockfish 19's official Windows build).

Licence: GPL-2.0-only (so code from [XCL](https://github.com/ivall/xcl) can be used).

Docs: [the journey so far](docs/journey.md) (what was built, measured and learned, roadblocks) ·
[status and next steps](docs/status.md) · [first run of a real title](docs/first-run.md) · [wasm32 or wasm64](docs/memory.md)

## How it works

```
title.exe ── lifter (native, once per build) ──► C, one function per guest function ──► clang ──► title.wasm
                                                                                                   │ imports
runtime (rt/, C on wasi-libc, linked into the same module) ◄───────────────────────────────────────┘
  process.c   image loading, import binding, guest calls, handles and waits, threads, TEB / TLS
  kernel32.c  threads, sync objects (events, semaphores, mutexes, critical sections, SRW, condition variables),
              memory, time, modules, code pages, standard handles
  msvcrt.c    C runtime: startup / exit, stdio, printf with Win64 varargs, strings, locale "C"
  ucrt.c      the UCRT (api-ms-win-crt-*), falling back to msvcrt.c where the semantics match
  seh.c       x64 exception handling over the images' .pdata / .xdata: RaiseException, RtlUnwindEx, vectored handlers
  vfs.c       the title's file system: console drives, the title's folder read-only, an in-memory overlay
  files.c     kernel32 / ntdll file functions over vfs.c
  gdk.c       the console's libraries, offline: kernelx, XMem, XGameRuntime (task queues, XAsync, XUser, saves,
              PLS, package), the console screen (user32 / DWM), GameInput's object
Imports nothing implements become stubs that log their first call and return 0 (E_NOTIMPL for the console's
HRESULT APIs), as in XCL; SW_STRICT=1 stops at them instead.
host (web/)   WASI calls, one worker per guest thread over one shared memory
```

- **Images**: the exe and every DLL it imports that sits beside it are lifted together, each rebased below
  4 GiB; imports between them are bound at lift time. The runtime reads their bytes from the title's folder.
- **Lifter** (`crates/lifter`): functions from the exception table, call targets, code pointers in data,
  jump tables, and landing pads from the language-specific exception data (GCC/LLVM LSDA, C scope tables).
  It reports what it could not lift. AVX-and-later functions are left out: the virtual CPU's CPUID reports
  SSE4.2 and POPCNT only, so titles pick their SSE paths.
- **Registers** live in a `Cpu` struct passed as `restrict`, so clang keeps them in wasm locals between calls.
- **Guest memory** is linear memory: a guest address is a wasm address. Images are rebased below 4 GiB.
  wasm32 by default; see [docs/memory.md](docs/memory.md) for wasm64.
- **Threads**: a guest thread is a pthread of the runtime, i.e. a worker. In the browser they come from a
  pool the page starts ahead of time (a worker cannot start one while it blocks).
- **Exceptions**: a lifted function with landing pads registers a `setjmp` landing; restoring a context
  longjmps to the frame's landing and jumps to the pad's label.

## Try it

Needs Rust, Node 24, [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 34 in `~/tools`, and for the C++
test [llvm-mingw](https://github.com/mstorsjo/llvm-mingw).

```sh
cargo build --release -p lifter
(cd fixtures/hello && cargo build --release)                       # our own Windows-format test titles
tools/build-title.sh fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe out/hello
node web/run-node.mjs out/hello/title.wasm fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe
node tools/run-chrome.mjs out/hello/title.wasm <the .exe>          # the same in headless Chrome
```

`SW_TRACE=1` logs missing imports and exception dispatch; `SW_TRACE=2` every import call.

## State (2026-11-24)

[![graphics ladder](https://yhozen.github.io/series-web/badge-ladder.svg)](https://yhozen.github.io/series-web/progress.html) [![D3D12 methods](https://yhozen.github.io/series-web/badge-d3d12.svg)](https://yhozen.github.io/series-web/progress.html) [![imports](https://yhozen.github.io/series-web/badge-imports.svg)](https://yhozen.github.io/series-web/progress.html) [![XCL checks](https://yhozen.github.io/series-web/badge-xcl.svg)](https://yhozen.github.io/series-web/progress.html)

[![progress map](https://yhozen.github.io/series-web/progress.svg)](https://yhozen.github.io/series-web/progress.html)

Video: [Doom as an Xbox console title in the browser](https://yhozen.github.io/series-web/) (D3D12.X on WebGPU, XAudio2, GameInput, saves in OPFS).
The map is drawn by `tools/progress.mjs` (`--refresh` re-reads the build outputs into `docs/progress-data.json`)
and published by `.github/workflows/progress.yml`.

| Title | What it checks | Node | Chrome |
|---|---|---|---|
| `fixtures/hello` (Rust, no_std) | recursion, switch, pointers, SSE loop, f64 | 7/7 | 7/7 |
| `fixtures/threads` (Rust, no_std) | 4 threads, `lock xadd`, critical sections, events, semaphores, TLS, ExitThread, timeouts | 9/9 | 9/9 |
| `fixtures/cxx` (C++, llvm-mingw, libc++ + libunwind) | throw through frames with destructors, `catch (...)`, `std::exception`, rethrow, throw in catch, in a thread | 7/7 | 7/7 |
| `fixtures/dll` (C++, llvm-mingw: an exe and the DLL it ships) | static and dynamic imports, DllMain on process and thread attach, `thread_local` in the DLL, an exception crossing from the DLL | 7/7 | – |
| **XCL's console test title** (`tests/testtitle` + its game DLL, Rust std, built for `x86_64-pc-windows-gnullvm`) | XCL's own checks of a console title: TLS, panics, kernelx, XMem, the console screen, GameInput, modules, game DLL, the VFS (console drives, overlay, deletes, listings, T:), XGameRuntime, XTaskQueue, XUser, saves, PLS | **28/50**: every CPU-side check but the 3 that need memory above 4 GiB (XCL's layout); the 19 D3D12.X / DirectStorage / MF / XAudio2 checks are not started | 28/50 |
| **Stockfish 19, official Windows x64 build** (MinGW, 103 MB) | `bench`: 2.9 M instructions lifted, worker threads, NNUE (109 MB) in shared memory | **2,497,913 nodes = the published signature**, 105,700 nodes/s | **2,497,913 nodes**, 110,821 nodes/s, 22.5 s |
| **Doom as a console title** (`fixtures/doom`: doomgeneric + Freedoom on D3D12.X, GameInput, XAudio2, XGameSave; llvm-mingw) | D3D12.X → WebGPU, `PresentX` and the frame clock, sound, keyboard / gamepad, saves that survive a reload | `-timedemo` frames equal native on every deterministic frame (null GPU back end) | frames **read back from WebGPU** equal native's; ~35 fps on SwiftShader; video: `fixtures/doom/video.sh` |

Stockfish's `bench` is deterministic and its node count is recorded in the release commit (`Bench: 2497913`), so
matching it checks every lifted instruction it ran.

No game files, lifted output or dumps belong in this repository (`out/` is ignored).
