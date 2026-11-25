# Narbacular Drop: the first Windows PC title (2026-11-25)

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
- **Text is drawn.** The game renders 256 characters with `TextOutA` into a bitmap at start (a font of its own
  making: height 60, no face name, `NONANTIALIASED_QUALITY`), and D3DX's font draws the level's title and its
  hint with `ExtTextOutW`, one glyph at a time (fonts of height 24 and 48, no face name, `ANTIALIASED_QUALITY`).
  So gdi32 text has to be real, not a stub: see [the second part of M4](#text-gdi-draws-glyphs-m4-the-second-part).
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

Without a sound device (the oracle as it was until M5) `FSOUND_Init(44100, 32, 0)` fails and the game loads no
sounds. With one ([M5](#played-input-sound-and-saves-m5)) it calls 28 of the 33 FSOUND functions it imports: it sets
its own file functions (`FSOUND_File_SetCallbacks`: FMOD reads `File.Ore` through the game), opens its music and loops
as streams and its effects as samples (all of them 3D, `FSOUND_HW3D`, but the music), and moves three listeners
a frame in the level (the camera and the two portals' views).

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
  each glyph the calls appear here too (8.4 `GetLevelDesc` a level frame; Wine: 8.3): real glyphs came with M4's
  second part, and with them these calls.
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

## Text: GDI draws glyphs (M4, the second part)

```sh
SW_TRACE=1 ... node web/run-node.mjs ...                     # "gdi32: font ..." for every font made
tests/run.sh gdi                                             # font.c natively against Wine's GDI (tools/oracle-wine/gdiprobe.c)
```

**What is asked for**, logged in our runtime (the same calls, with the same arguments, are in Wine's `+relay` log):

| Who, when | Font | Under Wine (`gdiprobe.c`) | Drawn |
|---|---|---|---|
| the game, at start | `CreateFontA` height 60, weight 0, no face name, `NONANTIALIASED_QUALITY` | Liberation Sans, 54 pixels an em | 256 characters, each with `TextOutA` centred on the bottom of a 64x64 DIB section, white on magenta, then copied into a texture of its own |
| D3DX, at start | `CreateFontIndirectA` height 24, weight 400, `DEFAULT_CHARSET`, `ANTIALIASED_QUALITY`, face `""` | Liberation Sans, 21 pixels an em | the level's hint, 63 characters, 28 glyphs |
| D3DX, at start | the same, height 48, italic | Liberation Sans Italic, 43 | nothing in this session |
| D3DX, in the level | the same, height 48 | Liberation Sans, 43 | the level's title, *Polishing Up*, 11 glyphs |

D3DX draws each glyph by itself: `ExtTextOutW(ETO_GLYPH_INDEX | ETO_OPAQUE)` of one glyph index at (0, 0) into a
32-bit DIB section, white on black with an opaque background, then `GetCharABCWidthsI` for its box, then copies
it into its glyph texture (39 glyphs on both sides, the same indices in the same order).

**Uniscribe or not.** Under Wine `usp10.dll` loads, so D3DX lays text out with `ScriptItemize`, `ScriptShape` and
`ScriptPlace` (93,636 shapes in the session); here `LoadLibraryA("usp10.dll")` fails and D3DX uses
`GetCharacterPlacementA` (12,838 calls), `GetFontLanguageInfo` and the ABC widths instead. Both paths end in the
same GDI calls, with the same glyph indices, and in the same Direct3D calls frame by frame (below). Uniscribe
would add an API of its own (items, runs, script analysis, clusters, glyph attributes) for no difference
Direct3D can see, so the census's answer stands: no `usp10.dll`. One thing the paths do differently: Uniscribe
places glyphs without the font's kerning pairs, while D3DX's GDI path asks `GetCharacterPlacement` to kern when
`GetFontLanguageInfo` says the font can (Wine's GDI says so for Liberation Sans). Kerned, our hint had its colon
2 pixels closer to the T than Wine's, and everything after it 2 pixels to the left; so our
`GetFontLanguageInfo` answers 0, and D3DX places its glyphs as under Wine (`GetCharacterPlacement` itself still
kerns as Wine's does when asked).

**How** (`rt/win32/font.c`, used by `gdi32.c`): every face name becomes Liberation Sans 2.1.5 (regular or
italic, SIL Open Font License, `rt/win32/fonts/`), the face and version Wine gives the game on the oracle's
machine; no game file is read. The two font files are built into the module with C23's `#embed` (0.8 MB more for
a Windows desktop title): no file for a host to provide, and the build stays a function of the repository. Their
outlines are read and rasterised with stb_truetype (`rt/win32/third_party/`). What Wine does was measured with
`tools/oracle-wine/gdiprobe.c` (each of the four fonts: the face chosen, `TEXTMETRICW`, kerning, Unicode ranges,
glyph indices, ABC widths, advances, `GetCharacterPlacementA` of the session's two texts with and without
`GCP_USEKERNING`, and the pixels of every glyph drawn as D3DX and the game draw them) and is done the same way:
the em size for a height (the largest whose `usWinAscent + usWinDescent` fits), FreeType's fixed-point rounding
of every metric, black boxes from the floor of a glyph's left edge to the ceiling of its right, kerning from the
`kern` table rounded as Wine rounds it, the opaque box (advance by the cell height), and antialiased glyphs as 17 levels of coverage blended through Wine's fixed ramp (white on black
gives 0x68, 0x7c, ... 0xff; measured), one-bit glyphs for `NONANTIALIASED_QUALITY`. Wine's FreeType also runs the
font's TrueType hinting, which stb_truetype does not have; `font.c` stands in with vertical hinting only: the
font's blue zones (baseline, x-height, cap height, ascender, descender) snapped to whole pixels, overshoots onto
the same pixel (kept, rounded, for one-bit glyphs, as Wine's are).

**Against Wine's GDI** (`tests/run.sh gdi`): every measurement D3DX and the game read is Wine's: 138 lines, the
four fonts' metrics, kerning pairs (908; 857 italic), 125 Unicode ranges, glyph indices, all 256 ABC widths and
advances, and the placements (advances, kerned or not, and the totals). Only `GetCharABCWidthsW` of U+007F, a
character the face lacks, differs, and nothing calls it. The pixels differ where hinting moves an edge:

| | glyphs | pixels that differ (of the DIB) | mean difference | text pixels, Wine / ours |
|---|---|---|---|---|
| height 24, antialiased | 30 | 0.69 % | 0.23 of 255 | 1,935 / 2,073 |
| height 48 italic, antialiased | 30 | 0.95 % | 0.34 | 6,847 / 7,192 |
| height 48, antialiased | 30 | 0.86 % | 0.34 | 6,700 / 7,133 |
| height 60, one bit (the game's 256 characters) | 256 | 0.72 % (9 % of the text's pixels) | | 81,356 / 80,460 |

(Without the vertical hinting: 0.85 %, 1.12 %, 0.98 %, 0.81 %.) What is left is Wine's horizontal hinting (a stem
or a bowl half a pixel narrower) and stem heights it rounds up.

**Direct3D 9, frame by frame**, the same session against Wine's `+d3d9` trace (`d3d9-compare.py`, which now also
folds Wine's `texture_2d_LockRect` passing through its own `surface_LockRect`):

| | before (no glyphs) | now |
|---|---|---|
| kinds of frame identical on both sides | 28 | 73 |
| Wine's frames whose calls we make too | 11,169 of 11,857 | 11,851 of 11,857 |
| our frames whose calls Wine makes too | 5,058 of 6,256 | 6,240 of 6,256 |
| level frames (Wine 687, ours 1,197) with a kind the other side has | Wine 0, ours 0 | Wine 681, ours 1,181 |
| methods called on both sides | 70 | 71 (`GetLevelDesc` too) |
| arguments with the same set of values | 140 | 142 (`Lock` with `NOOVERWRITE` too) |

A level frame now makes D3DX's 9 text draws (three texts in each of three scenes) with Wine's `GetLevelDesc` and
`NOOVERWRITE` locks. The 6 frames of Wine's and 16 of ours left over differ by a light or a particle vertex buffer
(frames of different lengths: Wine's clock is the wall's), and the frame in which D3DX makes the title's glyph
texture differs by one draw of the scene's.

**On the screen** (`tests/run.sh ndgfx`; the session has two checkpoints more for this, `level-title` 2.5 s
and `level-hint` 9 s after the last click, and the oracle's `plain` screenshots were taken again with it): the
title and the hint are where Wine has them, in its colours. Of the text's pixels in Wine's shot (white, or green)
1,787 of 1,789 are text in ours too for the title, 2,517 of 2,591 for the hint; ours has more (2,243 and 3,027):
its glyphs are a little bolder, Wine's being hinted narrower. Mean difference of each channel, and the share of
pixels over 16:

| | whole picture | | the text's box | |
|---|---|---|---|---|
| `level-title` (box 195,222-445,262) | 1.62 1.34 1.24 | 1.41 % | 10.3 10.8 11.1 | 24.1 % |
| `level-hint` (box 60,48-560,96) | 1.43 1.58 1.12 | 2.36 % | 3.76 8.73 4.00 | 21.6 % |
| `level` (no text, as before) | 1.33 1.01 0.89 | 0.69 % | | |

Two runs of Wine differ by 1.04 0.50 0.26 (0.11 %) in the hint's box, and by 1.14 0.71 0.59 (0.40 %) in a box of
the same size just below it, where we differ by 1.90 1.47 1.29 (1.19 %): what is left in the text's box is the
glyphs' edges (hinting). The title fades in and out, so its numbers also depend on the moment. The still
screens stay within 0.11 of Wine's. The menu shows none of this text (its words are the game's textures); where
the game shows the 256 characters it draws at start, none of the session's screenshots tells.

**A game bug found on the way.** With glyphs drawn, the first level sometimes crashed in its first frame, in
Node or in Chrome, depending on what the heap held: the player, starting the level, attaches the camera to
itself only if a pointer it sets a few instructions later is already non-zero (`0x41d9cb` tests `[this+0x1e4]`,
`0x41da5e` sets it); in a fresh, zeroed block the camera is not attached, and the frame then reads another
entity's float as a pointer. Under Wine the game plays on: the block held something non-zero there. A 32-bit title's
`HeapAlloc` (and the growth of a `HeapReAlloc`) without `HEAP_ZERO_MEMORY` now fills small blocks with `0xCD`,
the same on every host (`rt/kernel32.c`); stb_truetype's memory comes from a pool of `font.c`'s, not the title's
heap.

## Played: input, sound and saves (M5)

```sh
tools/oracle-wine/oracle.sh sound start                       # the oracle's sound device (start does it too)
O=tools/oracle-wine/oracle.sh; S=recipes/narbacular-drop/sessions/level1-portals.json
ORACLE_RUN=out/oracle/narbacular-drop/portals TRACE=+snoop $O start narbacular-drop "$EXE" && $O play narbacular-drop $S; $O stop narbacular-drop
SW_CLOCK=16667 SW_FMOD_TRACE=1 SW_AUDIO_WAV=out/nd.play.wav SW_SESSION=$S node web/run-node.mjs out/nd/title.wasm "<folder>/Narbacular Drop.exe" > out/nd.play.log
python3 tools/oracle-wine/fsound-compare.py out/oracle/narbacular-drop/portals/wine.log out/nd.play.log
python3 tools/oracle-wine/audio-compare.py out/nd.play.wav out/oracle/narbacular-drop/portals/wine-audio.wav
tests/run.sh ndplay                                           # all of it, Node and Chrome; recipes/narbacular-drop/video.sh: the video
```

**The oracle hears.** Wine now has a sound device without any hardware: a PipeWire daemon run as the user with a
null sink that its dummy driver consumes in real time (`tools/oracle-wine/pipewire.conf`), wireplumber to link
streams, and Wine's `winepulse.drv` talking to it. ffmpeg records the sink's monitor, so every oracle run leaves
`wine-audio.wav` beside its screenshots. (ALSA's `null` plugin was tried first: it is not paced, Wine wrote 7 times
faster than real time.) Two more things became oracle defaults on the way: `warn+heap` (Wine's heap fills new
blocks with 0x55; with sounds loaded, the zeroed blocks it handed out otherwise made the game crash entering the
level, the bug M4 found, now under Wine) and no pointer acceleration in the player (X scaled each motion event
above 4 pixels, so Wine's game saw other mouse deltas than the session's).

**FMOD 3** (`rt/win32/fmod3.c`) answers the 33 imports on the runtime's sound output, which is XAudio2's mixer
factored out of `rt/xaudio2.c` into `rt/mixer.c` (the same ring and AudioWorklet; Doom uses it unchanged). Sounds are
read through the game's own file functions (called with `rt_guest_call` when a sound is loaded or opened, the whole
file at once) and decoded by `rt/win32/audio_decode.c`: WAV (every one in `File.ore` is PCM, so ADPCM is not there),
Ogg Vorbis (stb_vorbis) and MP3 (minimp3), both vendored unchanged. Short looping streams are decoded once and shared
(20 torches play the same Ogg file); the music is decoded as it plays. Under the virtual clock the title's own thread
mixes, exactly up to the clock each time it moves, so the sound of a run is a function of the run: two runs of the
portal session give the same samples, and `SW_AUDIO_WAV=<file>` makes Node write them out.

What is measured and what is guessed (the details are at the top of `fmod3.c`):

| | |
|---|---|
| measured (`+snoop`) | every return value; channel handles `(use count << 12) \| channel`, software channels from 0 and hardware ones (3D) from 32, the lowest free one taken, the use count raised by a start and by `StopSound` but not when a sound ends by itself; `GetFrequency` of a new channel |
| measured (Wine's recording) | 2D sounds, stereo ones too, are panned with equal power (-3 dB a side in the middle): the first level's music came out 2.2 dB louder than Wine's with 0 dB there, and within 0.4 dB with -3 dB |
| from FMOD 3's documentation | volumes, `SetVolumeAbsolute` not scaled by the SFX master volume, a sound's minimum distance is the distance factor when it was made (32 here), the closest of several listeners is heard and without panning |
| guessed | 3D attenuation `(minimum distance / distance) ^ rolloff`, equal-power panning by the angle to the listener's right; no Doppler shift, no reverb (`Reverb_SetProperties` fails under Wine too: no EAX), no hardware channel limit (64) |

**Against Wine's calls** (`fsound-compare.py`; Wine's clock is the wall's, ours virtual, so phases last different
numbers of frames, and the game starts torch pops and footsteps at random times and picks among numbered variants):

| | menu-to-level | level1-portals |
|---|---|---|
| FSOUND calls, Wine / here | 71,917 / 65,617 | 51,626 / 68,232 |
| functions called | the same 28 | the same 29 (`SetPan` too) |
| calls that are not per frame: the same from the start, with the same results (handles included) | the first 177 (start-up, the title music, 20 torch loops), up to the game's first random choice of a sound | the first 177 |
| events with a counterpart on the other side | 796 of Wine's 869, of our 813 | 895 of Wine's 979, of our 907 |
| per frame, by phase (still screens / menu / level): `SetVolume`, `SetVolumeAbsolute`, `Listener_SetCurrent` | 0 / 20.03 / 4.08; 1.76 / 1.00 / 2.00; 0 / 0 / 3 under Wine, 0 / 20.02 / 4.05; 1.74 / 1.00 / 2.00; 0 / 0 / 3 here | the same to 0.2 |
| sounds started, by family, Wine / here | footsteps 53 / 47, torch pops 23 / 21, every other sound the same (torch loops 30, music 2, lava 1, crate 1, run 1) | footsteps 63 / 56, torch pops 23 / 21, runs 2 / 3; a floor button pressed and released under Wine only (where the princess lands after the portal differs a little); every other sound the same |

**The sound itself** (`audio-compare.py`; 1-second loudness envelopes, aligned): over menu-to-level ours is
-24.1 dB mean against Wine's -26.0 (Wine's recording starts while the game loads, ours does not), the envelopes
correlate at 0.84, and the first level, where the music at volume 128 is most of what is heard, is -22.2 dB here and
-21.8 dB under Wine.

**Input from the page.** In the browser a Windows desktop title's keyboard and mouse go into a ring of events the
runtime drains whenever DirectInput reads (`rt/win32/input.c`, `web/browser/main.js`): keys as DirectInput scan
codes (the full table), the mouse as relative motion under Pointer Lock (a click on the canvas takes the pointer;
Escape gives it back, as browsers do, and reaches the title as its Escape key), buttons and the wheel. The session
player feeds the same state; the console's GameInput path (Doom) is untouched.

**The sessions.** `level1-portals.json`: menu-to-level's steps into the first level, then look down at the cage's
floor and left-click (the blue portal), look up and right-click a wall beyond the grate (the red one), look down
through the linked pair, walk forward into it and out of the cage; a still shot after each. `menu-quit.json`: Quit
from the main menu. The game's cursor follows the mouse at about 1.25 x and 0.95 y, more for fast motion, so a
cursor that has to hit a menu item is first pushed into the top left corner; the pause menu's items take a click in
a band only a few pixels high, and where a fast move lands differs between Wine (real-time frames) and us (one
frame each 16.7 ms), which is why the session quits from the main menu and not the pause menu.

**On the screen** (headless Chrome, the portal session against the oracle's screenshots of it; mean difference of
each channel and the share of pixels over 16):

| Checkpoint | R G B | over 16 | |
|---|---|---|---|
| `level-hint` | 1.67 1.71 1.24 | 3.2 % | in the cage, the hint up |
| `look-down` | 1.36 1.58 1.17 | 2.4 % | the cage's floor |
| `blue` | 1.25 1.71 1.78 | 3.5 % | the blue portal on the floor, where Wine has it |
| `red` | 2.03 1.48 1.23 | 2.6 % | the red one beyond the grate |
| `linked` | 1.30 1.08 0.95 | 1.1 % | the room seen through the floor portal |
| `escaped` | 14.0 12.2 11.4 | 39 % | out of the cage, in the same place, the view turned a few degrees from Wine's (a floor of fine texture: every pixel moves) |
| `menu` | 10.0 6.7 5.0 | 18.6 % | the flickering torches, as at M4 |

The level's views are as close as M4's `level` was (1.3 1.0 0.9) with the camera moved by the session's mouse and
keys: the same motion turns and walks the princess as far under both clocks. Only the landing after the portal
differs a little, by the time Wine's frames take while she falls. In Chrome the session takes 7 minutes (6,484
frames; 5 to 8 a second in the level on SwiftShader).

With sound the game also draws random numbers for its sounds (which footstep, a pitch), and its particles then
come out a little differently: menu-to-level's last two checkpoints were recorded again
(`traces/menu-to-level.txt`: 9 and 1,337 Direct3D calls apart).

**What the game writes.** Only on Quit: `system.opt` beside its exe (20 bytes: two flags, the effects and music
volumes as floats, a word). The title's own folder now has a writable top layer that the host keeps (`rt/vfs.c`,
`vfs_writable_game`, set by the Windows desktop profile): in Node in `SW_SAVES/@game/` (S:\ stays at the folder's
top, as for Doom), in the browser in OPFS beside S:\'s files. `tests/run.sh ndplay` quits once and starts again: the
file is Wine's to the byte, and the second run (Node), or a second page in the same browser profile (Chrome), reads it.

**A video with sound** (`recipes/narbacular-drop/video.sh`, to `out/nd/video.mp4`): the portal session recorded in
headless Chrome through `tools/serve.mjs --capture`. Under the virtual clock the page places each presented frame
and the sound by the title's clock (the mixer's ring says where its first frame is on that clock), so the 108 s of
game play as 108 s of video, in sync, although SwiftShader took 6 minutes to draw them. The audio track (ffmpeg's
volumedetect): mean -26.4 dB, peak -2.5 dB; -35.9 dB over the first 15 s (the title music fading in, as the game
fades it), -22.0 dB in the level.

