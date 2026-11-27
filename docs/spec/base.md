# Bases

A base is a runtime that apps are built `FROM`: `base/<name>:<version>`. It imitates one system (its platform) and
promises its apps what that system gives them. A recipe names exactly one base; its image carries the base's layer
and records the base in its config.

`bases/<name>/<version>/base.toml` says what the base promises (the contract) and, in `[impl]`, how this
repository makes it (`pisco`'s). Another implementation of a base must keep the contract; its `[impl]` is its own.

```toml
name = "base/windows-x86"
version = 1
platform = "windows-x86"
stability = "stable"
architecture = "386"
os = "windows"
provides = ["advapi32.dll", "gdi32.dll", "kernel32.dll", "..."]

[cpu]
features = ["fpu", "tsc", "..."]

[registry]
'HKLM\Software\Microsoft\Windows NT\CurrentVersion' = { ProductName = "Microsoft Windows XP", CurrentVersion = "5.1" }

[files]
dirs = ['WINDOWS\Temp', 'WINDOWS\system32', 'Documents and Settings\player']

[libraries.d3d9]
description = "Direct3D 9 on WebGPU: the fixed-function pipeline and shader models 1.1 to 2.x"
provides = ["d3d9.dll"]

[impl]
sources = ["rt/*.c", "rt/windows-x86/*.c"]
opt = "-O2"
[impl.libraries.d3d9]
sources = ["rt/windows-x86/d3d9*.c"]
```

## The keys

<!-- base-keys -->
| Key | Means |
|---|---|
| `name` | `base/<name>` |
| `version` | an integer: what `:<version>` names ([below](#versions)) |
| `platform` | the system it imitates ([platforms/](platforms/)) |
| `stability` | `stable` or `experimental` ([README.md](README.md#stability)) |
| `architecture` | OCI's name of its CPU: `386`, `amd64` |
| `os` | `windows`, `linux`, `microvm`, `ps5` |
| `provides` | the system libraries (DLLs; the PS5's modules) it answers for without a library, as an import table names them; an app may import from them |
| `cpu.features` | the virtual CPU an app gets when its recipe does not say ([recipe.md](recipe.md#cpu-features)) |
| `registry` | the registry every app of the base sees; a recipe's values are added and win |
| `files.dir` | a folder of the base whose files are its drive's (its layer) |
| `files.dirs` | empty folders of its drive |
| `files.fonts` | fonts on its drive: their place, and the file that is put there |
| `files.system32` | the folder where a placeholder for each DLL it answers for goes |
| `libraries.<name>.description` | what the library is, for a person |
| `libraries.<name>.provides` | the DLLs the library answers for: an app that imports one must name the library |
| `impl` | the implementation's own: which sources make the runtime, and how they are compiled |
<!-- /base-keys -->

A builder must refuse a recipe that names a library its base does not have, and one whose app imports from a DLL of a
library the recipe does not name. `tests/run.sh layout` checks that each `provides` is what the sources answer for.

## The base's layer

What `[files]` names, and in `files.system32` a placeholder file for every DLL of `provides` (and one layer of the
same kind for each library the recipe names): an app that looks for a system DLL finds it, as on Wine. A placeholder
has no code; what it says is the builder's.

## Versions

`base/<name>:<version>` names one contract. It must get a new version when something an app can observe changes or
goes away: a DLL or a function no longer answered, a registry value or a file of the drive that changes, a default
CPU feature taken away, a behaviour an app depends on (a `[quirks]` becoming the default). Adding a DLL, a function, a
library, or answering a call that failed before, does not: an app built for the base still sees what it saw.

When a base gets a new version, the old one stays (its folder in `bases/`) until no recipe names it.

## The bases

| Base | Platform | Stability | Imitates |
|---|---|---|---|
| `base/windows-x86:1` | `windows-x86` | stable | 32-bit Windows XP SP3 desktop: Win32, GDI, Direct3D 9, OpenGL 1.1, DirectInput, DirectSound, FMOD |
| `base/windows-x64:1` | `windows-x64` | stable | 64-bit Windows 10 desktop: DXGI, Direct3D 12, the PC GDK, the MSVC runtimes |
| `base/xbox-series:1` | `xbox-series` | stable | the GDK's console build: kernelx, XMem, D3D12.X, DirectStorage, XAudio2 |
| `base/linux-x64:1` | `linux-x64` | stable | Linux at its system calls, for statically linked x86-64 programs |
| `base/microvm-x64:1` | `microvm-x64` | experimental | a small x86-64 machine, for a kernel and its programs |
| `base/ps5:1` | `ps5` | experimental | the PS5's system libraries, for homebrew |
