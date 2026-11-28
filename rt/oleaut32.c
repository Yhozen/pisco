// SPDX-License-Identifier: GPL-2.0-or-later
// oleaut32, for apps of every runtime: BSTRs (a 4-byte length in bytes before the characters, a final zero), the
// VARIANTs code initialises and clears, GetErrorInfo. Old apps import these by ordinal.
#include "rt.h"

static u64 bstr_new(const wchar16 *s, u32 chars) {
  u8 *p = malloc(4 + 2 * (size_t)chars + 2); if (!p) return 0;
  memcpy(p, &(u32){2 * chars}, 4); if (s) memcpy(p + 4, s, 2 * (size_t)chars); else memset(p + 4, 0, 2 * (size_t)chars);
  p[4 + 2 * chars] = p[5 + 2 * chars] = 0;
  return (uptr)p + 4;
}
IMP(oleaut32, SysAllocString) { RET(ARG(0) ? bstr_new(GWSTR(ARG(0)), (u32)rt_wcslen(GWSTR(ARG(0)))) : 0); }
IMP(oleaut32, SysAllocStringLen) { RET(bstr_new(ARG(0) ? GWSTR(ARG(0)) : 0, (u32)ARG(1))); }
IMP(oleaut32, SysFreeString) { if (ARG(0)) free((void *)GP(ARG(0) - 4)); }
IMP(oleaut32, SysStringLen) { RET(ARG(0) ? M32(ARG(0) - 4) / 2 : 0); }
IMP(oleaut32, GetErrorInfo) { MP(ARG(1)) = 0; RET(1); } // S_FALSE: no error object
// VARIANT: vt (2 bytes), 6 reserved, then the value; VT_BSTR (8) owns its string
IMP(oleaut32, VariantInit) { memset((void *)GP(ARG(0)), 0, 8); }
IMP(oleaut32, VariantClear) {
  u64 v = ARG(0);
  if (M16(v) == 8 && MP(v + 8)) free((void *)GP(MP(v + 8) - 4));
  M16(v) = 0; RET(0);
}
ALIAS(oleaut32, "#2", SysAllocString, 4);
ALIAS(oleaut32, "#4", SysAllocStringLen, 8);
ALIAS(oleaut32, "#6", SysFreeString, 4);
ALIAS(oleaut32, "#7", SysStringLen, 4);
ALIAS(oleaut32, "#8", VariantInit, 4);
ALIAS(oleaut32, "#9", VariantClear, 4);
