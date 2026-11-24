# Guest memory: wasm32 or wasm64

A guest address is an offset in linear memory (`GP(a)` in `rt/cpu.h`). The build picks the width:
`tools/build-title.sh <exe> <out> wasm32|wasm64`. The lifter moves images based above 4 GiB
(`0x140000000`, the Windows default) to `0x10000000` using their relocations (`--base` overrides).

## Measured (2026-11-24, Node 24 / V8, this VM, `fixtures/bench`, ms, best of 3)

| workload | native x86-64 | wasm32 | wasm64 | wasm64, memory above 5 GiB |
|---|---|---|---|---|
| calls (fib 32) | 7 | 40 | 40 | 46 |
| bytes (FNV, 64 MB) | 123 | 179 | 225 | 251 |
| sort (heapsort, 2M u32) | 325 | 968 | 1132 | 1332 |
| float (4x4 transform) | 34 | 90 | 131 | 139 |
| memory (20M random loads) | 181 | 220 | 227 | 234 |

- wasm64 costs 0–45 % over wasm32 here: 64-bit memories need explicit bounds checks.
- Growing a wasm64 memory past 5 GiB and touching ~200 MB of it: 157 MB resident. Address space that
  is never touched costs no RAM in V8 on Linux, so a title's large reservations are cheap in wasm64.

## What decides it for a title

- **wasm32 is enough** if the title's reservations plus commit fit below ~3.7 GiB (4 GiB minus the
  runtime and the low guard). Data point: Minecraft's GPU memory fit XCL's default 2 GiB unified window
  (XCL `HANDOFF.md`); its total commit and reservations are not known yet — the first run measures them.
- **wasm64 is needed** for anything larger, and for titles that reserve big ranges up front (XCL saw
  streaming pools reserved without commit and physical pages mapped into them).
- Browsers: wasm32 memories go to 4 GiB in Chrome, Firefox and Safari; wasm64 (memory64) is in Chrome
  133+ and Firefox 134+, capped at 16 GiB by the JS API, and **behind a flag in Safari**. A wasm64 build
  rules out WKWebView, so the macOS app would have to ship Chromium instead of using the system web view.

Plan: stay wasm32 by default, keep every guest address going through `GP()`, and switch per title when
the first run's peak shows it needs more.

## 32-bit titles: an image that must load low

A 32-bit exe usually wants `0x400000`, and an old one often has no relocations, so it cannot be moved above
the runtime as 64-bit images are. `tools/build-title.sh` gives such a title a gap instead (checked with
wasi-sdk 34's linker):

```
0 .. IMAGE_LOW            the runtime's own stack (--stack-first, -z stack-size: at most 8 MiB)
IMAGE_LOW .. IMAGE_HIGH   the title's images: the exe where it was linked, its DLLs after it
IMAGE_HIGH (to 1 MiB) ..  the runtime's data (--global-base), then its heap, which the guest's memory comes from
```

`IMAGE_LOW` / `IMAGE_HIGH` come from the lifter (`title.env`). Guest allocations stay below 2 GiB, as a program
that is not large-address-aware expects. Addresses below `0x10000` are not a trap in wasm: a null pointer
read returns the runtime's stack bytes (a known limit, as for 64-bit titles).
