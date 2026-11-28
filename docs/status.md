# Status and next steps, by runtime (2026-11-27)

PISCO is one recompiler, one package format and several runtimes ([plan.md](plan.md)). Each runtime has its own
apps, its own next steps and its own goal:

| Runtime | Runs | Next | Goal |
|---|---|---|---|
| [Xbox Series](#xbox-series) | Doom as a console app, XCL's console test app (50/50) | D3D12.X shaders from RDNA2 code | Minecraft, the Xbox Series build |
| [Windows x64](#windows-x64) | Stockfish 19, SimpleTriangleDesktop, SimplePBR; Desire, logo to ending, in Node and headless Chrome | Desire played by hand on a real GPU | desktop D3D12 titles |
| [Windows x86](#windows-x86) | Narbacular Drop, playable; Tag: The Power of Paint into its second level | Tag played by hand on a real GPU, then its later levels | 32-bit Direct3D 9 games of the 2000s |
| [Linux x64](#linux-x64) | fastfetch 2.71.0; neofetch 7.1.0 (bash, busybox); cowsay 3.8.4 (perl) | threads, signals, writes | statically linked command-line programs |
| [PS5](ps5.md) | the raymarching tech demo (CPU-drawn, AVX2): its three scenes, pad and music in the browser | more system functions, a GPU (AGC) app | PS5 homebrew |
| [Microvm x64](#microvm-x64) | Linux 6.18 and busybox lifted, as an image: `/init`'s programs print what they print on QEMU; a shell typed into in a terminal or the page; a virtio disk; a network in Node | the network in the page | Linux itself, lifted, and its programs |

## Levels

How far each recipe is checked, by the levels of [spec/sessions.md](spec/sessions.md#levels): **builds** (its
inputs give its files, the same twice; *locked* when its `recipe.lock` holds their tree digest), **starts** (its first
frame, or what it prints, as on the oracle), **plays** (its sessions' checkpoints and screens against the oracle's).

| Recipe | Base | Level | Oracle | Checked by `tests/run.sh` |
|---|---|---|---|---|
| hello32 | windows-x86 | starts | Wine | `images`, `win32` |
| narbacular-drop | windows-x86 | plays | Wine | `nd`, `ndgfx`, `ndplay`, `images` |
| tag | windows-x86 | plays (into level 2) | Wine | `tag`, `tagplay` |
| mohaa | windows-x86 | plays (menus and the level) | Wine | `mohaa`, `mohaagfx` |
| hello64 | windows-x64 | starts | Wine | `images` |
| simple-triangle-desktop | windows-x64 | starts | Wine | `st` |
| simple-pbr | windows-x64 | starts | Wine | `pbr` |
| desire | windows-x64 | plays | Wine | `desireplay`, `desire` |
| doom | xbox-series | plays (its timedemo, frame by frame) | the same Doom built natively | `doom`, `images` |
| linux-hello | linux-x64 | starts | this Linux | `linux`, `images` |
| fastfetch | linux-x64 | starts | this Linux | `fastfetch` |
| neofetch | linux-x64 | starts | this Linux | `neofetch` |
| cowsay | linux-x64 | starts | this Linux | `cowsay` |
| linux | microvm-x64 (experimental) | starts (`/init`'s programs) | QEMU | `kernel` |

No recipe is locked yet: the four whose inputs are pinned (narbacular-drop, tag, mohaa, desire's download) are
locked with `pisco build --lock` from the user's own copies; desire's C++ runtime input is not pinned.

## Shared: the recompiler, the core runtime, the packages

- **Gate A (CPU and OS layer on synthetic apps): passed** in Node and headless Chrome — our own apps
  (Rust, C++ with libc++/libunwind, a multi-image app with a DLL) and XCL's console test app (46 of its
  50 checks in Node, where the 4 that read pixels back cannot pass; all 50 in Chrome, below).
- wasm32 vs wasm64 measured (`docs/memory.md`).
- **App images**: `pisco build` makes an image FROM one base per runtime (`bases/`), the only list of the
  runtime's files; one app of each base is built twice with empty caches to equal digests (`tests/run.sh images`).

## Xbox Series

- **Graphics, sound, input and saves on a console app: Doom** (step 3 of the graphics ladder; `fixtures/doom`,
  `docs/journey.md` §3.9). D3D12.X → WebGPU in headless Chrome, XAudio2 → AudioWorklet, GameInput from the page,
  XGameSave's folder kept in OPFS. Its `-timedemo` frames, read back from WebGPU, equal native doomgeneric's on
  every frame native draws deterministically. Video: `fixtures/doom/video.sh`.
- **XCL's console test app: 50 of 50 checks** in headless Chrome (step 4 of the ladder; `docs/journey.md`
  §3.10, `tests/gfx/xcl.sh`): D3D12.X's UMA memory and console methods, DirectStorage, MFPlat, XG, XAudio2
  with XMA2 / Opus. What passing does not cover is listed in §3.10 (no raytracing, our own XG layout, no
  RDNA2 shaders yet).

### Next, toward its goal (Minecraft, the Xbox Series build), in the order it probably needs them

The graphics ladder is done up to Minecraft (steps 1–4: SimpleTriangleDesktop and SimplePBR, now Windows x64
apps; Doom; XCL's checks). What is left before and during step 5:

1. **The Minecraft files** (`docs/first-run.md`): every Minecraft-specific answer waits on them, among them
   which imports are missing, which shader stages it uses and whether it needs wasm64. `tools/census.sh
   <folder>/Minecraft.Windows.exe` answers those in minutes without running the game.
2. **Shaders, RDNA2 → WGSL**: the console ships GPU machine code. The census and the decoder exist
   (`crates/shaders`, from XCL); the translator is the big piece, with XCL's HLSL emitter (`gpu/recomp/emit.rs`)
   and its instruction tests as the reference. Steps 1–4 do not exercise it: their shaders are DXIL.
3. **WebGPU's limits**: no mesh shaders, no bindless heaps (XCL saw Minecraft use eight heaps of 73,727
   descriptors), no 64-bit atomics. The answer (per-draw bindings, compute emulation) is decided after the census.
4. **MSVC C++ exceptions**: FH3 works with the real `vcruntime140` (`docs/journey.md` §3.20), and FH4, which
   current `cl.exe` emits, with the real `vcruntime140_1` since Desire (MSVC 14.38) threw and caught through it
   ([desire.md](desire.md#progress), D2).
5. **Audio**: PCM, XMA2 and Opus voices play (FFmpeg); each XMA buffer is decoded whole when it is submitted,
   so XMA streamed across buffers is not handled, and XAudio2 has no device callbacks yet.
6. **Speed**: lifted code runs ~1.3–6× native on our micro-benchmarks. Next: keep flags lazily, register
   locals across calls, AVX2 paths (the virtual CPU reports up to SSE4.2 and POPCNT, no AVX), wasm64 only
   when needed.

Done since the first version of this list: D3D12.X (Doom, XCL's checks), GameInput readings (keyboard and
gamepad, from the page), saves kept in OPFS in the browser and in a folder in Node (`SW_SAVES`).

## Windows x64

- **A real 64-bit Windows program** (Stockfish 19's official build, MinGW-compiled, 103 MB) runs bit-exactly:
  `bench` searches 2,497,913 nodes, the signature published with the release, at ~106 k nodes/s in Node and
  ~111 k in Chrome.
- **The graphics ladder's steps 1–2**: SimpleTriangleDesktop (both builds, through DXGI, with Microsoft's C++
  runtime DLLs lifted and MSVC C++ exceptions) and SimplePBR (DirectXTK12, PBR with image-based lighting, HDR)
  render in headless Chrome (§3.20, §3.21).
- **Desire** (DESI:Re v0.1, a maze escape on its author's C++20 / Direct3D 12 engine), the first third-party
  D3D12 game: census and oracle done ([desire.md](desire.md)). `pisco build` makes its image from the zip and the
  user's C++ runtime DLLs; under Wine it runs on vkd3d and lavapipe, and Wine's log gives the seeds of its random
  maze. In Node, strict, it now runs from its logo to the maze and its end pictures, with Wine's start-up calls
  and, from Wine's seeds, Wine's maze (D1: a window with a message queue, WIC on stb_image, D3D12 Memory
  Allocator's methods, shaders compiled by `pisco build`); its C++ exceptions, MSVC's FH4, are caught (D2).
  Played in Node on the virtual clock with the keyboard (D3): logo, GAME OVER, the goal, with Wine's apps and
  Direct3D 12 methods; in headless Chrome its still screens are within 0.3 of 255 of Wine's (D4). Next: played
  by hand in a desktop browser on a real GPU.

## Windows x86

### Narbacular Drop, the first app

An ordinary 32-bit Windows game, by the same route (recompiled ahead of time, no
Wine at run time). Wine is the oracle it is compared with. What the game needs, measured:
[narbacular-drop.md](narbacular-drop.md). Two halves:

- **Compatibility layer**: a 32-bit mode in the lifter, a runtime built per guest width, a Win32 desktop layer
  (`rt/windows-x86/`: user32, gdi32, registry, DirectInput 8, Direct3D 9, FMOD 3), Direct3D 9 on the existing GPU
  ring with shaders generated by the runtime.
- **Recipes and images**: a recipe per app (`recipes/<app>/recipe.toml`: `from` a base, the user's
  installer by sha256, CPU features, registry values, DLL choices) built into an OCI image layout: ordered
  file layers that map onto `rt/vfs.c`'s overlay, the module, content-addressed so lifting and compiling are
  cached. An app's own layers are built locally and never published.

| # | Milestone | Passes when | State |
|---|---|---|---|
| M0 | Groundwork: toolchain script, licence map, the Wine oracle, the census | fixtures and Doom pass again; the oracle replays the session from a fresh prefix; the census answers the open questions | **done** (2026-11-24) |
| M1 | 32-bit lifter and CPU | all 91 indirect jumps of the game resolved, only MMX / SSE / 3DNow left unlifted; `hello32` and an x87 app print what they print under Wine | **done** (2026-11-24): `hello` as a 32-bit program passes 7/7 and `fixtures/x87` prints the 27 lines it prints under Wine, in Node and in headless Chrome. On the game: 349,540 instructions lifted, all 88 jump tables recovered, 8,279 MMX / 3DNow instructions left out. 3 indirect jumps stay unresolved: they are not tables but the C++ exception machinery's jumps (see below) |
| M2 | Win32 process for a 32-bit guest | the game reaches `Direct3DCreate9` under `SW_STRICT=1` with the same import sequence as under Wine | **done** (2026-11-24): the game runs its C runtime's start-up, reads `File.Ore`, registers its class, creates and shows its window, and calls `Direct3DCreate9` from the same address as under Wine. Its calls up to there are Wine's: 81 steps, repeats folded (`tests/run.sh nd`). Our Direct3D 9 then declines; the game shows its own message box and exits cleanly |
| M3 | Headless main loop (null GPU, DirectInput, silent FMOD, virtual clock, session player) | the session reaches the level in Node; Direct3D method counts equal Wine's | **done** (2026-11-24): in Node, in strict mode, the game plays the `menu-to-level` session to its end on a virtual clock: 6,256 frames in 4 s, exit code 0, the same log on every run (`tests/run.sh nd`). Frame by frame it makes Wine's Direct3D 9 calls: 27 kinds of frame are identical on both sides and cover 94 % of Wine's frames; the others are the level's, which are Wine's minus the 9 text draws a frame D3DX makes under Wine and not here, where GDI draws no glyphs yet ([narbacular-drop.md](narbacular-drop.md)) |
| M4 | Pixels: Direct3D 9 on WebGPU | checkpoint screenshots match Wine's in headless Chrome | **done** (2026-11-24): in headless Chrome (WebGPU on SwiftShader) the session plays to its end in strict mode and a PNG is saved at each of its 13 checkpoints (`tests/run.sh ndgfx`). The eight still screens equal Wine's to one level in 255 once Wine's D3DX runs the code ours runs; the menu and the level differ from Wine's by less than two of Wine's own runs differ from each other. Second part: GDI draws text with Liberation Sans (built in, OFL) through stb_truetype, measured and drawn as Wine's GDI does (`tests/run.sh gdi`: every metric, ABC width, advance and placement equal), so D3DX's level app and hint appear where Wine has them, and the level's frames make Wine's Direct3D calls (73 kinds of frame identical, covering 11,851 of Wine's 11,857 frames). Left: Wine's TrueType hinting (glyph edges differ by up to half a pixel) ([narbacular-drop.md](narbacular-drop.md)) |
| M5 | Input, sound, persistence | a scripted session places portals; music in the captured video; files survive a reload | **done** (2026-11-24): the oracle has a sound device (a PipeWire null sink, recorded). FMOD 3 plays on XAudio2's mixer, factored out (`rt/mixer.c`), with WAV, Ogg Vorbis (stb_vorbis) and MP3 (minimp3) read through the game's own file functions; on the virtual clock the mix is the same on every run (`SW_AUDIO_WAV`). Against Wine's `+snoop`: the same functions, the first 177 non-per-frame calls identical with their results (channel handles included) up to the game's first random choice of a sound, per-frame calls equal phase by phase, sounds started by family equal but for footsteps and torch pops, whose times are random; the first level's loudness within 0.4 dB of Wine's recording. The page feeds the keyboard (scan codes) and the mouse under Pointer Lock. `level1-portals` places both portals and walks out of the cage, in Node and in Chrome (its level views within 1-2 levels in 255 of Wine's on average); `system.opt`, written on Quit, survives a rerun (`SW_SAVES`) and a reload (OPFS), equal to Wine's to the byte (`tests/run.sh ndplay`). Video with sound: `recipes/narbacular-drop/video.sh` (108 s, its audio track at -26.4 dB mean, -2.5 dB peak) ([narbacular-drop.md](narbacular-drop.md)) |
| M6 | Recipes and images | `pisco build` + `pisco run` from a clean checkout; identical digests on a second build | **done** (2026-11-25): `pisco` (`crates/pisco`) builds a recipe and the user's installer (sha256-checked, extracted with innoextract) into an OCI image layout with our own media types: the base's layer, one per library, the game's, the module, and a config with the exe, display, CPU, DLL choices and registry ([images.md](images.md)). Two builds with empty caches give the same manifest digest, for hello32 and Narbacular Drop; hello32 (FROM the same base) shares its base layer blob; a registry-only change rebuilds in about a second with nothing lifted, compiled or linked, the same module blob; the registry reaches advapi32 at run time from the config. `pisco run` plays menu-to-level in Node with the 13 checkpoints of `tests/run.sh nd`, and in headless Chrome with the still screens within 0.11 of 255 of the oracle's (`tests/run.sh images`). The installer's files equal the Wine install's but for `fmod.dll` (the oracle's is UPX-unpacked) |
| M7 | Playable in the browser | a full playthrough by hand on a real GPU | **done as a proof of concept** (2026-11-25): the image of M6, served by `tools/serve.mjs` and opened in Chrome on an M1 MacBook Pro, is played by hand: "so smooth", and mouse-look under Pointer Lock feels right. Measured with the page's `&stats=1` counter (the app's own presents a 3 s interval, not capped at the display's rate): 294 to 355 frames a second in lighter views (142 to 184 draws a frame), 103 to 174 in the heaviest (318 to 322 draws a frame), against 10 to 20 on the test machine's SwiftShader; no WebGPU error and no draw left out over 4.4 M draws. A full playthrough and a scripted level 1 were dropped from the milestone: a playable game was the proof of concept asked for. Not checked by hand: Esc and a re-click under Pointer Lock, the later levels |

Decided by the census: no x86 exception dispatch, no shader translation, no clip planes or stencil for the
first level; gdi32 text is needed (memory DCs, DIB sections, a real font); Uniscribe is not (D3DX falls back
to GDI). The runtime's stack sits where the game's fixed image must load (`0x400000`), so the 32-bit profile
needs its own memory layout.

What is left for Narbacular Drop, none of it needed to play: a scripted Quit from the pause menu (its items take a
click in a band a few pixels high, and a fast scripted move lands differently on Wine's clock and ours; the scripted
check quits from the main menu; by hand it was not tried), ADPCM WAVs, Doppler and reverb, deleting the game's own files across
runs, Wine's TrueType hinting (glyph edges differ by up to half a pixel), bold faces and other font families, a full
playthrough by hand and a recording of one. The installer's `fmod.dll` is never run: the runtime's FMOD answers.

What Windows desktop titles have since M6: a recipe `FROM` a base (`bases/windows-x86/1`) with named libraries, built
by `pisco` into an app image whose layers make the C: drive and whose config gives the registry to advapi32
([images.md](images.md)); both hosts run an image (`web/image.mjs`).

What they had after M5: FMOD 3 (`rt/windows-x86/fmod3.c`: samples, streams, channels as FMOD numbers them,
3D with several listeners) on the runtime's one sound output (`rt/mixer.c`, shared with XAudio2; mixed on the app's
thread under a virtual clock), sound files decoded by `rt/windows-x86/audio_decode.c`; input from the page (keyboard,
Pointer Lock mouse) through a ring `rt/input.c` drains when DirectInput reads; a writable top layer over the
app's own folder that the host keeps (`SW_SAVES/@game` in Node, OPFS in the browser). Not yet: ADPCM WAVs, Doppler,
reverb, the other FSOUND functions (none of the 33 imports is missing, but FMOD has hundreds), deleting an app's own
files across runs.

What they had after M4's second part: gdi32 text drawn for real (`rt/windows-x86/font.c`): every
face is Liberation Sans, regular or italic, built into the module (`#embed`, SIL Open Font License), measured as
Wine's GDI measures it and rasterised with stb_truetype (17 grey levels blended as Wine blends them, or one bit),
into the DIB section a memory DC holds: `TextOutA`, `ExtTextOutA/W` (glyph indices, opaque and clipped
rectangles, alignment, advances), `GetCharacterPlacementA/W`, `GetCharABCWidthsI`, `GetCharWidthI`,
`GetFontUnicodeRanges`, `GetTextMetricsA/W`. A 32-bit app's uninitialised heap blocks are filled with `0xCD`, the
same on every host. Not yet: bold faces (no file: bold asks get the regular face), other families (Times New Roman,
Courier New), TrueType hinting, rotated text.

What they had after M4's first part: Direct3D 9 drawn. The device's state at each draw
becomes fixed-function shaders written by the runtime as WGSL (`rt/windows-x86/d3d9_ffp.c`: a pure function of a state
key, checked natively with naga), a pipeline, bindings and data, sent as explicit records on the same GPU ring
D3D12 uses (`rt/windows-x86/d3d9_gpu.c`, `rt/gpu_ring.c`, `rt/d3d12_cmd.h`) to the one back end (`web/gpu/backend.js`
with `explicit.js`; in Node `null.js` checks every record and draws nothing). The browser host plays sessions on
the virtual clock and saves a PNG at each checkpoint; `tools/oracle-wine/shot-compare.py` sets them against the
oracle's. Not drawn yet, because no app here has asked: texture stages past the first, shaders, stencil, user
clip planes, table fog, `DrawPrimitiveUP`, formats other than `A8R8G8B8` / `X8R8G8B8`, a Clear of part of a target.

What they had at M3: Direct3D 9 as a null device (`rt/windows-x86/d3d9*.c`: every object with
its references, state and bytes, state blocks, statistics; its adapter, format support, capabilities and a new
device's state are the oracle's, asked with `tools/oracle-wine/d3d9probe.c`), DirectInput 8 (mouse and keyboard,
immediate and buffered), an input state with a session player (`SW_SESSION`), the app's clock (`rt_now_us`;
virtual with `SW_CLOCK`), FMOD 3 as on a machine without a sound device, gdi32 with memory DCs, DIB sections and
fonts whose text is measured by estimate and not drawn. Not yet: real glyphs, sound, input from the page, device
loss and `Reset`, shaders, cube textures, queries.

What they had at M2: a build profile of their own (`PROFILE=win32`: the core of `rt/`
plus `rt/windows-x86/`, without the console's libraries); `rt/windows-x86/` with the start-up side of kernel32 (version,
environment, code page 1252 tables), user32 (classes, one window, the messages of creating, sizing and showing
it, sent to the title's window procedure), an empty registry, GDI objects, and a Direct3D 9 that only reports;
the loader sets an image's security cookie, as Windows does; a recipe (`recipes/<app>/recipe.toml`) gives the
build the virtual CPU's features, registry values and the shipped DLLs the runtime answers for.

What 32-bit apps have since M1: the lifter reads PE32 and lifts 32-bit code (`app.env` tells the build the
width); the runtime is built per guest width (`-DGUEST_BITS=32`: arguments on the stack, stdcall clean-up from
`tools/stdcall-sizes.sh`, the TEB on `fs`, 4-byte loader writes); a low image gets a gap between the runtime's
stack and its data ([memory.md](memory.md)). Not yet for 32-bit guests: exception dispatch (the three
unresolved jumps of the game are `_JumpToContinuation` and friends in its C runtime, reached only when a C++
exception is thrown), MMX and 3DNow (left unlifted: a recipe's `[cpu]` leaves them out of CPUID, as Narbacular Drop's and Tag's do, so apps
take other paths), `fbld` / `fbstp`, the x87 tag word and 80-bit precision. (Imports were then only the four the two
test apps call; Narbacular Drop and Tag have brought the rest.)

### Tag: The Power of Paint (2008), the second app

DigiPen's game draws everything through 16 D3DX effects compiled at run time, and plays its sound through FMOD Ex.
Built by `pisco build recipes/tag` from the installer, with Microsoft's `d3dx9_35.dll` from the installer's cab
lifted beside the exe. Measured ([docs/tag.md](tag.md) has each milestone):

- start-up: the same calls as under Wine up to `Direct3DCreate9` (699 steps);
- D3DX's HLSL compiler, as lifted code, writes the same 46 bytecodes as under Wine; the Direct3D 9 shader translator
  (vs/ps 1.1 to 2.x to WGSL) takes all of them;
- `menu-to-level1` in Node, strict: the same 76 Direct3D methods as Wine's trace, 96 counted arguments with the same
  values; in headless Chrome, level 1 within 1.4 of 255 of Wine's screenshots on average (`tests/run.sh tag`);
- `level2-paint` in Node, strict: level 2, the gun, paint and erase; FMOD Ex's first 531 loads, plays and settings as
  Wine's; `TAG.ini` kept and read back (`tests/run.sh tagplay`).

Still to do: playing it by hand on a real GPU (T6 of [the plan](plan.md)); the later levels.

### Medal of Honor: Allied Assault's demo (2001), the third app

The first OpenGL game, on id Tech 3 (Ritual's version of the Quake III engine), with game code in DLLs the engine
loads itself and Miles Sound System 6.1c beside it ([mohaa.md](mohaa.md)). What it brought to the runtime:

- **The lifter**: MMX (eight registers of their own, `c->mm[]`; each operation the SSE2 one on the low half, the
  high unpacks and the packs arranged to agree; `fixtures/mmx` prints Wine's 63 lines), `push`/`pop` of DS, ES and SS
  in a flat address space, and Miles' hand-written mixer found as code (balanced segment saves are not data).
- **Images**: a `wise` extract step (a Wise installer and the InstallShield 5 / 6 cabinets inside it, read without
  running either), `[code] load` (DLLs an app loads itself from its folder or below it, lifted with it), `[files]`
  (the settings a setup step writes).
- **The process**: DLLs loaded on demand (DllMain at LoadLibrary, PROCESS_DETACH at the last FreeLibrary, the
  sections read again on a second load), files still open at the end saved as Windows would have written them,
  wsock32's ordinals that differ from ws2_32's (10, 11, 12).
- **user32**: input as window messages (`WM_KEYDOWN`, `WM_CHAR` from TranslateMessage, the mouse's), `GetMessageA`,
  timers, the cursor (moved by the page's motion under Pointer Lock, set by `SetCursorPos`), display modes.
- **OpenGL 1.1** (`[libraries.opengl32]`, `rt/windows-x86/opengl32.c`): immediate mode and client arrays packed into
  one vertex stream, two texture units with their environments as WGSL (`gl_ffp.c`, every key validated by naga in
  `tests/gfx/gl-ffp.sh`), alpha test, fog, polygon offset, one clip plane, `glDrawPixels`, `glReadPixels`, pixel
  formats and `SwapBuffers` in gdi32. Not yet: lighting, texture coordinate generation, stencil, display lists,
  `glCopyTexSubImage2D`.
- **DirectSound** (`[libraries.dsound]`): software buffers on the runtime's mixer (no hardware buffers, no 3D).

| # | Milestone | State |
|---|---|---|
| M0 | census, recipe, oracle | **done** (2026-11-27): the build's 801 files equal Wine's install |
| M1 | lift and start in Node | **done** (2026-11-27): strict, on the virtual clock, the engine plays its logos and reaches the menu (2,649 frames, 48,715 draws, no record the null back end finds inconsistent); its console log is Wine's but for what the machines are (CPU, renderer, pixel formats, sound) |
| M2 | OpenGL's pixels in Chrome | **done** (2026-11-27): the menu within 0.4 of 255 of Wine's screenshot on average, the menus and the briefing within 0.1 |
| M3 | Sound: Miles on DirectSound | **done** (2026-11-27): the level session's sound within 0.3 dB of Wine's recording on average, the same course (correlation 0.93) |
| M4 | Played by hand | under way: how to play it is in [mohaa.md](mohaa.md#play-it); by hand on a real GPU still to do |

## Linux x64

- **fastfetch 2.71.0** ([its page](fastfetch.md)), built from its source as one static x86-64 musl program: every
  instruction lifted, run in strict mode in Node and in headless Chrome, from the lifted program and from the image
  `pisco build recipes/fastfetch` makes (`tests/run.sh fastfetch`). In the page its logo and colours are drawn (the
  page knows a terminal's colour sequences).
- **`fixtures/linux-hello`** (`tests/run.sh linux`, a default step and in CI): stdio, `printf`'s floating point,
  `malloc` and `mmap`, `qsort`, a jump table, thread-local storage, `setjmp` / `longjmp`, files. What it prints of
  its C library is what the same program prints run natively; what it prints of the machine is `expected.txt`.
- **neofetch 7.1.0** ([its page](neofetch.md)) on bash 5.2.37 and busybox 1.36.1, each lifted into a module of its
  own: 83 processes a run (82 forks, 10 programs executed), in strict mode in Node and in headless Chrome
  (`tests/run.sh neofetch`). The page is a terminal (a cursor that moves).
- **cowsay 3.8.4** ([its page](cowsay.md)) on perl 5.44.0: six runs print what the same perl prints natively, in
  Node and in headless Chrome (`tests/run.sh cowsay`).
- **`fixtures/linux-proc`** (in `tests/run.sh linux`): fork, a pipe from child to parent, execve, a script run
  through its `#!`, wait4, a longjmp across a fork, against its native run.
- A process is an instance of its program's module with memory of its own; the host keeps the process table and
  the pipes (`web/kernel.mjs`). The file system is read-only, its root the app's folder or its image's drive, with
  a generated `/proc`, `/sys`, `/dev` and `/etc` (`rt/linux-x64/`). It links none of the Windows core: only
  `rt/core.c`, `rt/clock.c` and `rt/builtins.c`.

### Next

1. **Threads** (`clone` with `CLONE_VM`, futexes that wait): on the workers the Windows runtimes' threads already
   run on.
2. **Signals**: `SIGCHLD`, `SIGPIPE`, `kill` between processes (a shell with job control needs them).
3. **A file system that can be written** (a tmpfs the processes share), so that a program can keep a file.
4. **Position-independent programs** (static-PIE: `R_X86_64_RELATIVE`), so that a distribution's own static
   binaries can be apps, and function discovery good enough for a stripped one.
5. **Input**: the page's keyboard as standard input, and answers to a terminal's queries; then an interactive shell.
6. **Fork without copying everything**: a fork copies the whole memory (64 MiB for bash, about 20 ms).
7. **Smaller modules for large programs**: perl's is 34 MB, some of its code decoded into more than one function.

## Microvm x64

A runtime that imitates a small x86-64 machine, so that a real Linux kernel is an app ([linux.md](linux.md); the
reasoning and the measurements: [research/kernel-as-an-app.md](research/kernel-as-an-app.md)).

- **K0, done**: `tools/build-linux.sh` builds Linux 6.18.65 and an initramfs of busybox; the lifter reads `vmlinux`
  (10,853 functions; 3,958 instructions of the system not lifted yet); QEMU boots it as the oracle, and what
  `/init`'s programs print is recorded (`tests/run.sh kernel`).
- **K1, done**: the machine (`rt/microvm-x64`): a software MMU over the kernel's page tables, exceptions,
  interrupts and system calls as nested runs of lifted code, the local APIC's timer on a virtual clock, a 16550,
  and a host stack per kernel task (JSPI). `fixtures/microvm-hello`, our own test kernel, prints what QEMU prints,
  in Node and headless Chrome (`tests/run.sh microvm`, a default step). With the machine mode the lifter leaves 56
  of Linux's 602,001 instructions.
- **K2, done**: Linux 6.18 lifted (a 42 MB module) boots on the machine in Node to `Run /init as init process`, in
  0.9 s, the same on every run (`tests/run.sh kernel`, `recipes/linux/traces/boot.txt`). It took the kernel's
  runtime constants read from memory, its static-call trampolines decoded as it rewrites them, labels where its
  `int3` self-test returns, the boot protocol, an MP table, the IOAPIC and a CMOS clock.
- **K3, done**: busybox lifted into the kernel's module (`lifter --program`): `/init`'s programs (pipes, `fork`,
  `exec`, a file written and hashed, signals) print what they print on QEMU, in 2.5 s from Node's start; ten
  programs executed in 18 ms (`tests/run.sh kernel`).
- **K4, done**: the serial port's input (a terminal's keys, the page's, a pipe), idle that waits for keys, and
  `pisco build recipes/linux` (two builds with empty caches, the same manifest). A shell typed into answers as
  `recipes/linux/traces/console.txt`.
- **K5, done**: virtio's block device (`rt/microvm-x64/virtio.c`, `SW_DISK`): `/init` formats the disk, writes it
  and reads it again.
- **K6, done**: programs at the same addresses, each lifted on its own (`lifter --user`) and found by the hash of
  the code page that runs: busybox and linux-hello in one `/init`, as on QEMU.
- **K7, done**: a signal delivered on the way back from a fault or an interrupt (the handler runs on top, the
  interrupted code goes on after `sigreturn`); a task's stacks freed when it ends: 4,000 programs in a row.
- **K8, done**: fastfetch and cowsay on perl run on the lifted Linux beside busybox, as on QEMU; fastfetch reads the
  kernel's own `/proc`.
- **K9, done**: bash (GPL-3.0) in a wasm module of its own, loaded by the host beside the kernel's
  (`web/side.mjs`, `[hints.pisco] modules`), runs as on QEMU.
- **K10, done**: in Node, a program no build lifted is lifted when it first runs (`web/lift.mjs`, `out/lift-cache`).
- **K11, done**: the program's image is taken from memory at its entry point, so a static program runs from the disk
  or anywhere, lifted when it first runs.
- **K12, done**: Alpine's busybox, position-independent and dynamically linked, runs on musl's loader, both lifted
  when they first run, as on QEMU.
- **K13, done**: a program's own libraries, linked or opened with `dlopen`, are each lifted when their code is
  first met (`fixtures/linux-dyn`), as on QEMU.
- **K14, done**: a network in Node (`SW_NET=1`): virtio's network device and unkernel's network stack; a DHCP
  lease, a name looked up and a page fetched from the host.
- **Research, speed and native paths** ([research/microvm-speed.md](research/microvm-speed.md)): lifted user code
  on the machine is 2 to 3 times flat memory, most of it the TLB check on every access (a lifter fix); a lifted
  x86-64 program runs as a task of unkernel's kernel with forks at 0.12 ms against 9; native hosts run the image's
  own kernel under KVM or FEX, and the GPU ring stays the one GPU contract.
- **Next**: the network in the page.

## Known limits

- 64-bit exceptions resume at the landing pads the lifter finds: GCC / LLVM LSDA, C scope tables, MSVC FH3 catch
  and cleanup funclets and catch continuations, and FH4's compressed tables (handlers named by a local
  `__GSHandlerCheck_EH4` too). 32-bit apps have no exception
  dispatch yet.
- AVX, AVX2, FMA and BMI are lifted (`avx.rs`, `rt/avx.h`; `tests/avx.sh` checks 120 results against the CPU). A VEX instruction `avx.rs` does not handle still traps. CPUID still does not report AVX, so apps that check it take their SSE paths; a PS5 app does not ask.
- x87 is emulated with doubles (no 80-bit precision) for Windows and console apps, which run it at 53 bits; a
  Linux app's registers keep 64 bits (`rt/cpu.h` `fpx`), its transcendental instructions compute in doubles.
- Memory protection, guard pages and write watches do not exist; the VFS keeps S:\ and (Windows desktop apps) what a
  app writes in its own folder, not deletions of its own files.
- One wait list serves all waits (events, semaphores, mutexes, address waits): correct, not fast with many threads.
