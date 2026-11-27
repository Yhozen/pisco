#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/linux-files/build.sh [root, default out/linux-files/root]: files (a static Linux x86-64 program, musl) at
# /files, in a tree with what a Linux drive has and a Windows one has not: Makefile beside makefile, a link to a file
# and one to a folder, a script without its executable bit and one with it. Environment: MUSL (musl's folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=${1:-$here/../../out/linux-files/root}
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
rm -rf "$root"; mkdir -p "$root/data"
"$MUSL/bin/musl-gcc" -O2 -static -no-pie "$here/files.c" -o "$root/files"
echo upper > "$root/Makefile"; echo lower > "$root/makefile"; echo hello > "$root/data/hello.txt"
ln -s data/hello.txt "$root/link-to-data"; ln -s data "$root/d"
printf '#!/files\n' > "$root/script.sh"; chmod 0644 "$root/script.sh"
printf '#!/files\n' > "$root/run.sh"; chmod 0755 "$root/run.sh"
echo "$root"
