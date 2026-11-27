#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# tests/layout.sh: the runtime's files are listed in one place, the bases (bases/<runtime>/<version>/base.toml).
#   - no script, test or program names runtime files by a glob of rt/ (they ask `pisco sources`)
#   - every rt/*.c and rt/<runtime>/*.c is a source of at least one base, and each base resolves
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); cd "$root"
fail=0
hits=$(grep -rnE '"?rt/(\*|gdk/\*|xbox-series/\*|windows-x(64|86)/\*|linux-x64/\*|microvm-x64/\*|ps5/\*)\.c' tools tests crates web --include=*.sh --include=*.py --include=*.mjs --include=*.js --include=*.rs | grep -v '^tests/layout.sh:')
[ -z "$hits" ] || { echo "runtime files listed outside bases/:"; echo "$hits" | head; fail=1; }
all=$(mktemp); trap 'rm -f "$all"' EXIT
for b in bases/*/*/base.toml; do
  ref="base/$(basename "$(dirname "$(dirname "$b")")"):$(basename "$(dirname "$b")")"
  target/release/pisco sources "$ref" --all-libraries >> "$all" || { echo "$ref does not resolve"; fail=1; }
done
orphans=$(ls rt/*.c rt/*/*.c | grep -vxF -f <(sort -u "$all"))
[ -z "$orphans" ] || { echo "runtime files no base links:"; echo "$orphans"; fail=1; }
[ $fail = 0 ] && echo "$(ls bases/*/*/base.toml | wc -l) bases list all $(ls rt/*.c rt/*/*.c | wc -l) runtime files; no other list of them"
exit $fail
