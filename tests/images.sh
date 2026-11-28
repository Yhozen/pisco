#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/images.sh: app images (milestone M6, docs/images.md), built by `pisco build` and run by `pisco run`.
#   1. hello32 (recipes/hello32) built twice, each time with an empty cache: the same manifest digest; its image run in
#      Node passes its 7 checks
#   1b. hello64 (Windows x64), Doom (Xbox Series, from out/doom/game) and linux-hello (Linux x86-64, with musl in MUSL)
#      the same way: one app of every base
#   2. Narbacular Drop from the user's installer (ND_SETUP; skipped when it is not found), the same: two builds with
#      empty caches give the same manifest digest. With the Wine-installed game there (ND) the extracted files are
#      compared with it (the differences are printed)
#   3. hello32's base layer is the same blob as Narbacular Drop's (both are FROM base/windows-x86:1)
#   4. a recipe that differs only in its registry, built with the first build's cache: nothing lifted, compiled or
#      linked again (the build's log says so; its time is printed), the same module blob, another config
#   5. `pisco run` of the first image with the menu-to-level session in Node: the checkpoints of tests/run.sh nd
#      (recipes/narbacular-drop/traces/menu-to-level.txt), and the null GPU back end finds every record consistent;
#      the changed registry reaches advapi32 at run time (the second image's first 2 s, traced: D3DX opens its key after
#      the first frame)
# Environment: ND_SETUP (narbacular_drop_setup.exe), ND (the installed game's folder, for the comparison), KEEP=1 to
# keep the second build's cache (out/images-test/cache-b; it is large).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
T=out/images-test; rm -rf $T; mkdir -p $T
SW=target/release/pisco
ND=${ND:-$HOME/.pisco/oracle/prefix-narbacular-drop/drive_c/Program Files (x86)/Narbacular Drop}
cargo build -q --release -p pisco -p pisco-lifter || { echo "cargo build failed"; exit 1; }
if [ -z "${ND_SETUP:-}" ]; then
  for f in recipes/narbacular-drop/narbacular_drop_setup.exe "$HOME/Downloads/narbacular_drop_setup.exe" .context/attachments/*/narbacular_drop_setup.exe .context/attachments/*/*/narbacular_drop_setup.exe; do
    [ -f "$f" ] && { ND_SETUP=$f; break; }; done
