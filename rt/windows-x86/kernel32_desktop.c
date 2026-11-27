// SPDX-License-Identifier: GPL-2.0-or-later
// kernel32 for Windows desktop titles: what an old program's own C runtime asks for when it starts (the
// system's version, start-up information, the environment, character tables of the ANSI code page) and the
// ANSI functions the core runtime does not have. The system we describe is Windows XP SP3, Western European
// (code page 1252): what a 2005 title was written for.
#include <ctype.h>
#include "../rt.h"
#include "../vfs.h"

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

// ---- the locale: English (United States), code page 1252, as the rest of this file says ----
// GetLocaleInfoA / W (lcid, LCTYPE, buffer, characters): the values an old C runtime and its setlocale read
static const char *locale_value(u32 type) {
  switch (type & 0xFFFF) {   // without LOCALE_NOUSEROVERRIDE / LOCALE_RETURN_NUMBER
  case 0x01: return "0409";            // LOCALE_ILANGUAGE
  case 0x03: return "ENU";             // LOCALE_SABBREVLANGNAME
  case 0x05: return "1";               // LOCALE_ICOUNTRY
  case 0x07: return "USA";             // LOCALE_SABBREVCTRYNAME
  case 0x0B: return "437";             // LOCALE_IDEFAULTCODEPAGE (OEM)
  case 0x0E: return ".";               // LOCALE_SDECIMAL
  case 0x0F: return ",";               // LOCALE_STHOUSAND
  case 0x10: return "3;0";             // LOCALE_SGROUPING
  case 0x14: return "$";               // LOCALE_SCURRENCY
  case 0x1001: return "English";       // LOCALE_SENGLANGUAGE
  case 0x1002: return "United States"; // LOCALE_SENGCOUNTRY
  case 0x1004: return "1252";          // LOCALE_IDEFAULTANSICODEPAGE
  case 0x59: return "en";              // LOCALE_SISO639LANGNAME
  case 0x5A: return "US";              // LOCALE_SISO3166CTRYNAME
  default: return 0;
  }
}
IMP(kernel32, GetLocaleInfoA) {
  const char *v = locale_value((u32)ARG(1)); u32 n = (u32)ARG(3);
  if (!v) { SET_LAST_ERROR(1004); RET(0); return; }   // ERROR_INVALID_FLAGS
  u32 need = (u32)strlen(v) + 1;
  if (!n) { RET(need); return; }
  if (n < need) { SET_LAST_ERROR(122); RET(0); return; }
  memcpy(GSTR(ARG(2)), v, need); RET(need);
}
IMP(kernel32, GetLocaleInfoW) {
  const char *v = locale_value((u32)ARG(1)); u32 n = (u32)ARG(3);
  if (!v) { SET_LAST_ERROR(1004); RET(0); return; }
  u32 need = (u32)strlen(v) + 1;
  if (!n) { RET(need); return; }
  if (n < need) { SET_LAST_ERROR(122); RET(0); return; }
  for (u32 i = 0; i < need; i++) M16(ARG(2) + 2 * i) = (u8)v[i];
  RET(need);
}
// (callback, flags): the callback sees each installed locale's id as hex text; there is one
IMP(kernel32, EnumSystemLocalesA) {
  static u64 text; if (!text) { text = (uptr)calloc(1, 16); strcpy(GSTR(text), "00000409"); }
  if (ARG(0)) rt_guest_call(c, ARG(0), text, 0, 0, 0);
  RET(1);
}
IMP(kernel32, GetConsoleCP) { RET(437); }

