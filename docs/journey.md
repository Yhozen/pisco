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

### 3.10 XCL's console checks, all 50 (2026-11-24)
The ladder's fourth rung: XCL's own console test title (`tests/testtitle` of github.com/ivall/xcl, its D3D12.X,
DirectStorage, Media Foundation and XAudio2 checks). Before: 28 of 50. Now **50 of 50 in headless Chrome**
(WebGPU on SwiftShader, every graphics check reading pixels back from the GPU) and 46 of 50 in Node (the 4
checks that read rendered pixels need the WebGPU back end). `tests/gfx/xcl.sh` runs it.
- **The GPU-visible region**: graphics memory (XMemAlloc / XMemVirtualAlloc with a graphics memory type,
  descriptor heaps, buffers) is ordinary linear memory whose address carries a tag above 4 GiB
  (`rt/gdk.c`). wasm32 keeps only an address's low 32 bits, so the title sees the console's region (16 GiB
  up, XCL's four windows) and every access lands in the block. Blocks come first fit from arenas, so
  neighbours are adjacent and merge when freed, as the checks expect.
- **UMA**: on the console device every buffer is guest memory and its GPU address is its CPU address;
  heaps have memory, buffers placed in them alias it, `CreatePlacedResourceX` makes a resource of the title's
  own memory. The back end treats guest memory as the truth: a segment of work uploads the ranges it reads
  and reads back the ranges it writes before anything later runs. Segments end at the console list's memory
  operations (`Write32/64BitValue…X`, `FillMemoryWith32BitValueX`, `CopyMemoryToMemoryX`, `Wait32/64BitValueX`),
  which then run on guest memory in order (`web/gpu/memops.js`, shared with the Node back end).
- **The rest of the console device and list**: `CreateCommandQueueX`, `CreateCommandListX` (the DMA list's
  short vtable), `CreateGraphicsPipelineStateX`, `CreateDerivedPipelineStateX` (patch parts as XCL found them),
  page pools, `GetFrameStatisticsX`, `KickoffX` (the words recorded so far are submitted; later they are
  replayed as state only), `RSSetDepthBiasX` (a pipeline variant; XCL's rule that all-zero turns it off),
  `DrawIndexedX` from title memory, occlusion queries (one WebGPU query per draw, summed per D3D12 query:
  WebGPU cannot nest them), `AllocateUploadMemory`, the AS descriptor format; unmodelled console slots log and
  return 0, as in XCL.
- **New libraries**: `d3d12_xs!D3D12CreateDevice` and `D3D12SerializeRootSignature`, IDXGIDevice1 and its
  adapter (`rt/dxgi.c`), MFPlat's device manager and `MFResetDXGIDeviceManagerX`, DirectStorage
  (`rt/dstorage.c`: file and memory sources, physical-page destinations, status arrays, fences, cancellation
  by tag), XG's texture computer, and `D3DCompile` served from shaders compiled ahead of time:
  `tools/hlsl-prep.mjs` takes the sources a run logs, compiles them with `dxc` and translates them.
- **Textures in title memory** follow CPU writes by hashing their bytes before work uses them (wasm has no
  write protection).
- **XMA2 and Opus**: FFmpeg's decoders (7.1.5, LGPL, built for wasm by `tools/build-ffmpeg-audio.sh`, wrapped
  by `rt/audio_codec.c`) decode a buffer to PCM16 when it is submitted. The wasm build's output is
  bit-identical to a native `ffmpeg` on XCL's packets (6,080 and 960 samples).
- **Bugs found on the way**: XAudio2's structures are packed (a buffer's context is at +36, `SamplesPlayed`
  at +12; Doom's own header had the same mistake); each worker's `performance.now()` counts from its own
  start, so times compared across threads were wrong (one clock for all threads now); `CopyTextureRegion`
  without a box left the box flag unwritten; the allocation size of a 2D array counted one slice.
- **What passing does not mean** (kept honest):
  - Raytracing: only the surface the check looks at (no raytracing tier, the AS descriptor, the two list
    slots). WebGPU has no raytracing.
  - XG's layout is ours (subresources one after another), not the console's tiling: textures a title lays out
    through XG at run time are right, textures tiled ahead of time by Microsoft's tools would not be.
  - A graphics reservation over 1 GiB gets address space only (wasm32 has 4 GiB in all); committing in it fails.
  - A pipeline with a console (RDNA2) shader is created but its draws are skipped: no RDNA2 → WGSL yet.
  - `D3DCompile` knows only sources prepared ahead of time; a new one fails and is logged.
  - A `Wait…X` holds the whole ring (every queue) while it waits; destroyed buffers keep their memory;
    DirectStorage runs on `Submit`, without zlib or BCPack; WebGPU's constant depth bias is an integer.
### 3.11 A second kind of title: an old Windows PC game (2026-11-24)
The question changed from "an Xbox title" to "any Windows game", with Narbacular Drop (2005, 32-bit,
Direct3D 9, DirectInput 8, FMOD 3) as the first. The references split in two: Wine and Proton for the
compatibility layer; winepak, PlayOnLinux and Lutris for the per-title recipe; OCI images for how a title is
packaged in layers from a base. The plan and its milestones are in [status.md](status.md).
- **Wine first, as the oracle.** A portable Wine (pure WoW64, so no 32-bit system libraries) installs and runs
  the game with no screen: Xvfb, a virtual desktop, XTEST input. `tools/oracle-wine/` holds it: `oracle.sh`
  (prefix, start, play, stop), `play.py` (one session-script format, meant to drive our own hosts too) and
  `summarize.py` (Wine's traces as names and counts).
- **The census** ([narbacular-drop.md](narbacular-drop.md)): one session from the menu into the first level,
  traced three times. The game calls 81 Win32 functions (it imports 186), raises no exceptions, creates no
  shaders, and draws everything with fixed-function Direct3D 9: 5.7 million calls, mostly triangle fans, many
  lights, vertex blending, vertex fog, point sprites, and portals rendered to textures.
- **Licences**: our own code became GPL-2.0-or-later with a map per directory ([LICENSING.md](../LICENSING.md),
  `REUSE.toml`), so code can come from Wine and DXVK into the runtime and from GPL-3.0 projects into separate
  tools. `rt/d3d12_layout.gen.h` is now generated straight from Microsoft's `d3d12.h` (MIT); the output is
  byte for byte what XCL's table gave.
- **A toolchain script and a test runner** (`tools/setup-toolchain.sh`, `tests/run.sh`): this machine had
  lost its toolchains, and the fixtures had only been run by hand.

### 3.12 32-bit code: the lifter's second mode, and the first 32-bit titles (2026-11-24)
Milestone M1. The lifter now reads PE32 and lifts 32-bit x86; the runtime builds for a 32-bit guest; two
32-bit titles run in Node and in headless Chrome and print exactly what they print under Wine.
- **One lifter, a width carried through**: PE32 loading, 4-byte relocations and import slots, 32-bit decoding,
  4-byte stack operations, `fs` as the TEB. An image without relocations stays where it was linked. The C for
  64-bit titles did not change by a byte (six titles compared before and after).
- **Functions without an exception table**: recursive descent from the entry point and call targets ("hard"
  entries), plus code addresses found in data, in immediates and by a scan of what nothing reached ("soft").
  Only hard entries cut a function; a soft entry inside another function is one more way into its body.
- **Jump tables** as Visual C++ writes them: `jmp [reg*4+table]` with the table after the function, and the
  two-level form through a byte table. Bounds are found by walking back along control flow (`memcpy` indexes
  its tables with negative numbers).
- **Instructions 64-bit code never has**: `pushad`, `pushfd` / `popfd` (EFLAGS.ID round-trips, so `cpuid`
  detection works), `enter`, `loop`, `jecxz`, `std` with string instructions, `rcl` / `rcr`, `xlat`; x87's
  transcendentals through libm, `fucomip`, `fxam`, and **precision control**: in 24-bit mode results are
  rounded to `float`, as Direct3D 9 leaves the FPU.
- **The runtime for a 32-bit guest** (`-DGUEST_BITS=32`): arguments read from the stack, a stdcall function's
  arguments removed after it returns (sizes from llvm-mingw's 32-bit import libraries:
  `tools/stdcall-sizes.sh`; an unknown size stops the run instead of guessing), the TEB on `fs` with the x86
  offsets, 4-byte loader writes, the low-image memory layout (`docs/memory.md`).
- **Two test titles** whose expected output is Wine's: `hello` built for i686 (7/7) and `fixtures/x87`
  (27 lines: control word, rounding modes, 24- and 53-bit precision, transcendentals, compares, the direction
  flag, EFLAGS.ID, a stdcall callback, a compiled jump table, 64-bit integer helpers).
- **The game lifts**: 349,540 instructions, 96 % of `.text`, all 88 jump tables; what is left out is MMX and
  3DNow only ([narbacular-drop.md](narbacular-drop.md)).

### 3.13 The game starts: a Win32 process for a 32-bit guest (2026-11-24)
Milestone M2. Narbacular Drop's own code now runs from its entry point to the moment it asks for Direct3D 9,
making the same calls as under Wine.
- **A profile for Windows desktop titles**: `tools/build-title.sh` links the core of `rt/` plus `rt/win32/`
  and leaves the console's libraries out (`PROFILE`; 32-bit titles get `win32` by default). Nothing was moved:
  the console profile is the list of files it always was.
- **`rt/win32/`**: the start-up side of kernel32 (Windows XP's version numbers, start-up information, an
  environment, `GetStringTypeW` / `LCMapStringW` / `CompareStringA` over code page 1252), user32 (window
  classes, one top-level window, and the messages Windows sends while a window is created, sized and shown,
  delivered by calling the game's window procedure), an empty registry, GDI object handles.
- **The recipe starts to matter**: `recipes/narbacular-drop/title.toml` tells the build the virtual CPU's
  features (a Pentium without MMX, so D3DX stays on its x87 code), and that the shipped `fmod.dll` is the
  runtime's to answer (`lifter --builtin`).
- **Comparing with Wine call by call** (`tools/oracle-wine/trace-diff.py`): both traces become the sequence
  of function names with repeats folded, and the first difference is shown with the calls around it. Wine's
  side is recorded once into a small file (`recipes/narbacular-drop/traces/start.txt`: names and counts), so
  the check needs neither Wine nor its 800 MB log.
- **Result**: the first run stopped at the C runtime's own "R6028 - unable to initialize heap"; with the layer
  in place the trace matched Wine's for 41 steps, then differed in one place (R23); after that fix, all 81
  steps up to `Direct3DCreate9`, in strict mode.

### 3.14 The main loop without a screen: a null Direct3D 9, input, a clock (2026-11-24)
Milestone M3. The game plays the reference session from its menu into its first level in Node, in strict mode,
with nothing drawn: 6,256 frames in four seconds, the same log on every run.
- **Asking the oracle instead of guessing** (`tools/oracle-wine/d3d9probe.c`): a small Windows program run under
  Wine prints what Direct3D answers where a trace only shows the question: the adapter and its modes,
  `CheckDeviceFormat` for 51 formats in 44 uses, every state of a new device, and corner cases one at a time
  (references, double locks, lights never set, recording). `tools/gen-d3d9-oracle.py` turns its output and the
  capabilities into a header: no number of the device was typed. It corrected guesses before they were made:
  the oracle has no `D32` depth buffers, a default-pool texture cannot be locked, an enabled light reads back as 128.
- **The null device** (`rt/win32/d3d9*.c`): objects in guest memory with the title's references and the device's
  own uses counted apart (a buffer released while bound lives until it is unbound), textures and buffers with
  real bytes, state blocks as lists of entries (recording goes to the block, not to the device), draws checked
  against what is bound. Statistics under Wine's own method names, a frame at a time if asked.
- **A clock the title cannot outrun** (`rt_now_us`, `SW_CLOCK`): virtual time moves when a frame is presented or
  the title sleeps, so loading takes no time and a run does not depend on the machine.
- **Input and the session player** (`rt/win32/input.c`, `dinput8.c`): one input state with its events; the
  game reads the keyboard as a state and the mouse only as buffered events, so `GetDeviceData` had to be real.
  The session script that drives Wine's X server drives this state (`SW_SESSION`).
- **Comparing by frame, not by total** (`tools/oracle-wine/d3d9-compare.py`): the two runs have different frame
  counts, so the unit is "the calls one frame makes". 27 kinds of frame are identical on both sides and cover
  94 % of Wine's frames; the level's frames differ by exactly the text D3DX draws under Wine (R26).
- **Result**: the first strict run after the layer was written went from the menu to the tutorial; three stops
  followed (R25 and R27 below, and a format question the probe had not asked).

### 3.15 Pixels: fixed-function Direct3D 9 on WebGPU (2026-11-24)
Milestone M4, the first part. In headless Chrome the game's frames appear, and the scripted session ends with a
PNG of each of its 11 checkpoints that matches Wine's screenshot of it
([narbacular-drop.md](narbacular-drop.md) has the table).
- **A second front end on the same ring.** Direct3D 9 does not pretend to be Direct3D 12: it writes eight explicit
  records in WebGPU's own terms (`rt/d3d12_cmd.h`: shader as WGSL text, sampler, pipeline with its vertex and bind
  layouts, buffer write, texture write, pass, viewport, draw) and shares resources, present and marks with the
  D3D12 records. The ring's writer moved to `rt/gpu_ring.c`, which both profiles link; `web/gpu/explicit.js` runs
  the records inside `backend.js`, `web/gpu/records.js` reads them for both back ends.
- **Shaders written by the runtime** (`rt/win32/d3d9_ffp.c`): Direct3D 9's fixed-function pipeline as a pure C
  function from a state key to WGSL, one for the vertex stage (pretransformed or transformed, 1 to 4 blended world
  matrices, lighting by up to 8 lights, material sources, fog, texture coordinates, points as quads) and one for
  the fragment stage (one texture stage, alpha test by `discard`, fog). Built natively too, where naga validates
  446 keys' worth (`tests/gfx/d3d9-ffp.sh`).
- **Draws from state** (`rt/win32/d3d9_gpu.c`): keys, pipeline, a uniform block, texture and sampler, buffers,
  made or found at each draw; fans drawn as indexed lists, point lists as instanced quads; buffers and textures
  written when their `dirty` counters have moved, with the lock's promise (`DISCARD`, `NOOVERWRITE`) carried along
  so that the back end knows whether a write may overtake the draws recorded before it. Direct3D 9's conventions
  that are not WebGPU's are each settled in one place and listed at the top of that file.
- **Order on WebGPU**: queue writes happen at once, passes only when submitted. The back end keeps one encoder a
  frame and, for a write to something recorded draws still read, either submits first (an ordered write), hands
  the buffer new storage (a discard), or just writes (an append). The game never needs any of it (it makes a new
  vertex buffer for every batch of particles); a run with every buffer rewritten before every draw, both ways,
  showed the two paths give the same frames.
- **The hosts**: the page takes the runtime's settings as the Node host does (`SW_CLOCK`, the session's steps),
  and saves the frame presented at each `shot` through the server; the null back end in Node checks every
  explicit record for what WebGPU would refuse, and a run with an inconsistent record fails.
- **Comparing pictures** (`tools/oracle-wine/shot-compare.py`): mean difference per channel, share of pixels over
  a threshold, difference pictures; standard library only.
- **Result**: the first frame drawn in Chrome was the menu as Wine draws it; nothing had to be corrected after
  it. What took thought was the last level or two of difference on the still screens (R29).

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
| R15 | No mouse motion reached the game under Wine | with no window manager nothing gives Wine's window the X input focus, and without it Wine reads no raw motion: DirectInput saw buttons with zero movement | the session player sets the focus itself, and moves the pointer only by relative steps (a warp arrives as one huge movement) |
| R16 | Wine's installer run failed twice | a first boot stopped at the Mono prompt left a prefix that cannot load 32-bit programs; the C locale cannot create the one file whose name has `©` | no Mono / Gecko, `LANG=C.UTF-8`, and `oracle.sh prefix` recreates a half-made prefix |
| R17 | `+snoop` logged no FMOD calls | the DLL is UPX-packed: when it loads, every export still points at zeros, and snoop skips those | unpack it first (`upx -d`); it behaves the same |
| R18 | `dxc` for Linux needs glibc 2.38 and Docker was not running | the Doom fixture compiles one HLSL shader | the Windows `dxc.exe` under the oracle's Wine, with paths given the `Z:` drive |
| R19 | A false function entry silently breaks a branch | flags live in the lifted function, and a jump to another entry becomes a call; without an exception table, words in data that look like code addresses can point into the middle of a function | hard and soft entries: only the entry point, exports and call targets cut a function |
| R20 | `fxch st(i)` exchanged st(0) with itself | the emitter read the first operand, which is always st(0) | the last operand; found when the 32-bit x87 work compared against Wine (no 64-bit test title used it) |
| R21 | The 32-bit `hello` crashed under Wine itself | Rust's `raw-dylib` imports stdcall functions under their decorated names (`_WriteFile@20`) on x86; kernel32 exports plain names | `import_name_type = "undecorated"`, and Wine's crash debugger turned off so a crash ends the run instead of waiting |
| R22 | 64-bit division in a 32-bit program with no C runtime | the compiler calls Visual C++'s helpers (`__aulldiv`, ...: arguments on the stack, removed by the callee) | thunks in the fixture around a shift-and-subtract division |
| R23 | Five calls Wine never showed | the game's C runtime made a stack-protection cookie from the clock, the process and thread ids, the tick count and the performance counter | Windows' loader sets the cookie named in the image's load configuration directory, and so does Wine; now ours does too (a fixed value) |
| R24 | Wine's trace lacks some calls altogether | thread and process ids, the last error, locks, interlocked operations and the performance counter never appear in a relay trace | the comparison leaves those out of our side too; the list is in `trace-diff.py`, from a whole session's trace |
| R25 | D3DX's font could not be created; the game then called through a null pointer | on Windows 2000 and later D3DX requires gdi32's `GetCharABCWidthsI`, `GetCharWidthI` and `GetFontUnicodeRanges`, found with `GetProcAddress`; Uniscribe is the optional part | the three functions; `usp10.dll` stays absent |
| R26 | A level frame had 9 indexed draws fewer than under Wine | D3DX copies each glyph from a GDI bitmap into a texture and draws nothing for a glyph whose bitmap is empty; our GDI draws no text yet | left as it is, measured (a box per glyph brings the calls back); real glyphs come with the pixels |
| R27 | D3DX "loaded" Direct3D's debug DLL | module names were matched as substrings, so `d3d9d.dll` was `d3d9.dll` | desktop titles' DLL names are matched exactly |
| R28 | The start-up test would have traced the main loop for five minutes | since M3 the game no longer stops at Direct3D | a session of one empty step (`SW_SESSION_STEPS='wait 0'`) ends the run at the first frame |
| R29 | Still screens differed from Wine's by one to nine levels of 255, red and blue more than green | Wine's CPU has MMX and SSE and D3DX decodes and resamples pictures with them; the recipe's CPU has neither, and D3DX's plain code rounds differently | an oracle run with D3DX's processor-specific code turned off (three registry values it reads): the still screens then agree to one level, the rasterisers' rounding |
| R30 | Is a difference in a moving scene an error? | the oracle's clock is real, ours virtual: a menu with flickering lights and a level with a walking creature are shot at other moments | measured what two runs of Wine differ by (more than ours differs from either), and took a series of shots to find the creature where the oracle has it |

### Open
| # | Roadblock | Why it matters | Plan |
|---|---|---|---|
| O1 | **Graphics beyond Doom's and XCL's checks** | D3D12.X → WebGPU works for Doom and XCL's checks (depth, index buffers, derived pipelines, queries, UMA memory, copies); vertex / constant buffers with real data, many descriptors, compute and DXGI swap chains are written but untested | SimpleTriangleDesktop then SimplePBR (the ladder's steps 1–2) |
| O2 | **Shader translation** RDNA2 → WGSL | the console ships GPU machine code, not portable shaders | the census tells which stages and instructions Minecraft needs; XCL's HLSL emitter is the reference; mesh/NGG programs may need compute + indirect draws |
| O3 | **WebGPU limits** | no mesh shaders, no bindless heaps (XCL saw Minecraft use eight heaps of 73,727 descriptors), no 64-bit integers, no shared memory | per-draw bindings traced from each shader (VIctory's approach), compute emulation where needed; decided after the shader census |
| O4 | MSVC C++ exceptions | Minecraft is an MSVC title; `__CxxFrameHandler3/4` are not implemented | an MSVC-ABI test title already builds (`fixtures/msvc`); implement if the title throws in normal play |
| O5 | Audio codecs | PCM, XMA2 and Opus voices decode (FFmpeg); XMA buffers are decoded whole at submit, each buffer on its own; GameInput has no device callbacks yet | stream XMA across buffers when a title needs it |
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
- **Measure the title before designing for it.** The import table said 186 functions, exceptions and a shader
  compiler; one traced session said 81 functions, no exceptions and no shaders for the first level, and that
  text and render-to-texture are needed. The plan follows the trace, not the import table.
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
| XCL console title | 50 / 50 checks in Chrome (WebGPU), 46 / 50 in Node (no pixels there) |
| Our test titles | hello 7/7, threads 9/9, C++ exceptions 7/7, DLL 7/7, benchmark checksums 5/5 |
| 32-bit test titles | hello (i686) 7/7; `fixtures/x87` 27 lines equal Wine's output; both in Node and Chrome |
| Narbacular Drop, started | 274 calls from the entry point to `Direct3DCreate9`: the same 81 folded steps as under Wine |
| Narbacular Drop, menu to level | 6,256 frames (104 s of its time) in 4.2 s in Node; 7.2 M Direct3D 9 calls, 70 methods; 27 kinds of frame identical to Wine's, covering 94 % of Wine's 11,857 frames |
| Narbacular Drop, drawn | in headless Chrome on SwiftShader: 529,685 draws in 8,650 passes over 6,256 frames, 31 vertex and 8 fragment shaders, 33 pipelines; about 4 minutes (menu 18 to 25 frames a second, still screens 60 to 75, level 13); still screens within 1 level of Wine's (with D3DX's plain code), menu and level within what two runs of Wine differ by |
| Narbacular Drop, lifted | 3,955 functions, 349,540 instructions, 88 jump tables, 96.2 % of `.text`; 8,279 MMX / 3DNow instructions not lifted; 3 indirect jumps unresolved (C++ exception code) |
| Doom (console title) | 871 functions, 37 jump tables, 0 unlifted; 7,156-frame timedemo matches native on all 7,123 frames native draws deterministically, in Node and read back from WebGPU in Chrome; ~35 fps in headless Chrome on SwiftShader |
| Runtime imports implemented | 464 functions (before graphics) plus the D3D12 / D3D12.X objects, XAudio2 and GameInput readings across kernel32, ntdll, msvcrt, ucrtbase, kernelx, XMem, XGameRuntime, user32, DWM, GameInput, oleaut32, advapi32 |

