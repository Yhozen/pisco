// SPDX-License-Identifier: GPL-2.0-or-later
// ucrtbase.dll (the api-ms-win-crt-* API sets resolve here). Functions with msvcrt's semantics are found in
// msvcrt.c: the import lookup falls back to it (see find_import in process.c).
#include <wchar.h>
#include "rt.h"

extern const char *rt_command_line;
u64 rt_msvcrt_iob(int i);
u64 rt_msvcrt_argv(int *argc);
u64 rt_msvcrt_environ(void);
u64 rt_msvcrt_stream_fd(u64 file);
void rt_msvcrt_format(Cpu *c, u64 file, u64 buf, u64 cap, u64 fmt, u64 va, int *len);

static u64 commode, fmode, argc_g, argv_g, environ_g;
static void init_globals(void) {
  if (argc_g) return;
  int argc; u64 argv = rt_msvcrt_argv(&argc);
  argc_g = (uptr)calloc(1, 8); M32(argc_g) = (u32)argc;
  argv_g = (uptr)calloc(1, 8); M64(argv_g) = argv;
  environ_g = (uptr)calloc(1, 8); M64(environ_g) = rt_msvcrt_environ();
  commode = (uptr)calloc(1, 8); fmode = (uptr)calloc(1, 8);
}
IMP(ucrtbase, __acrt_iob_func) { RET(rt_msvcrt_iob((int)ARG(0))); }
IMP(ucrtbase, __p__commode) { init_globals(); RET(commode); }
IMP(ucrtbase, __p__fmode) { init_globals(); RET(fmode); }
IMP(ucrtbase, __p___argc) { init_globals(); RET(argc_g); }
IMP(ucrtbase, __p___argv) { init_globals(); RET(argv_g); }
IMP(ucrtbase, __p__environ) { init_globals(); RET(environ_g); }
IMP(ucrtbase, _configure_narrow_argv) { init_globals(); RET(0); }
IMP(ucrtbase, _configure_wide_argv) { RET(0); }
IMP(ucrtbase, _initialize_narrow_environment) { init_globals(); RET(0); }
IMP(ucrtbase, _initialize_wide_environment) { RET(0); }
IMP(ucrtbase, _get_initial_narrow_environment) { init_globals(); RET(M64(environ_g)); }
IMP(ucrtbase, _set_app_type) {}
IMP(ucrtbase, _set_invalid_parameter_handler) { RET(0); }
IMP(ucrtbase, _set_new_mode) { RET(0); }
IMP(ucrtbase, _configthreadlocale) { RET(2); }
IMP(ucrtbase, _fpreset) {}
IMP(ucrtbase, _seh_filter_exe) { RET(0); } // EXCEPTION_CONTINUE_SEARCH
IMP(ucrtbase, _lock_file) {}
IMP(ucrtbase, _unlock_file) {}
IMP(ucrtbase, _crt_atexit) { extern void imp_msvcrt_atexit(Cpu *); imp_msvcrt_atexit(c); }
IMP(ucrtbase, _register_onexit_function) {
  // an _onexit_table_t: first / last / end pointers into a malloc'd array
  u64 t = ARG(0), fn = ARG(1);
  u64 first = M64(t), last = M64(t + 8), end = M64(t + 16);
  if (last == end) { u64 n = first ? (end - first) / 8 : 0, cap = n ? 2 * n : 8;
    u64 p = (uptr)realloc((void *)GP(first), cap * 8); last = p + 8 * n; first = p; end = p + 8 * cap; }
  M64(last) = fn; M64(t) = first; M64(t + 8) = last + 8; M64(t + 16) = end;
  RET(0);
}
IMP(ucrtbase, _initialize_onexit_table) { memset((void *)GP(ARG(0)), 0, 24); RET(0); }
IMP(ucrtbase, _execute_onexit_table) {
  u64 t = ARG(0), first = M64(t), last = M64(t + 8);
  while (last > first) { last -= 8; u64 f = M64(last); if (f) rt_guest_call(c, f, 0, 0, 0, 0); }
  RET(0);
}
IMP(ucrtbase, _initterm_e) {
  for (u64 p = ARG(0), e = ARG(1); p < e; p += 8) { u64 f = M64(p); if (f) { u32 r = (u32)rt_guest_call(c, f, 0, 0, 0, 0); if (r) { RET(r); return; } } }
  RET(0);
}
IMP(ucrtbase, strnlen) { RET(strnlen(GSTR(ARG(0)), ARG(1))); }
IMP(ucrtbase, wcsnlen) { const wchar16 *s = GWSTR(ARG(0)); u64 n = 0; while (n < ARG(1) && s[n]) n++; RET(n); }
IMP(ucrtbase, mbrtowc) { if (!ARG(1)) { RET(0); return; } u8 ch = M8(ARG(1)); if (ARG(0)) M16(ARG(0)) = ch; RET(ch ? 1 : 0); }
IMP(ucrtbase, wcrtomb) { if (!ARG(0)) { RET(1); return; } u16 w = (u16)ARG(1); M8(ARG(0)) = w < 128 ? (u8)w : '?'; RET(1); }
IMP(ucrtbase, _errno) { RET(THREAD(c)->teb + TEB_ERRNO); }