// ---- resources: the .rsrc of a lifted image, found through its PE header's data directory 2 ----
static u32 resource_dir(const ModuleDef *m, u32 *size) { // the directory's RVA, read from the image's file (its headers are not loaded)
  u8 h[1024];
  if (host_read_file(m->name, (u32)strlen(m->name), 0, sizeof h, (u64)(uptr)h) != sizeof h) return 0;
  u32 pe; memcpy(&pe, h + 0x3C, 4);
  if (pe > sizeof h - 0x100 || memcmp(h + pe, "PE\0\0", 4)) return 0;
  u32 rva; memcpy(&rva, h + pe + 24 + 96 + 8 * 2, 4); memcpy(size, h + pe + 24 + 96 + 8 * 2 + 4, 4);
  return rva;
}
// one level of the directory: the entry whose id (a number) or name (UTF-16, case-insensitive) matches `key`
static u32 resource_find(u64 root, u64 dir, u64 key, int wide) {
  u16 named = M16(dir + 12), ids = M16(dir + 14); u64 e = dir + 16;
  char want[128] = ""; u32 want_id = 0;
  if (key < 0x10000) want_id = (u32)key;
  else {
    if (wide) { const wchar16 *w = GWSTR(key); size_t i = 0; for (; w[i] && i < 127; i++) want[i] = (char)w[i]; want[i] = 0; }
    else snprintf(want, sizeof want, "%s", GSTR(key));
    if (want[0] == '#') { want_id = (u32)atoi(want + 1); want[0] = 0; }
  }
  for (u32 i = 0; i < (u32)named + ids; i++, e += 8) {
    u32 name = M32(e), off = M32(e + 4);
    if (i < named) {
      if (!want[0]) continue;
      u64 s = root + (name & 0x7FFFFFFF); u16 n = M16(s); if (strlen(want) != n) continue;
      int same = 1; for (u16 k = 0; k < n && same; k++) same = toupper((u8)want[k]) == toupper((int)(M16(s + 2 + 2 * k) & 0xFF));
      if (same) return off;
    } else if (!want[0] && name == want_id) return off;
  }
  return 0xFFFFFFFFu;
}
// FindResource(module, name, type): the data entry of the first language found
static u64 find_resource(Cpu *c, u64 module, u64 name, u64 type, int wide) {
  const ModuleDef *m = module ? rt_module_of(module) : &rt_modules[0];
  u32 size, rva = m ? resource_dir(m, &size) : 0;
  if (!rva) { SET_LAST_ERROR(1812); return 0; }   // ERROR_RESOURCE_DATA_NOT_FOUND
  u64 root = m->base + rva, at = root;
  u32 o = resource_find(root, at, type, wide); if (o == 0xFFFFFFFFu || !(o >> 31)) { SET_LAST_ERROR(1813); return 0; }   // ERROR_RESOURCE_TYPE_NOT_FOUND
  at = root + (o & 0x7FFFFFFF);
  o = resource_find(root, at, name, wide); if (o == 0xFFFFFFFFu || !(o >> 31)) { SET_LAST_ERROR(1814); return 0; }    // ERROR_RESOURCE_NAME_NOT_FOUND
  at = root + (o & 0x7FFFFFFF);
  if (M16(at + 12) + M16(at + 14) == 0) { SET_LAST_ERROR(1815); return 0; }
  o = M32(at + 16 + 4);                          // the first language
  if (o >> 31) { SET_LAST_ERROR(1815); return 0; }
  return root + o;                               // IMAGE_RESOURCE_DATA_ENTRY: data RVA, size, code page
}
IMP(kernel32, FindResourceA) { RET(find_resource(c, ARG(0), ARG(1), ARG(2), 0)); }
IMP(kernel32, FindResourceW) { RET(find_resource(c, ARG(0), ARG(1), ARG(2), 1)); }
IMP(kernel32, LoadResource) { const ModuleDef *m = ARG(0) ? rt_module_of(ARG(0)) : &rt_modules[0]; RET(m && ARG(1) ? m->base + M32(ARG(1)) : 0); }
IMP(kernel32, LockResource) { RET(ARG(0)); }
IMP(kernel32, SizeofResource) { RET(ARG(1) ? M32(ARG(1) + 4) : 0); }
IMP(kernel32, FreeResource) { RET(0); }

