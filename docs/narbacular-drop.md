# Narbacular Drop: the first Windows PC title (2026-11-24)

What the game is made of and what it asks Windows for, measured before any code was written for it. The plan
these facts feed is in [status.md](status.md); how the measurements are taken is in
[`tools/oracle-wine/README.md`](../tools/oracle-wine/README.md). The game's files stay outside the repository:
this page holds names and counts only.

## The program

| | |
|---|---|
| `Narbacular Drop.exe` | PE32, i386, Windows GUI, Visual C++ 7.1 (2005). 1.45 MB, image `0x400000–0x57C000`, relocations stripped, no exception table, no TLS, one thread |
| Code | about 386,000 instructions, 16 % of them x87; 91 indirect jumps, 87 of the form `jmp [reg*4+table]`; about 11,000 MMX / SSE / 3DNow instructions behind CPU detection |
| Linked in | the C runtime, D3DX9 (with its HLSL compiler, libpng, libjpeg, zlib) |
| Imports | 186 functions: kernel32 102, fmod 33, gdi32 27, user32 19, advapi32 3, d3d9 1, dinput8 1 |
| `fmod.dll` | FMOD 3, UPX-packed; unpacks cleanly to 348 KB with relocations |
| `File.ore` | 35.8 MB, an uncompressed archive: 70 WAV, 2 OGG, 1 MP3, PNG / JPEG / BMP / DDS, `.x` meshes, one effect file (`vs_1_1` + `ps_1_1`, compiled from HLSL text when it is first used) |
| Installer | InnoSetup 5.0.8, sha256 `0966948f956c5e9362d465e749a70dbfd2be1baadd7104e67b257d73248d7121`; one installed file name contains `©` |

## The reference session

`recipes/narbacular-drop/sessions/menu-to-level.json`: wait for the menu, move the game's cursor onto
*Play Game*, click through three story screens and five tutorial screens, and wait in the first level
(nine clicks). Under Wine on this machine (software rendering) it takes about two minutes and reproduces the
same screens on every run, from a freshly made prefix.

## What it asks for in that session

### Windows (calls made from the game's own image)

81 functions in all (counting the ones it looks up at run time), far fewer than it imports:

| DLL | Called / imported | Functions called |
|---|---|---|
| kernel32 | 35 / 102 | GetVersionExA, GetModuleHandleA, HeapCreate, GetStartupInfoA, GetStdHandle, GetFileType, SetHandleCount, GetCommandLineA, GetEnvironmentStringsW, FreeEnvironmentStringsW, WideCharToMultiByte, MultiByteToWideChar, GetACP, GetCPInfo, GetStringTypeW, LCMapStringW, CompareStringA, GetModuleFileNameA, HeapAlloc, HeapReAlloc, HeapFree, HeapSize, GetProcAddress, LoadLibraryA, IsProcessorFeaturePresent, GetSystemInfo, GetSystemTimeAsFileTime, SetUnhandledExceptionFilter, CreateFileA, SetFilePointer, ReadFile, CloseHandle, FindFirstFileA, GetLastError, IsBadWritePtr |
| user32 | 15 / 19 | RegisterClassA, CreateWindowExA, ShowWindow, SetWindowPos, GetClientRect, GetWindowRect, GetDesktopWindow, IsWindow, PeekMessageA, TranslateMessage, DispatchMessageA, DefWindowProcA, LoadIconA, LoadCursorA, FillRect |
| gdi32 | 18 / 27 | CreateCompatibleDC, CreateDIBSection, CreateFontA, CreateFontIndirectA, CreateSolidBrush, GetStockObject, SelectObject, SetTextColor, SetBkColor, SetBkMode, SetTextAlign, SetMapMode, TextOutA, ExtTextOutW, GetTextMetricsW, GetFontLanguageInfo, DeleteObject, DeleteDC |
| advapi32 | 1 / 3 | RegOpenKeyA |

- **Files:** the whole of `File.Ore` is read with one `ReadFile` of 35.8 MB (note the capital O: names are
  matched without case). It also looks for `Startup.cfg`, `system.opt` and the folders `audio`, `levels`,
  `video` beside it.