// printf family: __stdio_common_v*printf(options, ..., format, locale, va_list)
// option bit 1 (_CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR): C99 snprintf results
IMP(ucrtbase, __stdio_common_vfprintf) { int n; rt_msvcrt_format(c, ARG(1), 0, 0, ARG(2), ARG(4), &n); RET(n); }
IMP(ucrtbase, __stdio_common_vsprintf) {
  u64 opt = ARG(0), buf = ARG(1), cap = ARG(2); int n;
  rt_msvcrt_format(c, 0, buf, cap, ARG(3), M64(R[RSP] + 0x30), &n);
  if (!buf || !cap) { RET(n); return; }
  if ((u64)n < cap) { M8(buf + n) = 0; RET(n); return; }
  M8(buf + cap - 1) = 0;
  RET((opt & 2) ? (u64)n : (u64)-1);
}
IMP(ucrtbase, __stdio_common_vsnprintf_s) {
  u64 buf = ARG(1), cap = ARG(2), max = ARG(3); int n;
  rt_msvcrt_format(c, 0, buf, cap < max ? cap : max + 1, M64(R[RSP] + 0x28), M64(R[RSP] + 0x38), &n);
  if ((u64)n < cap && (u64)n <= max) { M8(buf + n) = 0; RET(n); } else { if (cap) M8(buf + (max < cap ? max : cap - 1)) = 0; RET(-1); }
}
IMP(ucrtbase, __stdio_common_vsprintf_s) {
  u64 buf = ARG(1), cap = ARG(2); int n;
  rt_msvcrt_format(c, 0, buf, cap, ARG(3), M64(R[RSP] + 0x30), &n);
  if ((u64)n < cap) { M8(buf + n) = 0; RET(n); } else { if (cap) M8(buf) = 0; RET(-1); }
}
IMP(ucrtbase, __stdio_common_vfwprintf) { RET(0); }
IMP(ucrtbase, __stdio_common_vswprintf) { if (ARG(1) && ARG(2)) M16(ARG(1)) = 0; RET(0); }
IMP(ucrtbase, _set_fmode) { init_globals(); M32(fmode) = (u32)ARG(0); RET(0); }
IMP(ucrtbase, __setusermatherr) {}
IMP(ucrtbase, _query_app_type) { RET(1); }
IMP(ucrtbase, terminate) { rt_fatal("terminate() called from 0x%llx", M64(R[RSP])); }
IMP(ucrtbase, _invalid_parameter_noinfo) { rt_fatal("CRT invalid parameter (from 0x%llx)", M64(R[RSP])); }
IMP(ucrtbase, _invalid_parameter_noinfo_noreturn) { rt_fatal("CRT invalid parameter (from 0x%llx)", M64(R[RSP])); }