// ---- paths ----
static const char temp_dir[] = "C:\\WINDOWS\\Temp\\";
IMP(kernel32, GetTempPathA) { u32 n = (u32)ARG(0), need = (u32)sizeof temp_dir; if (n < need) { RET(need); return; } memcpy(GSTR(ARG(1)), temp_dir, need); RET(need - 1); }
static u32 temp_file_name(const char *path, const char *prefix, u32 unique, char *out) {
  static u32 seq; u32 u = unique ? unique : ++seq;
  snprintf(out, 260, "%s%s%.3s%X.tmp", path, *path && path[strlen(path) - 1] != '\\' ? "\\" : "", prefix, u & 0xFFFF);
  if (!unique) { u32 e; VFile *f = vfs_open(out, 0x40000000u, 2, &e); if (f) vfs_close(f); }   // created empty, as Windows does
  return u & 0xFFFF;
}
IMP(kernel32, GetTempFileNameA) { char out[300]; u32 u = temp_file_name(GSTR(ARG(0)), ARG(1) ? GSTR(ARG(1)) : "", (u32)ARG(2), out); strcpy(GSTR(ARG(3)), out); RET(u); }
IMP(kernel32, GetTempFileNameW) {
  char *p = rt_narrow(GWSTR(ARG(0))), *x = ARG(1) ? rt_narrow(GWSTR(ARG(1))) : strdup(""), out[300];
  u32 u = temp_file_name(p, x, (u32)ARG(2), out); free(p); free(x);
  size_t i = 0; for (; out[i]; i++) M16(ARG(3) + 2 * i) = (u8)out[i]; M16(ARG(3) + 2 * i) = 0;
  RET(u);
}
// (name, characters, buffer, &file part): a relative name is under the title's folder, where it runs
IMP(kernel32, GetFullPathNameA) {
  const char *n = GSTR(ARG(0)); char full[300];
  if ((n[0] && n[1] == ':') || (n[0] == '\\' && n[1] == '\\')) snprintf(full, sizeof full, "%s", n);
  else if (n[0] == '\\') snprintf(full, sizeof full, "C:%s", n);
  else snprintf(full, sizeof full, "C:\\title\\%s", n[0] == '.' && (n[1] == '\\' || n[1] == '/') ? n + 2 : n);   // the current directory (files.c)
  for (char *p = full; *p; p++) if (*p == '/') *p = '\\';
  u32 need = (u32)strlen(full) + 1;
  if ((u32)ARG(1) < need) { RET(need); return; }
  strcpy(GSTR(ARG(2)), full);
  if (ARG(3)) { char *s = strrchr(GSTR(ARG(2)), '\\'); MP(ARG(3)) = s ? ARG(2) + (u64)(s + 1 - GSTR(ARG(2))) : 0; }
  RET(need - 1);
}
IMP(kernel32, ExpandEnvironmentStringsA) { // no variables are set: the text is copied as it is
  const char *s = GSTR(ARG(0)); u32 need = (u32)strlen(s) + 1;
  if (ARG(1) && (u32)ARG(2) >= need) memcpy(GSTR(ARG(1)), s, need);
  RET(need);
}
IMP(kernel32, MoveFileA) { u32 e = vfs_rename(GSTR(ARG(0)), GSTR(ARG(1)), 0); if (e) SET_LAST_ERROR(e); RET(e == 0); }

// ---- memory, strings, atomics ----
IMP(kernel32, GlobalMemoryStatus) { // MEMORYSTATUS: a 2 GB machine with most of it free
  u64 p = ARG(0); M32(p) = 32; M32(p + 4) = 25;
  M32(p + 8) = 0x7FFFFFFFu; M32(p + 12) = 0x60000000u; M32(p + 16) = 0x7FFFFFFFu; M32(p + 20) = 0x70000000u; M32(p + 24) = 0x7FFE0000u; M32(p + 28) = 0x70000000u;
}
IMP(kernel32, InterlockedCompareExchange) { u32 want = (u32)ARG(2); __atomic_compare_exchange_n((u32 *)GP(ARG(0)), &want, (u32)ARG(1), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); RET(want); }
IMP(kernel32, IsDBCSLeadByte) { RET(0); }
IMP(kernel32, lstrcmpiA) { int r = strcasecmp(GSTR(ARG(0)), GSTR(ARG(1))); RET((u32)(r < 0 ? -1 : r > 0)); }
