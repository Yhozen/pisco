# The plan (2026-11-26)

PISCO (PISCO Imitates Systems, Compiles Originals) runs x86 titles in WebAssembly. It is four kinds of parts (a
fourth runtime, Linux x64, with fastfetch as its first title, does not fit the drawing):

```
PISCO     recompiler (crates/lifter)  ·  packages (pisco: recipe FROM a base -> title image)  ·  hosts (browser, Node)
runtimes  Xbox Series (GDK, D3D12.X)       Windows x64 (DXGI, D3D12, PC GDK)       Windows x86 (Win32, D3D9, DirectInput, FMOD)
packages  Doom as a console title, XCL's   Stockfish, SimpleTriangleDesktop,       Narbacular Drop, Tag (next)
          test title; goal: Minecraft      SimplePBR
```

- **The recompiler** turns a title's x86 or x86-64 code into C, once per build (`crates/lifter`, both widths; a
  Windows PE or a static Linux ELF).
- **Packages**: `pisco build` reads a recipe (`recipes/<title>/title.toml`) and the user's own copy of the title, and
  writes a title image (an OCI image layout, [images.md](images.md)) `FROM` one base.
- **Runtimes** are peers. Each imitates one system and is one base (`bases/<runtime>/1/base.toml`), which lists its
  files: the core (`rt/*.c`) every Windows and console title links, plus its own folder (and `rt/gdk/`, which the two
  64-bit Windows-family runtimes share). The Linux runtime links only the core's files that imitate no system
  (`rt/core.c`, `rt/clock.c`, `rt/builtins.c`) and its own folder. None of them is "the" runtime: what started as the goal, Minecraft on the Xbox Series runtime, is now that
  runtime's goal, one among several.
- **Hosts** run an image: the browser page (`web/browser/`, WebGPU) and Node (`web/run-node.mjs`, a null GPU).

## The runtimes

| Runtime | Base | Its files | Runs | Next | Its goal |
|---|---|---|---|---|---|
| Xbox Series | `base/xbox-series:1` | `rt/gdk/`, `rt/xbox-series/` | Doom (D3D12.X on WebGPU, XAudio2, GameInput, saves), XCL's console test title 50/50 | the rungs in [status.md](status.md#xbox-series) | Minecraft (Bedrock), the Xbox Series build |
| Windows x64 | `base/windows-x64:1` | `rt/gdk/`, `rt/windows-x64/` | Stockfish 19, Microsoft's SimpleTriangleDesktop and SimplePBR | Desire, a small D3D12 game on its own engine ([desire.md](desire.md)) | desktop D3D12 titles |
| Windows x86 | `base/windows-x86:1` | `rt/windows-x86/` | Narbacular Drop, playable; Tag: The Power of Paint into its second level (Direct3D 9 shaders as WGSL, D3DX's effects, FMOD Ex) | Tag played by hand on a real GPU (T6's check), then its later levels | 32-bit Direct3D 9 games of the 2000s |
| Linux x64 | `base/linux-x64:1` | `rt/linux-x64/` | fastfetch 2.71.0; neofetch 7.1.0 on bash and busybox, its processes forked and executed; cowsay on perl 5.44 ([fastfetch.md](fastfetch.md), [neofetch.md](neofetch.md), [cowsay.md](cowsay.md)) | threads, signals, a file system that can be written | statically linked command-line programs |

A new runtime is a new folder and a new base; a new title of an existing runtime is a recipe.

## The rules a change follows

1. **A title links only its base.** The base lists the runtime's files; nothing else does (`tests/run.sh layout`
   checks it). A recipe's `libraries` add files of its base only. The licence map (`LICENSING.md`) is by base.
2. **Built by `pisco`.** A title is a recipe plus the user's copy; `pisco build` makes its image the same way every
   time (two builds with empty caches give the same digest: `tests/run.sh images`). Workarounds for one title are
   recipe options (`[quirks]`, `[cpu]`, `[registry]`, `[dll]`), not changes everyone gets.
3. **Checked by `tests/run.sh`.** Every check has a step there; a step whose inputs (a game, an SDK) are not on the
   machine says so in one line and is skipped. Windows titles are compared with Wine running the same files (the
   oracle, `tools/oracle-wine/`); console behaviour follows XCL's findings; a Linux title is compared with itself,
   run on the Linux that builds it, for everything that does not depend on the machine.
4. **Nothing of a title in the repository.** No game files, installers, Microsoft DLLs, lifted output, shader dumps
   or screenshots: `out/` is ignored, and a recipe names a file by its sha256.
5. **Licences checked.** Our own code is GPL-2.0-or-later with SPDX headers; `reuse lint` passes; code from others
   keeps its licence and has a row in `THIRD_PARTY.md`.
6. **One pull request per milestone, none straight to `main`;** the docs (this file, `status.md`, `journey.md`, the
   title's page, the progress map) change in the same pull request as the code.

## Now (2026-11-26)

| # | Milestone | State |
|---|---|---|
| A1 | the 32-bit side of the shared runtime made right (commit 2f16767's maths, printf, SList), the samples' tests reachable | done |
| A2 | a folder per runtime | done |
| A3 | a base per runtime, one list of the runtime's files, recipes for every base, `[quirks]` | done |
| A4 | these docs; the progress map by runtime | done |
| A5 | CI for the tests that need no game files, and `reuse lint` | done |
| T0 to T5 | Tag: The Power of Paint on the Windows x86 runtime: census and oracle, lift and start, the main loop, the shader translator, pixels, sound and input | done ([tag.md](tag.md#progress)) |
| T6 | Tag playable: played by hand on a real GPU through level 3 | done |
| L0 to L4 | a fourth runtime, Linux x64: the core's neutral parts apart, the lifter reads an ELF, a C library starts and finds a machine, fastfetch in Node and in the page | done ([fastfetch.md](fastfetch.md#progress)) |
| N0 to N3 | Linux processes (fork, execve, wait4, pipes), images of several programs, neofetch on bash and busybox in Node and in the page | done ([neofetch.md](neofetch.md#progress)) |
| D0 | Desire (DESI:Re v0.1) on the Windows x64 runtime: census, recipe (`pisco build` reads a zip and adds the user's C++ runtime DLLs), and the oracle (Direct3D 12 under Wine on lavapipe) | done ([desire.md](desire.md#progress)) |
| D1 | Desire lifted and started: the first frame in Node, strict, with Wine's seeds drawing Wine's maze | done ([desire.md](desire.md#progress)) |
| D2 | MSVC's FH4 exception tables in the lifter | done ([desire.md](desire.md#progress)) |
| D3 | Desire's main loop in Node on the virtual clock: keyboard and window messages for 64-bit titles | done ([desire.md](desire.md#progress)) |
| D4 | Desire's pixels in Chrome against Wine's | done ([desire.md](desire.md#progress)) |
| D5 | Desire playable by hand: the docs and how to play it are done; playing it by hand on a real GPU is still to do | under way |
| B0 | experimental, optional: development on boat.dev sandboxes (a snapshot with everything built, the tests on several sandboxes at once); cowsay ported that way | done ([dev-machines.md](dev-machines.md)) |