// ---- strings, numbers, files Doom-era C code asks for ----
#include <ctype.h>
#include <strings.h>
#include "vfs.h"
int rt_file_write(int fd, const void *p, u32 n);
IMP(ucrtbase, _stricmp) { RET((s64)strcasecmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
IMP(ucrtbase, _strnicmp) { RET((s64)strncasecmp(GSTR(ARG(0)), GSTR(ARG(1)), (size_t)ARG(2))); }
IMP(ucrtbase, putchar) { char ch = (char)ARG(0); rt_file_write(1, &ch, 1); RET((u8)ch); }
IMP(ucrtbase, atof) { double v = atof(GSTR(ARG(0))); memset(&X[0], 0, 16); X[0].pd[0] = v; }
IMP(ucrtbase, _mkdir) { u32 e = vfs_mkdir(GSTR(ARG(0))); RET(e && e != 183 ? -1 : 0); }
IMP(ucrtbase, remove) { RET(vfs_remove(GSTR(ARG(0)), 0) ? -1 : 0); }
IMP(ucrtbase, rename) { RET(vfs_rename(GSTR(ARG(0)), GSTR(ARG(1)), 0) ? -1 : 0); }
// sscanf: each conversion of the format is done by the host's sscanf on the rest of the input, then stored
// through the guest pointer from the argument list (a Win64 va_list: 8-byte slots)
IMP(ucrtbase, __stdio_common_vsscanf) { // (options, buffer, count, format, locale, va_list)
  const char *in = GSTR(ARG(1)), *f = GSTR(ARG(3)); u64 va = ARG(5); int assigned = 0;
  while (*f) {
    if (isspace((u8)*f)) { while (isspace((u8)*in)) in++; f++; continue; }
    if (*f != '%' || f[1] == '%') { if (*f == '%') f++; if (*in != *f) break; in++; f++; continue; }
    const char *spec = f++; int suppress = 0; if (*f == '*') { suppress = 1; f++; }
    while (isdigit((u8)*f)) f++;
    int size = 0; // 0 int, 1 short, 2 char, 3 long long
    if (*f == 'h') { size = 1; f++; if (*f == 'h') { size = 2; f++; } } else if (*f == 'l') { f++; if (*f == 'l') { size = 3; f++; } else size = 4; } else if (*f == 'I' && f[1] == '6' && f[2] == '4') { size = 3; f += 3; } else if (*f == 'z' || *f == 'j') { size = 3; f++; }
    char conv = *f; const char *end = f + 1;
    if (conv == '[') { end = f + 1; if (*end == '^') end++; if (*end == ']') end++; while (*end && *end != ']') end++; if (*end) end++; }
    if (!conv) break;
    char one[64]; size_t n = (size_t)(end - spec); if (n > 50) break;
    memcpy(one, spec, n); one[n] = 0;
    // the host conversion with our own sizes: widths and suppression stay as written
    char host[80]; int k = 0; for (size_t i = 0; i < n; i++) { char ch = one[i]; if (ch == 'h' || ch == 'l' || ch == 'I' || ch == 'z' || ch == 'j' || (ch == '6' && one[i + 1] == '4' && i > 0 && one[i - 1] == 'I') || (ch == '4' && i > 1 && one[i - 1] == '6' && one[i - 2] == 'I')) continue; host[k++] = ch; }
    host[k] = 0;
    char fmt2[96]; snprintf(fmt2, sizeof fmt2, "%s%%n", host);
    int used = -1;
    if (conv == 'n') { if (!suppress) { u64 p = M64(va); va += 8; M32(p) = (u32)(in - GSTR(ARG(1))); } f = end; continue; }
    if (suppress) { if (strchr("diouxX", conv)) { long long v; snprintf(fmt2, sizeof fmt2, "%%lld%%n"); sscanf(in, fmt2, &v, &used); } else if (strchr("feEgGaA", conv)) { double v; sscanf(in, "%lf%n", &v, &used); } else { char tmp[1024]; snprintf(fmt2, sizeof fmt2, "%s%%n", host); sscanf(in, fmt2, tmp, &used); } if (used < 0) break; in += used; f = end; continue; }
    u64 p = M64(va); va += 8;
    if (strchr("di", conv)) { long long v; char h2[96]; snprintf(h2, sizeof h2, "%.*sll%c%%n", (int)(strlen(host) - 1), host, conv); if (sscanf(in, h2, &v, &used) < 1 || used < 0) break; if (size == 3) M64(p) = (u64)v; else if (size == 1) M16(p) = (u16)v; else if (size == 2) M8(p) = (u8)v; else M32(p) = (u32)v; }
    else if (strchr("ouxX", conv)) { unsigned long long v; char h2[96]; snprintf(h2, sizeof h2, "%.*sll%c%%n", (int)(strlen(host) - 1), host, conv); if (sscanf(in, h2, &v, &used) < 1 || used < 0) break; if (size == 3) M64(p) = v; else if (size == 1) M16(p) = (u16)v; else if (size == 2) M8(p) = (u8)v; else M32(p) = (u32)v; }
    else if (strchr("feEgGaA", conv)) { double v; if (sscanf(in, "%lf%n", &v, &used) < 1 || used < 0) break; if (size == 4) MF64(p) = v; else MF32(p) = (float)v; }
    else { if (sscanf(in, fmt2, GSTR(p), &used) < 1 || used < 0) break; }
    in += used; assigned++; f = end;
  }
  RET(assigned);
}
IMP(user32, MessageBoxW) { char *t = rt_narrow(GWSTR(ARG(1))), *cap = ARG(2) ? rt_narrow(GWSTR(ARG(2))) : 0; rt_log("message box: %s%s%s", cap ? cap : "", cap ? ": " : "", t); free(t); free(cap); RET(1); }
IMP(user32, MessageBoxA) { rt_log("message box: %s", GSTR(ARG(1))); RET(1); }