- **Time:** only `GetSystemTimeAsFileTime`, about once a frame. No `Sleep`: the main loop spins on `PeekMessageA`.
- **Window:** class `MyWndClass`, a 640x480 client area. One message goes through `DispatchMessageA` in the whole session.
- **Registry:** `HKLM\Software\Microsoft\Direct3D` is opened 19 times (by D3DX) and does not exist under Wine. The
  values D3DX would read there are `DisableMMX`, `DisablePSGP`, `DisableD3DXPSGP` and `D3DXDoNotMute`.
- **Found at run time, not in the import table** (`LoadLibraryA` / `GetProcAddress`): `d3d9.dll!DebugSetMute`
  (17,504 calls), `kernel32!IsProcessorFeaturePresent`, gdi32's `GetCharABCWidthsI`, `GetCharWidthI`,
  `GetFontUnicodeRanges`, and all of `usp10.dll` (Uniscribe). D3DX falls back to plain GDI when `usp10.dll`
  is missing.
- **Text is drawn.** The game renders 256 glyphs with `TextOutA` into a bitmap at start (its own font), and
  D3DX's font draws the in-level hint with `ExtTextOutW`. So gdi32 text has to be real, not a stub.
- **Exceptions:** none raised by the game in this session. x86 exception dispatch can wait.

### Direct3D 9

90 methods, 5.7 million calls. The device is created with hardware vertex processing and as a pure device
(flags `0x50`), without `FPU_PRESERVE`, so Direct3D switches the x87 to single precision.

| Area | What is used |
|---|---|
| Shaders | none created in this session: everything is fixed-function |
| Draws | `DrawPrimitive`: triangle fans 318,810, strips 24,321, point lists 7,716, lists 2,787. `DrawIndexedPrimitive`: lists 58,534, 16-bit indices. No `DrawPrimitiveUP` |
| Vertex formats | FVF `XYZ\|NORMAL\|TEX1`, `XYZRHW\|DIFFUSE\|TEX1`, `XYZRHW\|TEX1`, `XYZRHW\|DIFFUSE`, `XYZ\|DIFFUSE`, `XYZ\|PSIZE\|DIFFUSE`, `XYZB1` and `XYZB3` with `NORMAL\|TEX1`, `XYZ\|TEX1` with a 3-component coordinate; 67 vertex declarations (meshes) |
| Transform and lighting | lighting with many lights (`SetLight` 644,376, `LightEnable` 2.8 million), materials, ambient, normalised normals, local viewer; vertex blending with 1 to 3 weights (`WORLD0–3`), not indexed |
| Fog | linear vertex fog; start 0, end 3,000 or 10,000 |
| Points | point sprites with scaling, size 64 |
| Texturing | one stage: `MODULATE` or `SELECTARG1` of texture and diffuse. Wrap and clamp; point, linear and anisotropic filters |
| Blending | alpha test (`GREATER`, `GREATEREQUAL`), `SRCALPHA` / `INVSRCALPHA`, depth bias for decals |
| Not used | stencil (always off), user clip planes (always off), multisampling, sRGB |
| Targets | two render-target textures (`X8R8G8B8`) with `SetRenderTarget` 2,748 times: the portals are rendered to textures. Clears of colour + depth, or depth only |
| Textures | 343 created, all `A8R8G8B8` with one level; filled through `LockRect`, `UpdateSurface` and `UpdateTexture`. One volume texture, 256x256x4 with 9 levels |
| Buffers | about one new vertex buffer a frame (`WRITEONLY`, default pool); locks with `DISCARD` and `NOOVERWRITE` |
| State blocks | recorded 31 times, applied 168,754 times, captured 20,007 times |
| Other | `TestCooperativeLevel` every frame; `CreateQuery` type 4 as a probe; `GetRenderTarget`, `GetViewport`, `GetDeviceCaps` |

The capabilities the oracle's device reports are in [`tools/oracle-wine/d3d9caps.txt`](../tools/oracle-wine/d3d9caps.txt)
(8 active lights, 4 blend matrices, 8 textures, shader model 3): what our device should answer `GetDeviceCaps` with.

### Sound (FMOD)

