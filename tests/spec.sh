#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/spec.sh: the specification (docs/spec/) and what pisco does are the same lists.
#   1. the keys of a recipe, pisco's hints, the extract kinds, CPU features, DLL choices and quirks: the rows between
#      the markers of docs/spec/recipe.md, and what `pisco recipe --keys` prints, the same in both directions
#   2. the config's fields and the layer roles of docs/spec/image.md: the schema's (config.schema.json) and pisco's
#   3. every recipe in recipes/ is read (schema 1); a recipe with a misspelt key in any of its tables, an unknown
#      schema, an input pinned twice or a name not in a list is refused
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
P=target/release/pisco; fail=0; T=out/spec-test; rm -rf $T; mkdir -p $T
cargo build -q --release -p pisco || { echo "cargo build failed"; exit 1; }
# the rows of a marked table of a spec page: its first column's `names`, or every `name` of a marked paragraph
marked() { awk -v m="$2" '$0 == "<!-- " m " -->" {on=1; next} $0 == "<!-- /" m " -->" {on=0} on' "$1" |
  if [ "${3:-table}" = table ]; then grep '^| `' | sed 's/^| `\([^`]*\)`.*/\1/'; else grep -o '`[^`]*`' | tr -d '`'; fi; }
same() { # what, the spec's list, pisco's list
  local a b; a=$(echo "$2" | sort -u); b=$(echo "$3" | sort -u)
  if [ "$a" != "$b" ]; then echo "$1: the spec and pisco differ (< spec, > pisco):"; diff <(echo "$a") <(echo "$b") | grep '^[<>]'; fail=1; fi; }
keys=$($P recipe --keys)
of() { echo "$keys" | awk -v k="$1" '$1 == k {print $2}'; }
same "recipe keys" "$(marked docs/spec/recipe.md keys)" "$(of key)"
same "hints.pisco" "$(marked docs/spec/recipe.md hints.pisco)" "$(of hint)"
same "extract kinds" "$(marked docs/spec/recipe.md extract-kinds)" "$(of extract-kind)"
same "CPU features" "$(marked docs/spec/recipe.md cpu-features words)" "$(of cpu-feature)"
same "DLL choices" "$(marked docs/spec/recipe.md dll-choices)" "$(of dll-choice)"
same "quirks" "$(marked docs/spec/recipe.md quirks)" "$(of quirk)"
# the image: the config's fields (docs/spec/image.md) are the schema's, and the layer roles are the ones pisco writes
same "config fields" "$(marked docs/spec/image.md config)" "$(node -e 'console.log(Object.keys(JSON.parse(require("fs").readFileSync("docs/spec/config.schema.json")).properties).join("\n"))')"
same "layer roles" "$(marked docs/spec/image.md roles)" "$(grep -ohE '\(ROLE, "[a-z]+"\)' crates/pisco/src/build.rs | sed 's/(ROLE, "\(.*\)")/\1/')"
n=0; for r in recipes/*/; do $P recipe "$r" --json > /dev/null 2> $T/err || { echo "$r: $(cat $T/err)"; fail=1; }; n=$((n + 1)); done
# refused: each line a change made to recipes/tag/recipe.toml (sed), and a word the error must have
bad=0
while IFS='|' read -r change want; do
  [ -z "$change" ] && continue
  sed "$change" recipes/tag/recipe.toml > $T/recipe.toml
  if out=$($P recipe $T --json 2>&1 > /dev/null); then echo "accepted, and must not be: sed '$change'"; fail=1
  elif ! echo "$out" | grep -q -- "$want"; then echo "sed '$change': refused, but the message does not say $want: $out"; fail=1
  else bad=$((bad + 1)); fi
done <<'EOF'
s/^schema = 1/schema = 2/|schema = 2
/^schema = 1/d|no schema
s/^display_name/display_nam/|display_nam is not a key of \[about\]
s/^id = "installer"/ids = "installer"/|ids is not a key of \[input\]
s/^where = /wher = /|wher is not a key of \[input\]
s/^app = /ap = /|ap is not a key of \[extract\]
s/^kind = "cab"/kind = "cabinet"/|kind = "cabinet"
s/^to = 'C:.WINDOWS.system32'/too = 'C:\\WINDOWS'/|too is not a key of \[extract\]
s/^display = /dispay = /|dispay is not a key of \[run\]
s/^features = \["fpu"/feature = ["fpu"/|feature is not a key of \[cpu\]
s/^input = "installer"/&\nroot = "x"/|innosetup: root is not one of its keys
s/^exe = /kernel = /|\[run\] kernel: not a key of windows-x86
s/"cx8"\]/"cx9"]/|cx9 is not one of
s/= "builtin"/= "built-in"/|"built-in": one of native, builtin
s/^split = 16/splitt = 16/|\[hints.pisco\] splitt
s/^\[hints.pisco\]/[hint.pisco]/|hint is not a key of a recipe
s/^sha256 = .*/&\nbuilt_by = "x"/|exactly one of sha256
s/^sha256 = .*/sha256 = "abc"/|64 hex digits
s/^name = "tag"/name = "Tag"/|lower-case
s/^\[registry\]/[quirks]\nheap_fil = 1\n[registry]/|heap_fil: not a quirk
EOF
[ $fail = 0 ] && echo "the spec's lists are pisco's ($(echo "$keys" | wc -l) names); $n recipes read; $bad wrong recipes refused, each saying why"
exit $fail
