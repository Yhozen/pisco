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

Screenshots and Wine's log go to the run folder, `out/oracle/<title>/` (`ORACLE_RUN` overrides it). `out/` is
ignored by git: screenshots and logs of a title never enter the repository.

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

## What a headless run needs (found the hard way)

- **The X focus.** With no window manager nothing gives Wine's window the input focus, and without it Wine
  reads no raw mouse motion: a DirectInput title sees buttons but no movement. `{"focus": true}` sets it.
- **Relative motion only.** An absolute pointer warp arrives as one huge movement.
- **A virtual desktop** (`prefix` sets 1024x768): a full-screen title otherwise minimises when it loses focus.
- **`LANG=C.UTF-8`**: an installer with a non-ASCII file name fails in the C locale.
- **No Mono / Gecko** (`WINEDLLOVERRIDES`): their install prompts wait for a click nobody can give.
- **A prefix whose first boot was interrupted is broken** (32-bit programs fail to load `kernel32`); `prefix`
  recreates one that has no `system.reg`.
- **No crash debugger** (`winedbg.exe=d`): a crashing program otherwise waits in Wine's debugger for input.
- **No sound device here**: FMOD's `FSOUND_Init` fails, so a title loads no sounds. A sound census needs a
  PulseAudio-compatible server first.
