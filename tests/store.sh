#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/store.sh: the store (docs/images.md#the-store, docs/spec/distribution.md), in a PISCO_HOME of its own.
#   1. `pisco install hello32` (an official recipe, by its name) from another folder, then `pisco run hello32` there:
#      it prints what hello prints
#   2. a second app FROM the same base (the recipe copied under another name) shares the base's layer: one blob
#   3. `pisco remove` of one keeps what the other uses, and frees the rest; installing again replaces, and leaves
#      the app's saves alone
#   4. a third party's recipe from a git repository: by <url>#app, by @ns/app (pisco source add) and by owner/repo/app
#      (GitHub's place stood in for by PISCO_GITHUB), the user's file given with --input and checked by its sha256;
#      the image records the commit. Refused: a third party's recipe with built_by, with an input outside its folder,
#      or in a folder that is not its name; and the input when its sha256 is not the recipe's
#   5. inputs: installed again, the third party's app needs no --input (the store's inputs keep it by sha256); an
#      input with a url is fetched from a web server on this machine, checked, and kept; one whose url gives other
#      bytes is refused and nothing of it kept
#   6. a registry (tests/registry.mjs, with bearer tokens as ghcr.io's): hello32, made only from this repository's
#      sources, pushed (not without credentials; a second push sends no blob), removed, pulled by tag and by digest
#      into the store, and run. An app made from the user's files is refused: its layers are local-only
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
grep -q "^hello32 .*official" <<< "$($P list)" || bad "pisco list: $($P list)"
$P inspect hello32 > /dev/null || bad "pisco inspect hello32"

# 2. another app FROM base/windows-x86:1 shares its layer
mkdir -p $T/hello-again
sed -e 's/^name = "hello32"/name = "hello-again"/' -e "s|^file = \"../../|file = \"$root/|" recipes/hello32/recipe.toml > $T/hello-again/recipe.toml
$P install $T/hello-again > $T/install2.log 2>&1 || { cat $T/install2.log; bad "pisco install <dir>"; }
base_of() { node -e 'const fs=require("fs"),d=process.argv[1];const i=JSON.parse(fs.readFileSync(d+"/index.json"));const m=JSON.parse(fs.readFileSync(d+"/blobs/sha256/"+i.manifests[0].digest.slice(7)));console.log(m.layers.find(l=>l.annotations["org.pisco.layer.role"]==="base").digest.slice(7))' "$1"; }
b1=$(base_of $PISCO_HOME/apps/hello32); b2=$(base_of $PISCO_HOME/apps/hello-again)
[ "$b1" = "$b2" ] || bad "the two apps' base layers differ: $b1 $b2"
grep -q "^hello-again .*dir:" <<< "$($P list)" || bad "pisco list does not say where hello-again came from"

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

# 4. third parties: a git repository of recipes
exe=$root/fixtures/hello/target/i686-pc-windows-msvc/release/hello.exe; sha=$(sha256sum $exe | cut -d' ' -f1)
repo=$T/gh/someone/recipes; mkdir -p $repo/recipes/hello-third
third() { sed -e "s/^name = \"hello32\"/name = \"$1\"/" -e 's|^file = .*|file = "hello.exe"|' -e "s|^built_by = .*|sha256 = \"$sha\"|" recipes/hello32/recipe.toml; }
third hello-third > $repo/recipes/hello-third/recipe.toml
mkdir -p $repo/recipes/scripted $repo/recipes/outside $repo/recipes/misnamed
sed 's/^name = "hello32"/name = "scripted"/' recipes/hello32/recipe.toml > $repo/recipes/scripted/recipe.toml
third outside | sed 's|^file = .*|file = "../../../../.ssh/id_rsa"|' > $repo/recipes/outside/recipe.toml
third hello-third > $repo/recipes/misnamed/recipe.toml
git -C $repo init -q && git -C $repo add -A && git -C $repo -c user.name=t -c user.email=t@t commit -qm recipes
commit=$(git -C $repo rev-parse HEAD)
$P install "file://$repo#hello-third" > $T/third.log 2>&1 && bad "a third party's recipe installed without the user's file"
grep -q "hello.exe" $T/third.log || bad "no input, and the message does not name it: $(tail -2 $T/third.log)"
$P install "file://$repo#hello-third" --input program=$root/fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe > $T/third.log 2>&1 && bad "a file with another sha256 was taken"
$P install "file://$repo#hello-third" --input program=$exe > $T/third.log 2>&1 || { tail -3 $T/third.log; bad "pisco install <git url>#app"; }
grep -q "third party" $T/third.log || bad "installing a third party's recipe did not say so"
$P run hello-third > $T/run3.txt 2>&1; grep -q PASS $T/run3.txt || bad "pisco run hello-third: $(tail -3 $T/run3.txt)"
grep -q "$commit" $PISCO_HOME/apps/hello-third/index.json || bad "the image does not record the source's commit"
$P source add @someone "file://$repo" > /dev/null || bad "pisco source add"
grep -q "@someone/hello-third" <<< "$($P search hello)" || bad "pisco search does not find @someone/hello-third: $($P search hello)"
$P install @someone/hello-third --input program=$exe > /dev/null 2>&1 || bad "pisco install @ns/app"
PISCO_GITHUB=file://$T/gh $P install someone/recipes/hello-third --input program=$exe > /dev/null 2>&1 || bad "pisco install owner/repo/app"
for r in scripted:built_by outside:"plain file name" misnamed:"folder of its name"; do
  out=$($P install @someone/${r%%:*} --input program=$exe 2>&1) && bad "@someone/${r%%:*} installed, and must not be"
  echo "$out" | grep -q "${r#*:}" || bad "@someone/${r%%:*}: refused, but not for its ${r#*:}: $out"
