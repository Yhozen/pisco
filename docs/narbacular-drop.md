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
