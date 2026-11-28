# Images: the guide to `pisco`

An app is built from a **recipe** into an **image**: a folder in the OCI image layout holding the app's files, the
runtime's files, the compiled module and a config. What a recipe, a base and an image mean, for any builder or host,
is the specification ([spec/](spec/README.md)); this page is how `pisco` does it. `pisco` (`crates/pisco`) builds, runs and inspects
images; the hosts read them through one module, [`web/image.mjs`](../web/image.mjs).

```sh
cargo build --release -p pisco -p pisco-lifter
target/release/pisco build recipes/narbacular-drop --source ~/Downloads/narbacular_drop_setup.exe   # -> out/images/narbacular-drop
target/release/pisco run out/images/narbacular-drop --session recipes/narbacular-drop/sessions/menu-to-level.json   # Node
target/release/pisco run out/images/narbacular-drop --chrome                                                    # headless Chrome
target/release/pisco inspect out/images/narbacular-drop [--files]
target/release/pisco census out/images/narbacular-drop      # or a title's .exe: what the base and its libraries answer for
target/release/pisco install hello32 && target/release/pisco run hello32   # by name, from the store (below)
```

The model is OCI's, with three differences:

1. **No `RUN`.** We cannot run an installer inside our own runtime. A recipe's steps run on the builder's machine,
   with a fixed vocabulary: extract the installer, place its files, set registry values.
2. **Code is layered at build time.** A title is one statically linked wasm module. A base's "libraries" are sets of
   runtime sources (with cached objects); the image carries only the linked module.
3. **App layers are never published.** The base and library layers and the recipe can be shared; the app layer
   and the module are built locally from the user's own installer, checked by its sha256. They are marked
   `org.pisco.local-only` in the manifest. Nothing of them is ever committed (`out/` is ignored).

## The format

The image's layout, media types, layer roles (`base`, `library`, `app`, `shaders`, `module`, `program`), its file
semantics, the tree digest and every field of the config are the specification's: [spec/image.md](spec/image.md),
with the config's JSON Schema in [spec/config.schema.json](spec/config.schema.json) (`tools/check-config.mjs <image>`
checks one). What is `pisco`'s own:

- **One module per program.** The lifted code, the runtime and the import tables are linked into one module
  (`title.wasm`); a Linux app's other programs are a module each (role `program`, named in the config's `programs`),
  a machine's are in the kernel's module or in side modules (`side`).
- **The CPU features are built into the module** (`rt_cpu_features`, by `tools/recipe-gen.py`); **the registry is
  not**: the host hands the config's values to the runtime at start (`SW_REGISTRY`, one value a line:
  `<key>\t<name>\tdword|sz\t<value>`), and `rt/windows-x86/advapi32.c` reads them. So a registry change leaves
  the module as it was.
- **`build`** in the config: the lift key, a hash of the lifter's sources, clang's version and levels, and the
  versions of the extractors a recipe's steps ran (`tools`).
- **The lock**: `pisco build --lock` writes `recipe.lock` (the tree digest of the app's files) for a recipe whose
  inputs are all pinned; a build that finds one stops when its files differ.

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

A recipe is `recipes/<app>/recipe.toml`, schema 1: [spec/recipe.md](spec/recipe.md) has every key. In short:

```toml
schema = 1
name = "narbacular-drop"
from = "base/windows-x86:1"                 # bases/windows-x86/1/base.toml
libraries = ["d3d9", "dinput8", "fmod3"]    # the base's libraries the app links; checked against its imports

[[input]]                                   # what the user supplies, known by its sha256
id = "installer"
file = "narbacular_drop_setup.exe"          # looked for beside the recipe unless `pisco build --input installer=<path>`
sha256 = "0966948f..."                      # (or built_by = "<script>": made here; or unpinned = "why")

[[extract]]                                 # in order: innosetup, zip, wise, file or folder first; then
kind = "innosetup"                          # installshield (after wise), cab and copy
input = "installer"

[run]
exe = 'C:\Program Files\Narbacular Drop\Narbacular Drop.exe'
display = "640x480"

[cpu]
features = ["fpu", "tsc", "cx8"]            # the virtual CPU; default: the base's
[dll]
"fmod.dll" = "builtin"                      # the base answers for this shipped DLL
[quirks]
heap_fill = 0xCD
[registry]
'HKLM\Software\Microsoft\Direct3D' = { DisableMMX = 1 }
[hints.pisco]                               # pisco's own: the lifted C in 16 files, clang's level for it
split = 16
opt = "-O1"
```

`pisco build <recipe> --input <id>=<path>` names an input (`--source <path>`: the one the first step reads; `--with
<ID>=<folder>` as before). `pisco recipe <recipe> --json` prints the recipe as `pisco` reads it: `tools/build-title.sh`
(`RECIPE=...`) and `tools/recipe-gen.py` take that, so `pisco` is the one reader of recipes.

What the steps do here:

- `innosetup`: `innoextract` (tools/setup-toolchain.sh innoextract, or INNOEXTRACT); every folder of the installer's
  other than `{app}` must have a place in `map`.
- `zip`: read by `pisco` (stored or deflated); `wise` and `installshield`: read by `pisco` too
  (`crates/pisco/src/installers.rs`), without running either installer.
- `cab`: `cabextract` on the builder's machine (tools/setup-toolchain.sh cabextract, or CABEXTRACT).
- `copy`: from a folder input (`--input <id>=<folder>`, or the environment variable `<ID>`), into the app's folder
  or below it, where the lifter finds the DLLs beside the exe.
- `[warmup]`: the image is run in Node until `frames` presents (SW_EXIT_FRAMES), D3DCompile logging each source
  (SW_HLSL_CAPTURE=1); `tools/hlsl-prep.mjs` compiles them (dxc: DXC, or ~/tools/dxc/dxc under Wine) and translates
  them (dxil2spv, naga): the image's `shaders` layer, in the app's folder.

## Bases and libraries

`bases/<name>/<version>/base.toml` defines what a recipe's `from` names: one base per runtime, and the only list of
the runtime's files (`pisco sources <base | recipe>` prints a title's; `tools/build-title.sh` asks it; `tests/run.sh
layout` checks that no other list exists and that every file is in a base):

