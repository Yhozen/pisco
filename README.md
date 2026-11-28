<p align="center"><img src="docs/img/pisco.png" alt="PISCO logo: a glass of pisco on ice" width="160"></p>

<h1 align="center">PISCO</h1>

<p align="center"><b>Run anything, anywhere™</b><br>PISCO Imitates Systems, Compiles Originals.</p>

Pisco is distilled from wine, and this project owes a lot to [Wine](https://www.winehq.org): Windows PC apps are
checked against it screen by screen and call by call.

PISCO runs programs written for other systems in WebAssembly, in a browser tab or in Node. It recompiles their
x86 code ahead of time and supplies the system they were written for. "Anything, anywhere" is the goal; the
runtimes below are how far it has got. Nothing of an app is in this repository: you bring your own copy.

**[Changelog, with videos](https://yhozen.github.io/pisco/)** ·
[status and next steps](docs/status.md) · [the plan](docs/plan.md) · [the journey so far](docs/journey.md)

## Runtimes

A runtime imitates one system. An app's image is built FROM one of them (its base); what a recipe and an image
are is [the specification](docs/spec/README.md).

| Runtime (base) | Imitates | Runs today | Its goal |
|---|---|---|---|
| **Xbox Series** (`base/xbox-series`) | the GDK's console build: kernelx, XMem, XGameRuntime, D3D12.X, DirectStorage, XAudio2 | Doom as a console app; XCL's console test app, 50/50 | Minecraft (Bedrock), the Xbox Series build |
| **Windows x64** (`base/windows-x64`) | 64-bit Windows desktop: DXGI, D3D12, WIC, the PC GDK, the MSVC runtimes | Stockfish 19 (official build); Microsoft's SimpleTriangleDesktop and SimplePBR; [Desire](docs/desire.md) (2026), logo to ending | desktop D3D12 titles |
| **Windows x86** (`base/windows-x86`) | 32-bit Windows desktop of the 2000s: Win32, GDI, Direct3D 9, OpenGL 1.1, DirectInput, DirectSound, FMOD | [Narbacular Drop](docs/narbacular-drop.md) (2005), playable; [Tag: The Power of Paint](docs/tag.md) (2008) into its second level; [Medal of Honor: Allied Assault's demo](docs/mohaa.md) (2001, OpenGL), logos to its level | 32-bit Direct3D 9 and OpenGL games of the 2000s |
| **Linux x64** (`base/linux-x64`) | Linux at its system calls, for statically linked programs: files, memory, the clock, a generated `/proc`, fork / exec / pipes | [fastfetch](docs/fastfetch.md) 2.71.0; [neofetch](docs/neofetch.md) 7.1.0 on bash and busybox; [cowsay](docs/cowsay.md) 3.8.4 on perl | statically linked command-line programs |
| **Microvm x64** (`base/microvm-x64`) | a small x86-64 machine: long mode, page tables, a local APIC, a serial port, so that a kernel is the app | [Linux](docs/linux.md) 6.18 and busybox, lifted: `/init`'s programs print what they print on QEMU | Linux itself, lifted, and its programs |
| **PS5** (`base/ps5`) | the PS5's system libraries for homebrew: libkernel, VideoOut, Pad, AudioOut | the [raymarching tech demo](docs/ps5.md) v1.4 (CPU-drawn, AVX2): its three scenes, pad and music | PS5 homebrew |

Running Minecraft through the Xbox Series runtime is where the project started; it is now that runtime's goal, one
among several.

## Progress

[![graphics ladder](https://yhozen.github.io/pisco/badge-ladder.svg)](https://yhozen.github.io/pisco/progress.html) [![D3D12 methods](https://yhozen.github.io/pisco/badge-d3d12.svg)](https://yhozen.github.io/pisco/progress.html) [![imports](https://yhozen.github.io/pisco/badge-imports.svg)](https://yhozen.github.io/pisco/progress.html) [![XCL checks](https://yhozen.github.io/pisco/badge-xcl.svg)](https://yhozen.github.io/pisco/progress.html)

[![progress map](https://yhozen.github.io/pisco/progress.svg)](https://yhozen.github.io/pisco/progress.html)

What each test app checks, with its numbers: [docs/how-it-works.md](docs/how-it-works.md#what-the-test-apps-check).

## How it works

```
app (PE or ELF) ── lifter (once per build) ──► C ── clang ──► app.wasm ◄── linked with one runtime (rt/)
recipe + your copy of the app ── pisco build ──► app image (OCI layout) ──► a host: the browser page, or Node
```

- **The recompiler** (`crates/lifter`) turns an app's x86 or x86-64 code into C, one function per guest function,
  once per build. Nothing is interpreted or translated while the app runs.
- **A runtime** (`rt/`) answers what the app asks of its system: Win32 and Direct3D calls, Linux system calls,
  the PS5's libraries. Graphics go to WebGPU, sound to an AudioWorklet, files to the image and OPFS.
- **`pisco`** (`crates/pisco`) builds a recipe and your own copy of an app into an app image, the same way
  every time, and runs it.
- **The hosts** (`web/`) are the browser page and Node. A guest thread is a worker over shared memory.
- **Checked against the real thing**: Windows apps against Wine running the same files, Linux apps against
  themselves run natively, console behaviour against [XCL](https://github.com/ivall/xcl)'s findings.

The details (each part of the runtime, the lifter, threads, exceptions, every setting):
[docs/how-it-works.md](docs/how-it-works.md).

## Try it

Needs Rust and Node 24; `tools/setup-toolchain.sh` fetches the rest.

```sh
tools/setup-toolchain.sh wasi mingw musl                   # wasi-sdk, llvm-mingw, musl, into ~/tools
cargo build --release -p pisco -p pisco-lifter
tools/build-fastfetch.sh                                   # fastfetch from its source, a static Linux program
target/release/pisco build recipes/fastfetch               # -> out/images/fastfetch
SW_TTY=100x30 target/release/pisco run out/images/fastfetch
tests/run.sh                                               # every test app; a step without its inputs is skipped
```

A game is built the same way from its recipe and your own installer
(`pisco build recipes/narbacular-drop --source <the installer>`). More:
[building and running](docs/how-it-works.md#building-and-running), and each app's page.

## Docs

- [The plan](docs/plan.md): the structure, the runtimes, the rules a change follows.
- [Status and next steps](docs/status.md), by runtime. [The journey so far](docs/journey.md): what was built,
  measured and learned.
- [How it works](docs/how-it-works.md) · [app images and `pisco`](docs/images.md) ·
  [wasm32 or wasm64](docs/memory.md) · [hosts: the browser, its limits and a native host](docs/hosts.md)
- Apps: [Narbacular Drop](docs/narbacular-drop.md) · [Tag](docs/tag.md) · [Desire](docs/desire.md) · [MOHAA's demo](docs/mohaa.md) ·
  [fastfetch](docs/fastfetch.md) · [neofetch](docs/neofetch.md) · [cowsay](docs/cowsay.md) ·
  [the PS5 demo](docs/ps5.md) · [first run of a console app](docs/first-run.md)
- [Developing on boat.dev sandboxes](docs/dev-machines.md) · [writing a changelog entry](changelog/README.md)

## Licence

GPL-2.0-or-later for our own code; code from other projects keeps its own licence (the shader tools from
[XCL](https://github.com/ivall/xcl) are GPL-2.0-only). See [LICENSING.md](LICENSING.md).
