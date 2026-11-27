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

## The plan

| # | Milestone | Passes when |
|---|---|---|
| M0 | Census, recipe, oracle: `pisco build` reads Wise installers and InstallShield 5 / 6 cabinets | the build's files equal Wine's install; Wine reaches the menu every time |
| M1 | Lift and start in Node: the DLLs in subfolders lifted and loaded by path; the missing Win32 functions; Winsock's loopback; an `opengl32` library (`wgl`, OpenGL 1.1's state, records for the null GPU) | strict, the engine reaches its menu in Node, and `qconsole.log`'s start-up lines are Wine's (the GL strings and timings aside) |
| M2 | OpenGL on WebGPU: the fixed-function pipeline of `d3d9_ffp.c` from OpenGL's state (texture environments, two texture units, alpha test, fog, polygon offset, depth range), client arrays and immediate mode, `glDrawPixels` | the menu in headless Chrome is within Tag's threshold of Wine's screenshot |
| M3 | The level in Node: the keyboard and the mouse from Win32 messages, sessions through the menu into the demo's level, on the virtual clock | strict, the level loads and plays a scripted session; the console's checkpoints are Wine's |
| M4 | The level's pixels in Chrome | the still screens are within the threshold of Wine's |
| M5 | Sound: DirectSound for Miles (lifted, with its MP3 decoder), the intro videos' sound | what the title plays is close to what Wine records |
| M6 | Playable by hand on a real GPU; the docs | played from the logos through the level |

## Progress

- **M0** (2026-11-27): the census and the oracle above; `extract = "wise"` and InstallShield's cabinets in
  `pisco build`; the recipe.
