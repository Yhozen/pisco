#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/store.sh: the store (docs/images.md#the-store, docs/spec/distribution.md), in a PISCO_HOME of its own.
#   1. `pisco install hello32` (an official recipe, by its name) from another folder, then `pisco run hello32` there:
#      it prints what hello prints
#   2. a second app FROM the same base (the recipe copied under another name) shares the base's layer: one blob
#   3. `pisco remove` of one keeps what the other uses, and frees the rest; installing again replaces, and leaves
#      the app's saves alone
# Needs hello's 32-bit build (tests/run.sh build).
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
P=$root/target/release/pisco; T=$root/out/store-test; fail=0
rm -rf $T; mkdir -p $T/elsewhere
export PISCO_HOME=$T/home
cargo build -q --release -p pisco -p pisco-lifter || { echo "cargo build failed"; exit 1; }
[ -f fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe ] || { echo "skipped: hello's 32-bit build is not there (tests/run.sh build)"; exit 0; }
bad() { echo "FAIL: $*"; fail=1; }
blobs() { ls $PISCO_HOME/blobs/sha256 | sort; }

# 1. by name, from anywhere
(cd $T/elsewhere && $P install hello32 > $T/install.log 2>&1) || { cat $T/install.log; bad "pisco install hello32"; }
(cd $T/elsewhere && $P run hello32 > $T/run.txt 2>&1) || bad "pisco run hello32: $(tail -3 $T/run.txt)"
grep -q "PASS" $T/run.txt || bad "pisco run hello32 printed: $(head -3 $T/run.txt)"
[ -d $PISCO_HOME/saves/hello32 ] || bad "no saves/hello32"
$P list | grep -q "^hello32 .*official" || bad "pisco list: $($P list)"
$P inspect hello32 > /dev/null || bad "pisco inspect hello32"

# 2. another app FROM base/windows-x86:1 shares its layer
mkdir -p $T/hello-again
sed -e 's/^name = "hello32"/name = "hello-again"/' -e "s|^file = \"../../|file = \"$root/|" recipes/hello32/recipe.toml > $T/hello-again/recipe.toml
$P install $T/hello-again > $T/install2.log 2>&1 || { cat $T/install2.log; bad "pisco install <dir>"; }
base_of() { node -e 'const fs=require("fs"),d=process.argv[1];const i=JSON.parse(fs.readFileSync(d+"/index.json"));const m=JSON.parse(fs.readFileSync(d+"/blobs/sha256/"+i.manifests[0].digest.slice(7)));console.log(m.layers.find(l=>l.annotations["org.pisco.layer.role"]==="base").digest.slice(7))' "$1"; }
b1=$(base_of $PISCO_HOME/apps/hello32); b2=$(base_of $PISCO_HOME/apps/hello-again)
[ "$b1" = "$b2" ] || bad "the two apps' base layers differ: $b1 $b2"
$P list | grep -q "^hello-again .*dir:" || bad "pisco list does not say where hello-again came from"

# 3. remove one: the base's layer stays for the other; remove both: nothing stays
echo kept > $PISCO_HOME/saves/hello32/note
before=$(blobs | wc -l)
$P remove hello-again > /dev/null || bad "pisco remove hello-again"
blobs | grep -q "$b1" || bad "the base's layer went with hello-again, and hello32 still uses it"
[ "$(blobs | wc -l)" -lt "$before" ] || bad "removing hello-again freed nothing"
$P run hello32 > /dev/null 2>&1 || bad "hello32 does not run after hello-again's removal"
(cd $T/elsewhere && $P install hello32 > /dev/null 2>&1) || bad "installing hello32 again"
[ -f $PISCO_HOME/saves/hello32/note ] || bad "installing again lost the app's saves"
$P remove hello32 > /dev/null || bad "pisco remove hello32"
[ -z "$(blobs)" ] || bad "blobs left with no app installed: $(blobs | wc -l)"
$P run hello32 > /dev/null 2>&1 && bad "pisco run of a removed app ran"
[ $fail = 0 ] && echo "the store: installed by name, run from another folder, a base's layer shared by two apps, removed without what the other uses"
exit $fail
