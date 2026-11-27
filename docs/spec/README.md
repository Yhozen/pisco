# The PISCO specification

This folder says what a recipe, a base and an image mean, so that someone else's builder or host could honour
them as well as `pisco` does. A Docker image runs on podman because both follow OCI's specification; the aim here is
the same thing for an app: a recipe written once builds the same app, and the app works, on any implementation that
follows these pages.

The idea is winepak's (a packaged program that just works), with what Lutris, Bottles and PlayOnLinux taught:

- **A recipe never runs code** of its own: its steps come from a fixed list (extract, place, set), as Flatpak's
  manifests do and Lutris's install scripts do not.
- **The user brings the files**, and the recipe knows them by their sha256 (Lutris's `files:`, Flatpak's
  `extra-data`). Where the files come from is not part of the recipe, so a store or a URL can be added later
  without a recipe changing.
- **A base is a versioned runtime** that many apps share, as Flatpak's runtimes are.
- **The choices of Wine's users**, which DLL is the system's and which is the app's own, keep Wine's words.

`docs/images.md` is the guide to `pisco` itself (its commands, its cache). These pages are the contract.

| Page | Says |
|---|---|
| [recipe.md](recipe.md) | the recipe (`recipe.toml`), schema 1: every key, and the lists of names it may use |
| [base.md](base.md) | what a base promises its apps, its libraries, and when its version changes |
| [image.md](image.md) | the image: its layout, media types, config, layers, and how its files behave |
| [sessions.md](sessions.md) | sessions (what a player does, scripted) and what "the app works" means |
| [host.md](host.md) | what a host gives an app's module: version 0, not stable (generated from the sources) |
| [platforms/](platforms/) | each platform's `[run]`, its drive, and what is particular to it: [windows](platforms/windows.md) (`windows-x86`, `windows-x64`, `xbox-series`), [linux-x64](platforms/linux-x64.md), [microvm-x64](platforms/microvm-x64.md), [ps5](platforms/ps5.md) |

## The words

| Word | Means |
|---|---|
| **app** | what a recipe describes and an image holds: a game, a command-line program, an operating system's kernel |
| **program** | one executable of an app (neofetch is one app with several programs: bash, busybox) |
| **recipe** | the folder `recipes/<name>/` and its `recipe.toml`: what the app is, which files it is made from, how they are placed |
| **input** | a file (or folder) the recipe is made from and the user supplies, known by its sha256 |
| **base** | a runtime that apps are built `FROM`: `base/<name>:<version>`. It imitates one system |
| **platform** | the system a base imitates: `windows-x86`, `windows-x64`, `xbox-series`, `linux-x64`, `microvm-x64`, `ps5` |
| **library** | a part of a base an app may ask for by name (`d3d9`, `fmodex`): the DLLs it answers for |
| **image** | what a build makes: an OCI image layout with the app's files, its config and its compiled code |
| **host** | what runs an image: the browser page and Node today |
| **builder** | what makes an image from a recipe: `pisco build` today |
| **session** | a script of what a player does (keys, clicks, waits) and where to take screenshots |
| **oracle** | the platform's real system that an app's behaviour is checked against: Wine, or QEMU |

"Must", "must not" and "may" are used as in RFC 2119. Words a page uses for one implementation (a cache, a flag of
`pisco`) are marked as `pisco`'s.

## Stability

Every base says how settled it is (`stability` in [base.md](base.md)):

| Level | Bases | Means |
|---|---|---|
| **stable** | `windows-x86`, `windows-x64`, `xbox-series`, `linux-x64` | what these pages say of it changes only with a new schema (recipes, configs) or a new base version |
| **experimental** | `microvm-x64`, `ps5` | its platform page may change at any time, without a new schema. A builder must say so when it builds one |

What is common applies to every base, experimental or not: the recipe's schema and its strict reading, inputs, the
lock, the image's layout and the config's common fields. What is particular to an experimental base is on its own
platform page and nowhere else. A base becomes stable in a pull request that moves its page's keys into these pages.

The host interface ([host.md](host.md)) is version 0 for every base: an image says which version it was built for,
and a host refuses one it does not know, but version 0 itself may change in any pull request until it is 1.

## What conforming means

**A builder** conforms if, from a recipe and its inputs, it:

1. reads the recipe as [recipe.md](recipe.md) says, and refuses one it cannot read in full (an unknown key, a
   schema it does not know, a name not in the lists);
2. checks every input against its sha256 before using it;
3. makes the app's files that the recipe describes: the same paths and the same bytes, so the same **tree digest**
   ([image.md](image.md#the-tree-digest)) as any other conforming builder;
4. writes an image that [image.md](image.md) describes.

The compiled code (the modules) is the builder's own: two builders need not make the same bytes, nor even the same
number of modules, as long as a host they are built for runs them.

**A host** conforms if it runs an image built for a host interface version it knows, gives the app the files of its
layers as [image.md](image.md#files) says (by platform), and keeps what the app writes apart from the image.

**An app works** at one of three levels ([sessions.md](sessions.md#levels)): it *builds* (the tree digest is the
lock's), it *starts* (its first frame, or what it prints), it *plays* (a session's checkpoints match the oracle's).

## Versions

| What | Where it is said | Today |
|---|---|---|
| the recipe's schema | `schema` in `recipe.toml` | 1 |
| the config's schema | `schema` in the config | 1 |
| a base | `base/<name>:<version>` | 1 for every base |
| the host interface | `abi` in the config | `pisco.host/0` |
| the session's schema | `schema` in a session | 1 |

A reader must refuse a version it does not know rather than guess.
