# Recipes, schema 1

A recipe is a folder, `recipes/<name>/`, holding `recipe.toml` and what the recipe refers to (sessions, traces,
small build inputs such as a kernel's configuration). It never holds the app's own files: those are **inputs** the
user supplies, known by their sha256. A recipe is TOML 1.0.

```toml
schema = 1
name = "tag"
from = "base/windows-x86:1"
libraries = ["d3d9", "dinput8", "fmodex"]

[about]
display_name = "Tag: The Power of Paint"
version = "2008"
revision = 1

[[input]]
id = "installer"
file = "Tag_setup.exe"
sha256 = "bb69d916e788c6a658e4b75e945efd6960ce4ddef1f5cc0ce6db86efccbe8113"
where = "DigiPen's download of the game (Tag_setup.exe)"

[[extract]]
kind = "innosetup"
input = "installer"
app = 'C:\Program Files\Tag'

[[extract]]
kind = "cab"
from = 'C:\Program Files\Tag\DirectX\AUG2007_d3dx9_35_x86.cab'
files = ["d3dx9_35.dll"]
to = 'C:\WINDOWS\system32'

[run]
exe = 'C:\Program Files\Tag\Tag.exe'
display = "1024x768"

[cpu]
features = ["fpu", "tsc", "cx8"]

[dll]
"fmodex.dll" = "builtin"

[registry]
'HKLM\Software\Microsoft\Direct3D' = { DisableMMX = 1 }
```

## Reading a recipe

- A reader must refuse a recipe whose `schema` it does not know, or that has no `schema`.
- A reader must refuse a key it does not know, in every table, and a value of the wrong type. It should say which
  keys it knows.
- A reader must refuse a name that is not in the lists below (an extract kind, a CPU feature, a DLL choice, a quirk).
- Paths on the app's drive are written as the platform writes them: `C:\...` on the Windows platforms and the Xbox
  Series, `/...` on Linux and the machine. Names on a Windows drive are matched without regard to case.
- The one table that is not the spec's is `[hints.<builder>]`: a reader must ignore the tables of other builders,
  and may ignore its own.

## The keys

Every key a recipe may have, by table. A table not listed here may not appear. `tests/run.sh spec` checks that this
table and what `pisco` accepts are the same list (the rows between the two markers).

<!-- keys -->
| Key | Type | Required | Means |
|---|---|---|---|
| `schema` | integer | yes | 1 |
| `name` | string | yes | the app's name: lower-case letters, digits and `-`. The image's name and the key of its saves |
| `from` | string | yes | the base: `base/<name>:<version>` ([base.md](base.md)) |
| `libraries` | list of strings | no | the base's libraries the app asks for; each must be one of the base's |
| `about.display_name` | string | no | the app's name for people |
| `about.version` | string | no | the app's own version, as its makers write it |
| `about.revision` | integer | no | this recipe's revision, from 1, raised when what it builds changes |
| `about.notes` | string | no | anything a person should know |
| `input.id` | string | yes | how steps and the command line name it: lower-case letters, digits and `-` |
| `input.kind` | string | no | `file` (the default) or `folder` |
| `input.file` | string | yes | where a builder looks for it unless told: a name or path, from the recipe's folder |
| `input.files` | list of strings | no | a folder input: only these files of it (found without regard to case); without it, all of it |
| `input.sha256` | string, or list of strings | one of three | the input's sha256 (a folder's: its tree digest), or several accepted ones |
| `input.built_by` | string | one of three | the input is made on the builder's machine by this script of the repository (its bytes depend on the compiler) |
| `input.unpinned` | string | one of three | the input is not pinned, and why (any version of a redistributable) |
| `input.where` | string | no | for a person: where the input comes from |
| `extract.kind` | string | yes | what the step does: one of the [extract kinds](#extract-kinds) |
| `extract.input` | string | some kinds | the input it reads, by `id` |
| `extract.app` | string | no | where the installer's `{app}` goes (default: the folder of `run.exe`) |
| `extract.map` | table of strings | no | `innosetup`: where the installer's other folders go (`commonappdata = 'C:\...'`) |
| `extract.root` | string | no | `zip`: only the files under this folder of the archive, without it |
| `extract.groups` | table of strings | `installshield` | where each file group goes, below `{app}` (`''` the folder itself, `'-'` not installed) |
| `extract.from` | string | `cab` | the cabinet, a file an earlier step placed |
| `extract.files` | list of strings | `cab` | the files taken out of it |
| `extract.to` | string | `cab`, `copy` | the folder the files go to |
| `run.exe` | string | windows, linux | the program the app starts with |
| `run.display` | string | no | windows: the screen's size, `<width>x<height>` |
| `run.args` | list of strings | no | its arguments (a host's own arguments replace them, as a container's command does) |
| `run.kernel` | string | microvm | the kernel the machine boots ([platforms/microvm-x64.md](platforms/microvm-x64.md)) |
| `run.initrd` | string | no | microvm: the kernel's initramfs |
| `run.cmdline` | string | no | microvm: the kernel's command line |
| `cpu.features` | list of strings | no | the virtual CPU's features: [the list](#cpu-features). Default: the base's |
| `dll` | table of strings | no | a shipped DLL (lower-case name) and the [choice](#dll-choices) for it |
| `registry` | table of tables | no | `'HKLM\Key' = { Name = value }`: a number is a REG_DWORD (0 to 4294967295), a string a REG_SZ. Added to the base's; the recipe's wins |
| `files` | table of strings | no | small text files a setup step would write: `'<path>' = "<text>"`. A file of the same path is replaced |
| `quirks` | table | no | behaviour one app depends on: [the list](#quirks) |
| `code.entries` | list | no | addresses (hex strings or integers) where code starts that the binary does not show |
| `code.base` | string | no | where the exe is loaded, in hex, if not where it was linked |
| `code.load` | list of strings | no | DLLs the app loads itself (LoadLibrary), from its folder or below it |
| `warmup.frames` | integer | `warmup` | the app is run until it has shown this many frames; what it compiles (shaders) may be kept |
| `warmup.session` | string | no | the session it is run with (a file of the recipe) |
| `hints` | table | no | `[hints.<builder>]`: one builder's own options; others ignore them |
<!-- /keys -->

`pisco`'s hints, `[hints.pisco]`:

<!-- hints.pisco -->
| Key | Type | Means |
|---|---|---|
| `split` | integer | the lifted C in this many files, compiled in parallel (default 8) |
| `opt` | string | clang's level for the lifted C (default `-O2`) |
| `programs` | list of strings | microvm: programs lifted ahead of time and linked with the kernel |
| `modules` | list of strings | microvm: programs lifted ahead of time, each into a module of its own (kept apart for its licence) |
| `lifted` | string | microvm: a folder of programs already lifted into modules of their own (`<sha256>-<tools>.wasm`, a run's lift cache): a layer each |
<!-- /hints.pisco -->

## Inputs

Each `[[input]]` is a file or a folder the user supplies. It has exactly one of:

- `sha256`: the input is pinned. A builder must check it before it reads anything else of the input, and stop if
  it is none of the values given. A folder's sha256 is its [tree digest](image.md#the-tree-digest), over `files` when
  they are named.
- `built_by`: the input is made from sources by a script of the repository on the builder's machine (a local
  recipe: fastfetch, the hellos, Linux). Its bytes depend on the compiler, so the recipe has no lock.
- `unpinned`: the input is any of several versions, and the recipe says why. Its recipe has no portable lock either.

How the builder finds an input is not the recipe's business. `pisco` looks for `file` beside the recipe unless told
(`pisco build <recipe> --input <id>=<path>`; `--source <path>` names the input the first step reads; `--with
<ID>=<folder>` and the environment variable `<ID>`, upper-case, name a folder input).

## Extract steps

The `[[extract]]` steps run in order and place the app's files on its drive. Each reads an input or the files earlier
steps placed. What the steps place, with `[files]` after them, is the app's file tree; its digest is the lock's.

### Extract kinds

<!-- extract-kinds -->
| Kind | Reads | Places |
|---|---|---|
| `innosetup` | an Inno Setup installer | its files: `{app}` at `app`, its other folders where `map` says. Every folder it has must be placed |
| `zip` | a zip archive (stored or deflated) | its files at `app` (with `root`: those under that folder). A name that leaves the folder is refused |
| `wise` | a Wise installer, or a zip holding one `.exe` that is one | its files under `%MAINDIR%`, at `app` |
| `installshield` | the InstallShield 5 or 6 setup a `wise` step just unpacked (`data1.hdr`, `data<N>.cab`) | instead of those files, its file groups, each where `groups` says. Every group must be placed |
| `file` | a file | the file, as `run.exe` (or `run.kernel`) |
| `folder` | a folder | its files, as they are, at `app` (`app = "/"`: the drive's root) |
| `cab` | a cabinet placed by an earlier step | `files` of it, in `to` |
| `copy` | a folder input | its `files`, in `to` |
<!-- /extract-kinds -->

The first step is the one that unpacks the app (`innosetup`, `zip`, `wise`, `file` or `folder`), and there is one
such step. `pisco` also needs `installshield`, when there is one, to come straight after `wise`, and a `copy` to
place its files in the app's folder or below it (the lifter takes the DLLs it finds beside the exe).

## CPU features

The features the virtual CPU reports (`cpuid`, `IsProcessorFeaturePresent`, `/proc/cpuinfo`). An old app may need
fewer than the lifter supports, so that it does not take code paths that need the others.

<!-- cpu-features -->
`fpu` `tsc` `cx8` `cmov` `mmx` `fxsr` `sse` `sse2` `sse3` `ssse3` `sse4.1` `sse4.2` `popcnt` `cx16` `pclmul`
<!-- /cpu-features -->

## DLL choices

As Wine's users write them, for a DLL the app ships beside it:

<!-- dll-choices -->
| Choice | Means |
|---|---|
| `native` | the app's own copy is used (lifted with it); what a shipped DLL gets when it is not named |
| `builtin` | the base answers for it (a library must), and the app's copy is not used |
<!-- /dll-choices -->

`disabled` (the DLL is not found) is Wine's third choice; it is kept for a later schema.

## Quirks

Behaviour of the system that one app depends on and others must not get.

<!-- quirks -->
| Quirk | Value | Means |
|---|---|---|
| `heap_fill` | a byte, 0 to 255 | what a heap block the app did not ask to be zeroed holds, below 0x7F000 bytes into it (Windows' heap leaves its fill pattern there; above, it gives fresh pages, which are zero). Default: whatever the base's allocator leaves |
<!-- /quirks -->

## `[files]`

A file of `[files]` is written as text: on the Windows platforms and the Xbox Series its lines end with CR LF, on the
others with LF. It is placed after every extract step and replaces a file of the same path.

## The lock

`recipe.lock`, beside `recipe.toml`, is what a build of the recipe gave: the tree digest of the app's files
([image.md](image.md#the-tree-digest)). A builder that finds a lock must make the same tree digest, or stop and say
so. Only a recipe whose inputs are all pinned has a lock in the repository.

```toml
schema = 1
tree = "sha256:..."
files = 572
```

## Warm-up

`[warmup]` lets a builder run the app once, for `frames` frames (with `session`), and keep what it compiled then:
`pisco` compiles the HLSL a Direct3D app compiles at run time (D3DCompile) into WGSL. The result is a layer of its
own (`shaders`), never part of the tree digest. A builder may skip the warm-up; the app must then work without it
(more slowly the first time, or not at all on a host that cannot compile shaders).
