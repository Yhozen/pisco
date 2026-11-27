# Medal of Honor: Allied Assault, the single-player demo (2026-11-27)

The first OpenGL title, and the first on id Tech 3: the demo of 2015 Inc.'s Medal of Honor: Allied Assault
(Electronic Arts, 2001), a 32-bit Windows game on Ritual's derivative of the Quake III engine. Its one level is a
mission of the full game. It runs on the Windows x86 runtime, and asks of it what Narbacular Drop and Tag did not:
OpenGL 1.1 with its extensions of the time, the Miles Sound System over DirectSound, Winsock (the engine talks to its
own server through a loopback socket), and game code in DLLs it loads from a subfolder. This page starts as the
census and the oracle (milestone M0 of the plan below), measured before any runtime code was written for the title.
The game's files, logs and screenshots stay outside the repository (under `out/oracle/mohaa/`): this page holds
names and counts only.

## The files

| | |
|---|---|
| Download | `mohaaspdemo.zip`, 183,768,515 bytes, sha256 `236e23d9bcf07ac9a6b5a862a4b6f25f5566ceeab4072278b54407e9ffc22197`; one file, `mohaaspdemo.exe` |
| `mohaaspdemo.exe` | a Wise installer, 183,756,238 bytes, sha256 `9f7bfe82c8e62490836ee285ce9b6b34044358a2a299c55d1a1653e17f98728e`: a 14 kB setup stub, then 28 streams, each raw deflate followed by its CRC-32. One of them is the script; it installs to `%TEMP%\mohaainstall` an InstallShield 6 setup (`Setup.exe`, `data1.hdr`, `data1.cab`, `data2.cab`, ...) and DirectX 8's redistributable, and runs `Setup.exe` |
| The InstallShield setup | version `0x0100600c` (6); `data1.hdr` lists 870 files in 60 folders and 49 file groups. Each file in `data<N>.cab` is a series of chunks (a 16-bit length, then a deflate stream). The groups' destinations are in the setup's compiled script (`setup.inx`), which is not read: the recipe says where each group goes |
| Installed | 801 files, 186 MB, in `C:\Program Files\EA GAMES\MOHAADemo` |

What is installed, by part:

