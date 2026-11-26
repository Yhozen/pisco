# Title images

A Windows PC title is built from a **recipe** into a **title image**: a folder in the OCI image layout holding the
title's files, the runtime's files, the compiled module and a config. `pisco` (`crates/pisco`) builds, runs and inspects
images; the hosts read them through one module, [`web/image.mjs`](../web/image.mjs).

```sh
cargo build --release -p pisco -p pisco-lifter
target/release/pisco build recipes/narbacular-drop --source ~/Downloads/narbacular_drop_setup.exe   # -> out/images/narbacular-drop
target/release/pisco run out/images/narbacular-drop --session recipes/narbacular-drop/sessions/menu-to-level.json   # Node
target/release/pisco run out/images/narbacular-drop --chrome                                                    # headless Chrome
target/release/pisco inspect out/images/narbacular-drop [--files]
target/release/pisco census out/images/narbacular-drop      # or a title's .exe: what the base and its libraries answer for
```

The model is OCI's, with three differences:

1. **No `RUN`.** We cannot run an installer inside our own runtime. A recipe's steps run on the builder's machine,
   with a fixed vocabulary: extract the installer, place its files, set registry values.
2. **Code is layered at build time.** A title is one statically linked wasm module. A base's "libraries" are sets of
   runtime sources (with cached objects); the image carries only the linked module.
3. **Game layers are never published.** The base and library layers and the recipe can be shared; the game layer
   and the module are built locally from the user's own installer, checked by its sha256. They are marked
   `org.pisco.local-only` in the manifest. Nothing of them is ever committed (`out/` is ignored).

## The format

