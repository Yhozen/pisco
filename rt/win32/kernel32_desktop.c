// SPDX-License-Identifier: GPL-2.0-or-later
// kernel32 for Windows desktop titles: what an old program's own C runtime asks for when it starts (the
// system's version, start-up information, the environment, character tables of the ANSI code page) and the
// ANSI functions the core runtime does not have. The system we describe is Windows XP SP3, Western European
// (code page 1252): what a 2005 title was written for.
#include <ctype.h>
#include "../rt.h"

IMP(kernel32, GetVersionExA) {
  u64 p = ARG(0); u32 size = M32(p);
  if (size < 148) { SET_LAST_ERROR(122); RET(0); return; }
  memset((void *)GP(p + 4), 0, size - 4);
  M32(p + 4) = 5; M32(p + 8) = 1; M32(p + 12) = 2600; M32(p + 16) = 2; // 5.1.2600, VER_PLATFORM_WIN32_NT
  strcpy(GSTR(p + 20), "Service Pack 3");
  if (size >= 156) { M16(p + 148) = 3; M16(p + 152) = 0x100; M8(p + 154) = 1; } // SP 3, single-user terminal services, a workstation
  RET(1);
}
IMP(kernel32, GetVersion) { RET(0x0A280105u); }

// One heap: a private heap is the process heap under another handle (the handle is a block of its own)
IMP(kernel32, HeapCreate) { RET((uptr)calloc(1, 64)); }
IMP(kernel32, HeapDestroy) { RET(1); }
IMP(kernel32, SetHandleCount) { RET(ARG(0)); }

// STARTUPINFOA: nothing inherited, no window placement asked for

static const char *const environment[] = {"ALLUSERSPROFILE=C:\\Documents and Settings\\All Users", "COMPUTERNAME=PISCO", "ComSpec=C:\\WINDOWS\\system32\\cmd.exe",
  "NUMBER_OF_PROCESSORS=1", "OS=Windows_NT", "Path=C:\\WINDOWS\\system32;C:\\WINDOWS", "SystemDrive=C:", "SystemRoot=C:\\WINDOWS", "TEMP=C:\\WINDOWS\\Temp",
  "TMP=C:\\WINDOWS\\Temp", "USERNAME=player", "USERPROFILE=C:\\Documents and Settings\\player", "windir=C:\\WINDOWS"};
#define ENV_COUNT (sizeof environment / sizeof environment[0])
IMP(kernel32, GetEnvironmentStringsW) {
  size_t n = 1; for (unsigned i = 0; i < ENV_COUNT; i++) n += strlen(environment[i]) + 1;
  wchar16 *w = malloc(2 * n), *o = w;
  for (unsigned i = 0; i < ENV_COUNT; i++) { for (const char *s = environment[i]; *s; s++) *o++ = (u8)*s; *o++ = 0; }
  *o = 0; RET((uptr)w);
}
IMP(kernel32, GetEnvironmentStrings) {
  size_t n = 1; for (unsigned i = 0; i < ENV_COUNT; i++) n += strlen(environment[i]) + 1;
  char *a = malloc(n), *o = a;
  for (unsigned i = 0; i < ENV_COUNT; i++) { strcpy(o, environment[i]); o += strlen(o) + 1; }
  *o = 0; RET((uptr)a);
}
IMP(kernel32, FreeEnvironmentStringsW) { free((void *)GP(ARG(0))); RET(1); }
IMP(kernel32, FreeEnvironmentStringsA) { free((void *)GP(ARG(0))); RET(1); }

