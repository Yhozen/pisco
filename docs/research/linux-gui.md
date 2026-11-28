---
date: 2026-11-28
measured:
  pisco: this branch (conductor/linux-gui), on conductor/tabc-poc-bench
  tabc: https://github.com/fzakaria/tabc 8b7e7920606d5fc20197c19da92a1f308504dbe2 (1.0.0) with tools/tabc-pisco/tabc.patch
  unkernel: npm unkernel@0.2.0-261125-cec0a49, its one-CPU kernel (kwfb: /dev/fb0; kwinput: evdev)
  alpine: Docker Hub alpine:3.24.4, linux/arm64 (kwemu) and linux/amd64 (PISCO)
  chrome: 158.0.8127.88, headless, started by tabc
  host: the 4-core x86-64 test VM
status: research; a proof of concept in tools/tabc-pisco, nothing of it in any app or base
---

# A Linux desktop in a tab: Xorg on unkernel, x86-64 programs lifted or emulated at once

[tabc-pisco.md](tabc-pisco.md) runs amd64 containers in a browser tab: tabc's guest kernel (unkernel, Linux built
for wasm) with PISCO in it, their programs lifted ahead of time. This page gives that guest a screen, a keyboard
and a mouse and runs a desktop on them; then makes any x86-64 program start at once, lifted or not; then looks at
what makes it slow.

## Short answer

