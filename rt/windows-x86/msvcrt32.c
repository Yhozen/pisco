// SPDX-License-Identifier: GPL-2.0-or-later
// msvcrt.dll as the 32-bit DLLs of old Windows games call it (D3DX first: docs/tag.md lists its imports), beyond the
// core's msvcrt.c: the x87 "_CI" maths, whose argument and result travel on the x87 stack; the C++ runtime's entry
// points (operator new and delete, type_info's destructor, terminate, the frame handler of the fs:[0] chain); the
// low-level I/O tables; conversions, sorting, random numbers, the date and time as text.
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "../rt.h"

// ---- the x87 "_CI" functions: ST0 (and ST1 for two arguments) in, ST0 out, nothing on the stack ----
// (each written out: tools/gen-imports.sh collects the IMP(...) names as they are written)
IMP(msvcrt, _CIsin) { double x = ST(0); ST(0) = sin(x); }
IMP(msvcrt, _CIcos) { double x = ST(0); ST(0) = cos(x); }
IMP(msvcrt, _CItan) { double x = ST(0); ST(0) = tan(x); }
IMP(msvcrt, _CIasin) { double x = ST(0); ST(0) = asin(x); }
IMP(msvcrt, _CIacos) { double x = ST(0); ST(0) = acos(x); }
IMP(msvcrt, _CIatan) { double x = ST(0); ST(0) = atan(x); }
IMP(msvcrt, _CIsinh) { double x = ST(0); ST(0) = sinh(x); }
IMP(msvcrt, _CIcosh) { double x = ST(0); ST(0) = cosh(x); }
IMP(msvcrt, _CItanh) { double x = ST(0); ST(0) = tanh(x); }
IMP(msvcrt, _CIexp) { double x = ST(0); ST(0) = exp(x); }
IMP(msvcrt, _CIlog) { double x = ST(0); ST(0) = log(x); }
IMP(msvcrt, _CIlog10) { double x = ST(0); ST(0) = log10(x); }
IMP(msvcrt, _CIsqrt) { double x = ST(0); ST(0) = sqrt(x); }
IMP(msvcrt, _CIatan2) { double y = ST(0), x = ST(1); fpop(c); ST(0) = atan2(x, y); }   // atan2(ST1, ST0)
IMP(msvcrt, _CIpow) { double y = ST(0), x = ST(1); fpop(c); ST(0) = pow(x, y); }   // pow(ST1, ST0)
IMP(msvcrt, _CIfmod) { double y = ST(0), x = ST(1); fpop(c); ST(0) = fmod(x, y); }   // fmod(ST1, ST0)

// ---- the floating-point environment and classes ----
IMP(msvcrt, _clearfp) { u32 s = c->fsw & 0x3F; c->fsw &= ~0x3Fu; // the x87's exception flags, in float.h's bits
  RET((s & 1 ? 0x10 : 0) | (s & 2 ? 0x80000 : 0) | (s & 4 ? 0x08 : 0) | (s & 8 ? 0x04 : 0) | (s & 16 ? 0x02 : 0) | (s & 32 ? 0x01 : 0)); }
IMP(msvcrt, _statusfp) { u32 s = c->fsw & 0x3F; RET((s & 1 ? 0x10 : 0) | (s & 4 ? 0x08 : 0) | (s & 8 ? 0x04 : 0) | (s & 16 ? 0x02 : 0) | (s & 32 ? 0x01 : 0)); }
IMP(msvcrt, _finite) { double v = DA(0); RET(isfinite(v) ? 1 : 0); }
IMP(msvcrt, _isnan) { double v = DA(0); RET(isnan(v) ? 1 : 0); }
IMP(msvcrt, _fpclass) { // _FPCLASS_*: SNAN 1, QNAN 2, NINF 4, NN 8, ND 0x10, NZ 0x20, PZ 0x40, PD 0x80, PN 0x100, PINF 0x200
  double v = DA(0); u64 b; memcpy(&b, &v, 8); int neg = (int)(b >> 63);
  if (isnan(v)) { RET(b & (1ull << 51) ? 2 : 1); return; }
  if (isinf(v)) { RET(neg ? 4 : 0x200); return; }
  if (v == 0) { RET(neg ? 0x20 : 0x40); return; }
  if (fpclassify(v) == FP_SUBNORMAL) { RET(neg ? 0x10 : 0x80); return; }
  RET(neg ? 8 : 0x100);
}

