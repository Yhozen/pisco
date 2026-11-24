# Status and next steps (2026-11-24)

## Done

- **Gate A (CPU and OS layer on synthetic titles): passed** in Node and headless Chrome — our own titles
  (Rust, C++ with libc++/libunwind, a multi-image title with a DLL) and XCL's console test title (28 of its
  50 checks: all CPU-side ones but 3 that need XCL's above-4-GiB memory layout).
- **A real 64-bit Windows program** (Stockfish 19's official build, MinGW-compiled, 103 MB) runs bit-exactly:
  `bench` searches 2,497,913 nodes, the signature published with the release, at ~106 k nodes/s in Node and
  ~111 k in Chrome.
- wasm32 vs wasm64 measured (`docs/memory.md`).

- **Graphics, sound, input and saves on a console title: Doom** (step 3 of the graphics ladder; `fixtures/doom`,
  `docs/journey.md` §3.9). D3D12.X → WebGPU in headless Chrome, XAudio2 → AudioWorklet, GameInput from the page,
  XGameSave's folder kept in OPFS. Its `-timedemo` frames, read back from WebGPU, equal native doomgeneric's on
  every frame native draws deterministically. Video: `fixtures/doom/video.sh`.

## Not done, in the order Minecraft probably needs it

0. **The graphics ladder's steps 1–2** (SimpleTriangleDesktop, SimplePBR) and **4** (XCL's 19 graphics checks):
   the MSVC C++ runtime DLLs, the AVX rule in the lifter, DXGI swap chains, vertex / index / constant buffers,
   depth, BC textures. The shared D3D12 core they need exists (written for all of D3D12, tested on Doom's part).
1. **D3D12.X** (historical note, before 2026-11-24): nothing yet. First a null device (objects that accept every call, as XCL's M1) to reach the
   first frame submission (Gate B); its vtable layouts can come from XCL's `hle/d3d12_layout.rs`. Then command
   lists → WebGPU, title memory copied to GPU buffers before each submission (VIctory's `shadow.rs` model).
2. **Shaders**: the census exists (`crates/shaders`, decoder and container parser from XCL). A translator
   RDNA2 → WGSL is the big piece; XCL's HLSL emitter (`gpu/recomp/emit.rs`) is the reference.
3. **MSVC C++ exceptions** (`__CxxFrameHandler3/4`, `_CxxThrowException`): a test title builds
   (`fixtures/msvc`, clang for the MSVC ABI with import libraries from .def files) but the runtime has no
   handler yet. Needed only if the title throws during normal play; the census and a run will say.
4. **Audio** (XAudio2 → AudioWorklet, XMA2 / Opus decoding: XCL has pure-Rust XMA2 and uses Opus).
5. **Input**: GameInput's object exists with timestamps only; readings from the Gamepad API next.
6. **Persistence**: the VFS overlay is in memory; saves should go to OPFS in the browser, a folder in Node.
7. **Speed**: lifted code runs ~1.3–6× native on our micro-benchmarks. Next: keep flags lazily, register
   locals across calls, AVX2 paths (the virtual CPU reports SSE4.2 only), wasm64 only when needed.

## Known limits

- Exceptions resume only at landing pads the lifter found (GCC/LLVM LSDA, C scope tables); MSVC funclets not yet.
- x87 is emulated with doubles (no 80-bit precision).
- Memory protection, guard pages and write watches do not exist; the VFS has no persistence.
- One wait list serves all waits (events, semaphores, mutexes, address waits): correct, not fast with many threads.
