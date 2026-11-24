# The journey so far (2026-11-24)

A log of how this project came about, what was built, what we measured, what got in the way, and what we
learned. For the current state in one page see [status.md](status.md); for the first run on a real title see
[first-run.md](first-run.md).

## 1. The question

> How hard would it be to run the Xbox version of Minecraft on the web, or locally on macOS and Linux, using
> [XCL](https://github.com/ivall/xcl)?

**What XCL is** (four days old at the time, GPL-2.0-only, Rust): a Windows-only compatibility layer. It loads an
Xbox Series (GDK) game's x86-64 executable and runs that code *directly on the PC's CPU*. It replaces only
the console's own libraries (`kernelx`, XGameRuntime, `d3d12_xs`, …), forwards everything else to real Windows
and real D3D12, and recompiles the console's RDNA2 GPU code to HLSL → DXIL. Minecraft Preview (Bedrock, the
Xbox Series build) is listed as fully playable, offline and in trial mode.

**What we found about the obvious routes:**

| Route | State found in XCL's tracker | Verdict at the time |
|---|---|---|
| Linux: `xcl.exe` under Wine + vkd3d-proton | Runs since XCL PRs #92/#93; needs a **patched vkd3d-proton** (XCL swaps vtables of D3D12 objects, vkd3d-proton asserts on vtable identity, #175/#241) and a Wine with `userfaultfd` write watches. A heavy retail title reaches 30 fps; nobody had shown Minecraft. | moderate, mostly others' work |
| macOS: CrossOver + Apple's D3DMetal + Rosetta 2 | A fork (#258) reaches a retail title's menus on an M5 Max, with many workarounds (no `OpenExistingHeapFromAddress`, no FP64, 32-lane waves, instructions Rosetta lacks). Apple: **macOS 27 is the last release with general Rosetta.** | hard, with an expiry date |
| A browser tab | XCL needs a host x86-64 CPU, Windows itself, and D3D12. A browser has none of the three. | first judged "not realistic" |
| Bedrock without XCL | The Windows GDK build of Bedrock already runs on Linux under Proton with WineGDK (BedrockOnLinux 2.0, with sign-in and Realms); Xbox Cloud Gaming has a Minecraft page | a shortcut if the goal is only "Bedrock on Linux" |

## 2. The turn: build the operating system in WebAssembly

The answer "not realistic" was challenged: *you can implement the OS kernel on wasm.* That reframed the
problem into three parts we would build ourselves, each with a precedent:

1. **A CPU** — not an emulator but a **static recompiler** (the "lifter"): x86-64 → C → WebAssembly, ahead of
   time. Precedents: UEWASM (Unreal Engine games to wasm), the PS2 web recompilers. Dynamic emulators were
   ruled out by their own numbers (the fastest 64-bit one reports ~2 fps in 3DMark06).
2. **An operating system** — the Windows and console APIs the title calls, implemented in a runtime linked
   into the same wasm module. Precedent: VIctory's open PR #526 runs a title on native Linux with its own
   kernel32/ntdll/UCRT.
3. **A GPU API** — D3D12.X → WebGPU, with the console's shaders translated to WGSL. This is the risky part
   (WebGPU has no mesh shaders, no bindless heaps, no 64-bit integers, no shared CPU/GPU memory).

