// SPDX-License-Identifier: GPL-2.0-or-later
// msvcrt.dll and shell32's CommandLineToArgvW for lifted titles; also the process's exit and the virtual CPU's
// identity. Guest pointers are runtime pointers in wasm32, so most string functions call wasi-libc directly.
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"
#include "vfs.h"

u64 rt_create_thread(Cpu *c, u64 stack, u64 start, u64 param, int suspended, u32 *tid);
const char *rt_command_line = "title.exe";

// ---- strings ----
size_t rt_wcslen(const wchar16 *s) { size_t n = 0; while (s[n]) n++; return n; }
char *rt_narrow(const wchar16 *s) {
  size_t n = rt_wcslen(s); char *o = malloc(n * 3 + 1), *p = o;
  for (size_t i = 0; i < n; i++) {
    u32 cp = s[i];
    if (cp < 0x80) *p++ = (char)cp;
    else if (cp < 0x800) { *p++ = 0xC0 | cp >> 6; *p++ = 0x80 | (cp & 0x3F); }
    else { *p++ = 0xE0 | cp >> 12; *p++ = 0x80 | ((cp >> 6) & 0x3F); *p++ = 0x80 | (cp & 0x3F); }
  }
  *p = 0; return o;
}
wchar16 *rt_widen_guest(const char *s) {
  size_t n = strlen(s); wchar16 *o = malloc((n + 1) * 2);
  for (size_t i = 0; i <= n; i++) o[i] = (u8)s[i];
  return o;
}

// ---- files ----
int rt_file_write(int fd, const void *p, u32 n) { u32 done = 0; while (done < n) { ssize_t k = write(fd, (const char *)p + done, n - done); if (k <= 0) return done ? (int)done : -1; done += (u32)k; } return (int)done; }
int rt_file_read(int fd, void *p, u32 n) { ssize_t k = read(fd, p, n); return (int)k; }

// FILE*: a 48-byte guest block whose address maps to one of these
typedef struct { u64 g; int fd, eof, err, unget; VFile *vf; int append; } Stream;
static Stream streams[256];
static u64 iob;
#define FILE_BYTES (PTR_SIZE == 4 ? 32ull : 48ull)   // sizeof(FILE): what &_iob[n] steps by
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static Stream *stream(u64 g) { for (int i = 0; i < 256; i++) if (streams[i].g == g && g) return &streams[i]; return 0; }
static u64 stream_new(int fd) {
  pthread_mutex_lock(&stream_lock);
  for (int i = 3; i < 256; i++) if (!streams[i].g) { streams[i] = (Stream){(uptr)calloc(1, 48), fd, 0, 0, -1, 0, 0}; pthread_mutex_unlock(&stream_lock); return streams[i].g; }
  pthread_mutex_unlock(&stream_lock);
  return 0;
}
// a stream is a host descriptor (the standard ones) or a file of the title's file system
static int swrite(Stream *s, const void *p, u32 n) {
  if (!s->vf) return rt_file_write(s->fd, p, n);
  if (s->append) *vfs_pos(s->vf) = vfs_size(s->vf);
  return (int)vfs_write(s->vf, p, n);
}
static int sread(Stream *s, void *p, u32 n) { return s->vf ? (int)vfs_read(s->vf, p, n) : rt_file_read(s->fd, p, n); }
static int sputc(Stream *s, int ch) { u8 b = (u8)ch; return swrite(s, &b, 1) == 1 ? (u8)ch : -1; }
static int sgetc(Stream *s) {
  if (s->unget >= 0) { int v = s->unget; s->unget = -1; return v; }
  u8 b; if (sread(s, &b, 1) != 1) { s->eof = 1; return -1; }
  return b;
}
static u64 stream_open(Cpu *c, const char *name, const char *mode) {
  int plus = strchr(mode, '+') != 0; u32 access = 0x80000000u, disp = 3;
  if (mode[0] == 'w') { access = 0x40000000u | (plus ? 0x80000000u : 0); disp = 2; }
  else if (mode[0] == 'a') { access = 0x40000000u | (plus ? 0x80000000u : 0); disp = 4; }
  else if (plus) access |= 0x40000000u;
  u32 e; VFile *f = vfs_open(name, access, disp, &e);
  if (!f || vfs_is_dir(f)) { if (f) vfs_close(f); M32(THREAD(c)->teb + TEB_ERRNO) = e == 2 || e == 3 ? 2 : 13; return 0; }
  u64 g = stream_new(-1); Stream *s = stream(g); s->vf = f; s->append = mode[0] == 'a';
  return g;
}

