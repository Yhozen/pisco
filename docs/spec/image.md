# Images

An image is what a builder makes from a recipe and its inputs, and what a host runs: a folder in the
[OCI image layout](https://github.com/opencontainers/image-spec/blob/main/image-layout.md), with one manifest whose
config and layers have PISCO's own media types.

```
oci-layout                {"imageLayoutVersion":"1.0.0"}
index.json                an image index with one manifest; its annotation org.opencontainers.image.ref.name is the app's name
blobs/sha256/<digest>     the manifest, the config and the layers, each named by its sha256
```

A host must check every blob it reads against its digest.

## Media types

| Media type | What it is |
|---|---|
| `application/vnd.oci.image.manifest.v1+json` | the manifest (OCI's), with `artifactType` below |
| `application/vnd.pisco.app.v1` | the manifest's `artifactType`: a PISCO app |
| `application/vnd.pisco.app.config.v1+json` | the config ([below](#the-config)) |
| `application/vnd.pisco.layer.v1.tar` | a file layer: an uncompressed tar of files on the app's drive |
| `application/vnd.pisco.module.v1.wasm` | a module: compiled code a host runs (WebAssembly) |

A host must refuse an `artifactType` it does not know.

## The manifest

`schemaVersion` 2, `mediaType` OCI's, `artifactType`, `config`, `layers` and the annotations
`org.opencontainers.image.title` (the app's `name`) and `org.pisco.base` (the base, `base/<name>:<version>`).

Each layer has the annotation `org.pisco.layer.role`, and `org.opencontainers.image.title`:

<!-- roles -->
| Role | Media type | Holds | Shared |
|---|---|---|---|
| `base` | layer | the base's drive ([base.md](base.md)): its folders, files and the placeholders of the DLLs it answers for | by every app FROM the base |
| `library` | layer | one per library the recipe names: a placeholder for each DLL it answers for | by every app that names it |
| `app` | layer | the app's files: what the recipe's extract steps and `[files]` placed | never (below) |
| `shaders` | layer | what the warm-up compiled ([recipe.md](recipe.md#warm-up)), in the app's folder | never (below) |
| `module` | module | the module a host starts the app in | never (below) |
| `program` | module | a module of one more program (a Linux app's, a machine's), named in the config | never (below) |
<!-- /roles -->

File layers come first, in the order of the table; `module` is the first module. A layer that is never shared has
the annotation `org.pisco.local-only` = `"true"`: it is made from the user's own files, and must not be published
([distribution.md](distribution.md#what-may-be-published)). The app's own layers (`app`, `shaders`, `module`,
`program`) are local-only unless every input of the recipe is `built_by`: then the app is made from the
repository's own sources, and may be pushed to a registry as a whole.

## Files

The file layers stacked, lowest first, are the app's drive. A later layer's file replaces an earlier one's, and an
entry `.wh.<name>` removes `<name>` from the layers below (OCI's whiteouts).

A layer is a tar (ustar, with a pax `path` record for a name ustar cannot hold) of entries whose paths are relative to
the drive's root (`C:\` on the Windows platforms, `/` on the others), `/`-separated, without `.` or `..`. A builder
must write them so that the same files give the same bytes: entries sorted by path, a folder before what it holds,
mtime, uid and gid 0, no other records. Layers are not compressed, so a host may read a file at its offset in the
blob.

How the drive behaves is the platform's:

| | Windows platforms, Xbox Series | Linux, the machine, PS5 |
|---|---|---|
| names | matched without regard to case (ASCII); a name keeps its case | matched exactly |
| modes | none: folders are 0755, files 0644 in the tar | a file is executable (0755) or not (0644) |
| links | none | symbolic links (tar type `2`), their target as written |

Two names on a Windows drive that differ only by case are one name: a builder should warn of them, and which one a
host finds is not said.

## The tree digest

The digest of a file tree, the same whatever wrote it (a builder's layer, a folder on a disk):

1. one line for each entry: `<type>\t<data>\t<path>\n`, where `<path>` is as in a layer (no leading `/`);
   - a folder: type `d`, data `-`;
   - a file: type `f` (or `x` when it is executable, on a platform with modes), data its sha256 in hex;
   - a symbolic link: type `l`, data its target;
2. the lines sorted by `<path>`, byte by byte;
3. the sha256 of them all, one after the other: written `sha256:<hex>`.

A path with a tab or a line break has no tree digest; a builder must refuse it. The tree digest of the `app` layer
is the config's `tree.digest`, and is what a recipe's lock holds: two conforming builders must make the same one.

## The config

A JSON document, written with sorted keys and no white space, that depends only on what the image was built from.
[config.schema.json](config.schema.json) is its JSON Schema; every image a test builds is checked against it.

<!-- config -->
| Field | Means |
|---|---|
| `schema` | 1 |
| `name` | the app's name, the recipe's `name` |
| `about` | the recipe's `[about]`: `display_name`, `version`, `revision` (each may be null) |
| `platform` | the base's platform: `windows-x86`, `windows-x64`, `xbox-series`, `linux-x64`, `microvm-x64`, `ps5` |
| `base` | `base/<name>:<version>` |
| `architecture` | OCI's name of the CPU: `386`, `amd64` |
| `os` | the system the base imitates: `windows`, `linux`, `microvm`, `ps5` |
| `abi` | the host interface the modules are built for: `pisco.host/0` ([host.md](host.md)) |
| `run` | `exe` (for the machine: the kernel), `args`, and the platform's own: `display`; `initrd`, `cmdline` |
| `libraries` | the libraries the recipe names |
| `dll` | the recipe's `builtin` DLLs |
| `registry` | the registry the app sees: the base's and the recipe's, as `{ key, name, type: dword \| sz, value }`, sorted |
| `cpu` | `features`: the virtual CPU's |
| `tree` | `digest`: the `app` layer's tree digest; `files`: how many files it has |
| `source` | what the app was made from: the first input's `sha256` and the first step's `extract` kind, the `added` files (path and sha256) |
| `guest` | `bits` (32 or 64) and `image` (the lowest and highest address of the app's code, hex): what a host needs to lay out memory |
| `imports` | the functions the app imports (`dll!name`), for a census |
| `permissions` | what the app may be given beyond its drive ([recipe.md](recipe.md#permissions)): `network`; null for none |
| `programs` | a Linux app's programs: each path and the digest of the module that holds it |
| `side` | the machine's programs in modules of their own: each name and its module's digest |
| `lifted` | the machine's programs lifted ahead of time: the sha256 of the image the machine takes of each, and its module's digest |
| `build` | how this builder made it (tool versions, its keys); free for each builder |
<!-- /config -->

A host must ignore `build`, and may ignore `imports`. Arguments given when an image is run replace `run.args`, as a
container's command replaces its image's.