// ---- the C++ runtime ----
IMP(msvcrt, op_new) { RET((uptr)calloc(1, ARG(0) ? ARG(0) : 1)); }      // operator new(size_t)
IMP(msvcrt, op_delete) { free((void *)GP(ARG(0))); }                     // operator delete(void *)
IMP(msvcrt, type_info_dtor) {}                                           // type_info::~type_info (thiscall: this in ECX)
IMP(msvcrt, terminate) { rt_fatal("terminate() called from 0x%x", M32(R[RSP])); }
IMP(msvcrt, _purecall) { rt_fatal("a pure virtual function was called (from 0x%x)", M32(R[RSP])); }
// The census of Tag found no exception thrown in normal play (docs/tag.md): a throw stops the run with what it is
IMP(msvcrt, _CxxThrowException) { // (object, ThrowInfo*): the first catchable type's name
  u64 ti = ARG(1), cta = ti ? M32(ti + 12) : 0, ct = cta ? M32(cta + 4) : 0, td = ct ? M32(ct + 4) : 0;
  rt_fatal("a C++ exception was thrown (%s) from 0x%x: 32-bit exception dispatch is not implemented", td ? GSTR(td + 8) : "?", M32(R[RSP]));
}
// __CxxFrameHandler(record, frame, context, dispatcher): only an exception's dispatch would call it
IMP(msvcrt, __CxxFrameHandler) { RET(1); }   // ExceptionContinueSearch
IMP(msvcrt, _XcptFilter) { RET(0); }         // (code, pointers): EXCEPTION_CONTINUE_SEARCH
IMP(msvcrt, __dllonexit) { RET(ARG(0)); }    // (function, &begin, &end): a lifted DLL is never unloaded
ALIAS(msvcrt, "??2@YAPAXI@Z", op_new, 0);
ALIAS(msvcrt, "??3@YAXPAX@Z", op_delete, 0);
ALIAS(msvcrt, "??1type_info@@UAE@XZ", type_info_dtor, 0);
ALIAS(msvcrt, "?terminate@@YAXXZ", terminate, 0);

// ---- locks (the C runtime's numbered ones) ----
static pthread_mutex_t crt_locks[64];
static pthread_once_t crt_locks_once = PTHREAD_ONCE_INIT;
static void crt_locks_init(void) { pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE); for (int i = 0; i < 64; i++) pthread_mutex_init(&crt_locks[i], &a); }
IMP(msvcrt, _lock) { pthread_once(&crt_locks_once, crt_locks_init); pthread_mutex_lock(&crt_locks[ARG(0) & 63]); }
IMP(msvcrt, _unlock) { pthread_once(&crt_locks_once, crt_locks_init); pthread_mutex_unlock(&crt_locks[ARG(0) & 63]); }

// ---- low-level I/O: the descriptor table code reaches through __pioinfo (blocks of 32 ioinfo entries of 36
// bytes: the OS handle, flags, ...), and the FILEs of _iob (32 bytes each on x86) ----
u64 rt_msvcrt_iob(int i);
static u64 pioinfo, badioinfo;
u64 rt_msvcrt32_data(const char *name) {
  if (!strcmp(name, "msvcrt.dll!_iob")) return rt_msvcrt_iob(0);
  if (!strcmp(name, "msvcrt.dll!__pioinfo") || !strcmp(name, "msvcrt.dll!__badioinfo")) {
    if (!pioinfo) {
      badioinfo = (uptr)calloc(1, 36); M32(badioinfo) = 0xFFFFFFFFu; M8(badioinfo + 4) = 0x0A;   // FOPEN | FTEXT, no handle
      u64 block = (uptr)calloc(32, 36);
      for (u32 i = 0; i < 3; i++) { M32(block + 36 * i) = 0xFFFFFFF6u - i; M8(block + 36 * i + 4) = 0x81 | (i ? 0x40 : 0); } // the standard handles: FOPEN | FTEXT, FDEV
      pioinfo = (uptr)calloc(64, 4); M32(pioinfo) = (u32)block;
    }
    return !strcmp(name, "msvcrt.dll!__pioinfo") ? pioinfo : badioinfo;
  }
  return 0;
}
IMP(msvcrt, _isatty) { RET(0); }   // no descriptor is a console here