`FSOUND_SetMemorySystem` hands FMOD a 16 MB pool, then `FSOUND_Init(44100, 32, 0)` **fails** here because the
machine has no sound device, and the game loads no sounds. Afterwards it still calls `FSOUND_SetVolume` and
`FSOUND_3D_Listener_SetCurrent` every frame. The sound census needs a sound server for Wine first.

### Input

`DirectInput8Create` once; the rest goes through COM. The mouse is opened exclusive and foreground and read as
relative motion; the keyboard foreground and non-exclusive. user32 is asked for no input at all.

## What the lifter makes of it (M1)

`target/release/lifter "<the exe>" out/nd/title.c --split 16`, 2026-11-24:

| | `Narbacular Drop.exe` |
|---|---|
| Functions | 2,212 found from calls, the entry point and exports ("hard"); 1,743 more from code addresses in data and immediates ("soft") |
| Instructions lifted | 349,540 |
| `.text` | 96.19 % instructions, 0.23 % jump tables, 2.84 % padding, 0.74 % not reached (8.7 KB: a data table inside `.text` and unreferenced C runtime stubs) |
| Jump tables | 88 recovered: every `jmp [reg*4+table]`, the two-level byte-index form included |
| Indirect jumps left | 3, none a table: `_JumpToContinuation` and two neighbours in the C runtime's C++ exception code |
| Not lifted | 8,279 instructions, all MMX or 3DNow (`movq`, `pfmul`, `pfadd`, `femms`, ...): D3DX's CPU-specific paths, never taken when the virtual CPU reports neither |

When `fmod.dll` beside it is the unpacked one, it lifts too (78,137 instructions, 19 jump tables, 99.3 % of
its code): running the game's own FMOD is a real alternative to implementing its 33 functions. The packed
DLL as shipped is not liftable; a recipe has to unpack it or leave it out.

## Where it gets to in our runtime (M2)

```sh
RECIPE=recipes/narbacular-drop/title.toml SPLIT=16 OPT=-O1 tools/build-title.sh "<folder>/Narbacular Drop.exe" out/nd
SW_STRICT=1 SW_TRACE=2 SW_SESSION_STEPS='wait 0' node web/run-node.mjs out/nd/title.wasm "<folder>/Narbacular Drop.exe" > out/nd.run.log
python3 tools/oracle-wine/trace-diff.py recipes/narbacular-drop/traces/start.txt out/nd.run.log --until Direct3DCreate9
```

(`SW_SESSION_STEPS='wait 0'` is a session of one empty step: it ends the run when the first frame has been
presented. Since M3 the game no longer stops at Direct3D.)

The game's own start-up code runs: its C runtime makes its heap, reads the environment and builds its character
tables; the game reads all of `File.Ore`, looks for its three folders, registers `MyWndClass`, creates a 640x480
window, sizes it to a 640x480 client area, centres and shows it, then calls `Direct3DCreate9(32)`. Up to that
call its 274 calls are, with repeats folded, the same 81 steps as under Wine (434 calls there: the counts
differ where the environment does). At M2 Direct3D 9 was not implemented, so the game showed its message box
("Minimum Direct3D initialization failed...") and exited with code 0.

One difference had to be removed first: Windows' loader (and Wine's) sets the stack-protection cookie named in
the image's load configuration; without that the C runtime made its own from the clock, the thread id and the
performance counter, five calls Wine never showed.

## The session without a screen (M3)

```sh
S=recipes/narbacular-drop/sessions/menu-to-level.json
SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 SW_D3D9_FRAMES=1 SW_SESSION=$S node web/run-node.mjs out/nd/title.wasm "<folder>/Narbacular Drop.exe" > out/nd.session.log
python3 tools/oracle-wine/d3d9-compare.py out/oracle/narbacular-drop/d3d9/wine.log out/nd.session.log
```

