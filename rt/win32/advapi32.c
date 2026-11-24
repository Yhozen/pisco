// SPDX-License-Identifier: GPL-2.0-or-later
// The registry, for Windows desktop titles. For now it is empty: every key is missing, as on a system where
// nothing was configured (D3DX looks for HKLM\Software\Microsoft\Direct3D and carries on without it).
// Values a title's recipe sets come with the recipes.
#include "../rt.h"

IMP(advapi32, RegOpenKeyA) { if (rt_trace) rt_log("registry: no key %s", ARG(1) ? GSTR(ARG(1)) : ""); if (ARG(2)) M32(ARG(2)) = 0; RET(2); } // ERROR_FILE_NOT_FOUND
IMP(advapi32, RegOpenKeyExA) { if (rt_trace) rt_log("registry: no key %s", ARG(1) ? GSTR(ARG(1)) : ""); if (ARG(4)) M32(ARG(4)) = 0; RET(2); }
IMP(advapi32, RegQueryValueExA) { RET(2); }
IMP(advapi32, RegCloseKey) { RET(0); }