## Its image (M6)

`sw build recipes/narbacular-drop --source narbacular_drop_setup.exe` checks the installer's sha256, extracts it with
innoextract (Inno Setup 5.0.4 data: eight files into `{app}`, the "Read Me" twice, the later copy kept) and builds
the image ([images.md](images.md)): the base's layer, the d3d9, dinput8 and fmod3 libraries' layers, the game's
(40.6 MB, at `C:\Program Files\Narbacular Drop`), the module (9.8 MB) and the config. Against the Wine prefix's
installation (`--compare`) the files are the same except `fmod.dll`, which the oracle's tools UPX-unpacked
(the installer's is the packed one, kept there as `fmod.dll.packed`), and Wine's uninstaller (`unins000.exe`,
`unins000.dat`), which an extraction does not make. The census of its imports: 186, 130 answered by the base,
1 by d3d9, 1 by dinput8, the 33 FSOUND functions by fmod3, 21 not implemented (never called in the sessions).

The recipe now sets the three registry values the oracle's plain screenshots are taken with
(`HKLM\Software\Microsoft\Direct3D`: `DisableMMX`, `DisablePSGP`, `DisableD3DXPSGP` = 1), and advapi32 answers
from them: D3DX opens the key 19 times and reads `DisableMMX` 16 times. The sessions' recorded checkpoints
(frames and Direct3D calls) are unchanged, and `nd`, `ndgfx` and `ndplay` pass as before (the virtual CPU had
already taken D3DX off those paths). The calls up to `Direct3DCreate9` stay Wine's: the key is first opened after it.

## Played by hand (M7)

The image M6's checks ran on (49 MB) was copied to an M1 MacBook Pro, served with `node tools/serve.mjs 8642 web
<image>` and opened in Chrome (`/browser/index.html?image=/files/0/`; `&stats=1` adds the frame counter). The game
is played by hand there: mouse-look under Pointer Lock feels right ("so smooth"). The page's `gpu stats` lines,
logged every 3 s, count the game's own presents, which nothing caps at the display's refresh rate:

| Views | Presents in 3 s | Frames a second | Draws a frame |
|---|---|---|---|
| lighter | 881 to 1,065 | 294 to 355 | 142 to 184 |
| heaviest seen | 308 to 522 | 103 to 174 | 318 to 322 |

No WebGPU error and no draw left out over more than 4.4 million draws. On the test machine's SwiftShader the
same levels run at 10 to 20 frames a second, so the time there was the software rasteriser's. The numbers are
approximate (the 3 s timer drifts under load), and which views were which was not written down.

That made the proof of concept, and the milestone stopped there: no scripted level 1 and no full playthrough.
Not checked by hand yet: leaving Pointer Lock with Esc and clicking back in, both portals aimed by hand, solving
level 1, options kept across a reload in this browser (the scripted check in headless Chrome passes), and the
later levels. What is left is listed in [status.md](status.md).

## What this decides

| Question | Answer |
|---|---|
| Is x86 exception dispatch needed for the first level? | No. `RaiseException` and `RtlUnwind` stay stubs that stop the run |
| Is gdi32 text needed? | Yes: memory DCs, DIB sections, `TextOutA`, `ExtTextOutW` with a real font |
| Uniscribe? | No: report `usp10.dll` as missing and D3DX takes its GDI path, which ends in the same glyphs, the same GDI calls and the same Direct3D calls as Wine's Uniscribe path (M4, the second part) |
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