An image is a directory in the [OCI image layout](https://github.com/opencontainers/image-spec/blob/main/image-layout.md):

```
oci-layout                {"imageLayoutVersion":"1.0.0"}
index.json                an image index with one manifest, its org.opencontainers.image.ref.name = the title's name
blobs/sha256/<digest>     the manifest, the config, the layers and the module, each named by its sha256
```

The manifest is an ordinary `application/vnd.oci.image.manifest.v1+json` with an `artifactType`, a config and
layers of our own media types:

| Media type | What it is |
|---|---|
| `application/vnd.pisco.title.v1` | the manifest's `artifactType`: a PISCO title |
| `application/vnd.pisco.title.config.v1+json` | the config (below) |
| `application/vnd.pisco.layer.v1.tar` | a file layer: an uncompressed tar of files on the title's C: drive (paths relative to `C:\`) |
| `application/vnd.pisco.module.v1.wasm` | the title's module: lifted code, runtime and import tables, linked (`title.wasm`) |

The file layers come in this order, each with the annotation `org.pisco.layer.role`:

| Role | Content | Shared |
|---|---|---|
| `base` | the base's C: drive: `C:\WINDOWS` (`win.ini`, `system.ini`, `Temp`, the fonts gdi32 draws with), the user's folders, and a placeholder in `C:\WINDOWS\system32` for every DLL the base's runtime answers for | by every title FROM the base |
| `library` | one per library the recipe names: a placeholder for each DLL it answers for | by every title that names it |
| `game` | the files the installer installs, where it installs them | never (local only) |

then the module, also local only. A later layer's file replaces an earlier one's, and an entry `.wh.<name>`
removes `<name>` from the layers below (OCI's whiteouts; no layer we build uses them yet).

Layers are uncompressed so a host reads a file at its offset in the blob. They are deterministic: entries sorted,
directories before what they hold, every mtime, uid and gid 0, mode 0755 for directories and 0644 for files, a pax
`path` record for a name ustar cannot hold. All JSON documents are written with sorted keys and no white space,
and nothing in them depends on the time or the machine, so the same inputs give the same digests.

### The config

```json
{
  "architecture": "386", "os": "windows", "base": "base/windows-x86:1", "title": "narbacular-drop",
  "run": { "exe": "C:\\Program Files\\Narbacular Drop\\Narbacular Drop.exe", "display": "640x480", "args": [] },
  "guest": { "bits": 32, "image": ["0x400000", "0x57c000"] },
  "cpu": { "features": ["fpu", "tsc", "cx8"] },
  "dll": { "fmod.dll": "builtin" },
  "libraries": ["d3d9", "dinput8", "fmod3"],
  "registry": [{ "key": "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Direct3D", "name": "DisableMMX", "type": "dword", "value": 1 }, ...],
  "imports": ["advapi32.dll!RegCloseKey", ...],
  "source": { "sha256": "0966948f...", "extract": "innosetup" },
  "build": { "lift": "<lift key>", "lifter": "<hash of the lifter's sources>", "clang": "...", "opt": "-O1", "runtime_opt": "-O2" }
}
```

The CPU features are built into the module (`rt_cpu_features`, by `tools/recipe-gen.py`); the registry is not:
the host hands the config's values to the runtime at start (`SW_REGISTRY`, one value a line:
`<key>\t<name>\tdword|sz\t<value>`), and `rt/windows-x86/advapi32.c` reads them. So a registry change leaves the module
as it was. `imports` is the title's import table, for `pisco census`.

### How hosts read an image

[`web/image.mjs`](../web/image.mjs) parses the documents and the tars and stacks the file layers into a drive.
`web/run-node.mjs <image>` reads the blobs from the folder (checking every digest); in the browser the page
(`web/browser/main.js`, `?image=<url>`) fetches and checks the documents and the module, and the workers read the
drive from `tools/serve.mjs`, which serves an image's drive below `<image>/c/`. The runtime's file system
(`rt/vfs.c`) sees the drive under its writable overlay: the title's folder as before (`C:\title`, relative paths),
and the rest of `C:\` (`C:\WINDOWS\win.ini`). What the title writes stays where M5 put it, outside the image:
`SW_SAVES` in Node, OPFS in the browser (keyed by the image's title, so saves survive a rebuild).

The old invocation keeps working: `node web/run-node.mjs title.wasm <the title's .exe>`.

## Recipes

`recipes/<title>/title.toml`:

```toml
name = "narbacular-drop"
from = "base/windows-x86:1"                 # bases/windows-x86/1/base.toml
libraries = ["d3d9", "dinput8", "fmod3"]  # the base's libraries this title links; checked against its imports

[source]
file = "narbacular_drop_setup.exe"        # looked for beside the recipe unless `pisco build --source` names it
sha256 = "0966948f..."                    # checked before anything else; without it the build says so and goes on
extract = "innosetup"                     # innoextract (tools/setup-toolchain.sh innoextract); or "file": the source is the exe;
                                          # or "folder": the source is a folder (a title the user built), copied to {app}
# app = 'C:\Program Files\Narbacular Drop'  where {app} goes (default: the exe's folder)
# map = { commonappdata = 'C:\Documents and Settings\All Users\Application Data' }  the installer's other folders

[[source.cab]]                            # files the installer carries in a cabinet (cabextract on the builder's
cab = 'C:\Program Files\Tag\DirectX\AUG2007_d3dx9_35_x86.cab'   # machine: tools/setup-toolchain.sh cabextract,
files = ["d3dx9_35.dll"]                  # or CABEXTRACT=<path>), taken out into a folder of C: (recipes/tag)
to = 'C:\WINDOWS\system32'

[run]
exe = 'C:\Program Files\Narbacular Drop\Narbacular Drop.exe'   # a Linux title: a path from the root, "/usr/bin/fastfetch"
display = "640x480"
# args = ["-windowed"]

[cpu]
features = ["fpu", "tsc", "cx8"]          # the virtual CPU (cpuid, IsProcessorFeaturePresent); default: the base's

[dll]
"fmod.dll" = "builtin"                    # a shipped DLL that is not lifted: the runtime (a library) answers for it

[registry]                                # REG_DWORD from numbers, REG_SZ from strings; added to the base's, and wins
'HKLM\Software\Microsoft\Direct3D' = { DisableMMX = 1, DisablePSGP = 1, DisableD3DXPSGP = 1 }

[lifter]
split = 16                                # the lifted C in that many files (compiled in parallel); default 8
entries = []                              # extra function entries (hex), as runs report them
# base = "400000"                         # where the exe goes, if not where it was linked

[build]
opt = "-O1"                               # clang's level for the lifted C (default -O2; the runtime has the base's)

[quirks]
heap_fill = 0xCD                          # what a heap block the title did not ask to be zeroed holds (default: whatever
                                          # the runtime's allocator left); Narbacular Drop depends on it being non-zero
```

`tools/build-title.sh` reads the same recipe (`RECIPE=...`: `[cpu]`, `[registry]` built into the module, `[dll]`,
`[quirks]`, and the base and libraries for the runtime's files).

## Bases and libraries

`bases/<name>/<version>/base.toml` defines what a recipe's `from` names: one base per runtime, and the only list of
the runtime's files (`pisco sources <base | recipe>` prints a title's; `tools/build-title.sh` asks it; `tests/run.sh
layout` checks that no other list exists and that every file is in a base):

| Base | Runtime | Sources | Titles (recipes) |
|---|---|---|---|
| `base/windows-x86:1` | Windows x86 | `rt/*.c`, `rt/windows-x86/*.c` | hello32, Narbacular Drop |
| `base/windows-x64:1` | Windows x64 | `rt/*.c`, `rt/gdk/*.c`, `rt/windows-x64/*.c` | hello64, SimpleTriangleDesktop, SimplePBR (folders the user built) |
| `base/xbox-series:1` | Xbox Series | `rt/*.c`, `rt/gdk/*.c`, `rt/xbox-series/*.c` | Doom (`out/doom/game`, built by tests/gfx/doom.sh) |

`base/windows-x86:1` has:

- `[cpu] features`: the default virtual CPU (every feature the lifter supports);
- `[runtime]`: the core runtime sources (`rt/*.c`, `rt/windows-x86/*.c` less the libraries'),
  and their optimisation level;
- `[registry]`: the defaults every title sees (Windows XP SP3's version values, `ProgramFilesDir`);
- `[files]`: the layer: `files/` (C:\ at its top), empty folders, the fonts, the `system32` placeholders;
- `[libraries.<name>]`: `d3d9` (`rt/windows-x86/d3d9*.c`), `dinput8`, `fmod3` (`fmod3.c`, `audio_decode.c`). The DLLs a
  library answers for are the `IMP(dll, ...)` in its sources. `pisco build` refuses a title that imports from a
  library its recipe does not name, and says which ones a title needs (`pisco census <exe>`).

## The build and its cache

`pisco build` does, in order (the log on stderr says what each step found in the cache, and how long it took; the
manifest digest is the last line on stdout):

1. **the source**: its sha256 against the recipe's;
2. **extraction**: `innoextract` into `<cache>/extract/<sha256>-innosetup`, once per installer. `--compare <folder>`
   sets the extracted `{app}` against another installation (a Wine prefix's) and prints the differences. Each
   `[[source.cab]]` is taken out of its cabinet with `cabextract`, once per cabinet and list of files, into
   `<cache>/extract/<sha256>-innosetup-cab-<key>`; a missing `cabextract` stops the build and says how to get it;
3. **lifting**: the lifter and clang on the lifted C, once per **lift key**: the sha256 of the images the lifter
   reads (the exe and every shipped DLL that is not `builtin`), the lifter (its version and a hash of its sources
   and `Cargo.lock`; `pisco` builds it first), `rt/cpu.h` and `rt/modules.h`, the lifter's options, clang's version and
   the recipe's `opt`. `<cache>/lift/<key>/` holds the C and the objects;
4. **the runtime**: each source compiled once per key: its own bytes, every header, table, included source and
   font under `rt/`, the flags and clang's version (`<cache>/rt/<key>.o`); the import table
   (`tools/gen-imports.sh`, which `tools/build-title.sh` uses too) and the CPU features are compiled once per content;
5. **the link**, once per set of objects (`<cache>/link/<key>.wasm`);
6. **the image**: the layers, the module, the config, the manifest and the index, into `out/images/<name>` (`--out`).

The cache (`out/pisco-cache`, `--cache`) only saves time: an image never depends on what was in it. A change of the
recipe's registry, display or arguments touches only the config: nothing is lifted, compiled or linked again, and
the module blob is the same (tests/images.sh measures it). Objects are built with `-ffile-prefix-map` for the
repository and the cache, so no path of the build machine reaches them (stb_truetype's `assert` would carry its
own path otherwise), and two checkouts in different places give the same bytes. The module is linked as
`title.wasm` in a folder of its own: wasm-ld writes the output file's name into the module's name section, and the
first version of `pisco`, linking to a temporary name, made the second clean build differ by those two bytes.

## Tests

`tests/run.sh images` (tests/images.sh): hello32, hello64, Doom and Narbacular Drop (one title of each base) built twice
each with empty caches (the same manifest digests), their shared base layer, a registry-only rebuild, then `pisco run` of the menu-to-level session in
Node, with the checkpoints `tests/run.sh nd` checks, and the image's registry read by advapi32.
