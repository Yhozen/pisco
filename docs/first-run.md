# First run of a real title (Minecraft)

The title's files stay outside this repository (e.g. `~/games/minecraft`, the unpacked package: the folder with
`Minecraft.Windows.exe` and `MicrosoftGame.config`). Nothing below writes into that folder.

## 1. Census (minutes, nothing runs)

```sh
cargo build --release -p pisco-lifter -p pisco-shaders
tools/census.sh ~/games/minecraft/Minecraft.Windows.exe out/mc-census
```

It prints:
- **lifter**: images found (the exe and every DLL beside it it imports), functions, jump tables, AVX-only
  functions left out, and instructions not lifted yet (with counts per mnemonic: the first work list);
- **imports**: per DLL, how many the runtime implements; `out/mc-census/import-status.txt` lists the rest
  (they become logging stubs at run time);
- **shaders**: console shader containers found in the folder, programs per stage (does it use NGG / mesh
  shaders? geometry shaders?), the RDNA2 instructions they use. This sizes the WebGPU back end.

Also check `docs/memory.md`: the title's address-space needs decide wasm32 or wasm64.

## 2. Build and run headless

```sh
SPLIT=32 JOBS=$(nproc) OPT=-O2 tools/build-title.sh ~/games/minecraft/Minecraft.Windows.exe out/mc
SW_TRACE=1 node web/run-node.mjs out/mc/title.wasm ~/games/minecraft/Minecraft.Windows.exe
```

A big title takes ~10 minutes to compile the first time; later builds only recompile the parts that changed.
Expect it to stop early: at an instruction not lifted, an import whose stub answer is wrong, or the first
D3D12.X call (there is no graphics device yet). `SW_STRICT=1` stops at the first unimplemented import instead
of stubbing it, `SW_TRACE=2` logs every import call.

## 3. In Chrome

```sh
node tools/run-chrome.mjs out/mc/title.wasm ~/games/minecraft/Minecraft.Windows.exe
```

## What to bring back from a run

The census output, and the last 50 lines of a `SW_TRACE=1` run. They say what to implement next.
