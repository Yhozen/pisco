#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# setup.sh: fetches the oracle's portable Wine and makes the Python environment the input player needs
# (python-xlib, for XTEST). Skips what exists. See env.sh for the locations.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); . "$here/env.sh"; mkdir -p "$ORACLE_HOME"
if [ ! -x "$WINE_HOME/bin/wine" ]; then
  mkdir -p "$(dirname "$WINE_HOME")"; tmp=$(mktemp -d "$(dirname "$WINE_HOME")/.fetch.XXXXXX")
  url=https://github.com/Kron4ek/Wine-Builds/releases/download/$WINE_VERSION/wine-$WINE_VERSION-amd64-wow64.tar.xz
  echo "fetching $url"; curl -fsSL -o "$tmp/wine.tar.xz" "$url"; tar -xf "$tmp/wine.tar.xz" -C "$tmp"
  mv "$tmp/wine-$WINE_VERSION-amd64-wow64" "$WINE_HOME"; rm -rf "$tmp"
fi
if [ ! -x "$ORACLE_HOME/venv/bin/python" ]; then
  python3 -m venv "$ORACLE_HOME/venv"; "$ORACLE_HOME/venv/bin/pip" install -q python-xlib==0.33
fi
for t in Xvfb ffmpeg; do command -v $t >/dev/null || echo "setup.sh: $t is missing (the oracle needs it)" >&2; done
echo "wine: $("$WINE_HOME/bin/wine" --version)   python: $ORACLE_HOME/venv/bin/python"
