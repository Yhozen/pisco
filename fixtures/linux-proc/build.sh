#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/linux-proc/build.sh [root, default out/linux-proc/root]: a Linux file system with the fixture in it: bin/proc
# (statically linked with musl) and bin/script, a script that bin/proc runs through its #! line.
# Environment: MUSL (musl's folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=${1:-$here/../../out/linux-proc/root}; mkdir -p "$root/bin"
MUSL=${MUSL:-$HOME/tools/musl-1.2.5}
"$MUSL/bin/musl-gcc" -O2 -static -no-pie "$here/proc.c" -o "$root/bin/proc"
printf '#!/bin/proc --script\n' > "$root/bin/script"; chmod +x "$root/bin/script"
echo "$root/bin/proc"
