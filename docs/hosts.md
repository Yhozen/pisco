# Hosts: the browser, and what to do if it becomes the limit (2026-11-25)

A host runs a title's module: today the browser page (`web/browser/`, WebGPU) and Node (`web/run-node.mjs`, a null
GPU). This page records how the browser was found slow once, what in it is a real limit and what was ours, and the
alternatives that were weighed. **Short answer: stay in the browser; if a title hits one of the real limits below,
add a native Rust host (wasmtime + wgpu) next to it, not instead of it.**

## The case that raised the question: Tag in Chrome

Tag: The Power of Paint was slow in Chrome on an M1 (menu "horrible", the frame rate dropping when painting). A
2-minute Chrome Performance trace showed:

- **The title's thread spent 56 s of 121 s blocked in synchronous requests to `tools/serve.mjs`**, about 21,000 of
  them, 85–90 % of its CPU profile, in the menu and in the level alike. The GPU and the page's main thread were
  mostly idle.
- **They were file checks, not reads.** Tag reopens its textures dozens of times a second (14,717 requests were for
  `particle_bubble.png`, from the moment the gun fires). Each `CreateFileA` asks the host for the file's kind and
  size up to five times (`vfs_open`, `vfs_is_dir`, `GetFileSize`, `CreateFileMapping`). `worker.js` cached a file's
  bytes but not these answers, so each one was an HTTP round trip of about 2 ms.
- **Fixed in the worker**: answers to `stat` and `list` are kept, as the bytes are (the server's files never change
  while a title runs; its writes stay in `rt/vfs.c`'s overlay). The same `level2-paint` session for 300 s in headless
  Chrome (WebGPU on SwiftShader, virtual clock): 875 frames before, 1,321 after.
- **A second cost**: in a few 2-second stretches the page's main thread was 90 % in `GPUQueue.writeBuffer` (tasks up
  to 190 ms) and the title waited for ring space. Tag locks a 384 KiB vertex buffer whole, with `NOOVERWRITE`, a
  dozen times a frame to append a few vertices; a whole lock says nothing of which bytes changed, so all of it was
  sent each time (Node's null back end: 50 GB over the 7,988-frame session).
- **Fixed in the Direct3D 9 layer** (`d3d9_gpu.c`, `buffer()`): a buffer of 16 KiB or more locked whole without
  `DISCARD` keeps a copy of what the GPU holds, and only the bytes that differ are sent. The same session in Node:
  1.1 GB instead of 50 GB, 68 s instead of 97 s, the same draws and passes, every record consistent.

Node never showed the first problem: there a file check is a local call of microseconds.

## The second case: Desire in Chrome

Desire (Direct3D 12, [desire.md](desire.md)) on an M1 ran at 18–21 frames a second. A 45 s trace: the page's main
thread busy 37.6 s, 25.3 s of it in `queue.writeBuffer`. A root CBV has no size, so each of about 700 draws a frame
uploaded 64 KiB from its offset in the title's 192 KiB constant ring: 48 MB in 736 calls a frame. **Fixed in
`web/gpu/backend.js`**: a buffer's ranges are merged before upload (0.13 MB, one call), each pipeline's bind group
layouts are kept, and within a segment a bind group of the same objects is made once.

After (11.7 s trace, walking the maze): about 38 frames a second, frame times 17 / 20 / 39 ms at p10 / p50 / p90.
The title's thread is 61 % asleep in Present's 60 Hz pacing, 29 % waiting on its fence for the page, under 10 % in
the game. The page's main thread is 43 % idle; inclusive, `draw` 25 %, `bindGroups` 15 %, `execute` 15 %,
`createBindGroup` 5 %, `writeBuffer` under 1 %. The GPU process was busy 2.5 s of 11.7. If a Direct3D 12 title needs
more, the next steps are ours again: bind groups cached across segments (or keyed by the descriptor heap's
contents), and pipelining, so the page runs frame N while the title records frame N+1 (the fence wait).

## What in the browser is a real limit

| Limit | What it costs | Who it hits |
|---|---|---|
| WebGPU's feature set | no bindless heaps, mesh shaders or 64-bit integers, as journey O3 lists | the Xbox Series runtime (Minecraft used eight heaps of 73,727 descriptors in XCL) |
| Memory | wasm32: 4 GiB; wasm64: 16 GiB through the JS API, 0–45 % slower here (`memory.md`), incomplete in Safari | titles that reserve or commit more than ~3.7 GiB |
| No fast blocking I/O | a blocked worker has only synchronous XHR (or OPFS sync handles) | titles that touch many files; fixed for checks by caching |
| A process boundary per GPU call | every WebGPU call and upload crosses to the GPU process | titles that upload a lot per frame |
| Threads | a pool of workers made ahead of time (roadblock R3); `Atomics.wait` only off the main thread | titles with many threads |
| The page | Escape and Ctrl+W belong to the browser, sound needs a click, code warms up while it plays | every title, mildly |

What the browser gives: a URL and nothing to install, a sandbox, and the host every Wine comparison is checked in.

## Measured: lifted code is not the limit

- Tag's `level2-paint` session in Node (V8, the null GPU back end checking every record): 7,988 frames in 97 s,
  about 82 frames a second on the 4-core test VM, loading included.
- The same benchmark title (`fixtures/bench`) on V8 and on wasmtime 51 (a throwaway Rust host, the bench's
  checksums equal), ms:

| workload | native | Node (V8) | wasmtime 51 |
|---|---|---|---|
| calls (fib 32) | 5 | 25 | 25 |
| bytes (FNV, 64 MB) | 119 | 147 | 117 |
| sort (heapsort, 2M u32) | 496 | 990 | 700 |
| float (4x4 transform) | 57 | 77 | 78 |
| memory (20M random loads) | 145 | 166 | 159 |

- Tag's module (29 MB) on wasmtime: refused as built (clang emits the legacy exception instructions, `try`, which
  wasmtime will not implement); after `wasm-opt --translate-to-exnref` (binaryen 133) it compiled ahead of time in
  18.5 s to 55 MB of machine code. Not run: that needs the GPU, audio and input parts of a host.

