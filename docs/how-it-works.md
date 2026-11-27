# How it works

The short version is in the [README](../README.md#how-it-works); [plan.md](plan.md) has the structure and the rules a
change follows, and [journey.md](journey.md) how each part came to be.

```
title (PE or ELF) ── lifter (native, once per build) ──► C, one function per guest function ──► clang ──► title.wasm
                                                                                                   │ imports
runtime (rt/, C on wasi-libc, linked into the same module) ◄───────────────────────────────────────┘
 what imitates no system, every title:
  core.c      the log, guest memory, the virtual CPU (CPUID, the time stamp counter), the search of the lifted
              functions, random numbers, the end of the process
  clock.c     the title's clock, real or virtual (SW_CLOCK)
  builtins.c  the compiler-rt helpers lifted code needs
 the Windows core, every Windows and console title (the other rt/*.c):
  process.c   image loading, import binding, guest calls, handles and waits, threads, TEB / TLS
  kernel32.c  threads, sync objects (events, semaphores, mutexes, critical sections, SRW, condition variables),
              memory, time, modules, code pages, standard handles
  msvcrt.c    C runtime: startup / exit, stdio, printf (each guest width's arguments), strings, maths, locale "C"
  ucrt.c      the UCRT (api-ms-win-crt-*), falling back to msvcrt.c where the semantics match
  oleaut32.c  BSTRs and VARIANTs
  seh.c       x64 exception handling over the images' .pdata / .xdata: RaiseException, RtlUnwindEx, vectored handlers
  vfs.c       the title's file system: console drives, the title's folder read-only, an in-memory overlay;
              an image's file layers make C:, and what a desktop title writes in its folder is kept by the host
  files.c     kernel32 / ntdll file functions over vfs.c
  input.c     keys and the mouse for desktop titles, and the player of scripted sessions
  mixer.c     the one sound output, shared by XAudio2, FMOD and the PS5's AudioOut (mixed to the virtual clock when
              there is one)
  gpu_ring.c  the ring of records a GPU back end reads (web/gpu/): what D3D12, Direct3D 9 and the PS5's VideoOut
              write to
 one folder per runtime (each base lists its own; `pisco sources <base>` prints them):
  gdk/        what the Xbox Series and Windows x64 runtimes share: D3D12 (d3d12.c), DXGI and MFPlat, D3DCompile
              from precompiled shaders, XAudio2 + audio_codec.c (XMA2 / Opus via FFmpeg), XGameRuntime (task
              queues, XAsync, XUser, saves, PLS, package), GameInput, the title's window (user32 / DWM)
  xbox-series/ the console's own: kernelx, XMem (with the GPU-visible region), D3D12.X's frame clock and list
              methods, XG, DirectStorage
  windows-x64/ DXGI's factory and swap chains, the PC GDK's QueryApiImpl objects
  windows-x86/ user32, gdi32 (text drawn: font.c, stb_truetype, Liberation Sans), the registry, DirectInput 8,
              FMOD 3 (fmod3.c, audio_decode.c), and Direct3D 9 (d3d9*.c: the device; d3d9_ffp.c: its
              fixed-function pipeline as WGSL; d3d9_gpu.c: its draws)
  linux-x64/  Linux at its system calls (syscall.c): the start of a process, memory, files and a generated /proc,
              fork / exec / pipes with a worker per process. It links none of the Windows core
  ps5/        the PS5's system libraries a homebrew title calls: libkernel, the C library, VideoOut, Pad, AudioOut.
              It links none of the Windows core either
host (web/)   WASI calls, one worker per guest thread over one shared memory; image.mjs reads a title image
pisco (crates/pisco) builds a recipe and the user's own copy of a title into a title image (OCI layout), and runs it
```

Imports of a Windows or console title that nothing implements become stubs that log their first call and return 0
(E_NOTIMPL for the console's HRESULT APIs), as in XCL; a Linux system call nothing answers returns `-ENOSYS`.
`SW_STRICT=1` stops at them instead.

- **Images**: a Windows exe and every DLL it imports that sits beside it are lifted together, each rebased below
  4 GiB; imports between them are bound at lift time. The runtime reads their bytes from the title's folder. A Linux
  title is one statically linked ELF; a PS5 title is one native executable (an ELF of type 0xFE10), its imports
  named by NID.
- **Lifter** (`crates/lifter`): functions from the exception table (a PE's `.pdata`, an ELF's `.eh_frame`), symbols
  where there are some, call targets, code pointers in data, jump tables, and landing pads from the
  language-specific exception data (GCC/LLVM LSDA, C scope tables, MSVC's FH3 and FH4). It reports what it could
  not lift.
- **The virtual CPU**: CPUID reports up to SSE4.2 and POPCNT, so a title that asks picks its SSE paths. AVX, AVX2,
  FMA and BMI are lifted all the same (`avx.rs`, `rt/avx.h`), for titles built for a CPU that has them, as PS5
  ones are; what `avx.rs` does not handle becomes a trap.
- **Registers** live in a `Cpu` struct passed as `restrict`, so clang keeps them in wasm locals between calls.
- **32-bit titles**: the same lifter in a 32-bit mode. Without an exception table, functions come from the
  entry point and call targets, then from code addresses in data and immediates; `jmp [reg*4+table]`
  switches are recovered; x87 keeps its control word (rounding, 24-bit precision). The runtime is built for
  the guest's width (stdcall arguments on the stack, the TEB on `fs`). MMX and 3DNow are not lifted; the
  recipe gives a title its virtual CPU's features, so a 32-bit title takes its x87 paths.
- **Guest memory** is linear memory: a guest address is a wasm address. Images are rebased below 4 GiB.
  wasm32 by default; see [memory.md](memory.md) for wasm64.
- **Threads**: a guest thread is a pthread of the runtime, i.e. a worker. In the browser they come from a
  pool the page starts ahead of time (a worker cannot start one while it blocks).
- **Processes** (Linux): a forked process is a worker with memory of its own, copied from its parent's
  ([neofetch.md](neofetch.md)).
- **Exceptions**: a lifted function with landing pads registers a `setjmp` landing; restoring a context
  longjmps to the frame's landing and jumps to the pad's label.

## Building and running

Needs Rust, Node 24, [wasi-sdk](https://github.com/WebAssembly/wasi-sdk) 34 in `~/tools`, and for the C++
tests [llvm-mingw](https://github.com/mstorsjo/llvm-mingw). `tools/setup-toolchain.sh` fetches them (and the
shader tools the Doom test needs, and musl for the Linux titles); `tests/run.sh` builds and runs every test title,
and skips in one line a step whose inputs are not on the machine.

A title without a recipe, straight from its executable:

```sh
cargo build --release -p pisco-lifter
(cd fixtures/hello && cargo build --release)                       # our own Windows-format test titles
cargo build --release -p pisco                                      # build-title.sh asks it for the base's files
tools/build-title.sh fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe out/hello   # BASE=windows-x64 (64-bit default)
node web/run-node.mjs out/hello/title.wasm fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe
node tools/run-chrome.mjs out/hello/title.wasm <the .exe>          # the same in headless Chrome
```

A Windows PC title is built from its recipe and your own installer into a title image (an OCI image layout:
[images.md](images.md)); the installer is checked by its sha256, extracted with innoextract
(`tools/setup-toolchain.sh innoextract`), and never copied into the repository:

```sh
cargo build --release -p pisco -p pisco-lifter
target/release/pisco build recipes/narbacular-drop --source ~/Downloads/narbacular_drop_setup.exe   # -> out/images/narbacular-drop
target/release/pisco run out/images/narbacular-drop --session recipes/narbacular-drop/sessions/menu-to-level.json
target/release/pisco run out/images/narbacular-drop --chrome          # headless Chrome; in a browser of your own:
node tools/serve.mjs 8000 web out/images/narbacular-drop           # then http://127.0.0.1:8000/browser/index.html?image=/files/0/
target/release/pisco build recipes/tag --source ~/Downloads/Tag_setup.exe                          # -> out/images/tag, the same way
target/release/pisco build recipes/desire --source ~/Downloads/DESI-Re-v0.1-Windows-x64.zip --with REDIST=<the x64 C++ runtime DLLs>   # -> out/images/desire
target/release/pisco inspect out/images/narbacular-drop               # its layers, config and registry
target/release/pisco census <a title's .exe>                          # what the base and its libraries answer for
```

The Linux titles are built from their sources ([fastfetch.md](fastfetch.md), [neofetch.md](neofetch.md),
[cowsay.md](cowsay.md)), the PS5 demo from its release ([ps5.md](ps5.md)).

`SW_TRACE=1` logs missing imports and exception dispatch; `SW_TRACE=2` every import call. `SW_CLOCK=16667` makes
time virtual (it moves by that many microseconds when a frame is presented). For Windows desktop titles,
`SW_SESSION=<session.json>` plays a scripted session of mouse and keyboard input (both hosts; in Chrome the frame
presented at each of its `shot` steps is saved as a PNG beside the wasm, in `shots/`), and `SW_D3D9_STATS=1` and
`SW_D3D9_FRAMES=1` print what the title asked Direct3D 9 for and the shaders made for it.

## What the test titles check

What each runtime runs and what comes next is in [status.md](status.md); this is the list by title.

| Title | What it checks | Node | Chrome |
|---|---|---|---|
| `fixtures/hello` (Rust, no_std) | recursion, switch, pointers, SSE loop, f64 | 7/7 | 7/7 |
| `fixtures/threads` (Rust, no_std) | 4 threads, `lock xadd`, critical sections, events, semaphores, TLS, ExitThread, timeouts | 9/9 | 9/9 |
| `fixtures/cxx` (C++, llvm-mingw, libc++ + libunwind) | throw through frames with destructors, `catch (...)`, `std::exception`, rethrow, throw in catch, in a thread | 7/7 | 7/7 |
| `fixtures/dll` (C++, llvm-mingw: an exe and the DLL it ships) | static and dynamic imports, DllMain on process and thread attach, `thread_local` in the DLL, an exception crossing from the DLL | 7/7 | – |
| `hello` again as a 32-bit program (i686), and `fixtures/x87` (C, llvm-mingw, no SSE) | the lifter's 32-bit mode and the runtime for a 32-bit guest: stdcall, the x87 with its control word (rounding, 24-bit precision), the direction flag, EFLAGS.ID, a jump table as Visual C++ writes them. Expected output: what the same programs print under Wine | 7/7; 27 of 27 lines | 7/7; 27 of 27 |
| **Narbacular Drop** (2005; the user's own copy, built with `recipes/narbacular-drop/title.toml`) | a real 32-bit Windows game from its entry point into its first level: its C runtime, files, window, Direct3D 9 (fixed-function, drawn through shaders the runtime writes), D3DX, DirectInput, FMOD 3 (sound from its MP3, Ogg and WAV files), scripted sessions on a virtual clock, the keyboard and Pointer Lock mouse from the page, files it writes kept between runs | the same calls as under Wine up to `Direct3DCreate9` (81 steps, repeats folded); the session's 13 checkpoints over 6,256 frames, the same log on every run, each frame making the Direct3D calls it makes under Wine, text draws included (strict mode); its 539,750 draws reach the null GPU back end as consistent records; a second session places both portals and walks out of the cage, its sound the same on every run, its FMOD calls Wine's up to the game's first random choice of a sound; `system.opt`, written on Quit, read by the next run | the session drawn with WebGPU, a PNG at each checkpoint: the 8 still screens within one level in 255 of Wine's (with D3DX's plain code, as here), the menu and the level within what two runs of Wine differ by, the level's title and hint drawn by GDI where Wine draws them; 12 to 20 frames a second on SwiftShader (`tests/run.sh ndgfx`); the portal session's level views within 1 to 2 levels of Wine's on average, `system.opt` kept across a reload in OPFS (`tests/run.sh ndplay`); a recording with sound (`recipes/narbacular-drop/video.sh`); built from the installer as an image, the same results (`tests/run.sh images`). **Played by hand in Chrome on an M1 MacBook Pro**: 294 to 355 frames a second in lighter views, 103 to 174 in the heaviest, no WebGPU error over 4.4 M draws |
| **XCL's console test title** (`tests/testtitle` + its game DLL, Rust std, built for `x86_64-pc-windows-gnullvm`) | XCL's own checks of a console title: TLS, panics, kernelx, XMem, the console screen, GameInput, modules, game DLL, the VFS, XGameRuntime, XUser, saves, PLS; D3D12.X (UMA memory, the console device and list methods, DMA queue, derived pipelines, depth bias, queries, textures in title memory), DirectStorage, MFPlat, XAudio2 with XMA2 and Opus | 46/50 (the 4 checks that read pixels need WebGPU) | **50/50** (`tests/gfx/xcl.sh`) |
| **Stockfish 19, official Windows x64 build** (MinGW, 103 MB) | `bench`: 2.9 M instructions lifted, worker threads, NNUE (109 MB) in shared memory | **2,497,913 nodes = the published signature**, 105,700 nodes/s | **2,497,913 nodes**, 110,821 nodes/s, 22.5 s |
| **SimpleTriangleDesktop** (Microsoft's GDK sample, clang-cl and real MSVC builds, with the MSVC C++ runtime DLLs lifted beside it) | the PC GDK's XGameRuntime, DXGI swap chains, vertex buffers, sRGB targets, MSVC C++ exceptions (`fixtures/msvc`: 6/6 with the real vcruntime140) | runs, null back end | frame read back from WebGPU = the colours and area computed from the sample's vertices, both builds (`tests/gfx/st.sh`) |
| **SimplePBR** (Microsoft's GDK sample, clang-cl, all of DirectXTK12) | PBR with image-based lighting, HDR target and tone map, BC textures and cube maps, sprite fonts, the thread pool | runs, null back end | the sample's scene, stable frame to frame, no WebGPU error, ~2 fps on SwiftShader (`tests/gfx/pbr.sh`) |
| **Doom as a console title** (`fixtures/doom`: doomgeneric + Freedoom on D3D12.X, GameInput, XAudio2, XGameSave; llvm-mingw) | D3D12.X → WebGPU, `PresentX` and the frame clock, sound, keyboard / gamepad, saves that survive a reload | `-timedemo` frames equal native on every deterministic frame (null GPU back end) | frames **read back from WebGPU** equal native's; ~35 fps on SwiftShader; video: `fixtures/doom/video.sh` |
| **Tag: The Power of Paint** (2008; the user's own copy, `recipes/tag`) | Direct3D 9 shaders (shader model 2) as WGSL, Microsoft's own D3DX from its installer lifted beside it, FMOD Ex | its 16 effects compile at run time to the same 46 shaders as under Wine; FMOD Ex's calls as Wine's | its first level within 1.4 of 255 of Wine's screenshots on average; it plays into its second level ([tag.md](tag.md)) |
| **Desire** (DESI:Re v0.1, 2026; `recipes/desire`) | a Direct3D 12 game on its author's own engine, MSVC's FH4 C++ exceptions, HLSL compiled when the image is built | given the seeds a Wine run drew, it builds Wine's maze byte for byte | logo to ending; its still screens within 0.3 of 255 of Wine's ([desire.md](desire.md)) |
| **fastfetch 2.71.0** (built from source, static, musl) | the Linux runtime: system calls, files, a generated `/proc` | prints `recipes/fastfetch/traces/pipe.txt` in strict mode, from the wasm and from its image | its logo and colours in the page ([fastfetch.md](fastfetch.md)) |
| **neofetch 7.1.0** on bash and busybox | fork, exec, pipes: 83 processes, each in a worker with memory of its own | prints `recipes/neofetch/traces/pipe.txt` | the same ([neofetch.md](neofetch.md)) |
| **cowsay 3.8.4** on perl 5.44 | a large interpreter built with gcc 13 | six runs (cowsay and cowthink, several cows) print what native perl prints | the first run, the same ([cowsay.md](cowsay.md)) |
| **`fixtures/avx`** (C, musl, built for Zen 2) | AVX, AVX2, FMA and BMI as lifted | 120 results equal what the CPU printed (`tests/run.sh avx`) | – |
| **ps5-raymarch-techdemo v1.4** (PS5 homebrew, fetched by `tools/build-ps5-raymarch.sh`) | a native PS5 executable: imports by NID, ten threads, AVX2 and FMA, VideoOut, Pad, AudioOut | its smoke test: 180 frames, 3 scenes | the same; the three scenes, read back from WebGPU, match the project's own screenshots ([ps5.md](ps5.md)) |

Stockfish's `bench` is deterministic and its node count is recorded in the release commit (`Bench: 2497913`), so
matching it checks every lifted instruction it ran.
