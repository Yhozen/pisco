# PS5 titles: the raymarching tech demo in a browser

`base/ps5` runs a native PS5 executable — homebrew built with the open ps5-payload-sdk and ps5-native-tool, an
ELF of type 0xFE10 ("SCE dynamic executable") — as a web title: lifted to C and WebAssembly like every PISCO title,
its calls into the PS5's system libraries answered by `rt/ps5/`. The first title is
[ps5-raymarch-techdemo](https://github.com/Alexius0815/ps5-raymarch-techdemo) v1.4: a CPU raymarcher (ten
threads, AVX2 and FMA code built for the PS5's Zen 2) with three scenes, a DualSense and synthesized music.
[AnyPS5](https://github.com/boykopovar/AnyPS5) (a PS5 compatibility layer for desktops) is the reference for the
executable's format and for what the system functions do.

## Run it

```sh
tools/setup-toolchain.sh wasi                  # wasi-sdk, once
cargo build --release                          # the lifter
tools/build-ps5-raymarch.sh                    # downloads the v1.4 release, checks its SHA-256, builds out/ps5-raymarch/title.wasm
node web/run-node.mjs out/ps5-raymarch/title.wasm out/ps5-raymarch/eboot.elf --smoke-test
#   [showcase-smoke] complete frames=180 scenes=3
SW_SHOT_EVERY=30 node tools/run-chrome.mjs out/ps5-raymarch/title.wasm out/ps5-raymarch/eboot.elf --smoke-test
#   headless Chrome (WebGPU on SwiftShader); every 30th frame saved as out/ps5-raymarch/shots/flip-<n>.png
```

In a browser of your own (WebGPU; cross-origin isolation comes from the server):

```sh
node tools/serve.mjs 8000 web out/ps5-raymarch
# http://127.0.0.1:8000/browser/index.html?wasm=/files/0/title.wasm&exe=/files/0/eboot.elf
#   (&args=--observatory starts in the third scene; &args=--smoke-test runs 180 frames and exits)
```

Controls: a standard gamepad maps button for button onto the DualSense. On the keyboard: WASD the left stick,
arrows the right stick, Space R2, Enter Circle (next scene), C Cross, X Square, T Triangle, Q / E L1 / R1,
Z / V the d-pad left / right (music), H L3 (help), M Options, P the touchpad. Click or press a key once for sound.

Settings (as for every title): `SW_CLOCK=16667` a virtual clock (a frame per flip, deterministic), `SW_PACE=off`
flips without waiting for vertical blank, `SW_SHOT_EVERY=<n>` a PNG of every n-th flip (headless Chrome).

An offline render in Node (no browser; each flip dumped, then encoded): `SW_CLOCK=16667`, `SW_DUMP_RAW=<file>` (packed
RGBA, 1920x1080), `SW_AUDIO_WAV=<file>` (48 kHz stereo float), `SW_MAX_FRAMES=<n>`, and optionally `SW_KEYS=<json>`
(the same list as `tools/run-chrome.mjs`, times in milliseconds on the virtual clock). Then ffmpeg:

```
ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -r 60 -i frames.rgba -i audio.wav \
  -c:v libx264 -profile:v high -pix_fmt yuv420p -crf 17 -c:a aac -b:a 192k -movflags +faststart out.mp4
```

## How

- **The executable** (`crates/lifter/src/ps5.rs`): its PT_LOAD segments are the sections; the dynamic segment's
  RELA table gives the relative relocations (the file holds 0 in their slots, so the lifter keeps each one's value,
  `rt_reloc_values`, and `rt/ps5/start.c` writes them) and the GLOB_DAT / JUMP_SLOT imports. An import's symbol is
  `NID#lib#module`; the library letter is matched to the library's name through the dynamic tags, and the NID
  (an 11-character base64 of the first 8 bytes of SHA-1 of the function's name and a fixed suffix) to a name by
  `ps5_nids.txt`, which lists the names the runtime answers for and their NIDs, computed once. The functions to
  lift start from the entry point and every FDE of `.eh_frame_hdr`. The image goes to 0x10000000 as other 64-bit
  titles do.
- **VEX** (`crates/lifter/src/avx.rs`, `rt/avx.h`): AVX, AVX2, FMA3 and BMI1/2 — the demo has about 4,700 such
  instructions. A ymm register is its xmm register (the low half) and `c->yh[n]` (the high half); a VEX instruction
  that writes an xmm register clears the high half. In-lane operations run the 128-bit helper on each half.
  `tests/avx.sh` checks 120 results of a Zen 2 build (`fixtures/avx`) against what the CPU printed. A fused
  multiply-add is a multiply then an add by default (the last bit can differ); `OPT='-O2 -DPISCO_FMA_EXACT=1'`
  makes float ones exact but for rare double rounding, about a third slower in the demo. The virtual CPU's CPUID is
  unchanged (it reports no AVX, LZCNT or BMI1: lzcnt and tzcnt stay bsr and bsf); a PS5 title does not ask.
- **The runtime** (`rt/ps5/`, `bases/ps5/1/base.toml`): `_start` gets the process parameters (argc, argv) as the
  PS5's loader passes them. libkernel's direct memory is guest memory; `scePthreadCreate` makes a runtime thread
  with its own Cpu and stack; `sysctlbyname` describes the virtual machine (`hw.model` "PISCO WEBASSEMBLY - LIFTED
  X86-64"). libSceVideoOut: the registered frame buffers are linear RGBA8, and a flip sends the buffer to the GPU
  back end as a texture and presents it. libScePad reads the page's input block (keys and the first gamepad).
  libSceAudioOut's ports are sources of the mixer, which the page plays through an AudioWorklet.
- **Headless Chrome**: `tools/run-chrome.mjs` now passes `--use-vulkan=swiftshader`. Without it, this machine's
  Chrome 156 could not create the canvas's swap chain texture at 1920x1080, and the WebGPU device was lost on the
  first present.

## Status

| | |
|---|---|
| Smoke test (180 frames, 3 scenes) | passes in Node and in headless Chrome |
| Pictures | the three scenes, read back from WebGPU, match the project's own screenshots |
| Input | the keyboard switches scenes and moves the camera (Chrome, `KEYS=`) |
| Sound | the music plays (a capture's audio is not silent: mean -14 dB) |
| Speed | about 20–35 frames a second in Node on 8 cores at 480x270 (the demo lowers its resolution to keep up); 60 on a PS5 |

Not done: only the system functions this demo calls (38 NIDs) are there, and an import without a name traps
when called; no GPU (AGC) title runs — this one draws on the CPU; `pisco build` images of a PS5 title are not
wired up (`tools/build-title.sh` is the way); `kern.proc.pathname` is not answered (the demo prints `path=-`).

## Licensing

PISCO's PS5 support (the lifter's `ps5.rs`, `avx.rs`, `rt/ps5/`, `rt/avx.h`) is GPL-2.0-or-later like the rest
of PISCO. The demo is GPL-3.0-or-later (Alexius0815, with parts from BlackBearReloaded's ps5-native-app-boilerplate)
and is not in this repository: `tools/build-ps5-raymarch.sh` downloads its release and keeps its LICENSE and
NOTICE.md beside the built title. A built `title.wasm` contains the demo's code, so it is distributed under the
demo's license. No Sony firmware, keys, SDK headers or libraries are used or needed: the PS5 functions are written
from their documented behaviour and AnyPS5 (GPL-2.0) as a reference — no AnyPS5 code is copied — and the NIDs in
`ps5_nids.txt` are computed from the public function names.
