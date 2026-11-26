# The Wine oracle

Runs a Windows title under Wine with no screen, driven by a script, so that our own runs have something to be
compared with: screenshots at named points, and traces of what the title asked Windows for. Nothing here runs
in the final product.

```sh
tools/oracle-wine/setup.sh                       # portable Wine (no 32-bit system libraries needed), python-xlib
O=tools/oracle-wine/oracle.sh
$O prefix narbacular-drop ~/games/narbacular_drop_setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-
$O start  narbacular-drop 'C:\Program Files (x86)\Narbacular Drop\Narbacular Drop.exe'
$O play   narbacular-drop recipes/narbacular-drop/sessions/menu-to-level.json
$O stop   narbacular-drop
```

Screenshots, Wine's log and what the title played (`wine-audio.wav`) go to the run folder, `out/oracle/<title>/`
(`ORACLE_RUN` overrides it). `out/` is ignored by git: screenshots, sound and logs of a title never enter the repository.

## The sound device

Wine plays into a PipeWire null sink, run as the user with no hardware and no root (`oracle.sh sound start`, which
`start` and `run` do when needed; `ORACLE_SOUND=0` runs a title without a device, as before M5): one `pipewire`
daemon with `pipewire.conf` (a null sink consumed in real time by PipeWire's dummy driver, and the PulseAudio
protocol, which Wine's `winepulse.drv` speaks), and `wireplumber`, which links the streams. Its runtime folder is
`$ORACLE_HOME/sound`. While a title runs, ffmpeg records the sink's monitor (48 kHz stereo) to the run folder's
`wine-audio.wav`; `stop` ends the recording. ALSA's `null` plugin is not an alternative: it is not paced, and Wine
then plays several times faster than real time.

```sh
python3 tools/oracle-wine/fsound-compare.py out/oracle/narbacular-drop/portals/wine.log out/nd.play.log   # +snoop FMOD calls against ours (SW_FMOD_TRACE=1)
python3 tools/oracle-wine/audio-compare.py out/nd.play.wav out/oracle/narbacular-drop/portals/wine-audio.wav  # levels and loudness envelopes
```

## Session scripts

One JSON file, played in order. The same file is meant to drive our own hosts (from milestone M3 on), so a
session describes what a player does, not how a host does it.

```json
{ "title": "narbacular-drop", "about": "...", "steps": [
  {"wait": 40000}, {"focus": true}, {"shot": "menu"},
  {"move": [-208, -95], "ms": 600}, {"click": "left"}, {"key": "w", "ms": 2000} ] }
```

| Step | Meaning |
|---|---|
| `{"wait": ms}` | let time pass |
| `{"focus": true}` | give the title the input focus (see below) |
| `{"move": [dx, dy], "ms": n}` | move the mouse by a relative amount, spread over `ms` |
| `{"click": "left"\|"right", "ms": n}` | press and release a button |
| `{"button": "right", "down": true\|false}` | press or release only |
| `{"key": "w", "ms": n}` or `{"key": "w", "down": true\|false}` | a key by X keysym name, or `space`, `enter`, `esc`, `up`, ... |
| `{"shot": "name"}` | save `<name>.png` (the title's client area) and `<name>.full.png` (the screen) |

`play.py` turns X's pointer acceleration off first: X would otherwise scale every motion event of more than 4
pixels, and the title under Wine would see other deltas than the session gives (our runtime passes them on unchanged).

In our own runtime the same file drives the title through `SW_SESSION=<session.json>` (the Node host and
`tools/run-chrome.mjs`; the steps reach the runtime's session player, `rt/windows-x86/input.c`). Time there is the
title's clock: with `SW_CLOCK=16667` it is virtual and moves by that many microseconds a presented frame, so
`wait` counts frames and a run is the same every time. `focus` does nothing (the title's window always has the
focus), and `shot` writes a line `checkpoint <name> frame <n>` with the Direct3D 9 calls made since the checkpoint
before. In headless Chrome the frame presented at a `shot` is also saved, as `<the wasm's folder>/shots/<name>.png`.

## Traces

`TRACE` sets Wine's debug channels for `start`; `summarize.py` turns the log into names and counts.

```sh
$O reg narbacular-drop add 'HKCU\Software\Wine\Debug' /v RelayFromInclude /d 'Narbacular Drop.exe' /f
ORACLE_RUN=out/oracle/narbacular-drop/relay TRACE=+relay,+seh $O start narbacular-drop '...exe'   # then play, stop
python3 tools/oracle-wine/summarize.py out/oracle/narbacular-drop/relay/wine.log --image 400000-57bfff
```

| Channel | Shows | Note |
|---|---|---|
| `+relay` | calls into Wine's own DLLs | system processes log everything; `summarize.py --image` keeps the calls made from the title's image |
| `+snoop` | calls into DLLs the title ships | a packed DLL must be unpacked first (`upx -d`): snoop skips exports whose first byte is 0 when the DLL loads |
| `+d3d9` | every Direct3D 9 method with its arguments | about 0.5 GB for two minutes of play |
| `+seh` | exceptions raised | |

## Comparing a run of ours with Wine's

```sh
# once, from a +relay log: Wine's calls from the title's image, repeats folded (names and counts only)
python3 tools/oracle-wine/trace-diff.py out/oracle/narbacular-drop/relay/wine.log --image 400000-57bfff \
  --until Direct3DCreate9 --record recipes/narbacular-drop/traces/start.txt
# then, any time: our run (SW_TRACE=2) against the recording; it shows the first place the two differ
python3 tools/oracle-wine/trace-diff.py recipes/narbacular-drop/traces/start.txt out/nd.run.log --until Direct3DCreate9
```

Repeats of one function are folded because how often it is called in a row depends on the machine (the
environment's size, the locale); which function comes next should not. Some calls never appear in Wine's
trace at all (thread and process ids, locks, interlocked operations, the performance counter); the tool leaves
them out of our side too.

`oracle.sh run <title> <program.exe>` runs a console program to its end and prints its output: how the
expected output of `fixtures/x87` is made.

## Direct3D 9: asking the oracle, and comparing with it

A trace shows what a title asked, not what it was told. `d3d9caps.c` (the device's capabilities) and
`d3d9probe.c` (the adapter and its modes, `CheckDeviceFormat` for the usual formats and uses, every state of a new
device, and corner cases one a line) are small Windows programs that print Direct3D's answers under Wine;
`tools/gen-d3d9-oracle.py` turns `d3d9caps.txt` and `d3d9probe.txt` into `rt/windows-x86/d3d9_oracle.gen.h`, which our
Direct3D 9 answers from. A question our device cannot answer from them is a reason to add a line to the probe:

```sh
$MINGW/bin/i686-w64-mingw32-clang tools/oracle-wine/d3d9probe.c -o out/caps32/d3d9probe.exe -ld3d9 -luser32
$O run narbacular-drop out/caps32/d3d9probe.exe | tr -d '\r' > tools/oracle-wine/d3d9probe.txt   # in a title's prefix: its virtual desktop decides the modes
python3 tools/gen-d3d9-oracle.py > rt/windows-x86/d3d9_oracle.gen.h
```

`d3d9-compare.py` sets a session in our runtime against Wine's `+d3d9` trace of it: the methods called, the
values of the arguments that matter, and what one frame is made of (the two runs differ in how many frames a
phase lasts, not in what a frame of a phase calls):

```sh
SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 SW_D3D9_FRAMES=1 SW_SESSION=recipes/narbacular-drop/sessions/menu-to-level.json \
  node web/run-node.mjs out/nd/title.wasm '<folder>/Narbacular Drop.exe' > out/nd.session.log
python3 tools/oracle-wine/d3d9-compare.py out/oracle/narbacular-drop/d3d9/wine.log out/nd.session.log
```

Counts that are not per frame (textures made, locks) can differ because a phase lasts longer on one side: Wine
tracing `+d3d9` draws far fewer frames. To tell that from a different path, set the files opened on each side:
`SW_FILES=1` prints every file our runtime opens (`file <name>`), Wine's `TRACE=+file` the same as `CreateFileW`
(the oracle needs its sound on, or Tag stops at FMOD's error box). For Tag both open the same 86 textures, and
only how often the particle texture is opened differs.

## GDI: asking the oracle about fonts

`gdiprobe.c` asks Wine's GDI about the fonts Narbacular Drop makes (the face it picks, metrics, kerning, ranges,
glyph indices, ABC widths, placements) and draws every glyph the game and D3DX draw, as they draw them;
`gdi-compare.py` sets the answers of `rt/windows-x86/font.c` (printed in the same format by `tests/gdi/fonts.c`) against
them. Its output holds pixels of the host's fonts, so it stays under `out/`:

```sh
$MINGW/bin/i686-w64-mingw32-clang tools/oracle-wine/gdiprobe.c -o out/caps32/gdiprobe.exe -lgdi32 -luser32
$O run narbacular-drop out/caps32/gdiprobe.exe | tr -d '\r' > out/oracle/narbacular-drop/gdiprobe.txt
tests/run.sh gdi
```

## Comparing screenshots

`shot-compare.py` sets our PNG of a checkpoint against the oracle's: the mean absolute difference of each colour
channel (0 to 255), the share of pixels where a channel differs by more than a threshold (16 unless told), and the
largest difference. Only Python's standard library (PNGs are read with `zlib`).

```sh
python3 tools/oracle-wine/shot-compare.py out/nd/shots/intro.png out/oracle/narbacular-drop/d3d9/intro.png
python3 tools/oracle-wine/shot-compare.py --table out/nd/shots out/oracle/narbacular-drop/d3d9 --diff out/nd/shots-diff   # every checkpoint; --diff: the differences as pictures
python3 tools/oracle-wine/shot-compare.py out/nd/shots/level-hint.png out/oracle/narbacular-drop/plain/level-hint.png --box 60,48,560,96   # a rectangle only
```

A still screen should be all but equal. A scene that moves (a menu with flickering torches, a level with flames and
a creature walking) is shot at another moment of its animation: the oracle's clock is the wall's, ours is virtual.
Two of the oracle's own shots of such a scene (two runs, or a second apart) say how much that alone is worth.

**The oracle's CPU is not the title's.** Wine runs on this machine's processor, with MMX and SSE; a recipe may give
our virtual CPU less (Narbacular Drop's has neither). D3DX decodes and resamples a title's images with
processor-specific code, and its plain code rounds differently: under Wine every still screen then differs from
ours by a level or two for that reason alone. For a like-for-like reference, turn that code off in the oracle
(three values D3DX reads, in the registry's 32-bit view), play the session into a folder of its own, and take the
values out again:

```sh
for v in DisableD3DXPSGP DisablePSGP DisableMMX; do $O reg narbacular-drop add 'HKLM\Software\Microsoft\Direct3D' /v $v /t REG_DWORD /d 1 /f /reg:32; done
export ORACLE_RUN=$PWD/out/oracle/narbacular-drop/plain
$O start narbacular-drop "$EXE" && $O play narbacular-drop $S; $O stop narbacular-drop; unset ORACLE_RUN
$O reg narbacular-drop delete 'HKLM\Software\Microsoft\Direct3D' /f /reg:32
```

`tests/gfx/nd.sh` compares with that folder when it is there (the still screens then agree to one level in 255).

## What a headless run needs (found the hard way)

- **The X focus.** With no window manager nothing gives Wine's window the input focus, and without it Wine
  reads no raw mouse motion: a DirectInput title sees buttons but no movement. `{"focus": true}` sets it.
- **Relative motion only.** An absolute pointer warp arrives as one huge movement.
- **A virtual desktop** (`prefix` sets 1024x768, or `ORACLE_SCREEN`): a full-screen title otherwise minimises when it
  loses focus. A title whose window is larger takes a bigger screen on a display of its own (Desire: 1600x900 on
  `:78`, docs/desire.md), since one X server serves every title on a display.
- **Direct3D 12** runs through Wine's `d3d12` (vkd3d) on lavapipe, Mesa's software Vulkan driver:
  `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`.
- **Window titles**: with `+relay` and `RelayInclude` holding `user32.SetWindowTextW`, `play.py` writes the title
  current at each shot to `<shot>.title.txt`.
- **`LANG=C.UTF-8`**: an installer with a non-ASCII file name fails in the C locale.
- **No Mono / Gecko** (`WINEDLLOVERRIDES`): their install prompts wait for a click nobody can give.
- **A prefix whose first boot was interrupted is broken** (32-bit programs fail to load `kernel32`); `prefix`
  recreates one that has no `system.reg`.
- **No crash debugger** (`winedbg.exe=d`): a crashing program otherwise waits in Wine's debugger for input.
- **A sound device** (above): without one FMOD's `FSOUND_Init` fails, and a title loads no sounds.
- **Heap filling** (`warn+heap`, always on in `start`): Wine's heap then fills new blocks with 0x55, as our runtime
  fills a 32-bit title's with 0xCD. Narbacular Drop reads a field of a new block before it sets it; with sound
  loaded, the zeroed blocks Wine handed out otherwise made it crash entering its first level.
