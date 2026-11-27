#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/run.sh [step...]: the one entry point. Builds the test titles from their sources, then runs the checks.
#   build    pisco's unit tests; hello, threads, bench (cargo) and cxx, dll (llvm-mingw), where tests/regress.sh looks for them;
#            the 32-bit titles: hello again for i686, and fixtures/x87
#   layout   tests/layout.sh: the runtime's files are listed only by the bases, and every one is in some base
#   node     tests/regress.sh: every title in Node (XCL's title and Stockfish too when their files are there)
#   bench    the benchmark title's checksums against the same work built natively
#   win32    the 32-bit titles in Node: hello, and the x87 and crt32 titles against what they printed under Wine
#            (fixtures/*/expected.txt; tools/oracle-wine/oracle.sh run makes them again)
#   linux    fixtures/linux-hello, a Linux x86-64 program statically linked with musl (tools/setup-toolchain.sh musl;
#            skipped without it): what it prints of its C library must be what it prints when run here, on Linux,
#            and what it prints of the machine must be the machine rt/linux-x64/ describes (expected.txt). Then
#            fixtures/linux-proc (fork, pipes, execve, a script, wait4, a longjmp across a fork) must print what it
#            prints run here
#   neofetch neofetch (docs/neofetch.md), when tools/build-neofetch.sh has built its root into out/neofetch: its image
#            built by `pisco build`, run by `pisco run` in strict mode on the virtual clock, its output as for a pipe;
#            it must print recipes/neofetch/traces/pipe.txt (NEOFETCH_RECORD=1 records it again), and the same in
#            headless Chrome
#   fastfetch  fastfetch as a Linux title (docs/fastfetch.md), when tools/build-fastfetch.sh has built it into
#            out/fastfetch: lifted, run in Node in strict mode on the virtual clock with its logo left out and its
#            output as for a pipe; what it prints must be recipes/fastfetch/traces/pipe.txt (FASTFETCH_RECORD=1
#            records it again). Then its image, built by `pisco build` and run by `pisco run`, must print the same
#   avx      tests/avx.sh: fixtures/avx (AVX, AVX2, FMA and BMI, built for Zen 2 with musl) lifted, its results against
#            what it printed on the CPU (fixtures/avx/expected.txt); skipped without musl
#   cowsay   cowsay on perl (docs/cowsay.md), when tools/build-cowsay.sh has built its root into out/cowsay: its image
#            built by `pisco build`, run by `pisco run` in strict mode with several cows and options; each must print
#            what the same root's perl prints here (the oracle), and the first the same in headless Chrome
#   microvm  fixtures/microvm-hello, a small x86-64 kernel of our own (built with the machine's gcc), lifted as a
#            machine's image and run on rt/microvm-x64 in Node: paging, a page fault returned from and one fixed up,
#            int3, ud2, the APIC's timer while halted, two tasks on their own stacks, a ring-3 task with system calls
#            preempted by the timer. What it prints on the serial port must be fixtures/microvm-hello/expected.txt,
#            which is what QEMU prints booting it (checked again when QEMU is there: tools/setup-toolchain.sh qemu)
#   kernel   the Linux title's kernel (docs/linux.md), when tools/build-linux.sh has built it into out/linux: the
#            lifter must read vmlinux as a machine's image (--machine) and say what it does not lift yet. With QEMU
#            (tools/oracle-qemu; said in one line without it) the kernel must run /init to its end there, and its
#            programs must print recipes/linux/traces/programs.txt (KERNEL_RECORD=1 records it again), on QEMU's
#            machine and on one without HPET, PIT and PIC, which is what rt/microvm-x64 will have. Then the kernel
#            lifted with the initramfs's busybox (tools/build-title.sh: minutes the first time) and booted on
#            rt/microvm-x64 in Node: its console must be the same twice, its boot up to "Run /init as init process"
#            recipes/linux/traces/boot.txt (the compiler's name and the kernel's sizes left out), and its programs
#            what they print on QEMU (programs.txt); KERNEL_RECORD=1 records the traces again
#   changelog  tools/changelog.mjs: every entry in changelog/ (the changelog on GitHub Pages) is well formed
#   chrome   hello, threads and cxx again in headless Chrome (WebGPU on SwiftShader), the 32-bit titles and linux-hello
#   nd       Narbacular Drop, when the installed game is there (ND: its folder; default: the oracle's prefix):
#            built with its recipe and run in strict mode. Its calls until it asks for Direct3D 9 must be the
#            ones it makes under Wine (recipes/narbacular-drop/traces/start.txt). Then the menu-to-level session
#            on the virtual clock with the null Direct3D 9 device: it must end with exit code 0 after every
#            checkpoint, each at the frame and with the Direct3D calls recorded in traces/menu-to-level.txt,
#            and a second run must give the same log (ND_RECORD=1 records the checkpoints again). Its draws go
#            through the fixed-function shaders to the null GPU back end, which must find every record consistent;
#            the shaders it made must pass naga
#   ffp      tests/gfx/d3d9-ffp.sh: Direct3D 9's fixed-function pipeline as WGSL (rt/windows-x86/d3d9_ffp.c), built
#            natively: the shaders of a representative set of state keys, validated with naga
#   sm       tests/gfx/d3d9-sm.sh: Direct3D 9's shader translator (rt/windows-x86/d3d9_sm.c), built natively: shaders
#            assembled to cover shader models 1.1 to 2.x, each one's WGSL validated with naga (SM_CAPTURED: a folder of
#            a title's captured bytecode, translated too)
#   d3dx     fixtures/d3dx: Microsoft's D3DX (the user's own d3dx9_35.dll: D3DX_DLL, default ~/untrusted/tag/cab/) lifted
#            with a 32-bit program that preprocesses and compiles HLSL through it; what it prints must be what it prints
#            under Wine with the same DLL (fixtures/d3dx/expected.txt). Skipped without the DLL
#   gdi      rt/windows-x86/font.c (gdi32's fonts) built natively by tests/gdi/fonts.c, which asks it what
#            tools/oracle-wine/gdiprobe.c asks Wine's GDI; with the probe's answers (out/oracle/narbacular-drop/
#            gdiprobe.txt) every measurement D3DX and the game read must be Wine's (tools/oracle-wine/gdi-compare.py),
#            and the glyph pixels' differences are printed
#   ndgfx    tests/gfx/nd.sh: Narbacular Drop's session in headless Chrome, drawn (WebGPU on SwiftShader): a PNG at
#            every checkpoint, and the still screens close to the oracle's screenshots when those are there (minutes)
#   tag      tests/gfx/tag.sh: Tag: The Power of Paint from the user's installer (TAG_SETUP), its session in headless
#            Chrome in strict mode: a PNG at every checkpoint, the menu close to the oracle's screenshot (an hour)
#   tagplay  tests/gfx/tagplay.sh: Tag played (T5) in Node: level2-paint in strict mode, its FMOD Ex calls against the
#            oracle's +snoop log, TAG.ini kept in the saves and read back (needs out/img-tag: the tag step)
#   ndplay   tests/gfx/ndplay.sh: Narbacular Drop played (M5): the level1-portals session in Node (both portals, out of
#            the cage; its sound not silent and the same on a second run; its FMOD calls against Wine's +snoop trace
#            when the oracle's is there), system.opt written by Quit and read again by a second run (SW_SAVES), then
#            the same in Chrome: the session's screenshots, and a reload that finds system.opt in OPFS (minutes)
#   images   tests/images.sh: title images (M6): `pisco build` of hello32 and, from the user's installer (ND_SETUP; skipped
#            without it), Narbacular Drop, each twice with empty caches (the same manifest digest), their shared base
#            layer, a registry-only rebuild that lifts, compiles and links nothing, then `pisco run` of the menu-to-level
#            session in Node with the nd step's checkpoints, and the image's registry read by advapi32 (minutes). The
#            nd, ndgfx and ndplay steps keep building from the installed folder with tools/build-title.sh
#   doom     tests/gfx/doom.sh: Doom as a console title in Node and Chrome (minutes)
#   st       tests/gfx/st.sh: Microsoft's SimpleTriangleDesktop (Windows x64) in Chrome, and fixtures/msvc/eh.exe with
#            the real vcruntime140.dll in Node; skipped without the user-built sample (out/gdk-samples/local, from
#            tools/build-gdk-sample.sh) or the MSVC runtime DLLs (REDIST)
#   pbr      tests/gfx/pbr.sh: Microsoft's SimplePBR (Windows x64) in Chrome; skipped without the sample (built from
#            the SDKs in WINSDK by tools/build-gdk-sample-pbr.sh) or REDIST (minutes)
#   desireplay  tests/gfx/desireplay.sh: Desire (Windows x64, Direct3D 12) from the user's zip (DESIRE_ZIP) and REDIST, in
#            Node, strict: its calls until its main loop are Wine's, and its maze from Wine's seeds is Wine's (minutes)
#   mohaa    tests/gfx/mohaa.sh: Medal of Honor: Allied Assault's demo (Windows x86, OpenGL) from the user's download
#            (MOHAA_ZIP), in Node, strict, on the virtual clock: the logos to the menu, then into the level; its console
#            log and its sound against Wine's
#   mohaagfx tests/gfx/mohaa-chrome.sh: the menu and level sessions in headless Chrome, the still screens against the
#            oracle's screenshots (needs out/img-mohaa: the mohaa step; about an hour)
#   glffp    tests/gfx/gl-ffp.sh: OpenGL's fixed-function shader generator, every key validated by naga
#   desire   tests/gfx/desire.sh: Desire's sessions in headless Chrome, strict: a PNG at every checkpoint, the still
#            screens close to the oracle's screenshots (needs out/img-desire: the desireplay step; half an hour)
#   xcl      tests/gfx/xcl.sh: XCL's console test title in Chrome, when it has been built into out/xcltitle-game
#   audio    tests/audio/run.sh: the XMA2 / Opus decoders (builds FFmpeg's the first time)
# Default: build layout node bench win32 linux microvm avx ffp glffp sm gdi changelog. Needs tools/setup-toolchain.sh to have run. Exit status 1 if a step fails.
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"; mkdir -p out
MINGW=${MINGW:-$HOME/tools/llvm-mingw-20261117-ucrt-ubuntu-22.04-x86_64}; CXX=$MINGW/bin/x86_64-w64-mingw32-clang++
R=target/x86_64-pc-windows-msvc/release
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
fail=0
step() { # name, command...: one line, the log's tail on failure
  local name=$1 log=out/test-$1.log; shift
  if "$@" > "$log" 2>&1; then echo "ok    $name: $(grep -v '^ *$' "$log" | tail -${LINES_SHOWN:-1} | tr '\n' ';' | sed 's/;$//; s/;/; /g')"
  else echo "FAIL  $name (see $log)"; tail -6 "$log" | sed 's/^/        /'; fail=1; fi
}
build() {
  cargo build -q --release -p pisco-lifter -p pisco-shaders -p pisco || return 1
  cargo test -q --release -p pisco > out/test-pisco-unit.log 2>&1 || { tail -20 out/test-pisco-unit.log; return 1; }   # pisco's own (the zip reader)
  for t in hello threads bench bench-native; do (cd "fixtures/$t" && cargo build -q --release) || return 1; done
  # one C++ runtime inside each image (-static): nothing but the title's own DLL is shipped beside it
  "$CXX" -O2 -static fixtures/cxx/eh.cpp -o fixtures/cxx/eh.exe || return 1
  mkdir -p fixtures/dll/game
  "$CXX" -O2 -static -shared fixtures/dll/lib.cpp -o fixtures/dll/game/gamelib.dll -Wl,--out-implib,fixtures/dll/game/libgamelib.a || return 1
  "$CXX" -O2 -static fixtures/dll/main.cpp -o fixtures/dll/game/title.exe -Lfixtures/dll/game -lgamelib || return 1
  (cd fixtures/hello && cargo build -q --release --target i686-pc-windows-msvc) || return 1
  # the exe and its DLL again as 32-bit programs (several 32-bit images lifted together)
  local CXX32=$MINGW/bin/i686-w64-mingw32-clang++; mkdir -p fixtures/dll/game32
  "$CXX32" -O2 -static -shared fixtures/dll/lib.cpp -o fixtures/dll/game32/gamelib.dll -Wl,--out-implib,fixtures/dll/game32/libgamelib.a || return 1
  "$CXX32" -O2 -static fixtures/dll/main.cpp -o fixtures/dll/game32/title.exe -Lfixtures/dll/game32 -lgamelib || return 1
  fixtures/x87/build.sh out/x87 > /dev/null || return 1
  fixtures/crt32/build.sh out/crt32 > /dev/null || return 1
  fixtures/mmx/build.sh out/mmx > /dev/null || return 1
  # the MSVC-ABI exceptions title, when clang-cl is there (tests/gfx/st.sh runs it)
  local msvc=""; if command -v clang-cl > /dev/null && command -v lld-link > /dev/null; then fixtures/msvc/build.sh || return 1; msvc=", msvc's eh"; fi
  echo "built hello threads bench cxx dll$msvc, and hello, dll, x87 and crt32 as 32-bit programs"
}
win32() {
  local ok=0 exe=fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe out pass
  tools/build-title.sh "$exe" out/hello32 > out/hello32.build.log 2>&1 || { echo "hello32: build failed (out/hello32.build.log)"; return 1; }
  out=$(SW_STRICT=1 node web/run-node.mjs out/hello32/title.wasm "$exe" 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
  echo "hello32: $pass passed, $(grep -c '^\[FAIL\]' <<<"$out") failed"; [ "$pass" = 7 ] || { tail -3 <<<"$out"; ok=1; }
  tools/build-title.sh out/x87/x87.exe out/x87/wasm > out/x87.build.log 2>&1 || { echo "x87: build failed (out/x87.build.log)"; return 1; }
  SW_STRICT=1 node web/run-node.mjs out/x87/wasm/title.wasm out/x87/x87.exe > out/x87/got.txt 2>&1
  if cmp -s out/x87/got.txt fixtures/x87/expected.txt; then echo "x87: $(wc -l < fixtures/x87/expected.txt) lines equal what it prints under Wine"
  else echo "x87: differs from what it prints under Wine (< ours, > Wine):"; diff out/x87/got.txt fixtures/x87/expected.txt | head -12; ok=1; fi
  # the 32-bit exe and its DLL: all but the last check (a C++ exception thrown in the DLL: mingw's DWARF unwinding
  # for 32-bit code is not dispatched yet; MSVC's, which Windows games use, is T1's)
  tools/build-title.sh fixtures/dll/game32/title.exe out/dll32 > out/dll32.build.log 2>&1 || { echo "dll32: build failed (out/dll32.build.log)"; return 1; }
  out=$(SW_STRICT=1 timeout 120 node web/run-node.mjs out/dll32/title.wasm fixtures/dll/game32/title.exe 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
  echo "dll32: $pass passed, the exception across the DLL not yet (32-bit DWARF unwinding)"; [ "$pass" = 6 ] || { tail -3 <<<"$out"; ok=1; }
  tools/build-title.sh out/mmx/mmx.exe out/mmx/wasm > out/mmx.build.log 2>&1 || { echo "mmx: build failed (out/mmx.build.log)"; return 1; }
  SW_STRICT=1 node web/run-node.mjs out/mmx/wasm/title.wasm out/mmx/mmx.exe > out/mmx/got.txt 2>&1
  if cmp -s out/mmx/got.txt fixtures/mmx/expected.txt; then echo "mmx: $(wc -l < fixtures/mmx/expected.txt) lines (MMX, and SSE on MMX registers) equal what it prints under Wine"
  else echo "mmx: differs from what it prints under Wine (< ours, > Wine):"; diff out/mmx/got.txt fixtures/mmx/expected.txt | head -12; ok=1; fi
  tools/build-title.sh out/crt32/crt32.exe out/crt32/wasm > out/crt32.build.log 2>&1 || { echo "crt32: build failed (out/crt32.build.log)"; return 1; }
  SW_STRICT=1 node web/run-node.mjs out/crt32/wasm/title.wasm out/crt32/crt32.exe > out/crt32/got.txt 2>&1
  if cmp -s out/crt32/got.txt fixtures/crt32/expected.txt; then echo "crt32: $(wc -l < fixtures/crt32/expected.txt) lines (msvcrt.dll maths and printf, SList) equal what it prints under Wine"
  else echo "crt32: differs from what it prints under Wine (< ours, > Wine):"; diff out/crt32/got.txt fixtures/crt32/expected.txt | head -12; ok=1; fi
  return $ok
}
linux() {
  local d=out/linux-hello
  MUSL=$MUSL fixtures/linux-hello/build.sh $d > /dev/null || { echo "linux-hello: does not compile"; return 1; }
  tools/build-title.sh $d/hello $d/wasm > out/linux-hello.build.log 2>&1 || { echo "linux-hello: build failed (out/linux-hello.build.log)"; return 1; }
  # the C library: the program run here is the oracle
  $d/hello --pure one > $d/native.txt 2>&1; local want=$?
  SW_STRICT=1 node web/run-node.mjs $d/wasm/title.wasm $d/hello --pure one > $d/pure.txt 2>&1; local got=$?
  cmp -s $d/pure.txt $d/native.txt && [ $got = $want ] || { echo "linux-hello --pure differs from its run on this machine (< ours, > native; exit $got, $want):"; diff $d/pure.txt $d/native.txt | head -12; return 1; }
  # the machine: on the virtual clock, so the date it prints is the clock's first day
  SW_STRICT=1 SW_CLOCK=16667 node web/run-node.mjs $d/wasm/title.wasm $d/hello > $d/got.txt 2>&1
  cmp -s $d/got.txt fixtures/linux-hello/expected.txt || { echo "linux-hello differs from expected.txt (< ours, > expected):"; diff $d/got.txt fixtures/linux-hello/expected.txt | head -12; return 1; }
  echo "linux-hello: $(wc -l < $d/native.txt) lines equal its run on this machine, $(wc -l < fixtures/linux-hello/expected.txt) lines with the machine the runtime describes"
  # processes: the fixture's root is the file system, its program's module beside it (SW_ROOT, web/run-node.mjs)
  local p=out/linux-proc
  MUSL=$MUSL fixtures/linux-proc/build.sh $p/root > /dev/null || { echo "linux-proc: does not compile"; return 1; }
  tools/build-title.sh $p/root/bin/proc $p/wasm > out/linux-proc.build.log 2>&1 || { echo "linux-proc: build failed (out/linux-proc.build.log)"; return 1; }
  cp $p/wasm/title.wasm $p/root/bin/proc.wasm
  # natively: the same tree under its own path (the script's #! names /bin/proc there)
  rm -rf /tmp/pisco-linux-proc && mkdir -p /tmp/pisco-linux-proc/bin && cp $p/root/bin/proc /tmp/pisco-linux-proc/bin/ && printf '#!/tmp/pisco-linux-proc/bin/proc --script\n' > /tmp/pisco-linux-proc/bin/script && chmod +x /tmp/pisco-linux-proc/bin/script
  /tmp/pisco-linux-proc/bin/proc /tmp/pisco-linux-proc/bin/script > $p/native.txt 2>&1
  SW_STRICT=1 SW_ROOT=$p/root SW_EXE=/bin/proc timeout 120 node web/run-node.mjs $p/root/bin/proc.wasm $p/root/bin/proc /bin/script > $p/got.txt 2>&1
  cmp -s $p/got.txt $p/native.txt || { echo "linux-proc differs from its run on this machine (< ours, > native):"; diff $p/got.txt $p/native.txt | head -12; return 1; }
  echo "linux-proc: $(wc -l < $p/native.txt) lines (fork, pipes, execve, a script, wait4, longjmp) equal its run on this machine"
}
microvm() {
  local d=out/microvm-hello want=fixtures/microvm-hello/expected.txt
  fixtures/microvm-hello/build.sh $d > /dev/null || { echo "microvm-hello: does not build"; return 1; }
  tools/build-title.sh $d/hello $d/wasm > out/microvm-hello.build.log 2>&1 || { echo "microvm-hello: build failed (out/microvm-hello.build.log)"; return 1; }
  timeout 120 node web/run-node.mjs $d/wasm/title.wasm $d/hello 2>&1 | tr -d '\r' > $d/got.txt
  cmp -s $d/got.txt $want || { echo "microvm-hello differs from expected.txt (< ours, > expected):"; diff $d/got.txt $want | head -12; return 1; }
  local q="lifted, in Node"
  if command -v "${QEMU:-qemu-system-x86_64}" > /dev/null; then
    . tools/oracle-qemu/env.sh
    timeout 120 "$QEMU" -M "$ORACLE_QEMU_MACHINE,accel=tcg" -cpu "$ORACLE_QEMU_CPU" -m 256 -smp 1 -display none -no-reboot -nodefaults -no-user-config -serial stdio \
      -device isa-debug-exit,iobase=0xf4,iosize=0x04 -kernel $d/hello < /dev/null 2> $d/qemu.log | tr -d '\r' > $d/qemu.txt
    cmp -s $d/qemu.txt $want || { echo "microvm-hello under QEMU differs from expected.txt (< QEMU, > expected):"; diff $d/qemu.txt $want | head -12; return 1; }
    q="$q and under QEMU"
  fi
  echo "microvm-hello: $(wc -l < $want) lines equal expected.txt, $q"
}
kernel() {
  local d=out/linux want=recipes/linux/traces/programs.txt fns left
  mkdir -p $d/census
  target/release/lifter $d/vmlinux $d/census/title.c --split 16 --machine > $d/census.txt 2>&1 || { echo "the lifter does not read vmlinux:"; tail -3 $d/census.txt; return 1; }
  fns=$(sed -n 's/^lifted \([0-9]*\) functions.*/\1/p' $d/census.txt); left=$(sed -n 's/^not lifted yet, in the other functions: \([0-9]*\) instructions.*/\1/p' $d/census.txt)
  [ "${fns:-0}" -gt 5000 ] || { echo "the lifter found only ${fns:-0} functions in vmlinux ($d/census.txt)"; return 1; }
  echo "vmlinux: $fns functions read as a machine's image, $left instructions not lifted yet ($d/census.txt)"
  command -v "${QEMU:-qemu-system-x86_64}" > /dev/null || { echo "no QEMU: the kernel's boot on the oracle is not checked (tools/setup-toolchain.sh qemu)"; return 0; }
  tools/oracle-qemu/oracle.sh run linux > $d/oracle.txt 2>&1 || { echo "on QEMU the kernel did not run /init to its end ($d/oracle.txt):"; tail -4 $d/oracle.txt; return 1; }
  [ -z "${KERNEL_RECORD:-}" ] || tools/oracle-qemu/oracle.sh programs linux > $want
  tools/oracle-qemu/oracle.sh programs linux > $d/programs.txt
  cmp -s $d/programs.txt $want || { echo "on QEMU the programs print otherwise than the recorded trace (< now, > recorded):"; diff $d/programs.txt $want | head -8; return 1; }
  # the machine rt/microvm-x64 will be: no HPET, no PIT, no 8259 (QEMU's emulated CPU has no TSC-deadline timer, so the
  # kernel is told the TSC's rate and runs without a timer interrupt there)
  ORACLE_RUN=$d/oracle-small ORACLE_QEMU_MACHINE=q35,hpet=off,pit=off,pic=off tools/oracle-qemu/oracle.sh run linux tsc_early_khz=2900000 > $d/oracle-small.txt 2>&1 \
    || { echo "on QEMU without HPET, PIT and PIC the kernel did not run /init to its end ($d/oracle-small.txt):"; tail -4 $d/oracle-small.txt; return 1; }
  ORACLE_RUN=$d/oracle-small tools/oracle-qemu/oracle.sh programs linux | cmp -s - $want || { echo "without HPET, PIT and PIC the programs print otherwise than the recorded trace"; return 1; }
  echo "QEMU: /init ran to its end, $(wc -l < $want) lines from its programs equal the recorded trace, also without HPET, PIT and PIC"
  # lifted, on the machine: the boot, up to the first program
  PROGRAMS=$d/build/busybox-1.36.1/busybox_unstripped SPLIT=16 tools/build-title.sh $d/vmlinux $d/wasm > $d/wasm.log 2>&1 || { echo "the lifted kernel does not build ($d/wasm.log)"; tail -3 $d/wasm.log; return 1; }
  local boot=recipes/linux/traces/boot.txt k
  for k in 1 2; do (cd $d && SW_INITRD=initramfs.cpio SW_CMDLINE="$(cat ../../recipes/linux/cmdline)" timeout 600 node ../../web/run-node.mjs wasm/title.wasm vmlinux 2>&1) | tr -d '\r' > $d/boot$k.txt; done
  grep -q '^Run /init as init process' $d/boot1.txt || { echo "the lifted kernel did not reach /init ($d/boot1.txt):"; tail -4 $d/boot1.txt; return 1; }
  cmp -s $d/boot1.txt $d/boot2.txt || { echo "two boots of the lifted kernel differ ($d/boot1.txt, $d/boot2.txt)"; return 1; }
  # the boot up to /init, without the machine's own notes and what depends on the compiler that built the kernel
  norm() { sed -n '1,/^Run \/init as init process/p' "$1" | grep -v '^pisco: ' | sed -E 's/^(Linux version [^ ]+ \(pisco@pisco\)).*/\1/; s/^Memory: .*/Memory: (sizes)/'; }
  [ -z "${KERNEL_RECORD:-}" ] || norm $d/boot1.txt > $boot
  norm $d/boot1.txt | cmp -s - $boot || { echo "the lifted kernel's boot differs from $boot (< now, > recorded):"; diff <(norm $d/boot1.txt) $boot | head -8; return 1; }
  sed -n '/^Run \/init as init process/,$p' $d/boot1.txt | sed 1d | grep -v '^pisco: ' > $d/lifted-programs.txt
  cmp -s $d/lifted-programs.txt $want || { echo "lifted, the programs print otherwise than on QEMU (< lifted, > QEMU):"; diff $d/lifted-programs.txt $want | head -8; return 1; }
  echo "lifted: $(wc -l < $boot) lines of boot to /init, twice the same, equal to $boot; then /init's programs print what they print on QEMU"
}
neofetch() {
  local d=out/neofetch want=recipes/neofetch/traces/pipe.txt
  target/release/pisco build recipes/neofetch --out out/images/neofetch > out/neofetch.image.log 2>&1 || { echo "pisco build failed (out/neofetch.image.log)"; return 1; }
  SW_STRICT=1 SW_CLOCK=16667 timeout 300 target/release/pisco run out/images/neofetch > $d/got.txt 2>&1 || { echo "it ended with an error:"; tail -3 $d/got.txt; return 1; }
  [ -z "${NEOFETCH_RECORD:-}" ] || cp $d/got.txt $want
  cmp -s $d/got.txt $want || { echo "what it prints is not $want (< this run, > recorded):"; diff $d/got.txt $want | head -12; return 1; }
  # the system calls its processes make, for the progress map (tools/progress.mjs --refresh)
  SW_TRACE=1 SW_CLOCK=16667 timeout 300 target/release/pisco run out/images/neofetch > $d/trace.log 2>&1
  local forks execs; forks=$(grep -c '^pisco: fork: the child' $d/trace.log); execs=$(grep -c '^pisco: execve /' $d/trace.log)
  sed -n 's/^pisco: \([a-z_0-9]*\)(.*/\1/p' $d/trace.log | LC_ALL=C sort -u > $d/syscalls.txt
  SW_STRICT=1 SW_CLOCK=16667 SW_TTY=0 TIMEOUT=300 node tools/run-chrome.mjs out/images/neofetch 2>&1 | grep -v -e '^gpu: ' -e '^image: ' > $d/got-chrome.txt
  local trim="awk 1 | sed -e :a -e '/^\\n*\$/{\$d;N;ba' -e '}'"   # (the page's console has no last newline, nor the blank lines at the end)
  diff <(eval "$trim" < $want) <(eval "$trim" < $d/got-chrome.txt) > $d/chrome.diff || { echo "in Chrome it prints something else (< recorded, > Chrome):"; head -8 $d/chrome.diff; return 1; }
  echo "neofetch: $(wc -l < $want) lines as recorded, in strict mode, in Node and in Chrome; $forks processes forked, $execs programs executed"
}
cowsay() {
  local d=out/cowsay R=$PWD/out/cowsay/root n=0
  target/release/pisco build recipes/cowsay --out out/images/cowsay > out/cowsay.image.log 2>&1 || { echo "pisco build failed (out/cowsay.image.log)"; return 1; }
  # natively: the root's own perl, its library and its cows where they are here
  native() { local prog=$1; shift; PERL5LIB=$R/usr/lib/perl5/5.44.0/x86_64-linux:$R/usr/lib/perl5/5.44.0 COWPATH=$R/usr/share/cowsay/cows COWSAY_ONLY_COWPATH=1 $R/usr/bin/perl $R/usr/bin/$prog "$@"; }
  while IFS='|' read -r prog args; do
    n=$((n + 1)); eval "set -- $args"
    native "$prog" "$@" > $d/native-$n.txt 2>&1
    if [ "$prog" = cowsay ]; then SW_STRICT=1 timeout 300 target/release/pisco run out/images/cowsay -- "${@}" > $d/got-$n.txt 2>&1
    else SW_STRICT=1 SW_EXE=/usr/bin/$prog timeout 300 target/release/pisco run out/images/cowsay -- "${@}" > $d/got-$n.txt 2>&1; fi
    cmp -s $d/got-$n.txt $d/native-$n.txt || { echo "$prog $args: not what perl prints here (< ours, > native):"; diff $d/got-$n.txt $d/native-$n.txt | head -10; return 1; }
  done <<'RUNS'
cowsay|"Hello from PISCO"
cowsay|-f tux "a penguin, lifted"
cowsay|-e "^^" -T " U" -W 20 "a cow with other eyes and a tongue, its words wrapped at twenty columns"
cowsay|-f dragon -b "moo"
cowthink|"think different"
cowsay|-l
RUNS
  SW_STRICT=1 TIMEOUT=300 node tools/run-chrome.mjs out/images/cowsay 2>&1 | grep -v -e '^gpu: ' -e '^image: ' > $d/got-chrome.txt
  local trim="awk 1 | sed -e :a -e '/^\\n*\$/{\$d;N;ba' -e '}'"
  diff <(eval "$trim" < $d/native-1.txt) <(eval "$trim" < $d/got-chrome.txt) > $d/chrome.diff || { echo "in Chrome it prints something else (< native, > Chrome):"; head -8 $d/chrome.diff; return 1; }
  echo "cowsay: $n runs (cows, eyes, wrapping, cowthink, the list of cows) print what perl prints here; the first in Chrome too"
}
fastfetch() {
  local d=out/fastfetch want=recipes/fastfetch/traces/pipe.txt
  SPLIT=16 tools/build-title.sh $d/fastfetch $d/wasm > out/fastfetch.build.log 2>&1 || { echo "build failed (out/fastfetch.build.log)"; return 1; }
  SW_STRICT=1 SW_CLOCK=16667 timeout 300 node web/run-node.mjs $d/wasm/title.wasm $d/fastfetch --pipe -l none > $d/got.txt 2>&1 || { echo "it ended with an error:"; tail -3 $d/got.txt; return 1; }
  [ -z "${FASTFETCH_RECORD:-}" ] || cp $d/got.txt $want
  # the system calls it makes, for the progress map (tools/progress.mjs --refresh)
  SW_TRACE=1 SW_CLOCK=16667 timeout 300 node web/run-node.mjs $d/wasm/title.wasm $d/fastfetch --pipe -l none 2>&1 | sed -n 's/^pisco: \([a-z_0-9]*\)(.*/\1/p' | LC_ALL=C sort -u > $d/syscalls.txt
  cmp -s $d/got.txt $want || { echo "what it prints is not $want (< this run, > recorded):"; diff $d/got.txt $want | head -12; return 1; }
  target/release/pisco build recipes/fastfetch --out out/images/fastfetch > out/fastfetch.image.log 2>&1 || { echo "pisco build failed (out/fastfetch.image.log)"; return 1; }
  SW_STRICT=1 SW_CLOCK=16667 timeout 300 target/release/pisco run out/images/fastfetch -- --pipe -l none > $d/got-image.txt 2>&1
  cmp -s $d/got-image.txt $want || { echo "its image prints something else (< the image, > recorded):"; diff $d/got-image.txt $want | head -12; return 1; }
  echo "fastfetch: $(wc -l < $want) lines as recorded, in strict mode, from the lifted program and from its image"
}
bench() {
  tools/build-title.sh fixtures/bench/$R/bench.exe out/bench > out/bench.build.log 2>&1 || { echo "build failed (out/bench.build.log)"; return 1; }
  local want got; want=$(fixtures/bench-native/target/release/bench-native | grep -o 'checksum [0-9a-f]*')
  got=$(node web/run-node.mjs out/bench/title.wasm fixtures/bench/$R/bench.exe 2>&1 | grep -o 'checksum [0-9a-f]*')
  [ -n "$want" ] && [ "$want" = "$got" ] && echo "$(wc -l <<<"$want") checksums equal the native build's" || { echo "checksums differ from the native build's"; return 1; }
}
chrome() {
  local n exe out pass bad ok=0
  for n in hello threads cxx; do
    case $n in cxx) exe=fixtures/cxx/eh.exe ;; *) exe=fixtures/$n/$R/$n.exe ;; esac
    [ -f "out/regress-$n/title.wasm" ] || tools/build-title.sh "$exe" "out/regress-$n" > "out/regress-$n.build.log" 2>&1 || { echo "$n: build failed"; ok=1; continue; }
    out=$(TIMEOUT=300 node tools/run-chrome.mjs "out/regress-$n/title.wasm" "$exe" 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out"); bad=$(grep -c '^\[FAIL\]' <<<"$out")
    echo "$n: $pass passed, $bad failed"; { [ "$pass" -gt 0 ] && [ "$bad" = 0 ]; } || ok=1
  done
  # the 32-bit titles, when the win32 step has built them
  if [ -f out/hello32/title.wasm ]; then
    out=$(TIMEOUT=300 node tools/run-chrome.mjs out/hello32/title.wasm fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe 2>&1); pass=$(grep -c '^\[PASS\]' <<<"$out")
    echo "hello32: $pass passed, $(grep -c '^\[FAIL\]' <<<"$out") failed"; [ "$pass" = 7 ] || ok=1
  fi
  # the Linux title, when the linux step has built it: the same text as in Node
  if [ -f out/linux-hello/wasm/title.wasm ]; then
    SW_STRICT=1 SW_CLOCK=16667 TIMEOUT=300 node tools/run-chrome.mjs out/linux-hello/wasm/title.wasm out/linux-hello/hello 2>&1 | grep -v -e '^$' -e '^gpu: ' > out/linux-hello/got-chrome.txt
    if cmp -s out/linux-hello/got-chrome.txt fixtures/linux-hello/expected.txt; then echo "linux-hello: equal to expected.txt"; else echo "linux-hello: differs from expected.txt"; ok=1; fi
  fi
  # the machine, when the microvm step has built it: each of its tasks on a stack of its own in the page too
  if [ -f out/microvm-hello/wasm/title.wasm ]; then
    TIMEOUT=300 node tools/run-chrome.mjs out/microvm-hello/wasm/title.wasm out/microvm-hello/hello 2>&1 | tr -d '\r' | grep -v -e '^$' -e '^gpu: ' > out/microvm-hello/got-chrome.txt
    if cmp -s out/microvm-hello/got-chrome.txt fixtures/microvm-hello/expected.txt; then echo "microvm-hello: equal to what QEMU prints"; else echo "microvm-hello: differs from what QEMU prints"; ok=1; fi
  fi
  if [ -f out/x87/wasm/title.wasm ]; then
    TIMEOUT=300 node tools/run-chrome.mjs out/x87/wasm/title.wasm out/x87/x87.exe 2>&1 | grep -v -e '^$' -e '^gpu: ' > out/x87/got-chrome.txt   # the page's own first line names the GPU
    if cmp -s out/x87/got-chrome.txt fixtures/x87/expected.txt; then echo "x87: equal to what it prints under Wine"; else echo "x87: differs from what it prints under Wine"; ok=1; fi
  fi
  return $ok
}
nd() {
  RECIPE=recipes/narbacular-drop/title.toml SPLIT=16 OPT=-O1 tools/build-title.sh "$ND/Narbacular Drop.exe" out/nd > out/nd.build.log 2>&1 || { echo "build failed (out/nd.build.log)"; return 1; }
  # start-up, every call traced: a session of one empty step ends the run when the first frame has been presented
  SW_STRICT=1 SW_TRACE=2 SW_SESSION_STEPS='wait 0' timeout 300 node web/run-node.mjs out/nd/title.wasm "$ND/Narbacular Drop.exe" > out/nd.run.log 2>&1
  grep -q 'Direct3DCreate9' out/nd.run.log || { echo "did not reach Direct3DCreate9:"; tail -3 out/nd.run.log; return 1; }
  python3 -I tools/oracle-wine/trace-diff.py recipes/narbacular-drop/traces/start.txt out/nd.run.log --until Direct3DCreate9 || return 1
  # the session: menu, story, tutorial, the first level
  local want=recipes/narbacular-drop/traces/menu-to-level.txt code n
  nd_session > out/nd.session.log 2>&1; code=$?
  [ $code = 0 ] || { echo "the session ended with exit code $code:"; tail -3 out/nd.session.log | cut -c1-300; return 1; }
  grep -E '^pisco: (checkpoint |session done)' out/nd.session.log | sed 's/^pisco: //' > out/nd.session.checkpoints.txt
  grep -q '^session done' out/nd.session.checkpoints.txt || { echo "the session did not reach its end:"; tail -3 out/nd.session.log | cut -c1-300; return 1; }
  [ -z "${ND_RECORD:-}" ] || { grep '^#' $want > out/nd.session.head.txt; cat out/nd.session.head.txt out/nd.session.checkpoints.txt > $want; }
  diff <(grep -v '^#' $want) out/nd.session.checkpoints.txt > out/nd.session.diff || { echo "the session's checkpoints are not the recorded ones (< $want, > this run):"; head -6 out/nd.session.diff; return 1; }
  nd_session > out/nd.session.2.log 2>&1
  cmp -s out/nd.session.log out/nd.session.2.log || { echo "two runs of the session differ: out/nd.session.log, out/nd.session.2.log"; return 1; }
  # what reached the GPU ring: counted and checked by the null back end; the shaders this run made, by naga
  grep -q '^pisco: gpu (null back end): .* 0 of the records were inconsistent$' out/nd.session.log || { echo "the null GPU back end did not find the session's records consistent:"; grep '^pisco: gpu' out/nd.session.log | tail -3 | cut -c1-300; return 1; }
  local shaders="naga is not installed: its shaders were not validated"
  if command -v naga > /dev/null; then shaders=$(tests/gfx/d3d9-ffp.sh out/nd.session.log 2>&1) || { echo "the session's shaders: $shaders"; return 1; }; shaders="its $(wc -l < out/ffp/run-keys.txt) shaders pass naga"; fi
  n=$(grep -c '^checkpoint ' out/nd.session.checkpoints.txt)
  echo "session: $n checkpoints as recorded, $(sed -n 's/^session done: .*, \([0-9]*\) frames$/\1/p' out/nd.session.checkpoints.txt) frames, two runs give the same log"
  echo "$(sed -n 's/^pisco: gpu (null back end): \(.*\); 0 of the records were inconsistent$/\1/p' out/nd.session.log), all consistent, and $shaders"
}
d3dx() {
  fixtures/d3dx/build.sh out/d3dx "$D3DX_DLL" > /dev/null || return 1
  tools/build-title.sh out/d3dx/d3dx.exe out/d3dx/wasm > out/d3dx.build.log 2>&1 || { echo "build failed (out/d3dx.build.log)"; return 1; }
  SW_STRICT=1 timeout 600 node web/run-node.mjs out/d3dx/wasm/title.wasm out/d3dx/d3dx.exe > out/d3dx/got.txt 2>&1
  if cmp -s out/d3dx/got.txt fixtures/d3dx/expected.txt; then echo "the preprocessed text and 3 bytecodes equal what Microsoft's D3DX makes under Wine"
  else echo "differs from Wine's (< ours, > Wine):"; diff out/d3dx/got.txt fixtures/d3dx/expected.txt | head -8; return 1; fi
}
gdi() {
  mkdir -p out/gdi
  cc -O1 -DFONT_NO_EMBED -Irt/windows-x86 tests/gdi/fonts.c rt/windows-x86/font.c -lm -o out/gdi/fonts || return 1
  out/gdi/fonts rt/windows-x86/fonts > out/gdi/fonts.txt || return 1
  local probe=out/oracle/narbacular-drop/gdiprobe.txt
  [ -f $probe ] || { echo "$(grep -c '^font ' out/gdi/fonts.txt) fonts measured; no answers of Wine's in $probe to compare with (tools/oracle-wine/gdiprobe.c)"; return 0; }
  python3 -I tools/oracle-wine/gdi-compare.py $probe out/gdi/fonts.txt
}
nd_session() { SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 SW_SESSION=recipes/narbacular-drop/sessions/menu-to-level.json timeout 600 node web/run-node.mjs out/nd/title.wasm "$ND/Narbacular Drop.exe"; }
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
D3DX_DLL=${D3DX_DLL:-$HOME/untrusted/tag/cab/d3dx9_35.dll}
REDIST=${REDIST:-$HOME/untrusted/vcredist/x/Contents/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT}; export REDIST
WINSDK=${WINSDK:-$HOME/winsdk}
for s in ${*:-build layout node bench win32 linux microvm avx ffp glffp sm gdi changelog}; do case $s in
  build)  step build build ;;
  layout) step layout tests/layout.sh ;;
  node)   LINES_SHOWN=8 step node tests/regress.sh ;;
  bench)  step bench bench ;;
  win32)  LINES_SHOWN=4 step win32 win32 ;;
  avx)    if [ -x "$MUSL/bin/musl-gcc" ]; then step avx tests/avx.sh; else echo "skip  avx: no musl in MUSL=$MUSL (tools/setup-toolchain.sh musl)"; fi ;;
  linux)  if [ -x "$MUSL/bin/musl-gcc" ]; then LINES_SHOWN=2 step linux linux; else echo "skip  linux: no musl in MUSL=$MUSL (tools/setup-toolchain.sh musl)"; fi ;;
  cowsay) if [ -x out/cowsay/root/usr/bin/perl ]; then step cowsay cowsay; else echo "skip  cowsay: out/cowsay/root is not built (tools/build-cowsay.sh)"; fi ;;
  neofetch) if [ -x out/neofetch/root/bin/bash ]; then step neofetch neofetch; else echo "skip  neofetch: out/neofetch/root is not built (tools/build-neofetch.sh)"; fi ;;
  fastfetch) if [ -x out/fastfetch/fastfetch ]; then step fastfetch fastfetch; else echo "skip  fastfetch: out/fastfetch/fastfetch is not built (tools/build-fastfetch.sh)"; fi ;;
  microvm) if command -v gcc > /dev/null && [ "$(uname -m)" = x86_64 ]; then step microvm microvm; else echo "skip  microvm: it needs gcc for x86-64 (fixtures/microvm-hello/build.sh)"; fi ;;
  kernel) if [ -f out/linux/vmlinux ]; then LINES_SHOWN=2 step kernel kernel; else echo "skip  kernel: out/linux is not built (tools/build-linux.sh)"; fi ;;
  chrome) LINES_SHOWN=6 step chrome chrome ;;
  nd)     if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=3 step nd nd; else echo "skip  nd: the game is not installed (ND=$ND)"; fi ;;
  ffp)    if command -v naga > /dev/null; then step ffp tests/gfx/d3d9-ffp.sh; else echo "skip  ffp: naga is not installed (tools/setup-toolchain.sh naga)"; fi ;;
  sm)     if command -v naga > /dev/null; then step sm tests/gfx/d3d9-sm.sh ${SM_CAPTURED:+$SM_CAPTURED/*}; else echo "skip  sm: naga is not installed (tools/setup-toolchain.sh naga)"; fi ;;
  gdi)    LINES_SHOWN=5 step gdi gdi ;;
  changelog) step changelog node tools/changelog.mjs ;;
  d3dx)   if [ -f "$D3DX_DLL" ]; then step d3dx d3dx; else echo "skip  d3dx: no d3dx9_35.dll in D3DX_DLL=$D3DX_DLL (Tag's installer has one)"; fi ;;
  ndgfx)  if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=16 ND=$ND step ndgfx tests/gfx/nd.sh; else echo "skip  ndgfx: the game is not installed (ND=$ND)"; fi ;;
  ndplay) if [ -f "$ND/Narbacular Drop.exe" ]; then LINES_SHOWN=16 ND=$ND step ndplay tests/gfx/ndplay.sh; else echo "skip  ndplay: the game is not installed (ND=$ND)"; fi ;;
  tag)    if [ -f "${TAG_SETUP:-$HOME/games/Tag_setup.exe}" ]; then LINES_SHOWN=10 step tag tests/gfx/tag.sh; else echo "skip  tag: no installer at TAG_SETUP=${TAG_SETUP:-$HOME/games/Tag_setup.exe}"; fi ;;
  tagplay) if [ -f out/img-tag/index.json ]; then LINES_SHOWN=4 step tagplay tests/gfx/tagplay.sh; else echo "skip  tagplay: out/img-tag is not built (the tag step builds it)"; fi ;;
  doom)   LINES_SHOWN=4 step doom tests/gfx/doom.sh ;;
  st)     if [ ! -f "$REDIST/vcruntime140.dll" ]; then echo "skip  st: no MSVC runtime DLLs in REDIST=$REDIST"
          elif [ ! -f out/gdk-samples/local/SimpleTriangleDesktop.exe ]; then echo "skip  st: out/gdk-samples/local is not built (tools/build-gdk-sample.sh)"
          elif [ ! -f fixtures/msvc/eh.exe ]; then echo "skip  st: fixtures/msvc/eh.exe is not built (the build step, with clang-cl)"
          else LINES_SHOWN=4 step st tests/gfx/st.sh; fi ;;
  pbr)    if [ ! -f "$REDIST/vcruntime140.dll" ]; then echo "skip  pbr: no MSVC runtime DLLs in REDIST=$REDIST"
          elif [ ! -f out/gdk-samples/pbr-local/SimplePBR.exe ] && [ ! -d "$WINSDK/xwin" ]; then echo "skip  pbr: the sample is not built and there are no SDKs to build it in WINSDK=$WINSDK"
          else LINES_SHOWN=3 step pbr tests/gfx/pbr.sh; fi ;;
  images) LINES_SHOWN=9 step images tests/images.sh ;;
  mohaa)  if [ -f "${MOHAA_ZIP:-$HOME/games/mohaaspdemo.zip}" ]; then LINES_SHOWN=4 step mohaa tests/gfx/mohaa.sh; else echo "skip  mohaa: no download at MOHAA_ZIP=${MOHAA_ZIP:-$HOME/games/mohaaspdemo.zip}"; fi ;;
  mohaagfx) if [ -f out/img-mohaa/index.json ]; then LINES_SHOWN=5 step mohaagfx tests/gfx/mohaa-chrome.sh; else echo "skip  mohaagfx: out/img-mohaa is not built (the mohaa step builds it)"; fi ;;
  glffp)  if command -v naga > /dev/null; then step glffp tests/gfx/gl-ffp.sh; else echo "skip  glffp: naga is not installed (tools/setup-toolchain.sh naga)"; fi ;;
  desire) if [ -f out/img-desire/index.json ]; then LINES_SHOWN=12 step desire tests/gfx/desire.sh; else echo "skip  desire: out/img-desire is not built (the desireplay step builds it)"; fi ;;
  desireplay) if [ ! -f "${DESIRE_ZIP:-$HOME/games/DESI-Re-v0.1-Windows-x64.zip}" ]; then echo "skip  desireplay: no zip at DESIRE_ZIP=${DESIRE_ZIP:-$HOME/games/DESI-Re-v0.1-Windows-x64.zip}"
          elif [ ! -f "$REDIST/vcruntime140.dll" ]; then echo "skip  desireplay: no MSVC runtime DLLs in REDIST=$REDIST"
          else LINES_SHOWN=4 step desireplay tests/gfx/desireplay.sh; fi ;;
  audio)  step audio tests/audio/run.sh ;;
  xcl)    if [ -f out/xcltitle-game/xcl_testtitle.exe ]; then LINES_SHOWN=3 step xcl tests/gfx/xcl.sh; else echo "skip  xcl: out/xcltitle-game/xcl_testtitle.exe is not built"; fi ;;
  *) echo "unknown step: $s"; fail=1 ;;
esac; done
exit $fail