// ---- printf: the arguments from the call (from argument `idx` on) or from a va_list (`va`) ----
// Win64: every argument takes an 8-byte slot. 32-bit: int, char, pointers 4 bytes, double and long long 8.
typedef struct { Cpu *c; int idx; u64 va; int exp3; } Args; // exp3: msvcrt.dll's exponents of three digits (1e+004); ucrt's have two
static u64 next_arg(Args *a, int bytes) {
#if GUEST_BITS == 32
  Cpu *c = a->c; u64 at = a->va ? a->va : R[RSP] + 4 + 4 * (u64)a->idx;
  u64 v = bytes == 8 ? M64(at) : M32(at);
  if (a->va) a->va += bytes == 8 ? 8 : 4; else a->idx += bytes == 8 ? 2 : 1;
  return v;
#else
  (void)bytes; Cpu *c = a->c;
  if (a->va) { u64 v = M64(a->va); a->va += 8; return v; }
  return ARG(a->idx++);
#endif
}
typedef struct { char *buf; size_t cap, len; Stream *out; } Sink;
static void put(Sink *s, const char *p, size_t n) {
  if (s->out) { swrite(s->out, p, (u32)n); s->len += n; return; }
  for (size_t i = 0; i < n; i++, s->len++) if (s->buf && s->len < s->cap) s->buf[s->len] = p[i];
}
// msvcrt.dll writes an exponent with at least three digits (1.5e+004); the host's has two. `spec` is a whole
// conversion ("%-+12.3e"): formatted again without its width, the digit added, then padded to the width.
static int exp3(char *tmp, size_t cap, const char *spec, double v) {
  char bare[32]; size_t k = 0, i = 1; int width = 0, left = 0, zero = 0; bare[k++] = '%';
  for (; strchr("-+ #0", spec[i]) && spec[i]; i++) { if (spec[i] == '-') left = 1; else if (spec[i] == '0') zero = 1; else bare[k++] = spec[i]; }
  for (; isdigit((u8)spec[i]); i++) width = width * 10 + (spec[i] - '0');
  while (spec[i] && k < sizeof bare - 1) bare[k++] = spec[i++];
  bare[k] = 0;
  char body[400]; int n = snprintf(body, sizeof body, bare, v);
  if (n < 0 || n >= (int)sizeof body - 2) return snprintf(tmp, cap, spec, v);
  char *x = strpbrk(body, "eE");
  if (x && (x[1] == '+' || x[1] == '-') && isdigit((u8)x[2]) && isdigit((u8)x[3]) && !x[4]) { memmove(x + 3, x + 2, 3); x[2] = '0'; n++; }
  int pad = width > n ? width - n : 0, sign = body[0] == '-' || body[0] == '+' || body[0] == ' ';
  if ((size_t)(n + pad) >= cap) return snprintf(tmp, cap, spec, v);
  if (left) { memcpy(tmp, body, (size_t)n); memset(tmp + n, ' ', (size_t)pad); }
  else if (zero && isdigit((u8)body[sign])) { memcpy(tmp, body, (size_t)sign); memset(tmp + sign, '0', (size_t)pad); memcpy(tmp + sign + pad, body + sign, (size_t)(n - sign)); }
  else { memset(tmp, ' ', (size_t)pad); memcpy(tmp + pad, body, (size_t)n); }
  tmp[n + pad] = 0; return n + pad;
}
static void format(Sink *out, const char *f, Args *a) {
  char spec[32], tmp[512];
  while (*f) {
    if (*f != '%') { const char *e = strchr(f, '%'); size_t n = e ? (size_t)(e - f) : strlen(f); put(out, f, n); f += n; continue; }
    if (f[1] == '%') { put(out, "%", 1); f += 2; continue; }
    size_t k = 0; spec[k++] = *f++;
    while (strchr("-+ #0", *f) && k < 8) spec[k++] = *f++;
    int width = -1, prec = -1;
    if (*f == '*') { width = (s32)next_arg(a, 4); f++; } else while (isdigit((u8)*f) && k < 14) spec[k++] = *f++;
    if (*f == '.') { f++; if (*f == '*') { prec = (s32)next_arg(a, 4); f++; } else { prec = 0; while (isdigit((u8)*f)) prec = prec * 10 + (*f++ - '0'); } }
    int big = 0, wide = 0, half = 0;
    for (;;) {
      if (*f == 'l' && f[1] == 'l') { big = 8; f += 2; }
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { big = 8; f += 3; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { f += 3; }
      else if (*f == 'j') { big = 8; f++; }
      else if (*f == 'I' || *f == 'z' || *f == 't') { big = PTR_SIZE; f++; }
      else if (*f == 'l' || *f == 'w') { wide = 1; f++; }
      else if (*f == 'h') { half++; f++; }
      else if (*f == 'L') f++;
      else break;
    }
    char conv = *f ? *f++ : 0;
    if (width >= 0) k += (size_t)snprintf(spec + k, sizeof spec - k, "%d", width);
    if (prec >= 0) k += (size_t)snprintf(spec + k, sizeof spec - k, ".%d", prec);
    int n = 0;
    switch (conv) {
    case 'd': case 'i': {
      s64 v = big == 8 ? (s64)next_arg(a, 8) : half == 1 ? (s16)next_arg(a, 4) : half > 1 ? (s8)next_arg(a, 4) : (s32)next_arg(a, 4);
      memcpy(spec + k, "lld", 4); n = snprintf(tmp, sizeof tmp, spec, v); break;
    }
    case 'u': case 'x': case 'X': case 'o': {
      u64 v = big == 8 ? next_arg(a, 8) : half == 1 ? (u16)next_arg(a, 4) : half > 1 ? (u8)next_arg(a, 4) : (u32)next_arg(a, 4);
      spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = conv; spec[k + 3] = 0; n = snprintf(tmp, sizeof tmp, spec, v); break;
    }
    case 'p': n = snprintf(tmp, sizeof tmp, PTR_SIZE == 8 ? "%016llX" : "%08llX", next_arg(a, PTR_SIZE)); break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
      u64 bits = next_arg(a, 8); double v; memcpy(&v, &bits, 8);
      spec[k] = conv; spec[k + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, v);
      if (a->exp3 && conv != 'a' && conv != 'A') n = exp3(tmp, sizeof tmp, spec, v);
      break;
    }
    case 'c': case 'C': { char ch = (char)next_arg(a, 4); spec[k] = 'c'; spec[k + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, ch); break; }
    case 's': case 'S': {
      u64 p = next_arg(a, PTR_SIZE); char *str = 0;
      if (!p) str = strdup("(null)"); else if (wide || conv == 'S') str = rt_narrow(GWSTR(p));
      spec[k] = 's'; spec[k + 1] = 0;
      int need = snprintf(0, 0, spec, str ? str : GSTR(p));
      char *big_buf = need < (int)sizeof tmp ? tmp : malloc((size_t)need + 1);
      snprintf(big_buf, (size_t)need + 1, spec, str ? str : GSTR(p));
      put(out, big_buf, (size_t)need);
      if (big_buf != tmp) free(big_buf);
      free(str); continue;
    }
    case 'n': { u64 p = next_arg(a, PTR_SIZE); if (p) M32(p) = (u32)out->len; continue; }
    default: put(out, spec, k); if (conv) put(out, &conv, 1); continue;
    }
    if (n > (int)sizeof tmp - 1) n = sizeof tmp - 1;
    put(out, tmp, (size_t)n);
  }
}

// ---- startup, exit ----
static u64 atexit_fns[256]; static int atexit_n;
static pthread_mutex_t exit_lock = PTHREAD_MUTEX_INITIALIZER;
static int exiting;
static void run_atexit(Cpu *c) {
  pthread_mutex_lock(&exit_lock);
  if (exiting) { pthread_mutex_unlock(&exit_lock); return; }
  exiting = 1;
  pthread_mutex_unlock(&exit_lock);
  while (atexit_n > 0) rt_guest_call(c, atexit_fns[--atexit_n], 0, 0, 0, 0);
}

