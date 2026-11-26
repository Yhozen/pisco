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

- **D1**: the 64 missing imports (the CRT functions Microsoft's `msvcp140` and the game reach, `rand_s`, ole32 and
  WIC, seven user32 functions), a window with a message queue, D3D12 Memory Allocator's `...1` methods.
- **D2**: FH4 is not on the path to playing; it is checked with an asset taken away.
- **D3**: the seeds come from Wine's log, so our run draws the same maze; the titles at each checkpoint are compared
  without their figures; the virtual clock and the keyboard for 64-bit titles.
- **D4**: pixels against Wine on lavapipe: the logo and the maze before input are still screens.

## Progress

- **D0** (census, recipe and oracle): `pisco build` takes a zip and added files; the oracle runs the game on
  lavapipe, records the seeds and titles; this page.
