# fastfetch (2026-11-26)

The first Linux title: [fastfetch](https://github.com/fastfetch-cli/fastfetch) 2.71.0 (MIT), the system-information
tool, as one statically linked x86-64 program on the Linux x64 runtime (`base/linux-x64:1`, `rt/linux-x64/`). It was
chosen over neofetch, which is a bash script: running that means running bash and the programs it starts, and a
title here is one process. Nothing of fastfetch is in the repository: `tools/build-fastfetch.sh` fetches its source
(checked by sha256) and builds it.

What it shows is the machine the runtime describes, which is what the machine is: a Linux x86-64 system imitated
in WebAssembly, with the virtual CPU the lifter gives every title and the module's memory.

## The files

| | |
|---|---|
| Source | `fastfetch-2.71.0.tar.gz` from GitHub, sha256 `e71d93de41e8e4b2554c36d2ae51758112a21ecf3a8c98605a2c689251e62987` |
| Built with | llvm-mingw's clang 23 as a cross compiler (`--target=x86_64-linux-musl`; fastfetch is C23, which the usual gcc 11 to 13 do not compile), musl 1.2.5 built from its source (`tools/setup-toolchain.sh musl`), GNU ld, the kernel's headers from Alpine 3.20's `linux-headers` 6.6 and `libdrm-dev` 2.4.120 (fastfetch does not compile without `<drm/...>`), each checked by sha256 |
| Options | static, no threads, no library it would load at run time (Vulkan, Wayland, X, D-Bus, SQLite, zlib, Lua and the rest are off), `RelWithDebInfo` without `-g`: fastfetch's own `Release` strips the symbols the lifter finds functions by |
| `fastfetch` | ELF64, `ET_EXEC` at `0x400000–0x5fa000`, 2.2 MB, 1.3 MB of code, with `.symtab` and `.eh_frame` |

## Build it and run it

```sh
tools/setup-toolchain.sh wasi mingw musl
tools/build-fastfetch.sh                       # out/fastfetch/fastfetch
target/release/pisco build recipes/fastfetch   # out/images/fastfetch
target/release/pisco run out/images/fastfetch -- --pipe -l none        # Node: plain text
SW_TTY=100x30 target/release/pisco run out/images/fastfetch            # Node: the logo and colours, for a terminal
node tools/serve.mjs 8080 web out/images/fastfetch                     # the browser: http://127.0.0.1:8080/browser/index.html?image=/files/0/
SCREENSHOT=out/fastfetch.png node tools/run-chrome.mjs out/images/fastfetch   # headless Chrome, the page as a PNG
```

`tests/run.sh fastfetch` builds and runs it in strict mode on the virtual clock, from the lifted program and from
its image; what it prints must be `recipes/fastfetch/traces/pipe.txt`.

## What it took

| | |
|---|---|
| Lifted | 1,764 functions (1,653 named by the symbols and the unwind tables, the rest call targets and code addresses in data), 314,314 instructions, none left unlifted; 100 jump tables |
| Module | `title.wasm`, 5.8 MB; lifted and compiled in about a minute |
| System calls | 30 different ones in a run; all answered except `socket` (no network: `EAFNOSUPPORT`, and the Local IP line is left out) |
| Files it reads | `/etc/os-release`, `/proc/cpuinfo`, `/proc/meminfo`, `/proc/uptime` (by `sysinfo`), `/proc/self/exe`, `/etc/passwd`, `/proc/mounts`, and about a hundred it looks for and does not find (other distributions' files, package databases, DMI entries) |

What the lifter and the runtime gained for it, each also checked by `fixtures/linux-hello` (`tests/run.sh linux`),
whose output from its C library must be what the same program prints when run natively on the machine that builds it:

- **ELF**: the lifter reads a static, not position-independent x86-64 ELF; its loadable segments are what a PE's
  sections are. Functions start where the symbols and `.eh_frame` say, then as in a PE.
- **`syscall`** is a call into the runtime (`rt_syscall`, `rt/linux-x64/syscall.c`); **FS** is the thread pointer,
  which musl sets with `arch_prctl` after reading the auxiliary vector the runtime puts on the first stack.
- **`setjmp` / `longjmp`**: a lifted function returns by returning, so the C library's `longjmp` is replaced by the
  runtime's, which goes on at the address `setjmp` saved (since neofetch: by entering the function there,
  [neofetch.md](neofetch.md#what-it-took)).
- **The x87 at 64 bits of precision**: musl's `printf` and `strtod` do their arithmetic in `long double`. With the
  registers kept as doubles (right for Windows code, which runs the x87 at 53 bits) `printf("%f")` truncated
  instead of rounding. A Linux title's registers are the module's `long double` (binary128), each result rounded to
  64 bits (`rt/cpu.h` `fpx`).
- **The direction flag** (`std`: musl's `memmove` copies backwards with it) and `cvttpd2dq`.
- **The machine**: `/proc`, `/sys`, `/dev` and the files of `/etc` every system has are generated
  (`rt/linux-x64/proc.c`); the title's folder, or its image's drive, is the root, read-only.
- **The page** shows a title's text in a terminal's colours (the SGR sequences; no cursor movement), and a Linux
  title's image gets the page as its terminal (`SW_TTY`).

## Limits

- No threads: a program built with them would stop at its first `pthread_create`. Processes came later, with
  neofetch ([neofetch.md](neofetch.md)); fastfetch's own image has no other program to start, so the lines it gets
  from other programs (Shell's version, Packages) are left out.
- No network, no signals delivered, nothing can be written but the standard streams and `/dev/null`.
- A program must be linked `-static -no-pie` and keep its symbols (for `longjmp` to be found; without `.symtab` and
  `.eh_frame` the lifter still finds what calls and data refer to).
- No keyboard is wired to a title's standard input in the page; a terminal query a program sends is not answered.
- x87 results are rounded twice (to 113 bits, then to 64): a result exactly between two 64-bit values can differ in
  its last bit. Transcendental x87 instructions (`fyl2x`, `f2xm1`) compute in doubles.

## Progress

| # | Milestone | State |
|---|---|---|
| L0 | the core's system-neutral parts in files of their own (`rt/core.c`, `rt/clock.c`) | done |
| L1 | the lifter reads an ELF, `syscall`, FS; `base/linux-x64:1`; a static musl program prints in Node | done |
| L2 | a C library's start-up and the machine it finds: memory, files, `/proc`, `setjmp`, the x87 at 64 bits; `fixtures/linux-hello` against its native run | done |
| L3 | fastfetch built from its source, lifted, run in Node in strict mode; its image by `pisco build` | done |
| L4 | the page as a terminal: colours, a Linux title's image in headless Chrome | done |