The game creates its Direct3D 9 device (640x480 windowed, `X8R8G8B8`, a `D24X8` depth buffer, flags `0x50`),
its DirectInput keyboard and mouse and FMOD (whose `FSOUND_Init` fails, as on the oracle's machine), loads its
data and runs its main loop; the session player moves the mouse onto *Play Game* and clicks nine times; the run
ends with exit code 0 in the first level after 6,256 frames (104 s of the game's time, 4 s of ours). Direct3D 9
is a null device: nothing is drawn. `tests/run.sh nd` checks the eleven checkpoints against
`recipes/narbacular-drop/traces/menu-to-level.txt` and that a second run gives the same log.

Against Wine's trace of the same session (11,857 frames there: it renders in real time, our clock is virtual):

| | Wine | here |
|---|---|---|
| Direct3D 9 methods called | 75 | 70 of them, and no other |
| Methods with the same number of calls | | 39 (everything that is not per frame: 343 `CreateTexture`, 256 `UpdateSurface`, 102 `CheckDeviceFormat`, 67 `CreateVertexDeclaration`, 31 state blocks recorded, ...) |
| Kinds of frame (the calls one frame makes) | 79 | 72; 27 are the same and cover 11,168 of Wine's 11,857 frames |
| A menu frame | 165-172 draws, 19-20 indexed | the same |
| A level frame | 3 scenes, 4 target changes, 129-136 draws, 18-74 indexed | the same, with 9 indexed draws fewer (6 while two texts are up): 1,144 of our 1,199 level frames are a frame Wine has minus exactly those draws |
| Argument values (states, formats, pools, usages, flags, primitive types) | | 140 arguments took the same set of values; 2 did not |
| Win32 calls (`+relay`) | `CompareStringA` 97,897, `HeapReAlloc` 146,820, `IsBadWritePtr` 852, `TextOutA` 256, `ExtTextOutW` 39, `RegOpenKeyA` 19 | the same counts |

The differences, all of them:

- **Text.** D3DX's font renders each glyph with `ExtTextOutW` into a DIB section and copies it into a texture
  (39 glyphs, on both sides). Our gdi32 draws nothing yet, so every glyph's bitmap is empty and D3DX's sprite
  has nothing to draw: under Wine a level frame has 9 more `DrawIndexedPrimitive` (three texts in each of the
  three scenes), with `GetLevelDesc` on the glyph texture (5,703 calls, the one method we never see) and a
  vertex buffer `Lock` with `NOOVERWRITE` (5,700, the one flag value we never see). With a filled box in place of
  each glyph the calls appear here too (8.4 `GetLevelDesc` a level frame; Wine: 8.3): real glyphs come with M4.
  The other level frames differ from Wine's by one or two particle vertex buffers or one light, as frames of
  different lengths do.
- **Uniscribe.** Wine has `usp10.dll`, so D3DX lays text out with `ScriptShape` / `ScriptPlace` there and with
  `GetCharacterPlacementA` here (12,838 calls), as the census decided.
- **Reference counting and `QueryInterface`.** Wine's trace shows its own along with the title's
  (`GetDirect3D` and `CreateTexture` query interfaces inside Wine; `GetSurfaceLevel` and `GetRenderTarget` call
  `AddRef` inside Wine): our counts are the title's calls only. `surface_LockRect` likewise: 297 of Wine's 338
  are `texture_2d_LockRect` passing through.
- **The CPU.** The recipe's virtual CPU has no MMX; the oracle's has (one more `IsProcessorFeaturePresent` here).

Where Direct3D's behaviour had to be known and the trace does not show it, the oracle was asked:
`tools/oracle-wine/d3d9probe.c` prints the adapter's identity and modes, `CheckDeviceFormat` for 51 formats in
44 uses, a new device's every state, and three dozen corner cases (who holds which reference, locking twice,
lights that were never set, what a recording does to the device). Its output and `d3d9caps.txt` become
`rt/win32/d3d9_oracle.gen.h`. One answer mattered at once: `D32` is not a depth format there, so the game
takes `D24X8`.

## Pixels: Direct3D 9 on WebGPU (M4, the first part)

```sh
SW_STRICT=1 SW_CLOCK=16667 SW_SESSION=$S node tools/run-chrome.mjs out/nd/title.wasm "<folder>/Narbacular Drop.exe"   # out/nd/shots/<checkpoint>.png
python3 tools/oracle-wine/shot-compare.py --table out/nd/shots out/oracle/narbacular-drop/d3d9                       # tests/run.sh ndgfx does both
```

In headless Chrome (WebGPU on SwiftShader: this machine has no GPU) the session plays to its end on the virtual
clock, in strict mode, and the frame presented at each checkpoint is saved. Everything the game draws on the way
is drawn: 529,685 draws in 8,650 passes over 6,256 frames, none left out, no WebGPU error.

**How.** At each draw the device's state becomes a vertex key and a fragment key, and the runtime writes the two
fixed-function shaders for them as WGSL (`rt/win32/d3d9_ffp.c`); with the vertex layout and the raster, depth and
blend state they make a pipeline; matrices, lights, material and fog go into one uniform block. All of it travels
as explicit records on the GPU ring (`rt/d3d12_cmd.h`: shader, sampler, pipeline, buffer and texture writes, pass,
viewport, draw), beside the D3D12 records and through the same back end. Where Direct3D 9's conventions are not
WebGPU's (the byte order of colours, pixel centres, the winding that is culled, depth bias units, fans, points,
the alpha test) is written at the top of `rt/win32/d3d9_gpu.c`.