fi
# the digest of an image's blob: its manifest's layer of a role (base, library, app, module), or "config"
blob() { python3 -I -c 'import json, sys
d = sys.argv[1]; idx = json.load(open(d + "/index.json"))
m = json.load(open(d + "/blobs/sha256/" + idx["manifests"][0]["digest"][7:]))
role = sys.argv[2]
print(" ".join([m["config"]["digest"]] if role == "config" else [l["digest"] for l in m["layers"] if l.get("annotations", {}).get("org.pisco.layer.role") == role]))' "$1" "$2"; }
build() { # name, recipe, out, cache, more...: the manifest digest; the log in $T/<name>.log
  local name=$1 recipe=$2 out=$3 cache=$4; shift 4
  local t0=$(date +%s.%N)
  $SW build "$recipe" --out "$out" --cache "$cache" "$@" > $T/$name.out 2> $T/$name.log || { echo "$name: pisco build failed:" >&2; tail -4 $T/$name.log >&2; return 1; }
  awk -v a="$t0" -v b="$(date +%s.%N)" 'BEGIN { print b - a }' > $T/$name.time
  tail -1 $T/$name.out
}

# 1. hello32
a=$(build hello32-a recipes/hello32 $T/hello32-a $T/cache-a) || exit 1
b=$(build hello32-b recipes/hello32 $T/hello32-b $T/cache-b) || exit 1
[ "$a" = "$b" ] || { echo "hello32: two builds with empty caches differ ($a, $b)"; exit 1; }
pass=$(SW_STRICT=1 $SW run $T/hello32-a 2>&1 | grep -c '^\[PASS\]')
[ "$pass" = 7 ] || { echo "hello32: its image passed $pass of 7"; exit 1; }
echo "hello32: two builds with empty caches give manifest $a; the image passes 7 of 7 in Node"

# 1b. one app of each other base: hello64 (Windows x64) and Doom (Xbox Series, when tests/gfx/doom.sh has built it)
a=$(build hello64-a recipes/hello64 $T/hello64-a $T/cache-a) || exit 1
b=$(build hello64-b recipes/hello64 $T/hello64-b $T/cache-b) || exit 1
[ "$a" = "$b" ] || { echo "hello64: two builds with empty caches differ ($a, $b)"; exit 1; }
pass=$(SW_STRICT=1 $SW run $T/hello64-a 2>&1 | grep -c '^\[PASS\]')
[ "$pass" = 7 ] || { echo "hello64: its image passed $pass of 7"; exit 1; }
echo "hello64 (base/windows-x64:1): two builds with empty caches give manifest $a; the image passes 7 of 7 in Node"
if [ -f out/doom/game/doom.exe ]; then
  a=$(build doom-a recipes/doom $T/doom-a $T/cache-a) || exit 1
  b=$(build doom-b recipes/doom $T/doom-b $T/cache-b) || exit 1
  [ "$a" = "$b" ] || { echo "doom: two builds with empty caches differ ($a, $b)"; exit 1; }
  SW_STRICT=1 SW_PACE=off timeout 900 $SW run $T/doom-a -- -hashframes -timedemo demo1 > $T/doom.run.log 2>&1
  frames=$(grep -c '^frame' $T/doom.run.log); want=$(grep -c '^frame' out/doom/ref/native-t1.txt 2>/dev/null || echo 7156)
  [ "$frames" = "$want" ] || { echo "doom: its image drew $frames frames of the timedemo, natively $want"; exit 1; }
  echo "doom (base/xbox-series:1): two builds with empty caches give manifest $a; the image plays the timedemo's $frames frames"
else echo "doom: out/doom/game is not built (tests/run.sh doom): its image is not built"; fi
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
if [ -x "$MUSL/bin/musl-gcc" ]; then
  MUSL=$MUSL fixtures/linux-hello/build.sh out/linux-hello > /dev/null || { echo "linux-hello: does not compile"; exit 1; }
  a=$(build linux-hello-a recipes/linux-hello $T/linux-hello-a $T/cache-a) || exit 1
  b=$(build linux-hello-b recipes/linux-hello $T/linux-hello-b $T/cache-b) || exit 1
  [ "$a" = "$b" ] || { echo "linux-hello: two builds with empty caches differ ($a, $b)"; exit 1; }
  # the image's drive is the root: the program finds itself where the recipe put it
  SW_STRICT=1 SW_CLOCK=16667 $SW run $T/linux-hello-a > $T/linux-hello.run.log 2>&1
  diff <(sed 's|^exe: /hello$|exe: /usr/bin/hello|' fixtures/linux-hello/expected.txt) $T/linux-hello.run.log > $T/linux-hello.diff || { echo "linux-hello: its image does not print expected.txt (< expected, > the image):"; head -6 $T/linux-hello.diff; exit 1; }
  echo "linux-hello (base/linux-x64:1): two builds with empty caches give manifest $a; the image prints what fixtures/linux-hello/expected.txt says"
else echo "linux-hello: no musl in MUSL=$MUSL (tools/setup-toolchain.sh musl): its image is not built"; fi

# 1c. every config these builds made against the specification's schema (docs/spec/config.schema.json)
node tools/check-config.mjs $(for d in $T/*-a; do [ -f $d/index.json ] && echo $d; done) > $T/configs.log || { echo "configs that do not conform:"; grep -v conforms $T/configs.log | head -8; exit 1; }
echo "$(grep -c conforms $T/configs.log) configs conform to docs/spec/config.schema.json"
# 1d. the lock (docs/spec/recipe.md#the-lock): hello32 with its input pinned, locked, built again; then its input with
#     one byte changed (and pinned again): the build stops at the lock. A recipe with an input not pinned is not locked
mkdir -p $T/lock; exe=fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe; cp $exe $T/lock/hello.exe
sed "s|^file = .*|file = \"hello.exe\"|; s|^built_by = .*|sha256 = \"$(sha256sum $exe | cut -c1-64)\"|" recipes/hello32/recipe.toml > $T/lock/recipe.toml
build lock-1 $T/lock $T/lock/image $T/cache-a --lock > /dev/null || exit 1
grep -q '^tree = "sha256:' $T/lock/recipe.lock || { echo "lock: --lock wrote no recipe.lock"; exit 1; }
build lock-2 $T/lock $T/lock/image $T/cache-a > /dev/null && grep -q "^pisco: lock: the app's files are the lock's" $T/lock-2.log || { echo "lock: a build of the locked recipe did not check its lock"; exit 1; }
[ "sha256:$(sed -n 's/^tree = "sha256:\(.*\)"/\1/p' $T/lock/recipe.lock)" = "$(node -e 'const fs=require("fs"),d=process.argv[1],b=(x)=>JSON.parse(fs.readFileSync(d+"/blobs/sha256/"+x.slice(7)));console.log(b(b(JSON.parse(fs.readFileSync(d+"/index.json")).manifests[0].digest).config.digest).tree.digest)' $T/hello32-a)" ] || { echo "lock: its tree is not the image's"; exit 1; }
printf '\x01' | dd of=$T/lock/hello.exe bs=1 seek=1000 conv=notrunc status=none
sed -i "s|^sha256 = .*|sha256 = \"$(sha256sum $T/lock/hello.exe | cut -c1-64)\"|" $T/lock/recipe.toml
if build lock-3 $T/lock $T/lock/image $T/cache-a > /dev/null 2>&1; then echo "lock: an input with a byte changed gave the lock's files"; exit 1; fi
grep -q 'not the files the recipe gave when it was locked' $T/lock-3.log || { echo "lock: the changed input failed for another reason:"; tail -2 $T/lock-3.log; exit 1; }
if $SW build recipes/hello32 --out $T/lock/x --cache $T/cache-a --lock > /dev/null 2> $T/lock-4.log || ! grep -q 'is not pinned by a sha256' $T/lock-4.log; then echo "lock: a recipe built here was locked"; exit 1; fi
echo "the lock: hello32 pinned and locked, built again against it; one byte of its input changed stops the build; a recipe built here is not locked"

if [ -z "${ND_SETUP:-}" ] || [ ! -f "$ND_SETUP" ]; then echo "Narbacular Drop: no installer found (ND_SETUP): its image is not built"; exit 0; fi
# 2. Narbacular Drop, twice from nothing
cmp=(); [ -d "$ND" ] && cmp=(--compare "$ND")
a=$(build nd-a recipes/narbacular-drop $T/nd-a $T/cache-a --source "$ND_SETUP" "${cmp[@]}") || exit 1
b=$(build nd-b recipes/narbacular-drop $T/nd-b $T/cache-b --source "$ND_SETUP") || exit 1
[ "${KEEP:-0}" = 1 ] || rm -rf $T/cache-b
[ "$a" = "$b" ] || { echo "Narbacular Drop: two builds with empty caches differ ($a, $b)"; diff <(ls -R $T/nd-a/blobs) <(ls -R $T/nd-b/blobs) | head -6; exit 1; }
echo "Narbacular Drop: two builds with empty caches give manifest $a ($(printf '%.0f' "$(cat $T/nd-a.time)") s and $(printf '%.0f' "$(cat $T/nd-b.time)") s)"
grep '^pisco: compare' $T/nd-a.log | sed 's/^pisco: compare: */  installer against the Wine install: /; s/^  installer against the Wine install:   /    /' | sed "s|$ND|ND|g"

# 3. the base, shared
hb=$(blob $T/hello32-a base); nb=$(blob $T/nd-a base)
[ -n "$hb" ] && [ "$hb" = "$nb" ] || { echo "the base layers differ: hello32 $hb, Narbacular Drop $nb"; exit 1; }
echo "hello32 and Narbacular Drop share their base layer: $hb ($(stat -c %s $T/nd-a/blobs/sha256/${hb#sha256:}) bytes)"

# 4. the registry alone changed
mkdir -p $T/nd-reg
{ cat recipes/narbacular-drop/recipe.toml; printf "\n[registry.'HKCU\\\\Software\\\\pisco\\\\images-test']\nRebuilt = 1\n"; } > $T/nd-reg/recipe.toml
r=$(build nd-reg $T/nd-reg $T/nd-reg/image $T/cache-a --source "$ND_SETUP") || exit 1
grep -q '^pisco: lift: cached' $T/nd-reg.log && grep -q '^pisco: runtime: [0-9]* sources, 0 compiled' $T/nd-reg.log && grep -q '^pisco: link: cached' $T/nd-reg.log || { echo "a registry-only change built something again:"; grep -E '^pisco: (lift|runtime|link)' $T/nd-reg.log; exit 1; }
[ "$(blob $T/nd-reg/image module)" = "$(blob $T/nd-a module)" ] || { echo "a registry-only change made another module"; exit 1; }
[ "$(blob $T/nd-reg/image config)" != "$(blob $T/nd-a config)" ] && [ "$r" != "$a" ] || { echo "a registry-only change left the config as it was"; exit 1; }
echo "a registry-only change: rebuilt in $(printf '%.1f' "$(cat $T/nd-reg.time)") s (the first build took $(printf '%.0f' "$(cat $T/nd-a.time)") s), nothing lifted, compiled or linked again; the same module, another config: manifest $r"

# 5. pisco run: the session in Node, and the registry at run time
want=recipes/narbacular-drop/traces/menu-to-level.txt
SW_STRICT=1 SW_CLOCK=16667 SW_D3D9_STATS=1 timeout 900 $SW run $T/nd-a --session recipes/narbacular-drop/sessions/menu-to-level.json > $T/nd.session.log 2>&1 || { echo "pisco run: the session ended with exit code $?:"; tail -3 $T/nd.session.log | cut -c1-300; exit 1; }
grep -E '^pisco: (checkpoint |session done)' $T/nd.session.log | sed 's/^pisco: //' > $T/nd.checkpoints.txt
diff <(grep -v '^#' $want) $T/nd.checkpoints.txt > $T/nd.checkpoints.diff || { echo "pisco run: the checkpoints are not the recorded ones (< $want, > this run):"; head -6 $T/nd.checkpoints.diff; exit 1; }
grep -q '^pisco: gpu (null back end): .* 0 of the records were inconsistent$' $T/nd.session.log || { echo "pisco run: the null GPU back end did not find the records consistent"; exit 1; }
echo "pisco run (Node): menu-to-level, $(grep -c '^checkpoint ' $T/nd.checkpoints.txt) checkpoints as recorded, $(sed -n 's/^session done: .*, \([0-9]*\) frames$/\1/p' $T/nd.checkpoints.txt) frames, every GPU record consistent"
SW_STRICT=1 SW_TRACE=1 SW_CLOCK=16667 SW_SESSION_STEPS='wait 2000' timeout 300 $SW run $T/nd-reg/image > $T/nd-reg.run.log 2>&1
n=$(python3 -I -c 'import json, sys; d = sys.argv[1]; idx = json.load(open(d + "/index.json")); m = json.load(open(d + "/blobs/sha256/" + idx["manifests"][0]["digest"][7:])); print(len(json.load(open(d + "/blobs/sha256/" + m["config"]["digest"][7:]))["registry"]))' $T/nd-reg/image)
grep -q "^pisco: registry: $n values$" $T/nd-reg.run.log && grep -q '^pisco: registry: HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Direct3D\\DisableMMX = 1$' $T/nd-reg.run.log || { echo "the image's registry did not reach advapi32:"; grep 'registry' $T/nd-reg.run.log | head -3; exit 1; }
echo "the registry at run time: advapi32 has the config's $n values (the module has none built in), D3DX reads DisableMMX = 1 $(grep -c 'DisableMMX = 1$' $T/nd-reg.run.log) times"