static u64 data_initenv, data_commode, data_fmode, lconv_g, locale_c;
// the 32-bit C runtime's tables (rt/windows-x86/msvcrt32.c: _iob, __pioinfo); other bases have none
__attribute__((weak)) u64 rt_msvcrt32_data(const char *name) { (void)name; return 0; }
u64 rt_data_import(const char *name) {
  { u64 d = rt_msvcrt32_data(name); if (d) return d; }
  if (!strcmp(name, "msvcrt.dll!__initenv")) return data_initenv;
  if (!strcmp(name, "msvcrt.dll!_commode")) return data_commode;
  if (!strcmp(name, "msvcrt.dll!_fmode")) return data_fmode;
  return 0;
}
void rt_crt_init(void) {
  data_initenv = (uptr)calloc(1, 16); MP(data_initenv) = (uptr)calloc(1, 16);
  data_commode = (uptr)calloc(1, 8); data_fmode = (uptr)calloc(1, 8);
  iob = (uptr)calloc(3, FILE_BYTES);
  for (int i = 0; i < 3; i++) streams[i] = (Stream){iob + FILE_BYTES * (u64)i, i, 0, 0, -1};
  locale_c = (uptr)strdup("C");
  // struct lconv: ten char* fields, then the monetary chars
  lconv_g = (uptr)calloc(1, 128);
  MP(lconv_g) = (uptr)strdup(".");
  for (int i = 1; i < 10; i++) MP(lconv_g + PTR_SIZE * i) = (uptr)strdup("");
  memset((void *)GP(lconv_g + 80), 127, 8);
}

static u64 argv_g; static int argc_n;
u64 rt_msvcrt_argv(int *out_argc) {
  // argv from the command line, split on spaces (quotes keep spaces)
  int argc = 0;
  if (!argv_g) {
    char *cl = strdup(rt_command_line), *p = cl; u64 *v = calloc(64, 8);
    while (*p && argc < 63) {
      while (*p == ' ') p++;
      if (!*p) break;
      char *start = p, *o = p; int q = 0;
      while (*p && (q || *p != ' ')) { if (*p == '"') q = !q; else *o++ = *p; p++; }
      if (*p) p++;
      *o = 0; v[argc++] = (uptr)strdup(start);
    }
    argv_g = (uptr)v; free(cl); argc_n = argc;
  }
  *out_argc = argc_n;
  return argv_g;
}
u64 rt_msvcrt_environ(void) { return MP(data_initenv); }
u64 rt_msvcrt_iob(int i) { return iob + FILE_BYTES * (u64)i; }
IMP(msvcrt, __getmainargs) {
  int argc; u64 argv = rt_msvcrt_argv(&argc);
  M32(ARG(0)) = (u32)argc; MP(ARG(1)) = argv; MP(ARG(2)) = MP(data_initenv);
  RET(0);
}
// formats to a stream (file != 0) or a buffer; *len: the full length
void rt_msvcrt_format(Cpu *c, u64 file, u64 buf, u64 cap, u64 fmt, u64 va, int *len) {
  Sink o = {GSTR(buf), buf ? cap : 0, 0, file ? stream(file) : 0};
  if (file && !o.out) { *len = -1; return; }
  Args a = {c, 0, va, 0}; format(&o, GSTR(fmt), &a);
  *len = (int)o.len;
}
IMP(msvcrt, __set_app_type) {}
IMP(msvcrt, __setusermatherr) {}
IMP(msvcrt, _initterm) { for (u64 p = ARG(0), e = ARG(1); p < e; p += PTR_SIZE) { u64 f = MP(p); if (f) rt_guest_call(c, f, 0, 0, 0, 0); } }
IMP(msvcrt, atexit) { pthread_mutex_lock(&exit_lock); if (atexit_n < 256) atexit_fns[atexit_n++] = ARG(0); pthread_mutex_unlock(&exit_lock); RET(0); }
IMP(msvcrt, _onexit) { imp_msvcrt_atexit(c); RET(ARG(0)); }
IMP(msvcrt, exit) { u32 code = (u32)ARG(0); run_atexit(c); rt_exit_process(code); }
IMP(msvcrt, _exit) { rt_exit_process((u32)ARG(0)); }
IMP(msvcrt, _cexit) { run_atexit(c); }
IMP(msvcrt, _amsg_exit) { rt_fatal("runtime error R60%02llu", ARG(0)); }
IMP(msvcrt, abort) { rt_fatal("abort() called from 0x%llx", (u64)MP(R[RSP])); }
IMP(msvcrt, _assert) { rt_fatal("assertion failed: %s (%s:%u)", GSTR(ARG(0)), GSTR(ARG(1)), (u32)ARG(2)); }
IMP(msvcrt, signal) { RET(0); }
IMP(msvcrt, _errno) { RET(THREAD(c)->teb + TEB_ERRNO); }
IMP(msvcrt, __iob_func) { RET(iob); }
IMP(msvcrt, _beginthreadex) { u32 tid = 0; u64 h = rt_create_thread(c, ARG(1), ARG(2), ARG(3), (ARG(4) & 4) != 0, &tid); if (ARG(5)) M32(ARG(5)) = tid; RET(h); }
IMP(msvcrt, _endthreadex) { rt_thread_exit(c, (u32)ARG(0)); }
#if GUEST_BITS == 32
// setjmp / longjmp for 32-bit code. MSVC's jmp_buf: Ebp, Ebx, Edi, Esi, Esp, Eip, the SEH registration, TryLevel,
// a cookie ("VC20"), UnwindFunc, UnwindData[6]; the last word of UnwindData keeps the entry stack pointer of the
// lifted function the setjmp is in. The lifter makes the return address of each call to a setjmp a landing pad
// of that function, so its frame has a landing (rt/cpu.h LANDING): longjmp restores the registers and resumes
// there, as an exception resumes at a pad (rt/seh.c), with setjmp's result in EAX.
static void save_jmp(Cpu *c, u64 b) {
  M32(b) = (u32)R[RBP]; M32(b + 4) = (u32)R[RBX]; M32(b + 8) = (u32)R[RDI]; M32(b + 12) = (u32)R[RSI];
  M32(b + 16) = (u32)R[RSP] + 4; M32(b + 20) = M32(R[RSP]);          // the caller's ESP after the call, and where it returns
  M32(b + 24) = M32(THREAD(c)->teb + TEB_SEH); M32(b + 28) = 0; M32(b + 32) = 0x56433230u; M32(b + 36) = 0;
  M32(b + 60) = c->landing ? (u32)c->landing->entry_rsp : 0;
}
IMP(msvcrt, _setjmp3) { save_jmp(c, ARG(0)); RET(0); }   // (jmp_buf, count, ...): the extra arguments are for SEH unwinding
IMP(msvcrt, _setjmp) { save_jmp(c, ARG(0)); RET(0); }
IMP(msvcrt, longjmp) {
  u64 b = ARG(0); u32 v = (u32)ARG(1) ? (u32)ARG(1) : 1, want = M32(b + 60);
  Landing *l = c->landing;
  while (l && (u32)l->entry_rsp != want) l = l->prev;
  if (!want || !l) rt_fatal("longjmp to 0x%x: the frame its setjmp was called in (entry ESP 0x%x) has no landing", M32(b + 20), want);
  R[RBP] = M32(b); R[RBX] = M32(b + 4); R[RDI] = M32(b + 8); R[RSI] = M32(b + 12); R[RSP] = M32(b + 16); R[RAX] = v;
  M32(THREAD(c)->teb + TEB_SEH) = M32(b + 24);   // handlers registered since the setjmp are gone with their frames
  c->landing = l; l->resume = M32(b + 20);
  if (rt_trace) rt_log("longjmp to 0x%x (%u)", M32(b + 20), v);
  longjmp(*(jmp_buf *)l->jb, 1);
}
#else
IMP(msvcrt, _setjmp3) { RET(0); } // 64-bit: longjmp is not supported yet, a setjmp that only ever returns once
IMP(msvcrt, _setjmp) { RET(0); }
IMP(msvcrt, longjmp) { rt_fatal("longjmp is not implemented yet (from 0x%llx)", (u64)MP(R[RSP])); }
#endif

