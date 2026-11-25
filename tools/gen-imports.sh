#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# gen-imports.sh <guest bits> <lifted dir> <out.c> <runtime sources...>: the import table of a title's module.
# Every IMP(dll, name) the runtime sources define goes into rt_import_table (and <out dir>/imports.txt); for a
# 32-bit title also what each import removes from the stack (rt_stdcall_table): first rt/win32/stdcall.txt, then what
# the import libraries say (tools/stdcall-sizes.sh) of the runtime's functions and of the imports named in the
# lifted C (<lifted dir>/title*.c). Used by tools/build-title.sh and by `sw build` (crates/sw).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd); bits=$1; lifted=$2; out=$3; shift 3
list=$(dirname "$out")/imports.txt
grep -ho 'IMP([a-z0-9_]*, *[A-Za-z0-9_]*)' "$@" | LC_ALL=C sort -u | sed -E 's/IMP\(([a-z0-9_]+), *([A-Za-z0-9_]+)\)/\1 \2/' > "$list"
{ echo '#include "rt.h"'
  awk '{print "void imp_"$1"_"$2"(Cpu*);"}' "$list"
  echo 'typedef struct { const char *name; ImportFn fn; } ImportDef;'
  echo 'const ImportDef rt_import_table[] = {'
  awk '{print "  {\""$1".dll!"$2"\", imp_"$1"_"$2"},"}' "$list"
  echo '};'
  echo "const unsigned rt_import_table_count = sizeof rt_import_table / sizeof rt_import_table[0];"
  # 32-bit: what each import removes from the stack, for the runtime's and the title's own imports
  echo 'typedef struct { const char *name; int bytes; } StdcallDef;'
  echo 'const StdcallDef rt_stdcall_table[] = {'
  if [ "$bits" = 32 ]; then
    # first the runtime functions no import library describes (rt/win32/stdcall.txt), then what the libraries say
    { grep -v -e '^#' -e '^ *$' "$root/rt/win32/stdcall.txt"; { awk '{print $1".dll!"$2}' "$list"; grep -ho '"[^"]*![^"]*",' "$lifted"/title*.c | tr -d '",'; } | LC_ALL=C "$root/tools/stdcall-sizes.sh"; } | awk '{print "  {\""$1"\", "$2"},"}'
  fi
  echo '  {0, 0}};'
  echo "const unsigned rt_stdcall_table_count = sizeof rt_stdcall_table / sizeof rt_stdcall_table[0] - 1;"
} > "$out"