**What the session needed**: 31 vertex shaders, 8 fragment shaders, 33 pipelines, 4 samplers.

| Shaders | Keys |
|---|---|
| vertex, pretransformed (`XYZRHW`) | 3: with diffuse and texture coordinates, diffuse only, coordinates only |
| vertex, world geometry and plain meshes (`XYZ\|NORMAL\|TEX1`) | 10, all with linear fog: lit by 0 to 4 point lights; the same with normalised normals for 0 to 3; one without coordinates (a mesh drawn with no texture) |
| vertex, skinned meshes (declarations) | 16: 1 weight with 2 matrices, 3 weights with 2, 3 and 4 matrices, each lit by 0 to 3 lights |
| vertex, particles | 2: point sprites scaled by distance, with and without a size in the vertex |
| fragment | 8: `MODULATE` or `SELECTARG1` of texture and diffuse, alpha from the texture, from the diffuse colour or off, the alpha test `GREATER` or `GREATEREQUAL`, fog, a texture without alpha (the portals' targets), and no texture at all |

**Against Wine**, the mean absolute difference of each colour channel (0 to 255) and the share of pixels where a
channel differs by more than 16:

| Checkpoint | Wine as it runs here | | Wine with D3DX's plain code | | Two runs of Wine | |
|---|---|---|---|---|---|---|
| | R G B | over 16 | R G B | over 16 | R G B | over 16 |
| `menu` | 10.3 6.8 4.9 | 18.7 % | 8.9 5.8 4.3 | 15.4 % | 12.0 7.2 4.9 | 22.6 % |
| `menu-play` | 7.2 4.7 3.5 | 9.1 % | 8.1 5.2 3.8 | 11.5 % | 8.9 6.6 4.9 | 11.9 % |
| `intro` | 0.71 0.32 0.78 | 0 | 0.10 0.10 0.10 | 0 | 0 | 0 |
| `intro-2` | 0.75 0.35 0.77 | 0 | 0.11 0.11 0.10 | 0 | 0 | 0 |
| `intro-3` | 0.70 0.33 0.77 | 0 | 0.11 0.10 0.10 | 0 | 0 | 0 |
| `controls` | 0.39 0.38 0.39 | 0 | 0.11 0.11 0.11 | 0 | 2.7 2.6 2.3 | 1.67 % |
| `tutorial` | 0.33 0.27 0.37 | 0 | 0.07 0.07 0.07 | 0 | 0 | 0 |
| `tutorial-2` | 0.40 0.35 0.42 | 0 | 0.09 0.09 0.09 | 0 | 0 | 0 |
| `tutorial-3` | 0.28 0.19 0.32 | 0 | 0.04 0.04 0.04 | 0 | 0 | 0 |
| `tutorial-4` | 0.24 0.16 0.27 | 0 | 0.04 0.04 0.04 | 0 | 0 | 0 |
| `level` | 1.4 1.0 0.9 | 0.84 % | 1.2 1.0 0.9 | 0.52 % | 2.5 2.3 2.4 | 1.70 % |

- **The eight still screens** (story, controls, tutorial: one textured quad of pretransformed vertices each) are
  Wine's to within 9 levels at the worst pixel, and to within **one level** once Wine's D3DX runs the code ours
  runs. (`controls` has a two-frame cartoon along its top, which one of Wine's runs caught in its other frame:
  that is the difference between Wine's own two.) The oracle's CPU has MMX and SSE and D3DX uses them to decode and resample the pictures; the recipe's CPU
  has neither, and D3DX's plain code rounds differently (red and blue twice as much as green: the colour
  conversion of the JPEG files). With D3DX's processor-specific code turned off in the oracle
  ([`tools/oracle-wine/README.md`](../tools/oracle-wine/README.md)) the largest difference left is 1, on 9 to 24 %
  of the pixels, in both directions: the two rasterisers' rounding.
- **The menu** is a lit corridor with flickering torches, a swinging lantern, the title, four lines of text (they
  are textures of the game's own, not GDI's) and the game's cursor. Ours is the same picture, with the title, the
  text and the cursor at the same pixels (the title's letters, opaque, are within one level on 94 % of the box
  around them); the corridor, and what is blended over it, differs by its lighting at the moment of the shot. Two
  runs of Wine differ from each other by more than ours differs from either.
- **The level** (through a grate, three scenes a frame: two portal views into textures, then the screen) is the
  same picture. What differs is what moves: two flames (point sprites) and a creature (a skinned mesh) that walks
  across the room and is elsewhere at the moment of our shot; a series of shots half a second apart shows it at
  the oracle's place a second or so earlier (the oracle's clock runs while it loads, ours does not).