- **A desktop in a tab.** Xorg (Alpine's `xorg-server`, the `fbdev` driver, `evdev` input) on unkernel's
  framebuffer, JWM as the window manager, xterm, xeyes and xclock, in headless Chrome: the page shows the picture
  and sends the keys and the pointer. Typed into xterm, `uname -a` answers from the guest; a click on JWM's button
  opens its menu; the eyes follow the pointer. About 150 lines in tabc's page and supervisor
  ([How](#1-a-desktop-in-a-tab)).
  ![JWM, xterm, xeyes and xclock in the tab](../img/linux-gui-arm64.png)
- **Any x86-64 program, at once: a first tier.** A program PISCO has not lifted yet no longer waits for its lift
  (seconds for a small program, minutes for a large library): the task is started again on qemu-user, in the same
  guest, and the host lifts it meanwhile; the next run is lifted
  ([How](#2-any-program-at-once-a-first-tier)). Cold, `sh -c 'echo; md5sum'` in an amd64 image with nothing lifted
  takes 4 s on qemu-user; once lifted, 1.8 s.
- **A library lifted once, for every program.** A library's image (what is lifted, keyed by its sha256) depended
  on where the loader mapped it and on what other modules its relocations pointed at, so the same library was
  lifted again for each program that mapped it elsewhere: 23 of the 52 images an x86-64 desktop met were such
  repeats. Each library now gets an address of its own (from its path), and its image is made the same in every
  program: no large library is lifted twice any more (before: libraries of 0.1 to 1.4 MB, up to five times).
- **Fast where it is lifted, with two holes.** The x86-64 desktop, lifted: Xorg, twm, xeyes and xclock draw and
  answer (![](../img/linux-gui-x86-64.png)), apk installs the desktop in 24 s against 40 s for arm64 on kwemu; the
  picture costs the kernel's thread 3 to 5 times less than with the SDK's `frame()`. The holes: a process starts
  in 30 ms against 19 (`xsetroot` 200 times in the running desktop: 6.1 s against 3.8), and lifted Xorg answers
  xterm's `OpenFont` with BadAlloc (twm's opens), so xterm does not run on the x86-64 desktop yet
  ([Fast](#4-fast)).

## 1. A desktop in a tab

unkernel's kernel has a framebuffer driver (kwfb, `/dev/fb0`, XRGB8888 in the guest's memory) and an input driver
(kwinput: an evdev keyboard, mouse and tablet), offered when the kernel is created with `display`. tabc does not use
them. The patch (`tools/tabc-pisco/tabc.patch`):

- **The supervisor**: `TABC_DISPLAY=WIDTHxHEIGHT` puts a `display` in the page's configuration.
- **The worker** (`page/guest.js`, where the kernel runs): `createKernel({display})`; every 33 ms, between the
  kernel's slices, the rows of the framebuffer that changed since the last look are copied and posted to the page,
  as XRGB. The SDK's `frame()` copies the whole picture as RGBA, on the kernel's thread, each time; the display's
  `pixels()` is the framebuffer in place, reached by wrapping `DisplayHost`'s methods (the SDK exports the class).
  The page's keys, pointer, buttons and wheel go to the guest's evdev devices (`domCodeToLinux`,
  `domButtonToLinux`).
- **The page** (`page/main.js`): a canvas above the terminal; the rows to RGBA and `putImageData`, on the page's
  thread; pointer and key events over the canvas to the worker. `tabcInput()` sends the same messages, for a test.

In the guest (`docs/research/linux-gui/start.sh`): `apk add xorg-server xf86-video-fbdev xf86-input-evdev jwm twm
xterm font-misc-misc font-cursor-misc font-dejavu xeyes xclock`, an `xorg.conf` with the fbdev device, the three
evdev devices and GLX and glamor off (nothing uses them on a framebuffer, and GLX would load Mesa), then
`Xorg :0 vt7 -sharevts -novtswitch`, the window manager (`WM`: JWM, or twm, which needs only the X libraries) and
the apps. The kernel has no virtual consoles (`CONFIG_VT` is off): Xorg is given `/dev/console` as its console and
never switches it. On arm64 (kwemu), the install takes 42 s (the packages from a cache) and Xorg is up in 1 s.
The guest needs more than 512 MiB (the container's writable layer is in memory, and Xorg brings Mesa and LLVM):
`-m 2048m`; for that PISCO's runtime moved from 1 GiB to 2 GiB in the kernel's memory (no change in speed).

## 2. Any program, at once: a first tier

PISCO lifts a program the first time it runs, on the host, before it goes on: busybox 55 s, libcrypto 7.5 min.
That is the wait a person sees the first time anything new runs. The first tier removes it:

- **Who may go to the first tier.** x86load, which turns an exec of an x86-64 file into an x86-64 task, marks the
  task clean and keeps its first stack (argc, argv, environment, auxv). The task stays clean while it only does
  what a second start would do again: mmap, open and read files (not standard input), fstat, mprotect, brk,
  sigaction, the TTY queries a loader makes (`harmless()` in `runtime.c`). Anything else (a write, a fork, an
  ioctl that changes something) makes it dirty.
- **What happens on a miss.** Code that no lifted program holds is met: the runtime asks the page for the image
  without waiting. The page asks the supervisor (a second, so that one lifted for an earlier container comes back
  at once), and otherwise has it lifted in the background and answers "later". A clean task is then made to
  `execve("/.pisco/qemu-x86_64", ["qemu-x86_64", "-0", argv0, path, args...], envp)` from where it is: the same
  program, arguments and environment, on qemu-user (an aarch64 program under kwemu).
- **A dirty task sleeps.** It cannot be started again, so it waits for the lift, which goes ahead of the background
  ones; but not in the page, which would stop the whole kernel (the page's answer suspends the kernel's one thread
  through JSPI): the runtime makes the task `nanosleep` half a second and try again, RAX kept, and the kernel runs
  the other tasks meanwhile. Xorg loading its modules with `dlopen` after it has started is such a task.
- **What else is lifted.** At that moment musl's loader has usually mapped every library the program needs:
  `r_debug`'s `link_map` (through the program's `DT_DEBUG`) names them, and they are lifted in the background too.
  The next run finds them lifted.
- **Lifts outlive the container.** A lift for a later run is queued on disk by the supervisor (the image beside
  the modules, `*.queued`) and answered at once (202); a runner of its own (a shell loop under `flock`, in a
  session of its own) lifts the queue, oldest first, after the container and its supervisor are gone. A lift a
  task waits for runs at once, into a temporary file renamed when done, with a lock file the runner respects.
  (The first version kept the queue in the page and each lift's request open until it was done: six of them took
  every connection the browser has to the supervisor, and the guest's file reads, which go the same way, stopped.)
- **The tier, in the guest.** tabc's init bind-mounts `/tabc/bin` (x86load and qemu-x86_64) at `/.pisco` in the
  container's root. qemu-x86_64 is Alpine's (static, aarch64); `setup.sh` fetches it.

Measured on a cache with nothing lifted (`sh -c 'echo hello; md5sum /bin/busybox'`, amd64 alpine:3.24.4):

| | first run | the runs after |
|---|---|---|
| before (a lift is waited for) | about 70 s (musl's loader 10 s, busybox 55 s, then the command) | 1.8 s |
| the first tier | 4.1 s (`/bin/sh` and `md5sum` on qemu-user; both lifted meanwhile, about 65 s) | 1.8 s, lifted |

The x86-64 desktop of section 1, from an empty cache, shows the trade's other side: the install and Xorg's first
start ran mostly on qemu-user (apk's triggers: `update-mime-database` 10 min, `fc-cache` 3 min), and the first
desktop drew its background and little else for minutes; the run after it was lifted (section 4).

## 3. One library, one lift

A lifted module is keyed by the sha256 of the program's image, rebuilt from memory when its code is first met
(`ask_host` in `runtime.c`). For a library that image held two things that differ from one program to the next:

- **Its address.** musl's loader maps a library where the kernel puts it (`mmap(0, ...)`); with the bottom-up layout
  that depends on what the program mapped before, so libX11 sat at a different address in xterm and in xeyes.
- **Its relocated data.** The loader has filled its GOT with the addresses of other modules' symbols (and of the
  program's own: copy relocations, interposition) before any of its code runs.

So the same file was lifted once per program that mapped it differently: of the 52 images an x86-64 desktop's
first runs met, 23 were such repeats (same size, another sha256). Two changes in `runtime.c` make one image per
library:

- **An address per library.** The runtime notes the path of the last `.so` opened and, for the `mmap(0, ...)` of
  that descriptor which follows, asks the kernel for an address of its own: a 256 MiB slot of a 16 TiB region,
  from the path's hash (`lib_address`). It is a hint: where the slot is taken, the kernel chooses, and that
  library gets a lift of its own.
- **A canonical image.** Before the hash, each of the library's relocations is set to what its own base gives
  (`R_X86_64_RELATIVE`: base + addend) or to 0 (everything that depends on another module). The lifter reads code
  and the tables it needs from the image (jump tables are relative or `RELATIVE`), not other modules' addresses.

With them, libfreetype is one image (`30af9415…`) for fc-cache, xterm and JWM. Over the runs that followed, 100
images were met and none of the large ones repeats: the 9 images left of a size another one has are 12 to 33 KB
(modules of the same size, as far as sizes tell). Before, libraries of 106 KB to 1.4 MB were lifted three to five
times each.

## 4. Fast

### The picture: only the rows that changed, off the kernel's thread

The kernel runs on one thread, the worker's, and everything else the worker does takes time from the guest. The
first version sent a frame as the SDK gives it: every 33 ms `frame()` (the whole picture copied to RGBA), a hash of
it, and a post of the 3 MiB when the hash changed. The second reads the framebuffer in place, compares it with the
last frame row by row, and posts only the band of rows that changed, as XRGB; the page converts it and draws only
those rows. On the arm64 desktop with `bench.sh` running (`display:` lines of the log, every 10 s; both runs on a
machine also busy lifting, so the times are indicative):

| | the worker's time on the picture, per 10 s | frames per 10 s |
|---|---|---|
| whole frames, RGBA on the worker | 0.83 to 2.77 s (8 to 28 % of the kernel's thread) | 1 to 74, all 768 rows |
| changed rows, XRGB, converted on the page | 0.18 to 0.94 s (2 to 9 %) | 1 to 67, 150 to 680 rows |

In the same runs xterm's start took 3.0 to 7.0 s against 7.9 to 13.3 s, and its scroll of 20,000 lines 9.7 to
14.5 s against 13.2 to 18.7 s.

### The desktops side by side

`bench.sh` in the running desktop (twm on both, `WM=twm`; the lift runner paused, nothing else running; one run
each, after a warm-up run; the packages from a local cache):

| | arm64, kwemu | x86-64, PISCO (lifted) |
|---|---|---|
| `apk add` of the desktop (31 packages, from the cache) | 40 s | 24 to 27 s |
| Xorg up | 1 s | 1 to 2 s |
| `xsetroot` 200 times (a process start and a few requests each) | 3.8 s | 6.0 to 6.1 s |
| xterm start, scroll of 20,000 lines | 0.84 to 1.36 s, 1.72 to 1.98 s | not run: `OpenFont` fails (below) |

Without an X server the process start is closer: `xsetroot` failing to connect, 50 times, 1.2 to 1.4 s on arm64
against 1.6 to 2.3 s lifted; `/bin/true` 50 times, 0.43 to 0.51 s against 0.71 to 0.77 s. What a lifted process
pays more at its start is not known yet (each new address space looks its programs up again: `programs_forget`).

**Two bugs of the lifted desktop.** Lifted Xorg answers xterm's `X_OpenFont` with BadAlloc (with `-fn fixed`,
`-fn 6x13` or `-fa monospace`), while twm, started before it, opens its font set; the same packages on arm64 open
both. And once, Xorg was gone a few seconds after it started (`Can't open display`). Neither is found yet. The
first is in the server: with Xorg run on qemu-user (`/.pisco/qemu-x86_64 /usr/libexec/Xorg`) and every client
lifted, xterm opens its font and shows its prompt beside twm, xeyes and xclock. That Xorg gets no input, though:
qemu-user does not pass evdev's ioctls, so it has no keyboard or mouse (the lifted Xorg has both).

**Fixed on the way.** The runtime registered at most 64 programs (a desktop maps more: xterm's 65th library got
SIGILL); now 1024. The lifter had no 16-bit `idiv` in 64-bit code (libfreetype's `idiv si`: fc-cache got
SIGILL). Lifts, traps and exceptions in lifted code are logged with their address.

## What this says about the roadmap

The roadmap proposed alongside this work (an architecture of interchangeable execution back ends: the lifter,
FEX or Box64, KVM, unkernel; a P0 to P3 list) reads differently after a day of this. Where today's evidence bears
on it:

- **unkernel integration is the Linux direction, not a P1.** A kernel compiled to wasm with lifted user programs is
  what tabc-pisco is, and it runs a desktop, apk over TLS and Lua, faster than kwemu on compute
  ([tabc-pisco.md](tabc-pisco.md)). The lifted microVM (Linux itself lifted) stays the way to run a kernel that has
  no wasm build; for Linux, compiling the kernel wins on every count measured.
- **No universal software MMU: confirmed, and the hybrid shows where one belongs.** Lifted programs in a real kernel
  need its MMU (fork, mmap, copy on write); PISCO's TLB filled from `kw_user_addr` is that, and nothing of it
  reaches the other runtimes. The principle "a feature for one runtime costs the others nothing" held here: every
  change of this page is in `tools/tabc-pisco`.
- **Differential testing (P0): the first tier is a ready oracle.** The flags bug of tabc-pisco.md (one TLS handshake
  in three) took hours to find. The guest now holds qemu-user beside the lifted code: running a program both ways
  and comparing its output, or its registers at each system call, is a few lines in the runtime. That is cheaper
  than KVM and FEX as oracles and runs where the bugs show, in the browser.
- **Register summaries (P0): yes; calls are the gap left.** Lifted code beats kwemu on loops (2.6 to 4.8 times) and
  loses on call-heavy code (fib, 72 ms against 53). That is what register summaries address.
- **ABI thunks (P1): for graphics first.** In Linux the thunk that pays is the GPU: unkernel already forwards GL
  (virgl on WebGL2) and Vulkan (on WebGPU) from the guest's Mesa. A lifted x86-64 Mesa using them is the next step
  for a GPU desktop, before libc or libm thunks, which the lifted code already runs at near wasm speed.
- **What the roadmap misses: the lift is the user's wait.** The time a person sees is lifting (busybox 55 s,
  libcrypto 7.5 min), not running. Three things cut it, in order of effect: (1) a shared cache of lifted modules,
  keyed by the image's sha256, so that Alpine's libcrypto is lifted once for everyone (the canonical library images
  of section 3 make the key the same in every program); (2) the first tier (section 2), so that nobody waits; (3)
  a faster lift for the first run (`-O1`, or a split that keeps every compiler busy: one part of libcrypto took
  8 minutes alone).
- **Defer KVM, FEX and Box64, libkrun, native relinking** for the browser: none runs there. They belong to a native
  host, which [hosts.md](../hosts.md) already weighs.

## Limits

- **The first run of a large x86-64 install is slow either way.** On qemu-user under kwemu a heavy program crawls
  (`update-mime-database` 10 min); lifted, it waits for the host: the desktop and what apk's triggers run came to
  about 150 images, 21 MB of x86-64 code, lifted one at a time (libstdc++ alone: 121 MB of C, half an hour on 4
  cores). The first tier only trades one wait for the other; a shared cache of lifted modules is what removes it.
- **The first tier and its queue are a proof of concept.** Nothing chooses between waiting and qemu-user by the
  program's size; a lift a task waits for and the runner's lift of the same program wait on one lock file, so a
  stopped runner stops that task (it happened while the runner was paused for a benchmark); the queue is lifted
  oldest first, with no notion of what the desktop needs first.
- **Lifted Xorg's two bugs** (above): xterm's fonts, and one exit.
- A program loaded with `dlopen` after it has done work waits for its lift (asleep, the others run): lifted Xorg
  waited about a minute for its five modules the first time.
- PISCO needs the one-CPU kernel; the desktop runs on one CPU.
- The display is the framebuffer: no GPU. unkernel also has virtio-gpu (virgl on WebGL2) and a Vulkan driver on
  WebGPU, not tried here.

## Reproduce

```sh
tools/tabc-pisco/setup.sh   # tabc with the patch, the kernel, PISCO's runtime, qemu-x86_64
O=out/tabc-pisco G=$PWD/docs/research/linux-gui
run() {   # run <image arch> <command...>: the desktop in a headless Chrome, its DevTools on port 9333
  env TABC_DISPLAY=1024x768 TABC_CHROMIUM_FLAGS="--remote-debugging-port=9333 --window-size=1100,1000" \
    TABC_CHROMIUM=$(command -v google-chrome) TABC_SHARE=$O/share TABC_LOG_STDERR=1 \
    TABC_PISCO_LIFT=$PWD/tools/tabc-pisco/lift-side.sh ${1:+TABC_IMAGE_ARCH=$1} \
    $O/tabc run -m 2048m -v $G:/opt/gui alpine:3.24.4 "${@:2}"
}
run "" /opt/gui/start.sh                                 # arm64, kwemu: JWM, xterm, xeyes, xclock
run amd64 env WM=twm /opt/gui/start.sh                   # x86-64, PISCO (the first runs on qemu-user, see above)
run "" env WM=twm GUI_BENCH=1 /opt/gui/start.sh          # bench.sh once the desktop is up ("BENCH" lines)
node $G/shot.mjs 9333 shot.png                           # a screenshot of the tab, from another terminal
```

A package cache mounted at `/opt/apkcache` (`-v <dir>:/opt/apkcache`) makes the second install local. The lifted
modules are kept in `$XDG_CACHE_HOME/tabc/pisco/lifted` by the sha256 of each image and `lift-side.sh --tools`;
the background queue (`*.queued`, the images beside them) is in the same directory.