## The alternatives

| | Runs the module in | Files | GPU beyond WebGPU | Same on macOS, Windows, Linux | Work |
|---|---|---|---|---|---|
| 1. The browser, fixed | Chrome | cached, or OPFS | no | yes, in Chrome | small |
| 2. Electron | bundled Chromium | Node's file system | no | yes | small to medium |
| 3. Tauri, the title in its webview | the system's webview | partly | no | **no** | medium, with platform gaps |
| 4. A native Rust host: wasmtime + wgpu | Cranelift, ahead of time | direct | yes | yes | large |
| 5. No wasm: the lifted C compiled natively | machine code | direct | yes | one build per platform | largest |

1. **The browser, fixed.** What was done for Tag: cached file checks, smaller uploads. Keeps everything; does
   nothing for the GPU and memory limits.
2. **Electron.** The same page and engine, packaged with Chromium; files through Node instead of HTTP. About 100 MB
   an app and the same WebGPU limits. Journey O8 already expects to ship Chromium on macOS if a title needs wasm64.
3. **Tauri with the title in its webview.** The webview is the system's, a different engine on each platform:
   - macOS (WKWebView): WebGPU only from macOS 26; one project reports `SharedArrayBuffer` (our threads) unavailable
     on Tauri's custom protocol, worked around with a loopback HTTP server; a plugin exists only to lift a 60 fps cap.
   - Linux (WebKitGTK): no WebGPU found, so nothing would be drawn.
   - Windows (WebView2): Chromium, so it should work; WebGPU needed a flag as of the last report found (2023).
   - Tauri's calls into Rust are asynchronous, and our title's thread blocks: the Rust side would not answer file
     checks by itself.
   Tauri does fit as the window around option 4: a library, settings, building a title from its installer.
4. **A native Rust host: wasmtime + wgpu** — the proposed fix if the browser becomes the limit. A third host,
   inside `pisco run`, next to the browser and Node:
   - direct files, Metal / Vulkan / D3D12 through wgpu, real threads, the module compiled ahead of time at build;
   - wgpu's native-only features cover two of O3's gaps: arrays of textures (bindless) and 64-bit integers (mesh
     shaders unconfirmed);
   - reuses the WGSL the runtime and the translators already write, and a small contract: 16 `host` functions,
     about 20 WASI ones, and the GPU, audio and input rings (`rt/gpu_ring.c`, `rt/mixer.c`);
   - costs: porting `web/gpu/backend.js`, `explicit.js` and the worker (about 1,500 lines of JS) to Rust; building
     with the standard exception instructions (or converting with binaryen); a third host to keep equal with Node
     and Chrome in `tests/run.sh`; no URL. Risk: wasmtime's threads are tier 2, with known gaps for shared memory.
5. **No wasm.** Compile the lifted C to machine code (as N64 and Xbox 360 recompilation projects do). Probably the
   fastest (how much of the gap to native is wasm's and how much the lifting's was not measured), but no sandbox, no
   single artifact, and every user's machine needs a native toolchain since titles are built from the user's copy.
   Option 4 already gets machine code from the module without that.

Also looked at: Deno, whose WebGPU can draw into a native window and would keep `backend.js` as it is; its windows
are still marked unstable.

## When to start option 4

When a title needs one of: a WebGPU feature it lacks (bindless, mesh shaders, 64-bit integers), more than wasm32's
memory, or a speed the browser cannot reach after profiling shows the time is in the browser (the GPU process, the
upload path), not in our code. Profile first: the Tag case looked like a browser limit and was ours.

## Sources

- Tauri and WebGPU: [tauri#6381](https://github.com/tauri-apps/tauri/issues/6381);
  `SharedArrayBuffer` in WKWebView under Tauri: [pdfcraft#201](https://github.com/PDFCraftTool/pdfcraft/issues/201),
  [pdfcraft#202](https://github.com/PDFCraftTool/pdfcraft/pull/202);
  [tauri-plugin-macos-fps](https://github.com/userFRM/tauri-plugin-macos-fps)
- WebKitGTK and WebGPU: [WebKit bug 257694](https://bugs.webkit.org/show_bug.cgi?id=257694),
  [webkit-gtk list, 2023](https://lists.webkit.org/pipermail/webkit-gtk/2023-October/003947.html)
- Safari and memory64: [WebKit bug 311449](https://bugs.webkit.org/show_bug.cgi?id=311449)
- Wasmtime: [proposal status](https://docs.wasmtime.dev/stability-wasm-proposals.html),
  [no legacy exceptions](https://bytecodealliance.github.io/zulip-archive/stream/217126-wasmtime/topic/Exception.20Standard.html)
- wgpu native-only features: [FeaturesWGPU](https://docs.rs/wgpu-types/29.0.4/wgpu_types/struct.FeaturesWGPU.html)
- Deno: [WebGPU on the desktop](https://docs.deno.com/runtime/desktop/webgpu/)