| File | Size | What it is |
|---|---|---|
| `MOHAADemo.exe` | 1,519,662 | the engine: renderer, client, sound, input, the server's frame. PE32, linked 2001-11-27, not packed; sha256 `2884bff1...cb73501e4` |
| `system86.dll` | 106,539 | the engine's allocator (`MemoryMalloc`, `MemoryFree`), imported by the exe and by both game DLLs |
| `main/gamex86.dll`, `main/cgamex86.dll` | 2,207,797, 630,838 | the server's game and the client's game, native x86 (not Quake III's bytecode), loaded with `LoadLibrary` from `main\` |
| `main/Pak0.pk3` to `Pak7.pk3` | 103 MB | zips: maps, models, textures (TGA, JPEG), shaders, scripts, sounds |
| `main/video/*.RoQ`, `*.mp3` | | the 2015 and EA logos: id's RoQ video, decoded by the engine, with their sound as MP3 |
| `main/sound/` | 791 files | dialogue (WAV), music and ambient stereo (MP3), streamed from the folder |
| `snddrivers/Mss32.dll` and plugins | 348,160 + 4 | the Miles Sound System 6.1c, its MP3 decoder (`Mp3dec.asi`), 2D positional provider (`Mssfast.m3d`) and two filters (`.flt`); all DLLs |
| `main/ffx86.dll`, `IFC22.dll` | | Immersion's force feedback for a force-feedback mouse; the engine finds none and unloads it |
| `ijl10.dll` | 137,728 | Intel's JPEG library |
| `3dfxgl.dll`, `voodoogl.dll` | | 3dfx's OpenGL drivers, used only when `r_glDriver` names them |
| `Configure.exe`, `eReg/`, `redist/directx8/` | | the settings program, the registration reminder, DirectX 8's installer: not run |

### What the engine imports

`pisco census` of the exe against `base/windows-x86:1`: 246 imports from 9 DLLs (kernel32 129, user32 57, wsock32
24, gdi32 14, winmm 11, advapi32 6, system86 4, shell32 1), 153 implemented by the base, 4 lifted with the title
(`system86.dll`), 89 not implemented: mostly small (`GlobalAlloc`, the clipboard, `ChangeDisplaySettingsA`,
`SetWindowsHookExA`, the pixel-format functions of gdi32, `SwapBuffers`, `joyGetPosEx`, seven of Winsock's by
ordinal). The game DLLs import kernel32 only (80 functions, 8 not implemented) and `system86.dll`.

The renderer does not import OpenGL: it loads `C:\WINDOWS\system32\opengl32.dll` with `LoadLibrary` and asks
`GetProcAddress` for all 336 functions of OpenGL 1.1 and the `wgl` ones (the `qgl` table of id Tech 3), then the
extensions' functions with `wglGetProcAddress`. It uses `GL_ARB_multitexture`, `GL_EXT_compiled_vertex_array`,
`GL_ARB_texture_compression` with `GL_EXT_texture_compression_s3tc`, `GL_EXT_texture_env_add` and
`WGL_EXT_swap_control` when offered, and draws without each when not.

Miles is loaded the same way (`snddrivers\mss32.dll`), and in turn loads `DSOUND.DLL` and calls
`DirectSoundCreate` (it can use `waveOut` instead). DirectInput is `dinput.dll` (version 7), also loaded by name.

## The oracle

Wine 11.22 runs the demo on Mesa's llvmpipe (OpenGL 4.5 in software) in Xvfb, in its own prefix:

```
tools/oracle-wine/oracle.sh prefix mohaa ~/games/mohaaspdemo.exe   # the Wise installer: the InstallShield wizard, clicked through
MESA_EXTENSION_MAX_YEAR=2001 tools/oracle-wine/oracle.sh start mohaa 'C:\Program Files (x86)\EA GAMES\MOHAADemo\MOHAADemo.exe'
```

- **`MESA_EXTENSION_MAX_YEAR=2001`** is needed: the engine copies `GL_EXTENSIONS` into a fixed buffer, and Mesa's
  list (over 4 kB) overflows it; the process ends without a word after `...WGL_3DFX_gamma_control not found`.
  With the year set, Mesa lists 74 extensions and the demo reaches its menu every time.
- The engine writes its console to `main\qconsole.log`: the start-up checkpoints are compared by its lines.
- What Wine's run says (`qconsole.log`): mode 4 (800x600, full screen), a 32-bit colour, 24-bit depth and 8-bit
  stencil pixel format; DirectInput's cooperative level fails in Wine's virtual desktop and the engine falls back
  to the Win32 mouse; "Opening IP socket: localhost:12203"; "Sound opened using Direct Sound" with the provider
  "Miles Fast 2D Positional Audio".
- One choice depends on wall time: at start the renderer times `glDrawPixels` against `glTexSubImage2D` for the
  menu's background ("profiling DrawBackground methods") and keeps the faster. Our run, on the virtual clock, makes
  the same choice every time; which one is recorded, not judged.
- In the main menu, Wine's `+opengl` trace shows 45 functions of OpenGL called: immediate mode (`glBegin`,
  `glVertex2f`, `glTexCoord2f`, `glColor4ubv`), `glDrawElements` of locked arrays (`glLockArraysEXT`), textures
  (`glTexImage2D`, `glTexSubImage2D`, `glTexParameterf`), `glDrawPixels`, the matrix stack, blending, the scissor.

## The image

