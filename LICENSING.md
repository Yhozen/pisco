# Licensing

## How licensing works here

The root [`LICENSE`](LICENSE) is the GNU General Public License, version 2. Our own code is offered under
**GPL-2.0-or-later**: version 2 or, at your option, any later version. That covers everything in this
repository unless a file's `SPDX-License-Identifier` line, a `LICENSE` file in its directory, or the table
below says otherwise. Code taken from other projects keeps its original licence.

Every file's licence is recorded for tools in [`REUSE.toml`](REUSE.toml) and in the file's own
`SPDX-License-Identifier` line where the format allows one. The licence texts are in [`LICENSES/`](LICENSES).
`reuse lint` checks that nothing is left uncovered.

## Licences in this repository

| Path | SPDX licence | Why |
|---|---|---|
| everything not listed below | GPL-2.0-or-later | our own code |
| [`crates/shaders/`](crates/shaders) | GPL-2.0-only | the RDNA2 decoder, its tables and the console shader container parser come from [XCL](https://github.com/ivall/xcl) |
| `crates/shaders/src/rdna2_spec.rs` | GPL-2.0-only AND MIT | XCL's table, generated from AMD's instruction specification (MIT) |
| `rt/d3d12_layout.gen.h` | MIT | generated from `d3d12.h` of microsoft/DirectX-Headers |
| [`tests/data/llvm-amdgpu/`](tests/data/llvm-amdgpu) | Apache-2.0 WITH LLVM-exception | LLVM's decoder test vectors, with their `LICENSE.TXT` |

When you add code from another project, keep its licence line, add a row here and in `REUSE.toml`, and credit
it in [`THIRD_PARTY.md`](THIRD_PARTY.md).

## The rule that matters: what is linked together

A directory map labels files; it does not make incompatible licences compatible. What has to be consistent is
each **program**:

- **One linked program**: everything that ends up in one `title.wasm` (the lifted title, the runtime in
  `rt/`, the host code that completes it) must carry licences that can be combined.
- **Separate programs** that only exchange files and command lines can each have their own licence: the
  lifter, the shader tools, the test and oracle tooling, a recipe runner.

| Code from | Licence | In `title.wasm` | In a separate tool |
|---|---|---|---|
| Wine, vkd3d | LGPL-2.1-or-later | yes, in any build | yes |
| DXVK | Zlib | yes, in any build | yes |
| FFmpeg (built without `--enable-gpl`) | LGPL-2.1-or-later | yes, in any build (today: the XMA2 and Opus decoders) | yes |
| wasi-libc, compiler-rt | Apache-2.0 WITH LLVM-exception, MIT | yes (the exception covers GPL-2.0) | yes |
| XCL | GPL-2.0-only | only in a build with no GPL-3.0 or LGPL-3.0 code | yes (as today: `crates/shaders`) |
| VIctory | GPL-3.0 | only in a build with no GPL-2.0-only code | yes |
| Lutris | GPL-3.0 | no use for it there | yes |
| Phoenicis (PlayOnLinux) | LGPL-3.0 | no use for it there | yes |

Because our own code is GPL-2.0-or-later, it can sit in either kind of build: with GPL-2.0-only code the
result is distributed under GPL-2.0, with GPL-3.0 code under GPL-3.0. The two cannot meet in one program.

Today no code from XCL is linked into `title.wasm`: `crates/shaders` is a command-line tool, and the runtime
uses XCL only as a source of facts about the console (see `THIRD_PARTY.md`). To keep each build's set of
licences checkable, a title links only the runtime files of its profile (`tools/build-title.sh`, `PROFILE`):
`console` is `rt/*.c`; `win32`, for Windows desktop titles, is the core of `rt/` plus `rt/win32/`, without the
console's libraries (`gdk.c`, the D3D12 and DirectStorage front ends, XAudio2, the FFmpeg decoders).

## What is never in this repository

A title's files, anything lifted or built from them (`title.wasm`, shaders, screenshots, traces), and tools we
only download (wasi-sdk, llvm-mingw, Wine, dxc). They keep their own terms; `out/` is ignored.

This page describes how the repository is organised. It is not legal advice.
