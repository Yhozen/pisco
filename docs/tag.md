# Tag: The Power of Paint (2026-11-25)

The second Windows x86 title. Its census and oracle (milestone T0 of the PISCO plan) come first: what the game asks
of Windows, Direct3D 9, D3DX and FMOD Ex, measured under Wine before any code was written for it. How to play it
and each milestone after T0 are at the end ([Play it](#play-it), [Progress](#progress)). How the measurements are taken is in
[`tools/oracle-wine/README.md`](../tools/oracle-wine/README.md). The game's files, Microsoft's DLLs, logs,
screenshots and the captured shader bytecodes stay outside the repository (under `out/oracle/tag/`): this page
holds names and counts only.

## The files

| | |
|---|---|
| Installer | Inno Setup 5.1.10, sha256 `bb69d916e788c6a658e4b75e945efd6960ce4ddef1f5cc0ce6db86efccbe8113`; `{app}` only (572 files); its last step runs `DirectX\DXSETUP.exe` |
| `Tag.exe` | PE32, MSVC 8.0, image `0x400000–0x571000`, no relocations; 197 imports from 10 DLLs (kernel32 87, d3dx9_35 45, fmodex 23, user32 20, ws2_32 12 by ordinal, oleaut32 4, ole32 3, gdi32 1, d3d9 1, dinput8 1) |
| `d3dx9_35.dll` | in `DirectX\AUG2007_d3dx9_35_x86.cab` (cabextract warns of 9208 extra bytes at the cabinet's end, harmless); 3,727,720 bytes, built 2007-07-19, sha256 `2198022938156b790e9cfb0f7997494b66a11a1ad49b395be58251d635b66b26`; 334 exports; **image base `0x400000`, the same as `Tag.exe`**: it has relocations and must be moved (Wine put it at `0x79DC0000` and `0x7A2B0000` in two runs) |
| `fmodex.dll` | FMOD Ex, UPX-packed (320,000 bytes; 704,512 unpacked with `upx -d`); imports msvcrt 53, kernel32 42, winmm 22, wsock32 14, ole32 6, msacm32 6, advapi32 5, user32 2 |
| `Game Data\TAG.ini` | full screen (`bWindowed = 0`) at 1024x768, vsync on, post-processing on, `fMaxFPS = 60` |

### What `d3dx9_35.dll` imports

206 functions from 4 DLLs; no delay-loaded imports, nothing from d3d9 (it only calls the device it is handed).

| DLL | Count | Names |
|---|---|---|
| `msvcrt.dll` | 96 | stdio on file descriptors (`_write`, `_lseeki64`, `_isatty`, `_fileno`, `__pioinfo`, `__badioinfo`, `fread`, `fwrite`, `fflush`, `_iob`, `_tempnam`), strings and conversions (`_snprintf`, `_vsnprintf`, `sscanf`, `strtod`, `atof`, `atoi`, `atol`, `_itoa`, `_ultoa`, `wcstombs`, `wctomb`, `strchr`, `strncmp`, `_stricmp`, `_strnicmp`, `_strdup`, `ctype` and `isw*`), memory (`malloc`, `calloc`, `realloc`, `free`, `operator new`/`delete`, `memcpy`, `memmove`, `memset`, `qsort`), x87 maths by register (`_CIsin`, `_CIcos`, `_CItan`, `_CIasin`, `_CIacos`, `_CIatan`, `_CIatan2`, `_CIexp`, `_CIlog`, `_CIpow`, `_CIsqrt`, `_CIfmod`, `_CIsinh`, `_CIcosh`, `_CItanh`), `floor`, `ceil`, `ldexp`, `frexp`, `modf`, `_finite`, `_isnan`, `_fpclass`, `_clearfp`, `_controlfp`, **`_setjmp3`, `longjmp`**, **`_CxxThrowException`, `__CxxFrameHandler`, `_XcptFilter`, `??1type_info@@UAE@XZ`, `?terminate@@YAXXZ`**, `_purecall`, `rand`, `srand`, `setlocale`, `_strdate`, `_strtime`, `abort`, `exit`, CRT start-up (`_initterm`, `_onexit`, `__dllonexit`, `_lock`, `_unlock`, `_amsg_exit`, `_errno`) |
| `kernel32.dll` | 80 | files (`CreateFileA/W`, `ReadFile`, `WriteFile`, `SetFilePointer`, `GetFileSize(Ex)`, `SetEndOfFile`, `DeleteFileA/W`, `MoveFileA/W`, `GetTempPathA`, `GetTempFileNameA/W`, `GetFullPathNameA`, file mappings), resources (`FindResourceA/W`, `LoadResource`, `LockResource`, `SizeofResource`, `FreeResource`), `LoadLibraryA`, `GetProcAddress`, `FreeLibrary`, `GetModuleHandleA`, heap and `VirtualAlloc/Free`, critical sections, a mutex, TLS, interlocked operations, `IsProcessorFeaturePresent`, `GetSystemInfo`, `GetVersion(ExA)`, `GlobalMemoryStatus`, `IsBadReadPtr/WritePtr/CodePtr`, `RtlUnwind`, `SetUnhandledExceptionFilter`, `UnhandledExceptionFilter`, code pages (`GetACP`, `MultiByteToWideChar`, `WideCharToMultiByte`, `IsDBCSLeadByte`, `CompareStringA`, `lstrcmpiA`), `ExpandEnvironmentStringsA`, `OutputDebugStringA`, `FormatMessageA`, time |
| `gdi32.dll` | 27 | D3DX's fonts: `CreateFontIndirectA/W`, `CreateDIBSection`, `CreateCompatibleDC`, `SelectObject`, `GetGlyphOutlineA/W`, `GetTextMetricsA/W`, `GetOutlineTextMetricsA`, `GetCharacterPlacementA/W`, `ExtTextOutA/W`, `GetFontLanguageInfo`, `TranslateCharsetInfo`, `GetCurrentObject`, ... |
| `advapi32.dll` | 3 | `RegOpenKeyA`, `RegQueryValueExA`, `RegCloseKey` (the `Direct3D` switches) |

## The oracle

Tag runs under the oracle's Wine (11.22, software OpenGL in Xvfb) **full screen as shipped**: Wine's 1024x768
virtual desktop holds it, so `TAG.ini` is left as it is. Wine's own `d3dx9_35` is replaced by Microsoft's, so
that Wine runs the D3DX code the lifter will lift (`+loaddll` says `d3dx9_35.dll ... native`). From a new prefix:

```sh
O=tools/oracle-wine/oracle.sh
$O prefix tag                                                                   # an empty prefix first, then:
$O reg tag add 'HKCU\Software\Wine\DllOverrides' /v dxsetup.exe /d '' /f        # DXSETUP waits for ever under Wine
$O reg tag add 'HKCU\Software\Wine\DllOverrides' /v d3dx9_35 /d native /f
$O prefix tag ~/games/Tag_setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-  # 'C:\Program Files (x86)\Tag'
cabextract -F d3dx9_35.dll -d "$tmp" "$HOME/.pisco/oracle/prefix-tag/drive_c/Program Files (x86)/Tag/DirectX/AUG2007_d3dx9_35_x86.cab"
cp "$tmp/d3dx9_35.dll" "$HOME/.pisco/oracle/prefix-tag/drive_c/windows/syswow64/"
$O start tag 'C:\Program Files (x86)\Tag\Tag.exe' && $O play tag recipes/tag/sessions/menu-to-level1.json; $O stop tag
```

With the installer's `DXSETUP.exe` left to run, the install never ends (it was ended by its PID after ten minutes;
the game was installed by then). Disabled by the override, the installer finishes in under a minute.

For `+snoop`, `fmodex.dll` in the prefix is replaced by its `upx -d` copy, as Narbacular Drop's `fmod.dll` was
(journey R17). `SnoopInclude` with a list of DLLs gave no calls at all; without it, `SnoopExclude` keeps D3DX's
maths out of the log (3.8 million calls make it 570 MB).

At start the game writes an empty `Errors.txt` beside `Tag.exe`.

### Sessions

| Session | What it does | Under Wine |
|---|---|---|
| `recipes/tag/sessions/menu-to-level1.json` | the menu (shot at 40 s), the game's cursor onto Start Game, a 600 ms click, the paint splash, level 1 (an alley with the goal at its end) and its two tooltips | about 85 s; replayed from a prefix made new as above: its level-1 shots within 0.5 of 255 on average of an earlier prefix's (0.44 % of pixels by more than 16: the goal's colour is random), the menu's within 9 (its paint colours are random). Reached level 1 in each of the 14 runs made |
| `recipes/tag/sessions/level2-paint.json` | menu-to-level1's steps, then forward into the goal, through the loading screen into **level 2**, onto the green (jump) can's pedestal, which gives the gun; a shot at the floor, a turn and a shot at a wall, the right button there (erase) | about 165 s |

**Level 1 has no paint gun**: `level1.xml` has no gun pickup, and the left button there does nothing; the first
gun (green, jump) is in level 2. So the plan's `level1-paint` is `level2-paint`. Its timing is the weak point under
Wine: the waits are the wall's, and the game's frame rate there is low (150 frames in about 90 s under the `+d3d9`
trace) and varies, so a level that loads late leaves the walk short of the pedestal. With 20 s for level 2 to load
one run of two missed the gun; with the session's 35 s and 3.5 s walk, three runs: two took the gun, painted and
erased (where the turned shot lands differs between them), one crashed (below). Our runtime plays it on the virtual
clock, where the timing does not vary.

**One run crashed entering level 2**: `Unhandled page fault on write access to 00000008 at address 004A8A1B`, a
loop that clears a flag at `+8` of three objects, one of them a null pointer (`0x4A8A05`–`0x4A8A1F`). The game's
code, not D3DX's; it did not happen in the five other runs that entered level 2.

Clicks are held 600 ms: a 150 ms press on Start Game was missed (at a few frames a second, it can fall between two
reads of the mouse). The cursor (a spray can) moves by relative motion and starts at about (528, 215).

## Exceptions in normal play

`+seh` over menu-to-level1: **no exception is raised in the game's process** (no `dispatch_exception`; the one
in the log, `RPC_S_SERVER_UNAVAILABLE`, is in one of Wine's own 64-bit processes). No C++ exception
(`0xE06D7363`) and no access violation.

**`longjmp` is used in normal play**, by D3DX: 3,102, 3,148 and 4,915 times in three runs of the session, always
from the same `_setjmp3` site (`d3dx9_35` RVA `0x2005d2`) with the value 1, and **exactly one for every image file
D3DX opens**: in a run with `+seh,+snoop` each of the 2,829 `D3DXGetImageInfoFromFileA` and 319
`D3DXCreateTextureFromFileExA` calls is followed by one `longjmp`, though each call returns `S_OK` (a decoder that
does not recognise the file gives up that way). `_setjmp3` is called four times as often, from four sites
(RVAs `0x2005d2`, `0x2016a4`, `0x300186`, `0x300300`, equally). The count follows the image calls, which the game
also makes while it runs (2,619 of the 2,829 after FMOD's first `update`). Wine's `longjmp` unwinds between the two
frames (`RtlUnwindEx` in the trace). So T1 needs `_setjmp3` and `longjmp` in 32-bit msvcrt, but not
`RaiseException` dispatch or C++ catch blocks.

## Direct3D 9

From a `+d3d9` trace of menu-to-level1 (150 frames): 593,282 calls of interface methods.

| | |
|---|---|
| Device | `CreateDevice` HAL, `D3DCREATE_HARDWARE_VERTEXPROCESSING` (`0x40`); back buffer A8R8G8B8 1024x768, automatic depth buffer **D16**, presentation interval 1 (`+d3d`) |
| Device methods | `SetSamplerState` 89,893, `SetRenderState` 28,461, `SetVertexDeclaration` 21,141, `DrawIndexedPrimitive` 19,906, `SetVertexShader` 19,886, `SetPixelShader` 19,878, `SetVertexShaderConstantF` 19,858, `SetTexture` 19,140, `SetStreamSource` 19,024, `SetIndices` 19,024, `SetPixelShaderConstantF` 10,872, `SetRenderTarget` 3,304, `DrawPrimitiveUP` 1,130, `GetRenderTarget` 1,011, `GetDeviceCaps` 997, `GetVertexDeclaration` 981, `GetDirect3D` 912, `SetTransform` 671, `GetViewport` 645, `CreateTexture` 600, `Clear` 597, `GetDisplayMode` 381, `CreateVertexDeclaration` 274, `CreateVertexBuffer` 271, `CreateIndexBuffer` 270, `UpdateTexture` 258, `BeginStateBlock` 168, `EndStateBlock` 149, `BeginScene`/`EndScene`/`Present`/`TestCooperativeLevel` 150 each, `SetTextureStageState` 95, `GetCreationParameters` 50, `GetRenderTargetData` 44, `CreateOffscreenPlainSurface` 44, `CreateVertexShader` 23, `CreatePixelShader` 23, `SetNPatchMode` 10, `SetFVF` 2, `CreateQuery` 1 (`D3DQUERYTYPE_EVENT`, a support check), `CreateCubeTexture` 1 |
| State blocks | `IDirect3DStateBlock9::Apply` 101,188 and `Capture` 99,289: D3DX's effects, sprites and fonts save and restore state around everything they draw |
| Shader validator | `Direct3DShaderValidatorCreate9` 46 times, one for each shader D3DX compiles (Wine: a stub) |
| Textures | from D3DX's loader: A8R8G8B8 only (72 managed, 258 in system memory copied by `UpdateTexture`), X8R8G8B8 6 managed; **no DXT** |
| Render targets | textures only (no `CreateRenderTarget`, no depth-stencil surface besides the automatic one): **262 A8R8G8B8** (the paint maps: 128², 256² and 512² with 5 levels each; four of 1024x768) and **2 R32F** of 1024x768 (`NormalDepth.fx`, for the edge detection) |
| Cube | 1, `D3DXCreateCubeTextureFromFileA`: A8R8G8B8, 512, 10 levels, managed (the skybox) |
| Read-back | **`GetRenderTargetData` once a frame in level 1** (frames 107 to 150, the last 44 of the session): a 512x512 A8R8G8B8 render target into a system-memory surface, then `LockRect` |
| Draws | all triangle lists. `DrawIndexedPrimitive` 19,906 (16-bit indices), `DrawPrimitiveUP` 1,130 (strides 28 ×981 and 24 ×149), no `DrawPrimitive`, no `DrawIndexedPrimitiveUP` |
| Not called | `Reset` (only Toggle Full Screen would), `StretchRect`, `ColorFill`, `UpdateSurface`, `GetFrontBufferData`, `SetDepthStencilSurface`, `SetScissorRect`, `SetClipPlane`, `ProcessVertices`, `SetStreamSourceFreq` |
| Point sprites | **none**: no `D3DRS_POINTSPRITEENABLE` nor `POINTSIZE` set in the session, no `PointSprite` state in the 16 `.fx` files, and every draw is a triangle list |
| Render states set | `ALPHABLENDENABLE` 17,589, `ZWRITEENABLE` 2,026, `ZENABLE` 1,729, `SRCBLEND` 1,534, `DESTBLEND` 1,141, `BLENDOP` 992, `SEPARATEALPHABLENDENABLE` 990, `SRCBLENDALPHA`/`DESTBLENDALPHA` 982, `CULLMODE` 160, `ZFUNC` 149, and 22 others 1 to 11 times (alpha test, fog off, lighting off, `COLORWRITEENABLE`, `SRGBWRITEENABLE` off, two-sided stencil off, ...) |
| Formats asked | `CheckDeviceFormat` 841, all on an X8R8G8B8 adapter: A8R8G8B8 381 and R8G8B8 352 times (as texture and as render target), X8R8G8B8 8, and every other format of the list twice (DXT1 to DXT5, the depth and float formats, YUV), an enumeration |

## The shaders D3DX produces

The 16 `.fx` files are compiled by D3DX at start (`D3DXCreateEffectFromFileA` 16 times) into **46 bytecodes:
23 `vs_2_x` and 23 `ps_2_x`** (version tokens `0xFFFE0201` and `0xFFFF0201`: the effects' `vs_2_a` / `ps_2_a`),
27 of them distinct, 42,512 bytes; all are created before the first frame and none later in the session. Each
carries a `CTAB` comment; 14 also carry a `PRES` comment: a **preshader**, which D3DX's effect evaluates on the
CPU and not the GPU. The longest pixel shader has 101 instructions (more than `ps_2_0` allows).

They were captured as passed to `IDirect3DDevice9::CreateVertexShader` / `CreatePixelShader`: a copy of `Tag.exe`
importing a stand-in `d3d9.dll` under another name, whose `Direct3DCreate9` calls Wine's and patches the two
methods in the device's table to write the tokens to files first (kept outside the repository; the bytecodes are
in `out/oracle/tag/shaders/bin/`).

| | Instructions (count over the 23 shaders) | Modifiers, registers |
|---|---|---|
| `ps_2_x` | `add` 298, `cmp` 152, `dp3` 94, `nrm` 74, `mov` 54, `mul` 49, `mad` 40, `texld` 22, `pow` 21, `rsq` 10, `abs` 4, `dp2add` 4, `max` 4, `rcp` 4; `dcl` 58, `def` 14 | negation 230, `_sat` 63; inputs `t0`–`t2` (and `v0` once), samplers 18 `2d` and 1 `cube`, output `oC0` |
| `vs_2_x` | `dp4` 176, `dp3` 55, `mov` 29, `mad` 9, `add` 4, `mul` 4, `frc` 2, `max` 1, `min` 1, `rsq` 1; `dcl` 59, `def` 5 | negation 2; inputs position 23, texcoord0 18, normal 17, color 1; outputs `oPos`, `oT`, `oD0` once |

**Nothing outside plain shader model 2**: no flow control (`if`, `loop`, `rep`, `call`), no predication, no
relative addressing, no `texkill`, `texldp`, `texldb`, `texldl`, `dsx`/`dsy`, no `_pp` or `_centroid`, no integer or
boolean constants, no `sincos`, `lrp`, `exp`/`log`.

## D3DX, FMOD Ex and the socket

**D3DX** (`+snoop`, calls from `Tag.exe`): 40 of its 45 imported functions are called, 3,811,461 times over the
session, nearly all maths: `D3DXVec4Transform` 1,162,651, `D3DXMatrixMultiply` 917,805,
`D3DXMatrixRotationQuaternion` 379,628, `D3DXMatrixTranspose` 323,039, `D3DXQuaternionNormalize` 310,598,
`D3DXQuaternionMultiply` 299,874, `D3DXMatrixTranslation` 108,770, ... Then `D3DXIntersect` 9,380,
`D3DXGetImageInfoFromFileA` 3,181 (also while the game runs), `D3DXCreateTextureFromFileExA` 319,
`D3DXLoadMeshFromXA` and `D3DXLoadMeshHierarchyFromXA` 64 each, `D3DXComputeTangent` 64, `D3DXCreateLine` 22,
`D3DXCreateEffectFromFileA` 16, `D3DXCreateFontA` 10, `D3DXCreateTexture` 6, `D3DXCreateSprite` 5,
`D3DXCreateFontIndirectA` 3, `D3DXCreateBox`, `D3DXCreateSphere`, `D3DXCreateCubeTextureFromFileA` and
`D3DXMatrixPerspectiveFovLH` once.

**FMOD Ex** (`+snoop`): 21 of the 23 imported functions, 5,181 calls: `Channel::isPlaying` 762,
`Channel::set3DAttributes` 762, `Channel::setVolume` 662, `System::playSound` 649, `Channel::setLoopCount` 647,
`Channel::getLoopCount` 641, `System::set3DListenerAttributes` 191, `System::update` 191 (once a frame),
`Channel::getCurrentSound` 175, `Sound::getLength` 175, `Channel::getPosition` 175, `Channel::stop` 77,
`Sound::getName` 51, `System::createSound` 10, `Channel::setPaused` 5, `System::createStream` 2,
`Sound::setLoopCount` 2, and `FMOD_System_Create`, `System::init`, `System::set3DSettings`, `Channel::setMode` once.

**The UDP socket** is a LAN mode reached only from the developer console: `setnetmode host` or
`setnetmode client` creates the network object; the engine's start-up calls its initialisation, which returns at
once while there is none. When it runs: `WSAStartup(2.2)`, `socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)`, `bind` to
port **8008**, `ioctlsocket(FIONBIO)` (non-blocking), `setsockopt(SO_BROADCAST)`, then `sendto` / `recvfrom`
(broadcast). In the session neither `+winsock` nor `+relay` of `ws2_32` shows a single call.

## The lifter's census

`tools/census.sh` (its import status counts the files of every base).

| | `Tag.exe` | `d3dx9_35.dll` |
|---|---|---|
| Functions | 7,366 (3,113 hard, 4,253 soft) | 7,333 (5,169 hard, 2,164 soft) |
| Instructions | 334,333 distinct; 94.24 % of the code section reached | 971,445 distinct; 83.96 % reached |
| Jump tables | 185 recovered; 22 indirect jumps unresolved | 270 recovered; 6 unresolved |
| Not lifted | 40 (`mov` 18, `push` 11, `pop` 8, `daa` 2, `iretd` 1) | **7,727**: MMX and 3DNow (`movq` 2,423, `pfmul` 1,321, `pfadd` 745, `punpck*`, `femms` 129, ...), behind CPU detection |

Imports against what the runtime answers for (`pisco census Tag.exe --recipe recipes/tag`): 197 = 95 base,
1 d3d9, 1 dinput8, **100 not implemented**: d3dx9_35 45 (to be lifted), fmodex 23, ws2_32 12, oleaut32 4
(`#2`, `#6`, `#8`, `#9`), ole32 3 (`CoInitialize`, `CoCreateInstance`, `CoUninitialize`), user32 7
(`RegisterClassExA`, `GetWindowLongA`, `SetWindowLongA`, `GetSystemMetrics`, `EnumDisplayDevicesA`,
`SetCursorPos`, `ShowCursor`), kernel32 6 (`EnumSystemLocalesA`, `GetConsoleCP`, `GetLocaleInfoA/W`,
`IsDebuggerPresent`, `UnhandledExceptionFilter`).

`d3dx9_35.dll`'s 206: gdi32 22 of 27 (missing `GetGlyphOutlineA/W`, `GetOutlineTextMetricsA`,
`TranslateCharsetInfo`, `GetCurrentObject`), advapi32 3 of 3, kernel32 63 of 80 (missing the resource functions,
temporary files, `GetFullPathNameA`, `MoveFileA`, `GlobalMemoryStatus`, `InterlockedCompareExchange`,
`IsDBCSLeadByte`, `lstrcmpiA`, `ExpandEnvironmentStringsA`, `UnhandledExceptionFilter`), **msvcrt 44 of 96**
(missing: the `_CI*` maths, `_setjmp3`, the C++ exception and `type_info` functions, `operator new`/`delete`,
`_controlfp`, `_clearfp`, `_finite`, `_isnan`, `_fpclass`, `qsort`, `rand`, `srand`, `sscanf`, `atof`, `atol`,
`_itoa`, `_ultoa`, `_stricmp`, `_strnicmp`, `wcstombs`, `wctomb`, `isleadbyte`, the stdio internals `__pioinfo`,
`__badioinfo`, `_iob`, `_isatty`, CRT start-up `__dllonexit`, `_lock`, `_unlock`, `_XcptFilter`, `_purecall`,
`_strdate`, `_strtime`, `_tempnam`, `?terminate@@YAXXZ`).

## What this means for T1 to T4

- **No exception dispatch is needed** for normal play; `_setjmp3` / `longjmp` across D3DX's image decoders is,
  on every image file opened.
- `d3dx9_35.dll` wants `0x400000`, where `Tag.exe` (without relocations) sits: the DLL is relocated when loaded.
- Its MMX and 3DNow code (7,727 instructions) stays unreached only with a CPU without MMX, as for Narbacular
  Drop: the recipe gives `fpu`, `tsc`, `cx8` and the three `Direct3D` registry switches.
- T2: state blocks are the bulk of the calls (about 200,000 `Apply`/`Capture` in 150 frames); D3DX's preshaders
  run on the CPU; a 512x512 render target is read back every frame of play (`GetRenderTargetData`), which on
  WebGPU is an asynchronous copy.
- T3: the 46 bytecodes need only the plain SM2 arithmetic, `texld` on 2D and cube samplers, `dp2add`, `nrm`, `pow`,
  `cmp`, `abs`, `frc`; vertex inputs include no tangent or binormal in the session's shaders.
- T4: R32F render targets, a cube texture, `DrawPrimitiveUP`; no point sprites, no DXT, no `StretchRect`.

## Play it

```sh
target/release/pisco build recipes/tag --source ~/Downloads/Tag_setup.exe     # -> out/images/tag
node tools/serve.mjs 8000 web out/images/tag    # then http://127.0.0.1:8000/browser/index.html?image=/files/0/
```

In a desktop browser with WebGPU: click the page to give it the mouse (Pointer Lock), WASD to move, the mouse to
look, the left button to paint and the right button to erase once a gun is taken (level 2).
`TAG.ini` (full screen, 1024x768, the frame cap) is kept in the browser's storage between visits.

## Progress

- **T1, lift and start.** `pisco build recipes/tag` lifts `Tag.exe` with `d3dx9_35.dll` from the installer's cab
  beside it; under `SW_STRICT=1` the game reaches `CreateDevice` (full screen, 1024x768, D16). Its calls up to
  `Direct3DCreate9` are Wine's, step for step: `tools/oracle-wine/trace-diff.py recipes/tag/traces/start.txt
  <our SW_TRACE=2 log> --image 400000-570fff --until Direct3DCreate9` (699 steps; `HeapSize` had to answer the
  size asked for, as the CRT's `atexit` table grows on it).
- **T2, the main loop without a screen.** In Node, on the virtual clock and the null GPU back end, in strict mode,
  `menu-to-level1` plays to its end: menu, start, the paint splash, level 1 and its tooltips (4340 frames, 673,707
  draws, 13 pipelines, no inconsistent record). The 46 bytecodes the lifted D3DX compiles are Wine's byte for
  byte. Against Wine's `+d3d9` trace (`d3d9-compare.py`): the same 76 methods, 96 counted arguments with the same
  sets of values; textures made and locked differ in number only as long phases do (the same 86 texture files are
  opened, `SW_FILES=1`). FMOD Ex came here, not in T5: strict mode needs `FMOD_System_Create`.
- **T3, the shader translator.** `rt/windows-x86/d3d9_sm.c` turns Direct3D 9 bytecode, vs_1_1 to vs_2_x and ps_1_1
  to ps_2_x, into WGSL: a pure function of the tokens and a link key (fog, alpha test, the samplers' types for
  ps_1_x), beside the fixed-function generator and sharing its locations between the stages. Labels become WGSL
  functions, loops stop at 255 iterations as Direct3D's do, `def` constants win over the constant block, ps_1_x
  constants are clamped to [-1, 1]. What it does not take (shader model 3, ps_1_x's texture-addressing operations,
  `vPos` / `vFace`) it refuses with the reason. `tests/run.sh sm`: shaders assembled token by token cover every
  opcode, register, modifier and flow-control form from 1.1 to 2.x and pass naga, and bad inputs are refused;
  Tag's 46 bytecodes are all translated (their WGSL compiles in Chrome since T4).
- **T4, pixels.** In headless Chrome (WebGPU on SwiftShader), strict, on the virtual clock, `tests/run.sh tag` plays
  `menu-to-level1` with every title shader translated (21 WGSL modules, 13 pipelines, 673,707 draws, no draw left
  out, no WebGPU error) and sets the checkpoints against Wine's: level 1 1.24 / 0.95 / 1.35 of 255 on average
  (2.3 % of pixels over 16), its tooltips 1.37 / 1.20 / 1.42, the menu 5.1 / 5.2 / 4.3 (the logo's colours are
  random); the paint splash differs as its random splats and its timing do. What it took: infinities made at run
  time (Tint refuses `bitcast<f32>(0x7f800000u)` as a constant, naga does not), `GetRenderTargetData` reading the
  GPU's pixels back (1544 times in the session), bytes copied into render targets with `UpdateTexture` sent to the
  GPU (each wall's paint map), a depth buffer larger than its target. Not needed by Tag, so not done: point
  sprites with a vertex shader, a programmable stage beside a fixed-function one, BC textures, `StretchRect`.
- **T5, sound, input, saves.** FMOD Ex is set against Wine's `+snoop` log with the same kind of trace on our side
  (`SW_TRACE_DLL=fmodex`, `tools/oracle-wine/snoop-compare.py`): over `menu-to-level1` the first 531 loads, plays,
  pauses and settings are Wine's, call site and result, and the calls made once (51 `getName`, 10 `createSound`,
  2 `createStream`, 5 `setPaused`) are as many; after that Wine's phases end at other moments of its wall clock.
  It took two answers Tag's `fmodex.dll` gives under Wine: a channel that played to its end keeps its handle
  (`isPlaying` false, never `FMOD_ERR_INVALID_HANDLE`), and `Sound::getName` is the file's name without folders.
  Input is DirectInput's, as for Narbacular Drop: `level2-paint` in Node, strict, reaches level 2, takes the gun,
  paints the floor and a wall and erases (`tests/run.sh tagplay`); in headless Chrome the same session's shots against
  the oracle's (`paint5`): level 1 1.4, level 2 on arrival 5.5, then from the gun on 31 to 39 of 255 on average,
  where two of Wine's own runs differ by 25 to 72 (the walk's length follows Wine's wall-clock frame rate: the player
  stands a step elsewhere; the scene, the gun and the paint are the same). Saves: `AutoSave.xml` turned out to be level
  data the installer ships, never written (Wine's copy keeps its 2008 date after every session); what Tag writes is
  `TAG.ini`, which the saves folder keeps and a second run reads back (edited to 800x600, its targets are).
- **Played by hand** (2026-11-26), in Chrome on an M1 MacBook Pro, after three fixes found with a Chrome trace
  ([hosts.md](hosts.md)): file checks answered once in the browser's worker, only the changed bytes of a buffer
  locked whole sent to the GPU, and file mappings freed (Tag grew by about 100 KB a frame and failed at 2 GiB).
  It ran at the game's own cap (`fMaxFPS = 60`) through level 3 with nothing noticeably wrong. Tag has no jump key:
  green paint makes the player jump.
  Known gap: in level 3 the console says `Direct3D 9: a vertex shader reads v1 (usage 5, index 0), which the
  declaration does not have: not drawn`. A shader reads TEXCOORD0 that the draw's declaration lacks; Direct3D 9
  drivers and Wine give such an input (0, 0, 0, 1) and draw. The draw is left out here, with nothing visibly
  missing; the fix (that constant in the translated shader, `d3d9_sm.c`) waits for a level 3 checkpoint against Wine.