// Characters of code page 1252 as Latin-1 (0x80..0x9F differ in a few symbols no table here depends on)
static int is_upper(u32 ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 0xC0 && ch <= 0xDE && ch != 0xD7); }
static int is_lower(u32 ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 0xDF && ch <= 0xFF && ch != 0xF7); }
static u32 to_lower(u32 ch) { return is_upper(ch) ? ch + 32 : ch; }
static u32 to_upper(u32 ch) { return (is_lower(ch) && ch != 0xDF && ch != 0xFF) ? ch - 32 : ch; }
static u16 ctype1(u32 ch) { // C1_*: UPPER 1, LOWER 2, DIGIT 4, SPACE 8, PUNCT 0x10, CNTRL 0x20, BLANK 0x40, XDIGIT 0x80, ALPHA 0x100, DEFINED 0x200
  u16 t = 0x200;
  if (ch > 0xFF) return t;
  if (is_upper(ch)) t |= 0x101; if (is_lower(ch)) t |= 0x102;
  if (ch >= '0' && ch <= '9') t |= 0x84;
  if ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')) t |= 0x80;
  if (ch == ' ' || (ch >= 9 && ch <= 13) || ch == 0xA0) t |= 8;
  if (ch == ' ' || ch == 9 || ch == 0xA0) t |= 0x40;
  if (ch < 0x20 || ch == 0x7F || (ch >= 0x80 && ch <= 0x9F)) t |= 0x20;
  if ((ch > 0x20 && ch < 0x7F && !isalnum((int)ch)) || (ch > 0xA0 && ch < 0xC0) || ch == 0xD7 || ch == 0xF7) t |= 0x10;
  return t;
}
IMP(kernel32, GetStringTypeW) {
  u32 type = (u32)ARG(0); const wchar16 *s = GWSTR(ARG(1)); s32 n = (s32)ARG(2); u64 out = ARG(3);
  if (n < 0) n = (s32)rt_wcslen(s) + 1;
  for (s32 i = 0; i < n; i++) M16(out + 2 * i) = type == 1 ? ctype1(s[i]) : 0; // CT_CTYPE1; the other two tables: nothing set
  RET(1);
}
IMP(kernel32, GetStringTypeA) {
  u32 type = (u32)ARG(1); const u8 *s = (const u8 *)GP(ARG(2)); s32 n = (s32)ARG(3); u64 out = ARG(4);
  if (n < 0) n = (s32)strlen((const char *)s) + 1;
  for (s32 i = 0; i < n; i++) M16(out + 2 * i) = type == 1 ? ctype1(s[i]) : 0;
  RET(1);
}
// LCMAP_LOWERCASE 0x100, LCMAP_UPPERCASE 0x200; anything else copies. A size of 0 asks how much room is needed.
IMP(kernel32, LCMapStringW) {
  u32 flags = (u32)ARG(1); const wchar16 *s = GWSTR(ARG(2)); s32 n = (s32)ARG(3), cap = (s32)ARG(5); wchar16 *out = GWSTR(ARG(4));
  if (n < 0) n = (s32)rt_wcslen(s) + 1;
  if (!cap) { RET(n); return; }
  if (cap < n) { SET_LAST_ERROR(122); RET(0); return; }
  for (s32 i = 0; i < n; i++) out[i] = (flags & 0x100) ? to_lower(s[i]) : (flags & 0x200) ? to_upper(s[i]) : s[i];
  RET(n);
}
IMP(kernel32, LCMapStringA) {
  u32 flags = (u32)ARG(1); const u8 *s = (const u8 *)GP(ARG(2)); s32 n = (s32)ARG(3), cap = (s32)ARG(5); u8 *out = (u8 *)GP(ARG(4));
  if (n < 0) n = (s32)strlen((const char *)s) + 1;
  if (!cap) { RET(n); return; }
  if (cap < n) { SET_LAST_ERROR(122); RET(0); return; }
  for (s32 i = 0; i < n; i++) out[i] = (flags & 0x100) ? to_lower(s[i]) : (flags & 0x200) ? to_upper(s[i]) : s[i];
  RET(n);
}
// CSTR_LESS_THAN 1, CSTR_EQUAL 2, CSTR_GREATER_THAN 3; NORM_IGNORECASE is flag 1. Character by character, by code.
IMP(kernel32, CompareStringA) {
  u32 flags = (u32)ARG(1); const u8 *a = (const u8 *)GP(ARG(2)), *b = (const u8 *)GP(ARG(4)); s32 na = (s32)ARG(3), nb = (s32)ARG(5);
  if (na < 0) na = (s32)strlen((const char *)a); if (nb < 0) nb = (s32)strlen((const char *)b);
  for (s32 i = 0; i < na && i < nb; i++) {
    u32 x = a[i], y = b[i];
    if (flags & 1) { x = to_lower(x); y = to_lower(y); }
    if (x != y) { RET(x < y ? 1 : 3); return; }
  }
  RET(na == nb ? 2 : na < nb ? 1 : 3);
}
IMP(kernel32, GetUserDefaultLCID) { RET(0x0409); } // English (United States)
IMP(kernel32, IsValidLocale) { RET(1); }
IMP(kernel32, IsValidCodePage) { u32 cp = (u32)ARG(0); RET(cp == 1252 || cp == 437 || cp == 65001 || cp == 20127); }

// There is no memory protection: any address that is not null can be read and written
IMP(kernel32, IsBadReadPtr) { RET(!ARG(0)); }
IMP(kernel32, IsBadWritePtr) { RET(!ARG(0)); }
IMP(kernel32, IsBadCodePtr) { RET(!ARG(0)); }

IMP(kernel32, InterlockedIncrement) { RET(__atomic_add_fetch((u32 *)GP(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
IMP(kernel32, InterlockedDecrement) { RET(__atomic_sub_fetch((u32 *)GP(ARG(0)), 1, __ATOMIC_SEQ_CST)); }
IMP(kernel32, InterlockedExchange) { RET(__atomic_exchange_n((u32 *)GP(ARG(0)), (u32)ARG(1), __ATOMIC_SEQ_CST)); }