done

# 5. inputs kept by sha256, and fetched from a url
$P install @someone/hello-third > $T/again.log 2>&1 || { tail -3 $T/again.log; bad "installing hello-third again needed its input"; }
grep -q "store's inputs" $T/again.log || bad "the second install did not take the input from the store's inputs"
mkdir -p $T/web; cp $exe $T/web/hello.exe; cp $root/fixtures/hello/target/x86_64-pc-windows-msvc/release/hello.exe $T/web/other.exe
node -e 'const h=require("http"),fs=require("fs"),p=require("path");const s=h.createServer((q,r)=>{const f=p.join(process.argv[1],p.basename(q.url));fs.existsSync(f)?r.end(fs.readFileSync(f)):r.writeHead(404).end()}).listen(0,"127.0.0.1",()=>fs.writeFileSync(process.argv[2],String(s.address().port)))' $T/web $T/port &
web=$!; for _ in $(seq 50); do [ -s $T/port ] && break; sleep 0.1; done; port=$(cat $T/port)
for n in fetched wrong-bytes; do mkdir -p $repo/recipes/$n; third $n > $repo/recipes/$n/recipe.toml; done
sed -i "s|^file = .*|&\nurl = \"http://127.0.0.1:$port/hello.exe\"|" $repo/recipes/fetched/recipe.toml
sed -i "s|^file = .*|&\nurl = \"http://127.0.0.1:$port/other.exe\"|" $repo/recipes/wrong-bytes/recipe.toml
git -C $repo add -A && git -C $repo -c user.name=t -c user.email=t@t commit -qm urls
rm -rf $PISCO_HOME/inputs
out=$($P install @someone/wrong-bytes 2>&1) && bad "an input whose url gives other bytes was taken"
echo "$out" | grep -q "the recipe wants" || bad "wrong bytes refused, but not saying why: $out"
[ -z "$(ls -A $PISCO_HOME/inputs/sha256 2>/dev/null)" ] || bad "the wrong bytes were kept: $(ls -A $PISCO_HOME/inputs/sha256)"
$P install @someone/fetched > $T/fetch.log 2>&1 || { tail -3 $T/fetch.log; bad "an input with a url was not fetched"; }
[ -f $PISCO_HOME/inputs/sha256/$sha ] || bad "the fetched input is not kept by its sha256"
kill $web 2>/dev/null
$P remove fetched > /dev/null

# 6. a registry
node tests/registry.mjs $T $T/rport & reg=$!
for _ in $(seq 50); do [ -s $T/rport ] && break; sleep 0.1; done; R=127.0.0.1:$(cat $T/rport)/pisco
$P install hello32 > /dev/null 2>&1 || bad "pisco install hello32 (for the registry)"
$P push hello32 $R/hello32:1 > $T/push.log 2>&1 && bad "pushed without credentials"
grep -q "no token\|credentials" $T/push.log || bad "refused without credentials, but not saying so: $(tail -1 $T/push.log)"
export PISCO_REGISTRY_TOKEN=secret
$P push hello32 $R/hello32:1 > $T/push.log 2>&1 || { tail -3 $T/push.log; bad "pisco push hello32"; }
digest=$(grep -o 'sha256:[0-9a-f]*' $T/push.log | tail -1)
$P push hello32 $R/hello32:1 2>&1 | grep -q "0 blobs sent" || bad "a second push sent blobs again"
$P remove hello32 > /dev/null
unset PISCO_REGISTRY_TOKEN
$P pull $R/hello32:1 > $T/pull.log 2>&1 || { tail -3 $T/pull.log; bad "pisco pull by tag"; }
$P run hello32 > $T/run4.txt 2>&1; grep -q PASS $T/run4.txt || bad "the pulled hello32 does not run: $(tail -3 $T/run4.txt)"
grep -q "oci:$R/hello32:1@$digest" $PISCO_HOME/apps/hello32/index.json || bad "the pulled image does not say where it came from"
$P pull $R/hello32@$digest --as hello-pinned > /dev/null 2>&1 || bad "pisco pull by digest"
[ -d $PISCO_HOME/apps/hello-pinned ] || bad "pull --as did not name the app"
out=$(PISCO_REGISTRY_TOKEN=secret $P push hello-third $R/hello-third:1 2>&1) && bad "an app made of the user's files was pushed"
echo "$out" | grep -q "local-only" || bad "a local-only app refused, but not saying why: $out"
kill $reg 2>/dev/null
$P remove hello-third > /dev/null; $P remove hello32 > /dev/null; $P remove hello-pinned > /dev/null

[ $fail = 0 ] && echo "the store: installed by name, run from another folder, a base's layer shared by two apps, removed without what the other uses; a third party's recipe by URL, @ns and owner/repo, and what it may not do refused; inputs kept, and fetched from a url; pushed to a registry and pulled back, and the user's files never"
exit $fail