- **Absent**: what D3DX's font draws in the level (9 draws a frame under Wine, none here until GDI draws glyphs).
  Nothing of it shows in the oracle's shot at this checkpoint either.

**Speed** on SwiftShader, 4 cores: the whole session in about four minutes (237 to 264 s); the menu at 16 to 20
frames a second (about 170 draws a frame), the still screens at 60 to 75, the level at 12 to 13 (as many draws, in
three scenes with their clears). With the draws themselves left out the menu's frames go by at 115 a second, and
capping the anisotropy the game asks for (16) changes nothing: the time is the software rasteriser's, draw by draw.

**Checked besides**: the null back end in Node finds all of the session's records consistent (`tests/run.sh nd`);
every shader the session made, and those of 446 other state keys, pass naga (`tests/run.sh ffp`); rewriting
every vertex buffer before every draw, as an ordered write (about 5,800 submits more in a shorter session) or as
a discard (about 17,500 buffers given new storage), gives the same frames bit for bit.

**Not drawn**, because the session never asks: texture stages past the first, the volume texture (created, never
bound to a draw here), specular lighting, spot and directional lights (every light is a point light), table fog,
stencil, user clip planes, `DrawPrimitiveUP`, a `Clear` of part of a target. States the game sets that change
nothing here: `DITHERENABLE`, `LASTPIXEL`, `CLIPPING`, `WRAP0`, the multisample and patch states, `MIPMAPLODBIAS`,
`MAXMIPLEVEL`, `SRGBTEXTURE`, the bump-mapping stage states, `RESULTARG`.

## What this decides

| Question | Answer |
|---|---|
| Is x86 exception dispatch needed for the first level? | No. `RaiseException` and `RtlUnwind` stay stubs that stop the run |
| Is gdi32 text needed? | Yes: memory DCs, DIB sections, `TextOutA`, `ExtTextOutW` with a real font |
| Uniscribe? | No: report `usp10.dll` as missing and D3DX takes its GDI path |
| Shader translation for the first level? | No. The one effect is compiled when first used, later in the game |
| Clip planes or stencil for the portals? | No: render-to-texture |
| How much of kernel32? | 35 functions for this session, 11 of them not in `rt/` yet |

## Reproducing

```sh
O=tools/oracle-wine/oracle.sh; S=recipes/narbacular-drop/sessions/menu-to-level.json
EXE='C:\Program Files (x86)\Narbacular Drop\Narbacular Drop.exe'
$O prefix narbacular-drop <installer> /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
$O start narbacular-drop "$EXE" && $O play narbacular-drop $S && $O stop narbacular-drop      # screenshots
for t in +relay,+seh +d3d9 +snoop; do                                                          # traces
  ORACLE_RUN=out/oracle/narbacular-drop/${t//[+,]/} TRACE=$t $O start narbacular-drop "$EXE"
  ORACLE_RUN=out/oracle/narbacular-drop/${t//[+,]/} $O play narbacular-drop $S; $O stop narbacular-drop
done
```
