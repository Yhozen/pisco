#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# setup.sh: says whether the oracle's QEMU is there, and how to get it when it is not (a system package: nothing is
# fetched). The runs recorded in the repository were made with QEMU 11.0.0.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd); . "$here/env.sh"
if command -v "$QEMU" > /dev/null; then echo "qemu: $("$QEMU" --version | head -1)"; exit 0; fi
echo "setup.sh: $QEMU is missing (the oracle of the Linux app needs it):" >&2
echo "  Fedora, Amazon Linux:  sudo dnf install qemu-system-x86-core" >&2
echo "  Debian, Ubuntu:        sudo apt-get install qemu-system-x86" >&2
exit 1