## 7. Where everything is

| Path | What |
|---|---|
| `crates/lifter` | the static recompiler (x86-64 PE → C), and its census output |
| `crates/shaders` | console shader containers, RDNA2 decoder, `shader-census` |
| `rt/` | the runtime: process, kernel32, files, VFS, msvcrt, ucrt, seh, gdk, `d3d12.c` (D3D12 / D3D12.X front end), `xaudio2.c` |
| `web/gpu/` | the GPU back ends: `backend.js` (WebGPU) with `explicit.js` (the explicit records: Direct3D 9), `null.js` (Node), `ring.js`, `records.js`, `cmd.gen.js` (from `rt/d3d12_cmd.h`) |
| `rt/gpu_ring.c` | the ring's writer, shared by the D3D12 and Direct3D 9 front ends |
| `tools/shader-prep.mjs`, `tools/dxil-wgsl/` | DXIL shaders in a title → `shaders/<digest>.wgsl` (dxil-spirv + naga) |
| `fixtures/doom/` | Doom as a console title: platform file, header, build script, reference build, scripted sessions, video script |
| `web/run-node.mjs`, `web/browser/` | the hosts: Node, and the page + worker for browsers |
| `tools/build-title.sh`, `tools/census.sh`, `tools/run-chrome.mjs`, `tools/serve.mjs` | build, census, headless Chrome (WebGPU on SwiftShader, recording, reloads), a server with cross-origin isolation that also records video |
| `fixtures/` | our own test titles (sources; built binaries are ignored) |
| `tools/oracle-wine/` | the Wine oracle: prefix, run, session player, trace summaries, `shot-compare.py` (our screenshots against its) |
| `tools/setup-toolchain.sh`, `tests/run.sh` | the toolchains outside the repository; every test title, one line per check |
| `rt/win32/` | the Windows desktop layer: start-up kernel32, user32, registry, GDI, Direct3D 9 (`d3d9*.c`: the device and its state; `d3d9_ffp.c`: the fixed-function pipeline as WGSL; `d3d9_gpu.c`: clears, draws and presents as GPU ring records), DirectInput 8, the input state and session player, FMOD 3 (silent), `com.c` for COM objects |
| `tools/gen-com-layout.py`, `tools/gen-d3d9-oracle.py` | generated headers: COM vtables from the mingw-w64 headers; the oracle's Direct3D 9 answers |
| `web/session.mjs` | a session script as the text the runtime's player reads |
| `tools/stdcall-sizes.sh`, `tools/recipe-gen.py` | 32-bit builds: what each import removes from the stack; what the recipe says, for the runtime |
| `fixtures/x87/` | the 32-bit x87 title and what it prints under Wine |
| `recipes/<title>/` | per-title files: the recipe, session scripts, Wine's recorded call sequences |
| `docs/` | this log, status, first-run runbook, memory measurements, the Narbacular Drop census |