// ---- conversions ----
static void itoa_into(u64 v, int neg, char *dst, u32 radix) {
  char tmp[72]; int n = 0;
  if (radix < 2 || radix > 36) radix = 10;
  do { u32 d = (u32)(v % radix); tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= radix; } while (v);
  int k = 0; if (neg) dst[k++] = '-';
  while (n) dst[k++] = tmp[--n];
  dst[k] = 0;
}
IMP(msvcrt, _itoa) { s32 v = (s32)ARG(0); u32 r = (u32)ARG(2); int neg = r == 10 && v < 0; itoa_into(neg ? (u64)(-(s64)v) : (u32)v, neg, GSTR(ARG(1)), r); RET(ARG(1)); }
IMP(msvcrt, _ultoa) { itoa_into((u32)ARG(0), 0, GSTR(ARG(1)), (u32)ARG(2)); RET(ARG(1)); }
IMP(msvcrt, _ltoa) { s32 v = (s32)ARG(0); u32 r = (u32)ARG(2); int neg = r == 10 && v < 0; itoa_into(neg ? (u64)(-(s64)v) : (u32)v, neg, GSTR(ARG(1)), r); RET(ARG(1)); }
IMP(msvcrt, atol) { RET((s64)atoi(GSTR(ARG(0)))); }
IMP(msvcrt, atof) { DRET(atof(GSTR(ARG(0)))); }
IMP(msvcrt, isleadbyte) { RET(0); }   // code page 1252 has no lead bytes
IMP(msvcrt, wctomb) { u16 w = (u16)ARG(1); if (!ARG(0)) { RET(0); return; } if (w > 255) { RET(-1); return; } M8(ARG(0)) = (u8)w; RET(1); }
IMP(msvcrt, wcstombs) { // (dst, src, bytes): code page 1252's first 256 characters are Latin-1's
  const wchar16 *s = GWSTR(ARG(1)); u64 n = ARG(2), k = 0;
  for (; s[k]; k++) { if (s[k] > 255) { RET(-1); return; } if (ARG(0)) { if (k >= n) break; M8(ARG(0) + k) = (u8)s[k]; } }
  if (ARG(0) && k < n) M8(ARG(0) + k) = 0;
  RET(k);
}
int rt_vsscanf(Cpu *c, const char *in, const char *fmt, u64 va);
IMP(msvcrt, sscanf) { RET((s64)rt_vsscanf(c, GSTR(ARG(0)), GSTR(ARG(1)), R[RSP] + 12)); }   // (buffer, format, ...): cdecl, the pointers after them

// ---- sorting with the app's comparison (cdecl: it leaves the arguments) ----
typedef struct { Cpu *c; u64 base, size, cmp; } Sort;
static int compare(Sort *s, u64 i, u64 j) { return (s32)rt_guest_call(s->c, s->cmp, s->base + i * s->size, s->base + j * s->size, 0, 0); }
static void swap_items(Sort *s, u64 i, u64 j) { u8 *a = (u8 *)GP(s->base + i * s->size), *b = (u8 *)GP(s->base + j * s->size); for (u64 k = 0; k < s->size; k++) { u8 t = a[k]; a[k] = b[k]; b[k] = t; } }
static void heap_down(Sort *s, u64 root, u64 n) {
  for (;;) { u64 l = 2 * root + 1, m = root; if (l < n && compare(s, l, m) > 0) m = l; if (l + 1 < n && compare(s, l + 1, m) > 0) m = l + 1; if (m == root) return; swap_items(s, root, m); root = m; }
}
IMP(msvcrt, qsort) { // (base, count, size, compare): a heap sort (no recursion; the order of equal items is not promised by qsort)
  Sort s = {c, ARG(0), ARG(2), ARG(3)}; u64 n = ARG(1);
  if (n < 2 || !s.size) return;
  for (u64 i = n / 2; i-- > 0;) heap_down(&s, i, n);
  for (u64 e = n - 1; e > 0; e--) { swap_items(&s, 0, e); heap_down(&s, 0, e); }
}

// ---- rand: the C runtime's linear congruential generator ----
static u32 rand_state = 1;
IMP(msvcrt, srand) { rand_state = (u32)ARG(0); }
IMP(msvcrt, rand) { rand_state = rand_state * 214013u + 2531011u; RET((rand_state >> 16) & 0x7FFF); }

// ---- the date and time as text, and temporary names ----
static struct tm now_tm(void) { time_t t = (time_t)(rt_uptime_us() / 1000000) + 1199145600; struct tm m; gmtime_r(&t, &m); return m; } // the app's clock from 2008-01-01
IMP(msvcrt, _strdate) { struct tm m = now_tm(); snprintf(GSTR(ARG(0)), 9, "%02d/%02d/%02d", m.tm_mon + 1, m.tm_mday, m.tm_year % 100); RET(ARG(0)); }
IMP(msvcrt, _strtime) { struct tm m = now_tm(); snprintf(GSTR(ARG(0)), 9, "%02d:%02d:%02d", m.tm_hour, m.tm_min, m.tm_sec); RET(ARG(0)); }
IMP(msvcrt, _tempnam) { // (folder, prefix): a malloc'd name in the temporary folder
  static u32 seq; char b[300]; const char *dir = ARG(0) && *GSTR(ARG(0)) ? GSTR(ARG(0)) : "C:\\WINDOWS\\Temp";
  snprintf(b, sizeof b, "%s\\%s%u.tmp", dir, ARG(1) ? GSTR(ARG(1)) : "", ++seq);
  RET((uptr)strdup(b));
}