// ---- memory ----
IMP(msvcrt, malloc) { RET((uptr)calloc(1, ARG(0) ? ARG(0) : 1)); } // zeroed, as fresh pages from the system are: titles that read memory they never wrote see what they would natively
IMP(msvcrt, calloc) { RET((uptr)calloc(ARG(0) ? ARG(0) : 1, ARG(1) ? ARG(1) : 1)); }
IMP(msvcrt, realloc) { RET((uptr)realloc((void *)GP(ARG(0)), ARG(1))); }
IMP(msvcrt, free) { free((void *)GP(ARG(0))); }
IMP(msvcrt, _aligned_malloc) { u64 al = ARG(1) < 16 ? 16 : ARG(1); RET((uptr)aligned_alloc(al, (ARG(0) + al - 1) & ~(al - 1))); }
IMP(msvcrt, _aligned_free) { free((void *)GP(ARG(0))); }
IMP(msvcrt, _strdup) { RET((uptr)strdup(GSTR(ARG(0)))); }
IMP(msvcrt, memchr) { RET((uptr)memchr((void *)GP(ARG(0)), (int)ARG(1), ARG(2))); }
IMP(msvcrt, memcmp) { RET((s64)memcmp((void *)GP(ARG(0)), (void *)GP(ARG(1)), ARG(2))); }
IMP(msvcrt, memcpy) { memcpy((void *)GP(ARG(0)), (void *)GP(ARG(1)), ARG(2)); RET(ARG(0)); }
IMP(msvcrt, memmove) { memmove((void *)GP(ARG(0)), (void *)GP(ARG(1)), ARG(2)); RET(ARG(0)); }
IMP(msvcrt, memset) { memset((void *)GP(ARG(0)), (int)ARG(1), ARG(2)); RET(ARG(0)); }
IMP(msvcrt, strlen) { RET(strlen(GSTR(ARG(0)))); }
IMP(msvcrt, strchr) { RET((uptr)strchr(GSTR(ARG(0)), (int)ARG(1))); }
IMP(msvcrt, strrchr) { RET((uptr)strrchr(GSTR(ARG(0)), (int)ARG(1))); }
IMP(msvcrt, strcmp) { RET((s64)strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
IMP(msvcrt, strncmp) { RET((s64)strncmp(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2))); }
IMP(msvcrt, strcpy) { strcpy(GSTR(ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
IMP(msvcrt, strncpy) { strncpy(GSTR(ARG(0)), GSTR(ARG(1)), ARG(2)); RET(ARG(0)); }
IMP(msvcrt, strcat) { strcat(GSTR(ARG(0)), GSTR(ARG(1))); RET(ARG(0)); }
IMP(msvcrt, strstr) { RET((uptr)strstr(GSTR(ARG(0)), GSTR(ARG(1)))); }
IMP(msvcrt, strcoll) { RET((s64)strcmp(GSTR(ARG(0)), GSTR(ARG(1)))); }
IMP(msvcrt, strxfrm) { size_t n = strlen(GSTR(ARG(1))); if (ARG(2) > n) strcpy(GSTR(ARG(0)), GSTR(ARG(1))); RET(n); }
IMP(msvcrt, strerror) { RET((uptr)strerror((int)ARG(0))); }
IMP(msvcrt, getenv) { RET(0); }
// Windows' long is 32 bits, as in wasm32
static void set_end(Cpu *c, u64 endp, const char *start, char *end) { if (endp) MP(endp) = (uptr)end; (void)start; }
IMP(msvcrt, strtol) { char *e; long v = strtol(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET((s64)(s32)v); }
IMP(msvcrt, strtoul) { char *e; unsigned long v = strtoul(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET((u32)v); }
IMP(msvcrt, _strtoi64) { char *e; long long v = strtoll(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET(v); }
IMP(msvcrt, _strtoui64) { char *e; unsigned long long v = strtoull(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET(v); }
IMP(msvcrt, strtod) { char *e; double v = strtod(GSTR(ARG(0)), &e); set_end(c, ARG(1), 0, e); DRET(v); }
IMP(msvcrt, atoi) { RET((s64)atoi(GSTR(ARG(0)))); }
IMP(msvcrt, tolower) { RET((s64)tolower((int)ARG(0))); }
IMP(msvcrt, toupper) { RET((s64)toupper((int)ARG(0))); }
IMP(msvcrt, islower) { RET(islower((int)ARG(0)) ? 2 : 0); }
IMP(msvcrt, isupper) { RET(isupper((int)ARG(0)) ? 1 : 0); }
IMP(msvcrt, isspace) { RET(isspace((int)ARG(0)) ? 8 : 0); }
IMP(msvcrt, isdigit) { RET(isdigit((int)ARG(0)) ? 4 : 0); }
IMP(msvcrt, isalpha) { RET(isalpha((int)ARG(0)) ? 0x103 : 0); }
IMP(msvcrt, isalnum) { RET(isalnum((int)ARG(0)) ? 0x107 : 0); }
IMP(msvcrt, isxdigit) { RET(isxdigit((int)ARG(0)) ? 0x80 : 0); }
IMP(msvcrt, ceil) { DRET(ceil(DA(0))); }
IMP(msvcrt, floor) { DRET(floor(DA(0))); }
IMP(msvcrt, ldexp) { DRET(ldexp(DA(0), (s32)DA_INT(1))); }
IMP(msvcrt, log10) { DRET(log10(DA(0))); }
IMP(msvcrt, log) { DRET(log(DA(0))); }
IMP(msvcrt, exp) { DRET(exp(DA(0))); }
IMP(msvcrt, pow) { DRET(pow(DA(0), DA(1))); }
IMP(msvcrt, sqrt) { DRET(sqrt(DA(0))); }

// ---- wide characters are 16 bits on Windows ----
IMP(msvcrt, wcslen) { RET(rt_wcslen(GWSTR(ARG(0)))); }
IMP(msvcrt, wcscoll) { const wchar16 *a = GWSTR(ARG(0)), *b = GWSTR(ARG(1)); while (*a && *a == *b) { a++; b++; } RET((s64)*a - (s64)*b); }
IMP(msvcrt, wcsxfrm) { size_t n = rt_wcslen(GWSTR(ARG(1))); if (ARG(2) > n) memcpy(GWSTR(ARG(0)), GWSTR(ARG(1)), (n + 1) * 2); RET(n); }
IMP(msvcrt, towlower) { u32 ch = (u16)ARG(0); RET(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch); }
IMP(msvcrt, towupper) { u32 ch = (u16)ARG(0); RET(ch >= 'a' && ch <= 'z' ? ch - 32 : ch); }
IMP(msvcrt, iswctype) {
  u32 ch = (u16)ARG(0), mask = (u16)ARG(1), m = 0;
  if (ch < 128) { if (isupper(ch)) m |= 1; if (islower(ch)) m |= 2; if (isdigit(ch)) m |= 4; if (isspace(ch)) m |= 8; if (ispunct(ch)) m |= 0x10;
    if (iscntrl(ch)) m |= 0x20; if (ch == ' ' || ch == '\t') m |= 0x40; if (isxdigit(ch)) m |= 0x80; if (isalpha(ch)) m |= 0x100; }
  RET(m & mask);
}

// ---- locale: always "C" ----
IMP(msvcrt, setlocale) { RET(locale_c); }
IMP(msvcrt, localeconv) { RET(lconv_g); }
IMP(msvcrt, ___lc_codepage_func) { RET(0); }
IMP(msvcrt, ___mb_cur_max_func) { RET(1); }
static void tm_from_guest(struct tm *t, u64 g) { memset(t, 0, sizeof *t); int *f = (int *)GP(g); t->tm_sec = f[0]; t->tm_min = f[1]; t->tm_hour = f[2]; t->tm_mday = f[3]; t->tm_mon = f[4]; t->tm_year = f[5]; t->tm_wday = f[6]; t->tm_yday = f[7]; t->tm_isdst = f[8]; }
IMP(msvcrt, strftime) { struct tm t; tm_from_guest(&t, ARG(3)); RET(strftime(GSTR(ARG(0)), ARG(1), GSTR(ARG(2)), &t)); }
IMP(msvcrt, wcsftime) {
  struct tm t; tm_from_guest(&t, ARG(3)); char *f = rt_narrow(GWSTR(ARG(2))); char buf[512];
  size_t n = strftime(buf, sizeof buf, f, &t); free(f);
  if (n >= ARG(1)) { RET(0); return; }
  for (size_t i = 0; i <= n; i++) GWSTR(ARG(0))[i] = (u8)buf[i];
  RET(n);
}

// ---- stdio ----
#define FILEARG(n) stream(ARG(n))
IMP(msvcrt, fopen) { RET(stream_open(c, GSTR(ARG(0)), GSTR(ARG(1)))); }
IMP(msvcrt, _wfopen) { char *n = rt_narrow(GWSTR(ARG(0))), *m = rt_narrow(GWSTR(ARG(1))); RET(stream_open(c, n, m)); free(n); free(m); }
IMP(msvcrt, _fsopen) { RET(stream_open(c, GSTR(ARG(0)), GSTR(ARG(1)))); } // (name, mode, share flag)
IMP(msvcrt, _wfsopen) { char *n = rt_narrow(GWSTR(ARG(0))), *m = rt_narrow(GWSTR(ARG(1))); RET(stream_open(c, n, m)); free(n); free(m); }
IMP(msvcrt, fopen_s) { u64 g = stream_open(c, GSTR(ARG(1)), GSTR(ARG(2))); MP(ARG(0)) = g; RET(g ? 0 : M32(THREAD(c)->teb + TEB_ERRNO)); }
IMP(msvcrt, _wfopen_s) { char *n = rt_narrow(GWSTR(ARG(1))), *m = rt_narrow(GWSTR(ARG(2))); u64 g = stream_open(c, n, m); free(n); free(m); MP(ARG(0)) = g; RET(g ? 0 : M32(THREAD(c)->teb + TEB_ERRNO)); }
IMP(msvcrt, fseek) { Stream *s = stream(ARG(0)); if (!s || !s->vf) { RET(-1); return; } s64 d = (s32)ARG(1); u32 how = (u32)ARG(2); u64 *pos = vfs_pos(s->vf); s64 b = how == 0 ? 0 : how == 1 ? (s64)*pos : (s64)vfs_size(s->vf); if (b + d < 0) { RET(-1); return; } *pos = (u64)(b + d); s->eof = 0; s->unget = -1; RET(0); }
IMP(msvcrt, _fseeki64) { Stream *s = stream(ARG(0)); if (!s || !s->vf) { RET(-1); return; } s64 d = (s64)ARG(1); u32 how = (u32)ARG(2); u64 *pos = vfs_pos(s->vf); s64 b = how == 0 ? 0 : how == 1 ? (s64)*pos : (s64)vfs_size(s->vf); if (b + d < 0) { RET(-1); return; } *pos = (u64)(b + d); s->eof = 0; s->unget = -1; RET(0); }
IMP(msvcrt, ftell) { Stream *s = stream(ARG(0)); RET(s && s->vf ? (s64)(s32)*vfs_pos(s->vf) - (s->unget >= 0) : -1); }
IMP(msvcrt, _ftelli64) { Stream *s = stream(ARG(0)); RET(s && s->vf ? (s64)*vfs_pos(s->vf) - (s->unget >= 0) : -1); }
IMP(msvcrt, rewind) { Stream *s = stream(ARG(0)); if (s && s->vf) { *vfs_pos(s->vf) = 0; s->eof = 0; s->unget = -1; } }
IMP(msvcrt, _fdopen) { RET(stream_new((int)ARG(0))); }
IMP(msvcrt, fclose) { Stream *s = FILEARG(0); if (!s) { RET(-1); return; } if (s->vf) { vfs_close(s->vf); s->vf = 0; s->g = 0; } else if (s->fd > 2) { close(s->fd); s->g = 0; } RET(0); }
IMP(msvcrt, fflush) { RET(0); }
IMP(msvcrt, setvbuf) { RET(0); }
IMP(msvcrt, _fileno) { Stream *s = FILEARG(0); RET(s ? (u64)s->fd : (u64)-1); }
IMP(msvcrt, _setmode) { RET(0x4000); }
u64 rt_std_handle(int fd);
IMP(msvcrt, _get_osfhandle) { s32 fd = (s32)ARG(0); RET(fd >= 0 && fd <= 2 ? rt_std_handle(fd) : 0xFFFFFFFFFFFFFFFFull); }
IMP(msvcrt, _fstat64) { memset((void *)GP(ARG(1)), 0, 56); M16(ARG(1) + 6) = 0x2000; RET(0); } // a character device
IMP(msvcrt, _filelengthi64) { RET(-1); }
IMP(msvcrt, _lseeki64) { RET(-1); }
IMP(msvcrt, _write) { RET((s64)rt_file_write((int)ARG(0), (void *)GP(ARG(1)), (u32)ARG(2))); }
IMP(msvcrt, _read) { RET((s64)rt_file_read((int)ARG(0), (void *)GP(ARG(1)), (u32)ARG(2))); }
IMP(msvcrt, fwrite) {
  Stream *s = FILEARG(3); u64 n = ARG(1) * ARG(2);
  if (!s || !n) { RET(0); return; }
  int k = swrite(s, (void *)GP(ARG(0)), (u32)n);
  RET(k < 0 ? 0 : (u64)k / ARG(1));
}
IMP(msvcrt, fread) {
  Stream *s = FILEARG(3); u64 n = ARG(1) * ARG(2), got = 0;
  if (!s || !n) { RET(0); return; }
  u8 *p = (u8 *)GP(ARG(0));
  if (s->unget >= 0) { p[got++] = (u8)s->unget; s->unget = -1; }
  while (got < n) { int k = sread(s, p + got, (u32)(n - got)); if (k <= 0) { s->eof = 1; break; } got += (u64)k; }
  RET(got / ARG(1));
}
IMP(msvcrt, fputc) { Stream *s = FILEARG(1); RET(s ? (s64)sputc(s, (int)ARG(0)) : -1); }
IMP(msvcrt, putc) { Stream *s = FILEARG(1); RET(s ? (s64)sputc(s, (int)ARG(0)) : -1); }
IMP(msvcrt, putwc) { Stream *s = FILEARG(1); RET(s ? (s64)sputc(s, (int)(u16)ARG(0)) : 0xFFFF); }
IMP(msvcrt, fputs) { Stream *s = FILEARG(1); RET(s && swrite(s, GSTR(ARG(0)), (u32)strlen(GSTR(ARG(0)))) >= 0 ? 0 : -1); }
IMP(msvcrt, puts) { const char *p = GSTR(ARG(0)); rt_file_write(1, p, (u32)strlen(p)); rt_file_write(1, "\n", 1); RET(0); }
IMP(msvcrt, getc) { Stream *s = FILEARG(0); RET(s ? (s64)sgetc(s) : -1); }
IMP(msvcrt, fgetc) { Stream *s = FILEARG(0); RET(s ? (s64)sgetc(s) : -1); }
IMP(msvcrt, getwc) { Stream *s = FILEARG(0); int v = s ? sgetc(s) : -1; RET(v < 0 ? 0xFFFF : (u64)v); }
IMP(msvcrt, ungetc) { Stream *s = FILEARG(1); if (s && (s32)ARG(0) != -1) { s->unget = (u8)ARG(0); s->eof = 0; RET((u8)ARG(0)); } else RET(-1); }
IMP(msvcrt, ungetwc) { Stream *s = FILEARG(1); if (s && (u16)ARG(0) != 0xFFFF) { s->unget = (u8)ARG(0); RET((u16)ARG(0)); } else RET(0xFFFF); }
IMP(msvcrt, feof) { Stream *s = FILEARG(0); RET(s && s->eof); }
IMP(msvcrt, ferror) { Stream *s = FILEARG(0); RET(s && s->err); }
IMP(msvcrt, fgetpos) { Stream *s = stream(ARG(0)); if (!s || !s->vf || !ARG(1)) { RET(-1); return; } M64(ARG(1)) = *vfs_pos(s->vf) - (s->unget >= 0); RET(0); } // fpos_t: a 64-bit offset
IMP(msvcrt, fsetpos) { Stream *s = stream(ARG(0)); if (!s || !s->vf || !ARG(1)) { RET(-1); return; } *vfs_pos(s->vf) = M64(ARG(1)); s->unget = -1; RET(0); }
IMP(msvcrt, vfprintf) { Stream *s = FILEARG(0); if (!s) { RET(-1); return; } Sink o = {0, 0, 0, s}; Args a = {c, 0, ARG(2), 1}; format(&o, GSTR(ARG(1)), &a); RET(o.len); }
IMP(msvcrt, fprintf) { Stream *s = FILEARG(0); if (!s) { RET(-1); return; } Sink o = {0, 0, 0, s}; Args a = {c, 2, 0, 1}; format(&o, GSTR(ARG(1)), &a); RET(o.len); }
IMP(msvcrt, printf) { Sink o = {0, 0, 0, &streams[1]}; Args a = {c, 1, 0, 1}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
IMP(msvcrt, vprintf) { Sink o = {0, 0, 0, &streams[1]}; Args a = {c, 0, ARG(1), 1}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
IMP(msvcrt, _vscprintf) { Sink o = {0}; Args a = {c, 0, ARG(1), 1}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
// msvcrt: -1 and no terminator when the text does not fit
IMP(msvcrt, _vsnprintf) {
  Sink o = {GSTR(ARG(0)), ARG(1), 0, 0}; Args a = {c, 0, ARG(3), 1}; format(&o, GSTR(ARG(2)), &a);
  if (o.len < ARG(1)) { GSTR(ARG(0))[o.len] = 0; RET(o.len); } else RET(-1);
}
IMP(msvcrt, _snprintf) {
  Sink o = {GSTR(ARG(0)), ARG(1), 0, 0}; Args a = {c, 3, 0, 1}; format(&o, GSTR(ARG(2)), &a);
  if (o.len < ARG(1)) { GSTR(ARG(0))[o.len] = 0; RET(o.len); } else RET(-1);
}
IMP(msvcrt, sprintf) { Sink o = {GSTR(ARG(0)), (size_t)-1, 0, 0}; Args a = {c, 2, 0, 1}; format(&o, GSTR(ARG(1)), &a); GSTR(ARG(0))[o.len] = 0; RET(o.len); }
IMP(msvcrt, vsprintf) { Sink o = {GSTR(ARG(0)), (size_t)-1, 0, 0}; Args a = {c, 0, ARG(2), 1}; format(&o, GSTR(ARG(1)), &a); GSTR(ARG(0))[o.len] = 0; RET(o.len); }

// ---- shell32 ----
IMP(shell32, CommandLineToArgvW) {
  char *cl = rt_narrow(GWSTR(ARG(0)));
  char *words[64]; int n = 0; char *p = cl;
  while (*p && n < 64) {
    while (*p == ' ') p++;
    if (!*p) break;
    char *s = p, *o = p; int q = 0;
    while (*p && (q || *p != ' ')) { if (*p == '"') q = !q; else *o++ = *p; p++; }
    if (*p) p++;
    *o = 0; words[n++] = s;
  }
  size_t size = 8 * (size_t)(n + 1);
  for (int i = 0; i < n; i++) size += 2 * (strlen(words[i]) + 1);
  u8 *blk = calloc(1, size); u64 *v = (u64 *)blk; wchar16 *w = (wchar16 *)(blk + 8 * (n + 1));
  for (int i = 0; i < n; i++) { v[i] = (uptr)w; for (char *q = words[i]; *q; q++) *w++ = (u8)*q; *w++ = 0; }
  free(cl);
  M32(ARG(1)) = (u32)n;
  RET((uptr)blk);
}


// ---- the rest of the C math library (DA / FA / DRET / FRET in rt.h: the argument and result registers of each width) ----
// (written out: tools/build-title.sh finds runtime functions by their IMP lines)
IMP(msvcrt, sin) { DRET(sin(DA(0))); }
IMP(msvcrt, sinf) { FRET(sinf(FA(0))); }
IMP(msvcrt, cos) { DRET(cos(DA(0))); }
IMP(msvcrt, cosf) { FRET(cosf(FA(0))); }
IMP(msvcrt, tan) { DRET(tan(DA(0))); }
IMP(msvcrt, tanf) { FRET(tanf(FA(0))); }
IMP(msvcrt, asin) { DRET(asin(DA(0))); }
IMP(msvcrt, asinf) { FRET(asinf(FA(0))); }
IMP(msvcrt, acos) { DRET(acos(DA(0))); }
IMP(msvcrt, acosf) { FRET(acosf(FA(0))); }
IMP(msvcrt, atan) { DRET(atan(DA(0))); }
IMP(msvcrt, atanf) { FRET(atanf(FA(0))); }
IMP(msvcrt, sinh) { DRET(sinh(DA(0))); }
IMP(msvcrt, sinhf) { FRET(sinhf(FA(0))); }
IMP(msvcrt, cosh) { DRET(cosh(DA(0))); }
IMP(msvcrt, coshf) { FRET(coshf(FA(0))); }
IMP(msvcrt, tanh) { DRET(tanh(DA(0))); }
IMP(msvcrt, tanhf) { FRET(tanhf(FA(0))); }
IMP(msvcrt, asinh) { DRET(asinh(DA(0))); }
IMP(msvcrt, asinhf) { FRET(asinhf(FA(0))); }
IMP(msvcrt, acosh) { DRET(acosh(DA(0))); }
IMP(msvcrt, acoshf) { FRET(acoshf(FA(0))); }
IMP(msvcrt, atanh) { DRET(atanh(DA(0))); }
IMP(msvcrt, atanhf) { FRET(atanhf(FA(0))); }
IMP(msvcrt, exp2) { DRET(exp2(DA(0))); }
IMP(msvcrt, exp2f) { FRET(exp2f(FA(0))); }
IMP(msvcrt, log2) { DRET(log2(DA(0))); }
IMP(msvcrt, log2f) { FRET(log2f(FA(0))); }
IMP(msvcrt, log1p) { DRET(log1p(DA(0))); }
IMP(msvcrt, log1pf) { FRET(log1pf(FA(0))); }
IMP(msvcrt, expm1) { DRET(expm1(DA(0))); }
IMP(msvcrt, expm1f) { FRET(expm1f(FA(0))); }
IMP(msvcrt, cbrt) { DRET(cbrt(DA(0))); }
IMP(msvcrt, cbrtf) { FRET(cbrtf(FA(0))); }
IMP(msvcrt, round) { DRET(round(DA(0))); }
IMP(msvcrt, roundf) { FRET(roundf(FA(0))); }
IMP(msvcrt, trunc) { DRET(trunc(DA(0))); }
IMP(msvcrt, truncf) { FRET(truncf(FA(0))); }
IMP(msvcrt, rint) { DRET(rint(DA(0))); }
IMP(msvcrt, rintf) { FRET(rintf(FA(0))); }
IMP(msvcrt, nearbyint) { DRET(nearbyint(DA(0))); }
IMP(msvcrt, nearbyintf) { FRET(nearbyintf(FA(0))); }
IMP(msvcrt, fabs) { DRET(fabs(DA(0))); }
IMP(msvcrt, fabsf) { FRET(fabsf(FA(0))); }
IMP(msvcrt, erf) { DRET(erf(DA(0))); }
IMP(msvcrt, erff) { FRET(erff(FA(0))); }
IMP(msvcrt, erfc) { DRET(erfc(DA(0))); }
IMP(msvcrt, erfcf) { FRET(erfcf(FA(0))); }
IMP(msvcrt, tgamma) { DRET(tgamma(DA(0))); }
IMP(msvcrt, tgammaf) { FRET(tgammaf(FA(0))); }
IMP(msvcrt, lgamma) { DRET(lgamma(DA(0))); }
IMP(msvcrt, lgammaf) { FRET(lgammaf(FA(0))); }
IMP(msvcrt, floorf) { FRET(floorf(FA(0))); }
IMP(msvcrt, ceilf) { FRET(ceilf(FA(0))); }
IMP(msvcrt, sqrtf) { FRET(sqrtf(FA(0))); }
IMP(msvcrt, expf) { FRET(expf(FA(0))); }
IMP(msvcrt, logf) { FRET(logf(FA(0))); }
IMP(msvcrt, log10f) { FRET(log10f(FA(0))); }
IMP(msvcrt, atan2) { DRET(atan2(DA(0), DA(1))); }
IMP(msvcrt, atan2f) { FRET(atan2f(FA(0), FA(1))); }
IMP(msvcrt, fmod) { DRET(fmod(DA(0), DA(1))); }
IMP(msvcrt, fmodf) { FRET(fmodf(FA(0), FA(1))); }
IMP(msvcrt, hypot) { DRET(hypot(DA(0), DA(1))); }
IMP(msvcrt, hypotf) { FRET(hypotf(FA(0), FA(1))); }
IMP(msvcrt, fmin) { DRET(fmin(DA(0), DA(1))); }
IMP(msvcrt, fminf) { FRET(fminf(FA(0), FA(1))); }
IMP(msvcrt, fmax) { DRET(fmax(DA(0), DA(1))); }
IMP(msvcrt, fmaxf) { FRET(fmaxf(FA(0), FA(1))); }
IMP(msvcrt, copysign) { DRET(copysign(DA(0), DA(1))); }
IMP(msvcrt, copysignf) { FRET(copysignf(FA(0), FA(1))); }
IMP(msvcrt, remainder) { DRET(remainder(DA(0), DA(1))); }
IMP(msvcrt, remainderf) { FRET(remainderf(FA(0), FA(1))); }
IMP(msvcrt, fdim) { DRET(fdim(DA(0), DA(1))); }
IMP(msvcrt, fdimf) { FRET(fdimf(FA(0), FA(1))); }
IMP(msvcrt, nextafter) { DRET(nextafter(DA(0), DA(1))); }
IMP(msvcrt, nextafterf) { FRET(nextafterf(FA(0), FA(1))); }
IMP(msvcrt, powf) { FRET(powf(FA(0), FA(1))); }
IMP(msvcrt, _hypot) { DRET(hypot(DA(0), DA(1))); }
IMP(msvcrt, _hypotf) { FRET(hypotf(FA(0), FA(1))); }
IMP(msvcrt, _copysign) { DRET(copysign(DA(0), DA(1))); }
IMP(msvcrt, ldexpf) { FRET(ldexpf(FA(0), (int)FA_INT(1))); }
IMP(msvcrt, frexp) { int e; DRET(frexp(DA(0), &e)); M32(DA_INT(1)) = (u32)e; }
IMP(msvcrt, modf) { double ip; DRET(modf(DA(0), &ip)); MF64(DA_INT(1)) = ip; }
IMP(msvcrt, modff) { float ip; FRET(modff(FA(0), &ip)); MF32(FA_INT(1)) = ip; }
IMP(msvcrt, lround) { RET((s64)(s32)lround(DA(0))); }
IMP(msvcrt, lroundf) { RET((s64)(s32)lroundf(FA(0))); }
IMP(msvcrt, llround) { RET64((u64)llround(DA(0))); }
IMP(msvcrt, lrint) { RET((s64)(s32)lrint(DA(0))); }
IMP(msvcrt, lrintf) { RET((s64)(s32)lrintf(FA(0))); }
// wide character classes ("C" locale: ASCII only, as the narrow ones)
IMP(msvcrt, iswspace) { u32 ch = (u16)ARG(0); RET(ch < 128 && isspace((int)ch)); }
IMP(msvcrt, iswalpha) { u32 ch = (u16)ARG(0); RET(ch < 128 && isalpha((int)ch)); }
IMP(msvcrt, iswdigit) { u32 ch = (u16)ARG(0); RET(ch < 128 && isdigit((int)ch)); }
IMP(msvcrt, iswalnum) { u32 ch = (u16)ARG(0); RET(ch < 128 && isalnum((int)ch)); }
IMP(msvcrt, iswupper) { u32 ch = (u16)ARG(0); RET(ch < 128 && isupper((int)ch)); }
IMP(msvcrt, iswlower) { u32 ch = (u16)ARG(0); RET(ch < 128 && islower((int)ch)); }
IMP(msvcrt, iswpunct) { u32 ch = (u16)ARG(0); RET(ch < 128 && ispunct((int)ch)); }
IMP(msvcrt, iswxdigit) { u32 ch = (u16)ARG(0); RET(ch < 128 && isxdigit((int)ch)); }
IMP(msvcrt, iswprint) { u32 ch = (u16)ARG(0); RET(ch >= 32 && ch != 127); }

// ---- the floating-point control word, in the C runtime's own bit layout (float.h's _MCW_*), on the x87's ----
static u32 cw_from_fcw(u16 f) {
  u32 c = 0;
  if (f & 0x01) c |= 0x10; if (f & 0x02) c |= 0x80000; if (f & 0x04) c |= 0x08; if (f & 0x08) c |= 0x04; if (f & 0x10) c |= 0x02; if (f & 0x20) c |= 0x01;
  c |= (u32)((f >> 10) & 3) << 8;                                          // rounding: near, down, up, chop in both
  u32 pc = (f >> 8) & 3; c |= pc == 0 ? 0x20000 : pc == 2 ? 0x10000 : 0;   // precision: _PC_24, _PC_53, _PC_64
  return c;
}
static u16 fcw_from_cw(u32 c, u16 f) {
  f &= (u16)~0x0F3F;
  if (c & 0x10) f |= 0x01; if (c & 0x80000) f |= 0x02; if (c & 0x08) f |= 0x04; if (c & 0x04) f |= 0x08; if (c & 0x02) f |= 0x10; if (c & 0x01) f |= 0x20;
  f |= (u16)(((c >> 8) & 3) << 10);
  u32 pc = c & 0x30000; f |= (u16)((pc == 0x20000 ? 0 : pc == 0x10000 ? 2 : 3) << 8);
  return f;
}
static u32 control87(Cpu *c, u32 value, u32 mask) {
  u32 cw = (cw_from_fcw(c->fcw) & ~mask) | (value & mask);
  c->fcw = fcw_from_cw(cw, c->fcw);
  return cw_from_fcw(c->fcw);
}
IMP(msvcrt, _control87) { RET(control87(c, (u32)ARG(0), (u32)ARG(1))); }
IMP(msvcrt, _controlfp) { RET(control87(c, (u32)ARG(0), (u32)ARG(1) & ~0x80000u)); } // not the denormal mask
IMP(msvcrt, _controlfp_s) { u32 v = control87(c, (u32)ARG(1), (u32)ARG(2) & ~0x80000u); if (ARG(0)) M32(ARG(0)) = v; RET(0); }
