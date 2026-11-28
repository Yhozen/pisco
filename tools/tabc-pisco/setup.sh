#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# setup.sh [out]: tabc 1.0.0 built from source with PISCO in it (docs/research/tabc-pisco.md), in out (default
# out/tabc-pisco): tabc's source at its 1.0.0 commit with tabc.patch applied (an amd64 image allowed, the page's
# pisco.js, the supervisor's pisco/ route and its lifting on the host), tabc and its guest's init built by Go, and
# the share it serves: the page, unkernel's SDK with the patched kernel (build.sh), xterm, init, and PISCO's files.
# SHARE_STOCK=1 also makes share-stock: the same tabc's own page and kernel, unchanged, for the comparison.
# Then:
#   TABC_SHARE=$out/share TABC_IMAGE_ARCH=amd64 TABC_PISCO_LIFT=tools/tabc-pisco/lift-side.sh \
#     TABC_CHROMIUM=$(command -v google-chrome) $out/tabc run alpine:3.24.4 uname -m
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../.." && pwd); out=${1:-$root/out/tabc-pisco}
TABC_COMMIT=8b7e7920606d5fc20197c19da92a1f308504dbe2   # tabc 1.0.0
"$here/build.sh" "$out"
src=$out/tabc-src
if [ ! -d "$src/.git" ]; then git clone -q https://github.com/fzakaria/tabc "$src"; fi
git -C "$src" checkout -q -f "$TABC_COMMIT" && git -C "$src" clean -qfd
git -C "$src" apply "$here/tabc.patch"
(cd "$src" && go build -o "$out/tabc" ./cmd/tabc && CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -ldflags "-s -w" -o "$out/tabc-init" ./cmd/tabc-init)
U=$out/unkernel/package
share() {   # share <dir> <page dir> <kernel>
  rm -rf "$1"; mkdir -p "$1/unkernel/_chunks" "$1/xterm"
  cp -r "$2" "$1/page"; cp "$out/tabc-init" "$1/init"
  cp "$U/dist/web.mjs" "$1/unkernel/"; cp "$U/dist/_chunks/core.mjs" "$1/unkernel/_chunks/"; cp "$3" "$1/unkernel/unkernel.wasm"
  cp "$U/LICENSE" "$U/LICENSE-MIT" "$U/THIRD-PARTY.md" "$1/unkernel/"
  if [ ! -f "$out/xterm/xterm.js" ]; then
    mkdir -p "$out/xterm/x" && (cd "$out/xterm/x" && npm pack -s @xterm/xterm@5.5.0 @xterm/addon-fit@0.10.0 > /dev/null \
      && tar xzf xterm-xterm-5.5.0.tgz && cp package/lib/xterm.js package/css/xterm.css package/LICENSE .. && rm -rf package \
      && tar xzf xterm-addon-fit-0.10.0.tgz && cp package/lib/addon-fit.js ..)
  fi
  cp "$out/xterm/xterm.js" "$out/xterm/xterm.css" "$out/xterm/addon-fit.js" "$out/xterm/LICENSE" "$1/xterm/"
}
share "$out/share" "$src/page" "$out/kernel.wasm"
mkdir -p "$out/share/pisco" && cp "$out/runtime.wasm" "$out/tramp.wasm" "$out/x86load" "$out/share/pisco/"
if [ "${SHARE_STOCK:-}" = 1 ]; then
  rm -rf "$out/stock" && mkdir -p "$out/stock" && git -C "$src" archive "$TABC_COMMIT" page | tar -x -C "$out/stock"
  share "$out/share-stock" "$out/stock/page" "$U/dist/unkernel.wasm"
fi
echo "$out/tabc, $out/share$([ "${SHARE_STOCK:-}" = 1 ] && echo ", $out/share-stock")"