`pisco build recipes/mohaa --source mohaaspdemo.zip` reads the zip, the Wise installer inside it and the InstallShield
setup inside that (`extract = "wise"` with `[source.installshield.groups]`, `crates/pisco/src/installers.rs`), with
no program of the installer's run. `--compare` against Wine's installed folder: all 801 files equal (the 6 that
differ are those the game wrote). The exe and `system86.dll` are lifted and the image links (90 s); the game DLLs in
`main\` and Miles in `snddrivers\` are not lifted yet (M1).

## Running it

```sh
target/release/pisco build recipes/mohaa --source ~/games/mohaaspdemo.zip --out out/img-mohaa
SW_CLOCK=16667 target/release/pisco run out/img-mohaa --session recipes/mohaa/sessions/menu.json -- +set logfile 2
tools/oracle-wine/mohaa-play.sh recipes/mohaa/sessions/menu.json menu1      # the same session under Wine
```

- **Sessions** (`recipes/mohaa/sessions/`): `menu` (the logos, then the menu, untouched) and `level` (through the
  menu into the level: the big map, Easy, the briefing's Continue, Esc on the help screen; the level's first view).
  The engine's own cursor starts at the top left and moves one for one with the mouse.
- **The oracle** (`tools/oracle-wine/mohaa-play.sh`) starts each run from the state of the first start after
  Configure.exe: the two markers it leaves (`sm.001`, `sm.002`; the engine deletes `sm.002` once it has applied
  `newconfig.cfg`), none of what the engine leaves itself (`sm.000`, `main\configs`, `main\save`, the console log,
  `HKCU\Software\2015\MOHAA`), as our image starts every run.
- **The console log** (`main\qconsole.log`, with `+set logfile 2`) is compared line by line with Wine's by
  `tools/oracle-wine/mohaa-console.py`, once what describes the machine (CPU, OpenGL driver, pixel formats, sound
  driver, DirectInput, host name, times) is left out.

## What M1 took

- **MMX in the lifter.** The engine fills memory with MMX unconditionally (the demo needs a Pentium with MMX); Miles
  has MMX mixers. MMX gets eight registers of its own (`c->mm[]`); each operation is the SSE2 one on the low half of a
  vector, the unpacks of the high halves and the packs arranged so the two agree. Before, an MMX instruction the
  lifter recognised was lifted as its SSE form on `xmm0`-`xmm7`, silently. `fixtures/mmx` (written in assembly: a
  compiler of today makes SSE2 of MMX's intrinsics) prints 63 lines equal to Wine's on a real CPU.
- **Miles' mixer** (`MSSMIXER`, hand-written) saves DS and ES around its routines: `push`/`pop` of DS, ES and SS are
  stack moves in a flat address space, and the lifter's discovery accepts code that saves them in pairs and returns.
- **DLLs the engine loads itself**: `[lifter] load` lifts `main\gamex86.dll`, `main\cgamex86.dll`,
  `main\ffx86.dll`, Miles and its plugins with the exe; such a DLL is loaded on demand (DllMain at LoadLibrary, its
  sections read again if it is loaded a second time after its last FreeLibrary).
- **Configure.exe is not run.** The engine starts it when the markers it leaves are missing; the recipe's `[files]`
  gives the two markers and `newconfig.cfg` as it wrote them under Wine.
- **user32**: input becomes window messages (the engine reads keys from `WM_KEYDOWN`'s scan code and the mouse
  buttons from the messages' `MK_*` bits, and moves its cursor by `GetCursorPos` against the window's centre, which it
  sets back each frame); a window created visible is activated and focused as `ShowWindow` would; a mouse that
  DirectInput holds exclusively gets no messages (Narbacular Drop's menu relies on it).
- **Winsock**: wsock32 numbers `inet_addr`, `inet_ntoa` and `ioctlsocket` 10, 11, 12 where ws2_32 has 11, 12, 10;
  forwarding them as the same numbers made the engine's `ioctlsocket(FIONBIO)` an `inet_addr`, and its socket blocking.
- **The virtual clock**: `timeGetTime` moves it on when a title waits by reading it, as `QueryPerformanceCounter` did.
- **Files**: what is still open for writing when the process ends (the console log) is saved, as Windows would have
  written it.

## The plan

| # | Milestone | Passes when |
|---|---|---|
| M0 | Census, recipe, oracle: `pisco build` reads Wise installers and InstallShield 5 / 6 cabinets | the build's files equal Wine's install; Wine reaches the menu every time |
| M1 | Lift and start in Node: MMX, the DLLs in subfolders lifted and loaded by path, the missing Win32 functions, Winsock's loopback, input as window messages, an `opengl32` library (OpenGL 1.1 on the GPU ring) | strict, on the virtual clock, the `menu` and `level` sessions end with exit code 0, every record consistent; the console log is Wine's but for what describes the machine |
| M2 | OpenGL's pixels in Chrome | the menu and the level's first view in headless Chrome are within Tag's threshold (3 of 255 on average) of Wine's screenshots |
| M3 | Sound: DirectSound for Miles (lifted, with its MP3 decoder): the logos' and the menu's music, the level's sounds | what the title plays is close to what Wine records, and the same on every run |
| M4 | Playable by hand on a real GPU; the docs | played from the logos into the level |

## Progress

- **M0** (2026-11-27): the census and the oracle above; `extract = "wise"` and InstallShield's cabinets in
  `pisco build`; the recipe.
- **M1** (2026-11-27): in Node, strict, on the virtual clock, the `menu` session runs from the logos to the menu
  (2,649 frames, 48,715 draws, every record consistent for the null back end) and the `level` session into the level
  (5,301 frames; the briefing, the help screen, the level's first view with 270 draws a frame). The console log is
  Wine's but for what describes the machine: 58 of 58 lines for `menu`, 78 of 78 for `level`. Without a sound driver
  (M3) the engine says so and goes on (`tests/run.sh mohaa`).