| Base | Runtime | Sources | Titles (recipes) |
|---|---|---|---|
| `base/windows-x86:1` | Windows x86 | `rt/*.c`, `rt/windows-x86/*.c` | hello32, Narbacular Drop |
| `base/windows-x64:1` | Windows x64 | `rt/*.c`, `rt/gdk/*.c`, `rt/windows-x64/*.c` | hello64, SimpleTriangleDesktop, SimplePBR (folders the user built), Desire (a zip) |
| `base/xbox-series:1` | Xbox Series | `rt/*.c`, `rt/gdk/*.c`, `rt/xbox-series/*.c` | Doom (`out/doom/game`, built by tests/gfx/doom.sh) |

What a base promises its apps (`platform`, `stability`, `provides`, `[cpu]`, `[registry]`, `[files]`, each
library's `provides`) is the specification's: [spec/base.md](spec/base.md). `[impl]` is `pisco`'s: the runtime's
sources (`rt/*.c`, `rt/windows-x86/*.c` less the libraries'), their optimisation level, and each library's sources
(`[impl.libraries.d3d9] sources = ["rt/windows-x86/d3d9*.c"]`). `pisco base <base>` prints what a base promises;
`pisco base <base> --check` (in `tests/run.sh layout`) checks each `provides` against the `IMP(dll, ...)` of its
sources. `pisco build` refuses an app that imports from a library its recipe does not name, and says which ones it
needs (`pisco census <exe>`); it says so when a base is experimental.

## The store

`pisco install <recipe>` builds an app into the store, and `pisco run <name>` runs it from any folder; `pisco list`
says what is installed and `pisco remove <name>` takes it away. `<recipe>` is a name
([spec/distribution.md](spec/distribution.md#names)): `tag` for one of `recipes/` (the official source),
`owner/repo/tag` for a GitHub repository's, `@ns/tag` after `pisco source add @ns <git url>`, `<git url>#tag`, or a
recipe's folder. `pisco search [words]` looks in the official source and every `@ns`. The store is `$PISCO_HOME`
(`~/.pisco`):

```
blobs/sha256/<digest>   every app's blobs, once: apps FROM one base share its layer, apps that name a library its layer
apps/<name>/            an image layout whose blobs/ is a link to ../../blobs: a host reads it as any image folder
saves/<name>/           what the app keeps between runs (SW_SAVES; `--saves` says another place), kept across installs
tmp/                    an install builds here, then moves the app into apps/ by one rename
sources/                one checkout of each git source, at the commit last installed from
sources.toml            the namespaces: @ns = "<git url>"
```

`index.json`'s entry says where the app came from (`org.pisco.source`). `pisco inspect` and `pisco census` take an
installed app's name too. Removing an app removes the blobs no other app names. `out/images` stays where the tests
build.

## The build and its cache

`pisco build` does, in order (the log on stderr says what each step found in the cache, and how long it took; the
manifest digest is the last line on stdout):

1. **the source**: its sha256 against the recipe's;
2. **extraction**: `innoextract` into `<cache>/extract/<sha256>-innosetup`, once per installer. `--compare <folder>`
   sets the extracted `{app}` against another installation (a Wine prefix's) and prints the differences. Each
   `cab` step is taken out of its cabinet with `cabextract`, once per cabinet and list of files, into
   `<cache>/extract/<sha256>-innosetup-cab-<key>`; a missing `cabextract` stops the build and says how to get it.
   A zip is read by `pisco` itself (`<cache>/extract/<sha256>-zip`). `copy` step's files are copied into the
   extraction, whose folder then ends in `-add-<key>`, the key being their names and sha256s;
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