We also looked at [VIctory](https://github.com/Marvisak/VIctory) (GPL-3.0, one title, not Minecraft). It has
good ideas (its own D3D12.X objects instead of patching the host's, copying title memory to the GPU instead
of sharing it), but GPL-3.0 code cannot be mixed with XCL's GPL-2.0-only code. Once it was settled that the
new project may use GPL-2.0 code, **the project became GPL-2.0-only and XCL is its code donor**; VIctory
stays a source of ideas.

Both repositories are cloned (with XCL's macOS fork branches and its Linux PR) under the workspace's
`.context/repos/`.

## 3. What was built, step by step

### 3.1 The first slice: a Windows program running in Node
- Test title: Rust `no_std`, linked by `lld-link` into a real Windows PE with kernel32 imports.
- Lifter (`crates/lifter`, Rust, iced-x86 decoder): functions from `.pdata`, call targets, code pointers in
  data; one C function per guest function; registers and flags as C variables; `call`/`ret` push and pop the
  guest stack and use the wasm call stack for control.
- **7/7 checks** on the first evening. Bugs on the way: 8-bit registers numbered wrong (`spl` vs `ah`),
  functions only reachable through pointers in data, `__multi3` (128-bit multiply) missing in wasm.

### 3.2 wasm32 or wasm64 (measured, `docs/memory.md`)
- clang 15 crashed compiling SIMD for wasm64 → switched to **wasi-sdk 34** (clang 23).
- On a CPU benchmark (same source compiled natively for comparison): wasm64 costs **0–45 %** over wasm32;
  lifted code is **1.3–6×** slower than native on these workloads.
- Placing all guest memory above 5 GiB in wasm64 worked with **157 MB resident**: untouched address space
  costs no RAM in V8. But Safari only has memory64 behind a flag.
- Decision: **wasm32 by default**, switch per title. Images based at `0x140000000` are rebased below 4 GiB
  using their relocations.

### 3.3 A real runtime
- Rewritten on **wasi-libc with threads**: malloc, strings and formatting come from libc; in wasm32 a guest
  pointer *is* a runtime pointer, so guest `malloc` is the runtime's `malloc`.
- Windows process model: per-thread TEB (`gs` base), implicit TLS and TLS callbacks, handles, events,
  semaphores, mutexes, critical sections, SRW locks, condition variables, `WaitOnAddress`, one-time init.
- **One guest thread = one worker** over one shared memory. Node: the main thread only supervises, so an exit
  from any thread ends the process even while others block. Browser: see roadblock R3.
- Threads title: **9/9** in Node and in headless Chrome.

### 3.4 A real program: Stockfish 19
Chosen because it is a real, demanding 64-bit Windows program (the official MinGW build, 103 MB with its
neural net embedded), uses threads, a C++ runtime, file mappings, and has **a deterministic oracle**: its
`bench` node count is published in the release commit (`Bench: 2497913`).
- Census: 15,600 functions, **3.09 M instructions**, 181 imports. The "universal" binary carries ~8 copies of
  the engine for different CPUs; functions using AVX or later are left out, and the virtual CPU's CPUID
  reports SSE4.2 + POPCNT, so the engine picks its SSE path.
- Lifted: atomics (`lock` RMW, `xchg`, `cmpxchg` → wasm atomics), string instructions, bit operations,
  x87 (as doubles), CPUID, ~60 more SSE instructions.
- Image data is read from the user's `.exe` at start instead of being embedded in the generated C.
- First result: correct but slow (~3,000 nodes/s). A tick profile showed **90 % of the time in two NNUE
  functions** whose SSE helpers were scalar lane loops. Rewriting them on wasm SIMD (`pmaddwd` is
  `i32x4.dot_i16x8_s`, `pmaddubsw` is widening multiplies + a saturating pairwise add, …) and compiling at
  `-O2`: **105,700 nodes/s in Node, 110,800 in Chrome** (35× faster).
- **Result: 2,497,913 nodes — exactly the published signature**, in Node and in Chrome. Every instruction it
  ran was lifted correctly.

### 3.5 Exceptions (`rt/seh.c`)
- Windows x64 SEH over the image's own unwind tables: `RtlLookupFunctionEntry`, `RtlVirtualUnwind` (all
  unwind codes), `RaiseException` dispatch, `RtlUnwindEx`, `RtlCaptureContext`, vectored handlers,
  `__C_specific_handler`.
- Resuming: a lifted function with landing pads registers a `setjmp` landing tagged with its entry stack
  pointer; restoring a context finds that frame, loads the registers and `longjmp`s to the pad's label.
- The lifter finds landing pads in GCC/LLVM LSDAs and C scope tables.
- C++ title (llvm-mingw, libc++ + libunwind): **7/7** in Node and Chrome — throws through frames with
  destructors, `catch (...)`, `std::exception`, rethrow, throw inside a catch block, in a thread.

### 3.6 Several images: a title and the DLLs it ships
- The lifter loads the exe and every DLL beside it that it imports, rebases each into its own slot, binds
  imports between them at lift time, and emits a module table. The runtime reads each module from the
  title's folder by name, applies relocations per module, gives each its TLS index, runs `DllMain` on process
  and thread attach, and `LoadLibrary` / `GetProcAddress` see real modules.
- DLL title: **7/7** (static and dynamic imports, `DllMain`, `thread_local` in the DLL, an exception crossing
  from the DLL into the exe).

### 3.7 The console layer, checked against XCL's own console title
XCL's test title (a fake console executable importing kernelx, XGameRuntime, XMem, …) was built for
`x86_64-pc-windows-gnullvm` with llvm-mingw, together with its game DLL and llvm-mingw's `libunwind.dll`, and
lifted as one title.
- New modules: **VFS** (`rt/vfs.c`: console drives `G:` `GAME:` `D:` → the title's folder read-only, `S:`
  persistent local storage, `T:` temp; writes copy the file into an in-memory overlay; deletes leave
  whiteouts; listings merge), Win32 and ntdll **file APIs** over it, CRT streams over it, and **`rt/gdk.c`**:
  kernelx, XMem physical pages, XGameRuntime (task queues in all modes, XAsync, offline XUser, saves, PLS,
  package identity from `MicrosoftGame.config`, networking hints), the console screen (user32/DWM), a
  GameInput object whose COM methods are runtime functions.
- Imports nothing implements become **logging stubs** (E_NOTIMPL for the console's HRESULT APIs), as in XCL.
- Result: **28 of 50 checks in Node and in Chrome** — every CPU-side check except 3 that assume XCL's own
  above-4-GiB memory layout. The other 19 are D3D12.X, DirectStorage, Media Foundation and XAudio2: not
  started.

### 3.8 Ready for Minecraft
- `crates/shaders`: XCL's RDNA2 decoder (with its generated tables), its console shader container parser, and
  a `shader-census` tool that finds console shaders in any file of a title's folder. 19/19 tests, including
  XCL's LLVM corpus check of the decoder.
- `tools/census.sh`: one command for a title's lifter census, import status by DLL, and shader census.
- `docs/first-run.md`: the runbook for the day the files are here.

### 3.9 Graphics, sound, input, saves: Doom as a console title (2026-11-24)
The ladder agreed for graphics is SimpleTriangle → SimplePBR → Doom → XCL's graphics checks → Minecraft
(`.context/series-web/` in Horizon holds the plan and the research). This session went straight to the third
rung, Doom, because it needs the whole shared core anyway; SimpleTriangle and SimplePBR are still to do.
- **The title** (`fixtures/doom`): doomgeneric (pinned) and Freedoom, with a platform file written against the
  console's interfaces: `D3D12XboxCreateDevice`, display back buffers, the frame clock
  (`SetFrameIntervalX` / `ScheduleFrameEventX` / `WaitFrameEventX`), `PresentX`, GameInput (keyboard and
  gamepad), XAudio2 (Doom's sound effects as source voices) and XGameSave (its folder holds the config and
  saves). Our own header (`xbox.h`) declares them, with the console's slot order from XCL's findings. Built
  with llvm-mingw; the shader is DXIL from the public `dxc`, its root signature inside the shader.
- **D3D12 / D3D12.X front end** (`rt/d3d12.c`): COM objects with vtables generated from the PC layout
  (`rt/d3d12_layout.gen.h`) and the console's differences; command lists recorded as words; one ordered ring
  (`rt/d3d12_cmd.h`) of resources, root signatures, pipeline states, submissions, fences and presents.
  Descriptors are 32-byte records in guest memory; render-target and clear descriptors are copied into the
  stream when recorded, descriptor tables are read when the GPU runs them.
- **Back ends**: `web/gpu/null.js` in Node (completes fences and presents, checks every shader has a
  translation, writes a trace); `web/gpu/backend.js` in the page (WebGPU: passes opened lazily with pending
  clears, pipelines made at draw time per state, root signatures resolved per binding, CPU-visible buffers
  uploaded from guest memory when a submission uses them, presents drawn into the canvas).
- **Shaders**: `tools/shader-prep.mjs` finds every DXIL container in a title, `tools/dxil-wgsl/dxil2spv`
  (our driver over dxil-spirv's C API: fixed binding scheme by register, vertex inputs by signature row,
  `BaseVertex` dropped) and naga (`--keep-coordinate-space`) make `shaders/<digest>.wgsl` beside it.
  SimpleTriangleDesktop's two shaders go through too (inputs `SV_Position`@0, `COLOR`@1).
- **XAudio2** (`rt/xaudio2.c`): voices mixed in C into a 48 kHz stereo ring in shared memory on wall-clock
  time; the page plays it with an AudioWorklet. **GameInput**: readings from an input block the page fills
  from the keyboard and the Gamepad API. **Saves**: `S:\` is handed to the title from OPFS at start and every
  change goes back (a folder with `SW_SAVES` in Node).
- **Checks**: `-timedemo demo1` with `-hashframes` (time advances only by Doom's own sleeps, so every run
  draws the same frames) against doomgeneric built natively for Linux:
  - in Node, the title's framebuffer matches native on every frame native draws the same way on every run;
  - in Chrome, the frame **read back from WebGPU** equals the title's framebuffer on all 7,155 presented
    frames, and native on all 7,123 stable frames.
  The other 33 frames (the opening melt, and a few single pixels) are frames where Doom draws bytes of its own
  zone headers, which hold pointers: native Doom disagrees with itself on them from run to run (address
  randomisation), and frame 1817 matches once native's memory is placed below 4 GiB like ours.
- **The video** (`fixtures/doom/video.sh`, `out/doom/video/doom-series-web.mp4`): headless Chrome with WebGPU on
  SwiftShader, every frame read back from WebGPU, with the audio track from the mixer ring: title screen and
  attract demo, a new game from scripted keys, the automap, a save, a page reload (a new title process), the
  save loaded back. About 35 frames per second on the CPU-only renderer.

## 4. Roadblocks

### Resolved
| # | Roadblock | What it was | Resolution |
|---|---|---|---|
| R1 | clang 15 crashed on wasm64 SIMD | an LLVM backend bug (`insert_vector_elt` with an i64 index) | wasi-sdk 34 (clang 23) |
| R2 | Embedding a 100 MB image in C | the first lifter wrote the image's bytes into the generated C | the runtime reads sections from the user's own file at start; the lift holds only code |
| R3 | Threads hung in Chrome | a worker cannot start a nested worker while it blocks in `Atomics.wait` (the child needs the parent's event loop) | a pool of workers started by the page ahead of time, claimed through a shared control array (Emscripten's approach) |
| R4 | `switch` statements | jump tables in many shapes: base register loaded far earlier (GCC), bound compared in memory, index copied before the compare, bounded by `and`, targets in `.cold` parts, MSVC's image-relative tables | recovery follows the registers backwards; tables without a bound are rejected; 223 → 363 tables in Stockfish, 715 in XCL's title |
| R5 | Garbage decoded as code | values in data that look like code pointers led to decoding junk, then to links against functions that do not exist | entries that do not decode become trap stubs; direct calls and fall-throughs reference only real entries; the runtime reports a stray jump if one is ever taken |
| R6 | Data imports | `__initenv`, `_fmode`, … are variables, and were bound before the C runtime had allocated them | the C runtime is initialised before imports are bound |
| R7 | libunwind's exception protocol | `RtlUnwindEx` must walk from its caller through the dispatcher's frames back into the throwing frames, and libunwind does a nested ("collided") unwind from inside an unwind handler; the runtime's dispatcher is native code, not a guest frame | handler calls get a sentinel return address and the context the walk should continue from, emulating Windows' dispatcher frames |
| R8 | 35× too slow | scalar loops for SIMD helpers | wasm SIMD intrinsics with the same semantics, `-O2`; `restrict` on the CPU state so registers stay in wasm locals between calls |
| R9 | Rust's standard library on the console title | stack-overflow handler (vectored exceptions), stdout through `ntdll!NtWriteFile`, locks through `WaitOnAddress` | implemented |
| R10 | Half-filled vtables (Doom) | tables of method names divided by 8: a pointer is 4 bytes in wasm32 | `sizeof a / sizeof *a` |
| R11 | Stack arguments one slot early | `ARG(4)` is already at `RSP+0x28`; reading later ones by hand shifted them, `GetCopyableFootprints` returned size 0 and the upload buffers were 1 byte, so frames overwrote the WAD's file record | every argument through `ARG(n)` |
| R12 | A `switch` in `P_GiveAmmo` | the index was copied (`mov r9d, edx`) more than 24 instructions before the jump | the look-back window is 64 instructions (37 tables recovered in Doom) |
| R13 | 33 Doom frames differ from native | Doom draws bytes of its zone headers (pointers) in the opening melt and at a few column ends | shown to be Doom's own nondeterminism: native differs from itself there; not an emulation error |
| R14 | Black frames in Chrome | the back end skipped the first word of each command list, so `SetPipelineState` was lost | fixed; the page can log draw statistics (`&stats=1`) |

### Open
| # | Roadblock | Why it matters | Plan |
|---|---|---|---|
| O1 | **Graphics beyond Doom's** | D3D12 / D3D12.X → WebGPU works for what Doom uses (one textured pass, copies, fences, `PresentX`); vertex/index buffers, constant buffers, depth, many descriptors, compute and DXGI swap chains are written but untested | SimpleTriangleDesktop then SimplePBR (the ladder's steps 1–2), then XCL's 19 graphics checks |
| O2 | **Shader translation** RDNA2 → WGSL | the console ships GPU machine code, not portable shaders | the census tells which stages and instructions Minecraft needs; XCL's HLSL emitter is the reference; mesh/NGG programs may need compute + indirect draws |
| O3 | **WebGPU limits** | no mesh shaders, no bindless heaps (XCL saw Minecraft use eight heaps of 73,727 descriptors), no 64-bit integers, no shared memory | per-draw bindings traced from each shader (VIctory's approach), compute emulation where needed; decided after the shader census |
| O4 | MSVC C++ exceptions | Minecraft is an MSVC title; `__CxxFrameHandler3/4` are not implemented | an MSVC-ABI test title already builds (`fixtures/msvc`); implement if the title throws in normal play |
| O5 | Audio codecs | PCM XAudio2 voices, gamepad / keyboard input and saves in OPFS work (Doom); XMA2 and Opus are not decoded, GameInput has no device callbacks yet | XMA2 / Opus decoders when a title needs them |
| O6 | Speed | 1.3–6× native on micro-benchmarks; a game needs headroom | lazy flags, registers across calls, AVX2 paths, wasm64 only if needed |
| O7 | **The Minecraft files** | every Minecraft-specific answer (imports, memory, shaders, whether it needs `msvcp140` from its package) waits on them | run `tools/census.sh` first (`docs/first-run.md`) |
| O8 | macOS app | if Minecraft needs wasm64, WKWebView (Safari's engine, memory64 behind a flag) cannot run it | ship Chromium in the app instead of the system web view |

## 5. What we learned

- **Static recompilation of real 64-bit Windows code to wasm works, bit-exactly.** The deterministic
  benchmark matching its published signature is the strongest evidence we have; it also showed that the hard
  parts are not individual instructions but control flow (jump tables, landing pads) and the OS contract.
- **A deterministic oracle is worth more than any number of unit tests.** Stockfish's node count and XCL's
  own test title told us when something was wrong and when it was fixed.
- **Performance problems are concentrated.** A tick profile put 90 % of the time in two functions; one change
  to the SIMD helpers gave 35×. Profile before optimising.
- **wasm32 is the right default.** wasm64 works and untouched address space is free, but it costs up to 45 %
  and rules out Safari.
- **The browser's thread model needs a pool.** Workers cannot rely on each other's event loops.
- **Windows' exception model can be emulated faithfully** — including unwinding through native dispatcher
  frames — with `setjmp` landings in lifted functions and sentinel frames for handler calls.
- **XCL is the best donor**: its test title is an oracle for console API behaviour, and its decoder and
  container parser came over with their tests passing. Some of its checks encode its own design (memory above
  4 GiB) rather than the console's behaviour; those we do not chase.
- **Process lessons** (our own mistakes): `| head` on a build pipeline killed builds midway; editing runtime
  files while a background build was reading them broke a build; a research subagent once wrote a scratch file
  (`/tmp/x`) despite being read-only. Run long builds in the background, wait for them, then edit.

## 6. Numbers in one place

| What | Value |
|---|---|
| Stockfish 19 `bench` | 2,497,913 nodes (= published signature); 105,800 nodes/s Node, 110,800 Chrome, 22.5 s |
| Stockfish lift | 15,931 functions, 2.94 M instructions after AVX-only functions are left out, 363 jump tables, 0 unlifted |
| Stockfish compile | 16 parts in ~8 minutes on 4 cores (`-O2`); a relink takes seconds |
| Micro-benchmark, lifted vs native | 1.3–6× slower; wasm64 0–45 % slower than wasm32 |
| XCL console title | 28 / 50 checks (all CPU-side ones but 3 layout-specific), Node and Chrome |
| Our test titles | hello 7/7, threads 9/9, C++ exceptions 7/7, DLL 7/7, benchmark checksums 5/5 |
| Doom (console title) | 871 functions, 37 jump tables, 0 unlifted; 7,156-frame timedemo matches native on all 7,123 frames native draws deterministically, in Node and read back from WebGPU in Chrome; ~35 fps in headless Chrome on SwiftShader |
| Runtime imports implemented | 464 functions (before graphics) plus the D3D12 / D3D12.X objects, XAudio2 and GameInput readings across kernel32, ntdll, msvcrt, ucrtbase, kernelx, XMem, XGameRuntime, user32, DWM, GameInput, oleaut32, advapi32 |

## 7. Where everything is

| Path | What |
|---|---|
| `crates/lifter` | the static recompiler (x86-64 PE → C), and its census output |
| `crates/shaders` | console shader containers, RDNA2 decoder, `shader-census` |
| `rt/` | the runtime: process, kernel32, files, VFS, msvcrt, ucrt, seh, gdk, `d3d12.c` (D3D12 / D3D12.X front end), `xaudio2.c` |
| `web/gpu/` | the GPU back ends: `backend.js` (WebGPU), `null.js` (Node), `ring.js`, `cmd.gen.js` (from `rt/d3d12_cmd.h`) |
| `tools/shader-prep.mjs`, `tools/dxil-wgsl/` | DXIL shaders in a title → `shaders/<digest>.wgsl` (dxil-spirv + naga) |
| `fixtures/doom/` | Doom as a console title: platform file, header, build script, reference build, scripted sessions, video script |
| `web/run-node.mjs`, `web/browser/` | the hosts: Node, and the page + worker for browsers |
| `tools/build-title.sh`, `tools/census.sh`, `tools/run-chrome.mjs`, `tools/serve.mjs` | build, census, headless Chrome (WebGPU on SwiftShader, recording, reloads), a server with cross-origin isolation that also records video |
| `fixtures/` | our own test titles (sources; built binaries are ignored) |
| `docs/` | this log, status, first-run runbook, memory measurements |

Nothing is committed yet; the repository is local at `/home/vercel-sandbox/series-web`.
