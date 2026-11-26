# Desire (DESI:Re v0.1) (2026-11-26)

The first third-party Direct3D 12 game on the Windows x64 runtime: a one-stage maze escape on its author's own
C++20 engine (DESI:Re, by satokunDayo; [itch.io](https://satokundayo.itch.io/desire), free; the source is public on
GitHub without a licence). Its census and oracle (milestone D0 of the plan) come first: what the game asks of
Windows, Direct3D 12 and Microsoft's C++ runtime, measured under Wine before any runtime code was written for it.
The game's files, Microsoft's DLLs, logs and screenshots stay outside the repository (under `out/oracle/desire/`):
this page holds names and counts only.

## The files

| | |
|---|---|
| Zip | `DESI-Re-v0.1-Windows-x64.zip`, 507,507 bytes, sha256 `98e6f722bdf5d6395e61742b45d2600cf2d2689ef79da3a1c79096cf8d7ff634`; one top folder `DESI-Re-v0.1-Windows-x64` |
| `Desire.exe` | PE32+, MSVC 14.38 (`/MD`), image base `0x140000000` with relocations, 582 kB of code; sha256 `af1bc97f5d05106076cd71073629382ae8e18323a0d3f4044f10f8cd2db15b81`; 227 imports from 15 DLLs (below) |
| `Assets/` | 13 PNG (8-bit RGB or RGBA, not interlaced, 16x16 to 1280x720), `PoteraEnemy.fbx` (binary FBX 7400, the enemy, skinned), `Maps/generated-stage01.map` (17x17 characters: `#` wall, `.` path, `P` start, `G` goal) |
| `licenses/`, `THIRD_PARTY_NOTICES.md` | D3D12 Memory Allocator 3.2.0, ufbx 0.23.0, DirectXMath: MIT, all three compiled into the exe. The notices say the MIT terms are the libraries' only, not the engine's or the game's |

The zip does not carry Microsoft's C++ runtime (`msvcp140.dll`, `vcruntime140.dll`, `vcruntime140_1.dll`): the
recipe takes them from the user's copy of the x64 redistributable (`[[source.add]]`, `--with REDIST=<folder>`).
The one used here is the redistributable 14.44.35211 (`vc_redist.x64.exe` from Microsoft), its three DLLs taken out
of its `vcRuntimeMinimum` cabinet.

### What `Desire.exe` imports

| DLL | Count | What for |
|---|---|---|
| `MSVCP140.dll` | 62 | iostreams on files (the map, the FBX, `ifstream` / `ofstream`), locale and codecvt, `std::mutex` / `std::condition_variable` (`_Mtx_*`, `_Cnd_*`), `std::exception_ptr`, `std::random_device` (`_Random_device`), `steady_clock` (`_Query_perf_*`) |
| UCRT (`api-ms-win-crt-*`) | 81 | maths (26: `sin`, `acos`, `fmin`, `remainderf`, `_fdclass`, ...), stdio (23), heap (7, `_aligned_malloc`), start-up (18) |
| `KERNEL32.dll` | 43 | files and folders (`CreateFileW`, `FindFirstFileExW`, `GetFileAttributesExW`, `CreateDirectoryW`), SRW locks, an event, `QueryPerformanceCounter`, the unwinder (`RtlLookupFunctionEntry`, `RtlVirtualUnwind`) |
| `USER32.dll` | 21 | one window (`RegisterClassExW`, `CreateWindowExW`), the message pump (`PeekMessageW`), the keyboard (`GetAsyncKeyState`), `GetForegroundWindow` / `IsIconic`, `SetWindowTextW` (the title shows the scene's state) |
| `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll` | 12, 1 | `memcpy` and friends, C++ exceptions: **`__CxxFrameHandler4`** (MSVC's compressed exception tables, FH4) |
| `ole32.dll` | 3 | `CoInitializeEx`, `CoCreateInstance` (Windows Imaging Component: the PNGs), `CoUninitialize` |
| `D3DCOMPILER_47.dll` | 1 | `D3DCompile`: the shaders are HLSL text inside the exe, compiled at start |
| `d3d12.dll` | 2 | `D3D12CreateDevice` (by ordinal 101), `D3D12SerializeRootSignature` |
| `dxgi.dll` | 1 | `CreateDXGIFactory2` |

No threads, no sound, no network, no gamepad. The interfaces it asks for: `ID3D12Device` 1, 4, 8, 10 and 12 (D3D12
Memory Allocator's), `IDXGIFactory4`, `IDXGIAdapter3`, `IDXGISwapChain3`, `IWICImagingFactory` (factory 2) with
32bppRGBA.

### The shaders

Three HLSL sources are strings in the exe, compiled at start with `D3DCompile` as `vs_5_0` / `ps_5_0` (Wine: 7
calls, flags `0x800` and `0x40800`):

| Source | Entry points | What it does |
|---|---|---|
| sprite | `VSMain`, `PSMain` | a quad from `SV_VertexID`, rotated, one texture, point sampling, a tint |
| mesh | `VSMain`, `PSMain` | position, normal, uv, `uint4` blend indices and weights; 128 bones (`row_major float4x4`, two per bone) when skinned; `clip()` for alpha; one directional light |
| post-process | `VSMain`, `PSMain`, `BackgroundPS` | a full-screen triangle from `SV_VertexID`; the 320x180 scene scaled up, point or linear (3 taps), scanlines from `SV_Position.y`, a vignette; `BackgroundPS` fills with a colour |

The game draws at 320x180 into its own target and scales it to the 1280x720 window in the post pass.

### What the game does (from its source, read for facts only)

- A fixed update of 1/60 s; turning with the arrows is scaled by the frame's measured time (capped at 0.1 s).
  One command list a frame, `Present(1, 0)`, then a wait on a fence.
- The keyboard is polled: `GetAsyncKeyState` for all 256 keys every frame, ignored unless the window is the
  foreground one (`GetForegroundWindow`) and not minimised (`IsIconic`). W A S D move, the arrows turn, F2 restarts
  the round in the same maze, Enter / Space skips the logo and quits from the end pictures, Esc quits.
- **The maze and the enemy are random**: two `std::mt19937`, each seeded once by `std::random_device` (two calls of
  `_Random_device` a run, the first for the maze). Every run writes its maze to `Assets/Maps/generated-stage01.map`.
- The window's title carries the scene and the enemy's state, then frame rate and memory figures:
  `DESI:Re | BootLogo | Enter: Skip / Esc: Quit | 41 FPS | CPU 4.0 KiB | GPU Tex ...`,
  `DESI:Re | Maze | WASD: Move / Arrows : Look / F2: Retry / Esc: Quit | Enemy: FIND / Thinking | ...`,
  `GAME OVER | Enter: Quit / Esc: Quit`, `CLEAR | Enter: Quit / Esc: Quit`.

## The oracle

Desire runs under the oracle's Wine (11.22): Direct3D 12 through Wine's `d3d12` (vkd3d) on **lavapipe**, Mesa's
software Vulkan driver, in Xvfb; `D3DCompile` is Wine's (vkd3d-shader), the PNGs are read by Wine's
`windowscodecs`. Microsoft's `vcruntime140` and `vcruntime140_1` are beside the exe, as in our image; `msvcp140` is
Wine's own, because Wine's relay then prints what each `_Random_device` call returned: the seeds. The window is
1280x720 with its frame, so the X screen and the virtual desktop are 1600x900 (`ORACLE_SCREEN`), on a display of its
own. From a new prefix:

```sh
O=tools/oracle-wine/oracle.sh; export ORACLE_DISPLAY=:78 ORACLE_SCREEN=1600x900 ORACLE_SOUND=0
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json        # lavapipe
P=$($O prefix desire)
mkdir -p "$P/drive_c/Games" && unzip -q DESI-Re-v0.1-Windows-x64.zip -d "$tmp" && cp -r "$tmp/DESI-Re-v0.1-Windows-x64" "$P/drive_c/Games/Desire"
cp "$REDIST"/{msvcp140,vcruntime140,vcruntime140_1}.dll "$P/drive_c/Games/Desire/"
for d in vcruntime140 vcruntime140_1; do $O reg desire add 'HKCU\Software\Wine\DllOverrides' /v $d /d native,builtin /f; done
$O reg desire add 'HKCU\Software\Wine\DllOverrides' /v msvcp140 /d builtin /f
$O reg desire add 'HKCU\Software\Wine\Debug' /v RelayInclude /d 'user32.SetWindowTextW;msvcp140.?_Random_device@std@@YAIXZ' /f
TRACE=+relay $O start desire 'C:\Games\Desire\Desire.exe' && $O play desire recipes/desire/sessions/start.json; $O stop desire
```

`+loaddll` shows `Desire.exe`, `vcruntime140.dll` and `VCRUNTIME140_1.dll` native, `MSVCP140.dll`, `d3d12.dll`,
`dxgi.dll`, `D3DCOMPILER_47.dll` and `windowscodecs.dll` builtin. With the narrow `RelayInclude`, the log holds the
window's titles and the seeds; `play.py` writes the title current at each shot beside it (`<shot>.title.txt`). A
full `+relay` run (40 s, 552 MB of log) is kept filtered to the calls made from `Desire.exe`
(`out/oracle/desire/relay/exe.log.gz`): the import sequence D1 is held to.

### Sessions

| Session | What it does | Under Wine |
|---|---|---|
| `recipes/desire/sessions/start.json` | the boot logo (shot at 1.5 s), Enter, the maze before any other input, Esc | about 6 s; two runs: the logo and the maze drawn, different seeds (`f63e8205` / `55254cc0`, `76dcf081` / `5c498163`) and different maze files |
| `recipes/desire/sessions/caught.json` | the maze, then nothing until the enemy touches the player: GAME OVER (shot at 125 s), Enter | one run: seeds `99470982` / `dd700ca1`; the enemy searched (`FIND`, walking and thinking in turn) for 680 frames, chased for 40, and caught the player; then the GAME OVER picture |

The titles of the end pictures are a story scene's: after `GAME OVER | Enter: Quit / Esc: Quit` for one frame, the
picture's own title, whose Japanese text Wine's log shows as code units `\008e\0086...` (the game widens UTF-8 with
the ANSI code page). A walk to the goal (`goal`) needs a known maze, so it is written in D3 for a seed taken from
these logs.

Under lavapipe the game runs at 40 to 45 frames a second on the logo and 2 to 6 in the maze (the title says so).

## Exceptions in normal play

None in the game's thread: `+seh` over a 40 s run shows one exception, `RPC_S_SERVER_UNAVAILABLE` raised and
handled inside Wine's own RPC code on another thread. The game catches its own exceptions only when an asset fails
(its scene shows "Scene error" in the title), so FH4 (milestone D2) is not needed to reach the first frame or to
play, and it is checked on a broken asset folder instead.

## What Wine's run says the game calls

From the filtered `+relay` log (40 s; the game's own calls into builtin DLLs, by count): `GetAsyncKeyState`
291,584 (256 a frame), `_fdclass` 67,633, `sin` 67,465, `fmin` / `fmax` / `acos` about 21,000 each,
`_Mtx_lock` / `_Mtx_unlock` 6,471, `PeekMessageW`, `GetClientRect`, `GetForegroundWindow`, `GetCursorPos`, `IsIconic`
about 1,140 each (one a frame), `SetWindowTextW` 795, `CoCreateInstance` 10 (one per image read), `D3DCompile` 7,
`D3D12SerializeRootSignature` 3, `_Random_device` 2.

## The image

`pisco build recipes/desire --source DESI-Re-v0.1-Windows-x64.zip --with REDIST=<folder>` reads the zip itself
(`extract = "zip"`, `root` the top folder), copies the three runtime DLLs beside the exe, and lifts the four images
together: 5,334 functions (3,829 found from the exception tables), 43 jump tables, 260 AVX instructions made traps
(the virtual CPU reports no AVX). 364 imports: 219 answered by the base, 81 between the lifted images, **64 not
implemented**. In Node, strict, it stops at the first of them (`_dclass`).

## What this means for D1 to D5

- **D1**: the missing imports the run reaches (the CRT functions Microsoft's `msvcp140` and the game reach,
  `rand_s`, ole32 and WIC, seven user32 functions), a window with a message queue, D3D12 Memory Allocator's `...1`
  methods; and the shaders, which `D3DCompile` only finds if they were compiled before the run.
- **D2**: FH4 is not on the path to playing; it is checked with an asset taken away.
- **D3**: the seeds come from Wine's log, so our run draws the same maze; the titles at each checkpoint are compared
  without their figures; the virtual clock and the keyboard for 64-bit titles.
- **D4**: pixels against Wine on lavapipe: the logo and the maze before input are still screens.

## Progress

- **D0** (census, recipe and oracle): `pisco build` takes a zip and added files; the oracle runs the game on
  lavapipe, records the seeds and titles; this page.
- **D1** (lift and start): in Node, strict, Desire runs from its logo into the maze, the enemy's chase, GAME OVER
  and the end picture (its window title says each in turn). Its calls until its main loop's first `PeekMessageW`
  are Wine's (77 steps, `recipes/desire/traces/start.txt`), and given the seeds a Wine run's `std::random_device`
  returned (`SW_RANDOM`) it writes that run's maze byte for byte, for all three runs recorded
  (`recipes/desire/traces/mazes.txt`). `tests/run.sh desireplay` checks both. What it took:
  - **The CRT and kernel32**: `_dclass` / `_fdclass` / `_ldclass`, `rand_s` (never from the host: `SW_RANDOM`'s
    values first, then a generator seeded by `SW_SEED`), the start-up functions Microsoft's `msvcp140` reaches,
    `OutputDebugStringW`, `AreFileApisANSI`, `IsDebuggerPresent` and `UnhandledExceptionFilter` (now shared by both
    desktop runtimes), `GetNativeSystemInfo`.
  - **The window** (`rt/gdk/window.c`): classes keep their procedure, a window keeps the size it was made with
    (`GetClientRect` answers 1280x720), a queue of posted messages (`PostQuitMessage` is the game's way out),
    `DispatchMessageW` calls the procedure, `SetWindowTextW` logs each new title (`window title: ...`),
    `GetForegroundWindow`, `IsIconic`, `GetCursorPos`. `GetAsyncKeyState` answers that nothing is held (the
    keyboard comes in D3).
  - **WIC** (`rt/windows-x64/wic.c`): ole32's `CoInitializeEx` / `CoCreateInstance` and the imaging factory, a
    stream made from memory, a decoder from the stream, its frame, a converter to 32bppRGBA; PNG decoded by
    stb_image.
  - **D3D12 Memory Allocator**: `CreateCommittedResource1/2/3`, `CreateHeap1`, `CreatePlacedResource1/2`,
    `GetResourceAllocationInfo1/2/3` over the existing methods; `CheckFeatureSupport`'s OPTIONS and ARCHITECTURE
    fields at their offsets (heap tier 1, binding tier 3; UMA on the console only); the adapter answers
    `IDXGIAdapter2` to `4`.
  - **Shaders, compiled by `pisco build`** (`[shaders] frames = 30`): the image is run once with
    `SW_HLSL_CAPTURE=1` (`D3DCompile` logs each source and answers with a placeholder) until its 30th frame
    (`SW_EXIT_FRAMES`); `tools/hlsl-prep.mjs` compiles the seven sources with dxc and translates them; they become
    the image's `shaders` layer. Two fixes on the way: `rt_log` cut lines at 1 KiB, so long sources reached dxc
    cut short; and the mesh shader's `clip()` came out of dxil-spirv as SPIR-V's demote-to-helper, which naga
    does not read: `dxil2spv` now makes it `OpKill` (naga's `discard`).
  - **The lifter**: MSVC's two-level `switch` (a byte index table, then a table of image-relative offsets), whose
    bound was taken from the wrong compare: 78 tables recovered in the four images instead of 43.
  - **No FH4 yet**: nothing throws on the way. A shader that failed to compile did (the game throws on a failed
    HRESULT), and the catch stopped at a frame with no landing: D2's case.
- **D2** (FH4): under Wine, with `Assets/Textures/WallBrick.png` taken away, the game throws two C++ exceptions,
  catches them, prints to the debugger and shows `Scene error (Visual Studio Output) | Esc: Quit`; ours now does
  the same and runs on (`tests/run.sh desireplay` builds an image from a copy of the zip without that file).
  - **The lifter reads FH4's FuncInfo4** (compressed numbers, image-relative tables): the unwind map's destructor
    funclets, each catch's funclet and the continuations its handler map lists (from the function's start, or
    image-relative), plus the `lea rax,[continuation]` each funclet returns, as for FH3. Most of Desire's functions
    name not the import but a local `__GSHandlerCheck_EH4` (the security cookie's check, then
    `__CxxFrameHandler4`): a handler that calls or jumps to the import within its first instructions counts.
  - **`RtlUnwindEx` sets `ScopeIndex` to 0 for each frame**: the dispatcher context was reused across frames, so
    the count `__C_specific_handler` leaves there reached FH4's handler, which aborts on a state it did not set.
  - FH3 is unchanged (`fixtures/msvc/eh.exe` with the real DLLs: 6 of 6).
- **D3** (played in Node): the sessions `start`, `caught` and `goal` run strict on the virtual clock, each ended by
  the game itself (Esc, or Enter on a final picture); a second run of `goal` gives the same checkpoints and titles.
  At each checkpoint of `start` and `caught` the window's title is Wine's, the Japanese title of the GAME OVER
  picture included (`tools/oracle-wine/title-compare.py`: frame rate, memory figures and the enemy's
  walking-or-thinking left out). Every Direct3D 12 method the game calls is one vkd3d saw it call (51 of them;
  `tools/oracle-wine/d3d12-compare.py` on `VKD3D_DEBUG=trace`; Wine's DXGI also calls `GetDesc`, `GetDevice` and a
  fence's `Signal` itself). What it took:
  - **One input state for both desktop runtimes**: `rt/windows-x86/input.c` is now `rt/input.c`, with a table of
    virtual keys; user32's `GetAsyncKeyState` / `GetKeyState` read it, the page's events reach it as they reach
    DirectInput, and DXGI's `Present` plays the session. A `shot` step is each runtime's own (`rt_session_shot`):
    Direct3D 9's statistics and frame, or DXGI's, with the methods called since the last checkpoint
    (`SW_CALL_STATS=1`: every import's calls counted).
  - **The virtual clock for 64-bit titles**: with `SW_CLOCK`, `QueryPerformanceCounter`, `GetTickCount`, the file
    time and `Sleep` are the title's clock, and `Present` moves it a frame instead of waiting for the wall.
  - **`goal`** walks the maze of the Wine run with seeds `aee69d51` / `b9466b87` (the session's `"random"` gives our
    run the same seeds): W held 0.8 s a cell (2.5 units a second, cells of 2 units), the arrows 1 s a quarter turn.
    It is written for our virtual clock: under Wine on lavapipe the game runs at 2 to 6 frames a second, its
    time slows (a frame counts at most 0.1 s), and a held key does not mean the same distance, so Wine's run does
    not reach the goal and the ending is not compared with it.
