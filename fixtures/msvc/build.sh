#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# fixtures/msvc/build.sh: eh.exe, C++ exceptions with the MSVC ABI (__CxxFrameHandler3), built by clang-cl and
# lld-link with import libraries made from lib/*.def: no Windows SDK and no Microsoft file. tests/gfx/st.sh runs it
# with Microsoft's vcruntime140.dll beside it. Needs clang-cl, lld-link, llvm-dlltool (LLVM=: their folder).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
bin() { if [ -n "${LLVM:-}" ]; then echo "$LLVM/$1"; else command -v "$1"; fi; }
for d in kernel32 vcruntime140; do "$(bin llvm-dlltool)" -m i386:x86-64 -d "$here/lib/$d.def" -l "$t/$d.lib"; done
for f in eh support; do "$(bin clang-cl)" --target=x86_64-pc-windows-msvc /nologo /O2 /EHs /GR /GS- /c "$here/$f.cpp" "/Fo$t/$f.obj"; done
"$(bin lld-link)" /nologo "/out:$here/eh.exe" /entry:start /subsystem:console /nodefaultlib "$t/eh.obj" "$t/support.obj" "$t/kernel32.lib" "$t/vcruntime140.lib"
