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

// ---- the virtual CPU: what CPUID reports decides which code paths a title takes ----
void rt_cpuid(Cpu *c) {
  u32 leaf = (u32)R[RAX], sub = (u32)R[RCX], a = 0, b = 0, d = 0, cx = 0;
  (void)sub;
  switch (leaf) {
  case 0: a = 0xD; b = 0x756E6547; d = 0x49656E69; cx = 0x6C65746E; break; // GenuineIntel
  case 1:
    a = 0x000906EA; b = (8u << 16) | (8u << 8);
    { unsigned f = rt_cpu_features; // the title's recipe may leave features out (an old title must not pick MMX paths)
      cx = (f & CPU_SSE3 ? 1u << 0 : 0) | (f & CPU_PCLMUL ? 1u << 1 : 0) | (f & CPU_SSSE3 ? 1u << 9 : 0) | (f & CPU_CX16 ? 1u << 13 : 0)
         | (f & CPU_SSE41 ? 1u << 19 : 0) | (f & CPU_SSE42 ? 1u << 20 : 0) | (f & CPU_POPCNT ? 1u << 23 : 0);
      d = (f & CPU_FPU ? 1u << 0 : 0) | (f & CPU_TSC ? 1u << 4 : 0) | (f & CPU_CX8 ? 1u << 8 : 0) | (f & CPU_CMOV ? 1u << 15 : 0)
        | (f & CPU_MMX ? 1u << 23 : 0) | (f & CPU_FXSR ? 1u << 24 : 0) | (f & CPU_SSE ? 1u << 25 : 0) | (f & CPU_SSE2 ? 1u << 26 : 0); }
    break;
  case 0x80000000: a = 0x80000004; break;
  case 0x80000001: cx = 1u << 0; d = (1u << 11) | (1u << 20) | (1u << 29); break; // LAHF; SYSCALL NX LM
  case 0x80000002: case 0x80000003: case 0x80000004: {
    static const char brand[48] = "series-web virtual CPU (lifted x86-64)";
    const u32 *w = (const u32 *)(brand + 16 * (leaf - 0x80000002)); a = w[0]; b = w[1]; cx = w[2]; d = w[3]; break;
  }
  }
  R[RAX] = a; R[RBX] = b; R[RCX] = cx; R[RDX] = d;
}
#if GUEST_BITS == 32
u64 rt_rdtsc(void) { return rt_uptime_us() * 3000; } // a 3 GHz counter on the title's clock
#else
u64 rt_rdtsc(void) { return (u64)(host_now_us() * 3000.0); }
#endif

// ---- files ----
int rt_file_write(int fd, const void *p, u32 n) { u32 done = 0; while (done < n) { ssize_t k = write(fd, (const char *)p + done, n - done); if (k <= 0) return done ? (int)done : -1; done += (u32)k; } return (int)done; }
int rt_file_read(int fd, void *p, u32 n) { ssize_t k = read(fd, p, n); return (int)k; }

// FILE*: a 48-byte guest block whose address maps to one of these
typedef struct { u64 g; int fd, eof, err, unget; VFile *vf; int append; } Stream;
static Stream streams[256];
static u64 iob;
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

// ---- printf with Win64 arguments ----
typedef struct { Cpu *c; int idx; u64 va; } Args;
static u64 arg_n(Cpu *c, int n) { return ARG(n); }
static u64 next_arg(Args *a) { if (a->va) { u64 v = M64(a->va); a->va += 8; return v; } return arg_n(a->c, a->idx++); }
typedef struct { char *buf; size_t cap, len; Stream *out; } Sink;
static void put(Sink *s, const char *p, size_t n) {
  if (s->out) { swrite(s->out, p, (u32)n); s->len += n; return; }
  for (size_t i = 0; i < n; i++, s->len++) if (s->buf && s->len < s->cap) s->buf[s->len] = p[i];
}
static void format(Sink *out, const char *f, Args *a) {
  char spec[32], tmp[512];
  while (*f) {
    if (*f != '%') { const char *e = strchr(f, '%'); size_t n = e ? (size_t)(e - f) : strlen(f); put(out, f, n); f += n; continue; }
    if (f[1] == '%') { put(out, "%", 1); f += 2; continue; }
    size_t k = 0; spec[k++] = *f++;
    while (strchr("-+ #0", *f) && k < 8) spec[k++] = *f++;
    int width = -1, prec = -1;
    if (*f == '*') { width = (s32)next_arg(a); f++; } else while (isdigit((u8)*f) && k < 14) spec[k++] = *f++;
    if (*f == '.') { f++; if (*f == '*') { prec = (s32)next_arg(a); f++; } else { prec = 0; while (isdigit((u8)*f)) prec = prec * 10 + (*f++ - '0'); } }
    int big = 0, wide = 0, half = 0;
    for (;;) {
      if (*f == 'l' && f[1] == 'l') { big = 1; f += 2; }
      else if (*f == 'I' && f[1] == '6' && f[2] == '4') { big = 1; f += 3; }
      else if (*f == 'I' && f[1] == '3' && f[2] == '2') { f += 3; }
      else if (*f == 'I' || *f == 'z' || *f == 'j' || *f == 't') { big = 1; f++; }
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
      s64 v = big ? (s64)next_arg(a) : half == 1 ? (s16)next_arg(a) : half > 1 ? (s8)next_arg(a) : (s32)next_arg(a);
      memcpy(spec + k, "lld", 4); n = snprintf(tmp, sizeof tmp, spec, v); break;
    }
    case 'u': case 'x': case 'X': case 'o': {
      u64 v = big ? next_arg(a) : half == 1 ? (u16)next_arg(a) : half > 1 ? (u8)next_arg(a) : (u32)next_arg(a);
      spec[k] = 'l'; spec[k + 1] = 'l'; spec[k + 2] = conv; spec[k + 3] = 0; n = snprintf(tmp, sizeof tmp, spec, v); break;
    }
    case 'p': n = snprintf(tmp, sizeof tmp, "%016llX", next_arg(a)); break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
      u64 bits = next_arg(a); double v; memcpy(&v, &bits, 8);
      spec[k] = conv; spec[k + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, v); break;
    }
    case 'c': case 'C': { char ch = (char)next_arg(a); spec[k] = 'c'; spec[k + 1] = 0; n = snprintf(tmp, sizeof tmp, spec, ch); break; }
    case 's': case 'S': {
      u64 p = next_arg(a); char *str = 0;
      if (!p) str = strdup("(null)"); else if (wide || conv == 'S') str = rt_narrow(GWSTR(p));
      spec[k] = 's'; spec[k + 1] = 0;
      int need = snprintf(0, 0, spec, str ? str : GSTR(p));
      char *big_buf = need < (int)sizeof tmp ? tmp : malloc((size_t)need + 1);
      snprintf(big_buf, (size_t)need + 1, spec, str ? str : GSTR(p));
      put(out, big_buf, (size_t)need);
      if (big_buf != tmp) free(big_buf);
      free(str); continue;
    }
    case 'n': { u64 p = next_arg(a); if (p) M32(p) = (u32)out->len; continue; }
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
HOST("exit") __attribute__((noreturn)) void host_exit(u32 code);
void (*rt_exit_hook)(void);
void rt_exit_process(u32 code) { void (*hook)(void) = rt_exit_hook; rt_exit_hook = 0; if (hook) hook(); host_exit(code); }
static void run_atexit(Cpu *c) {
  pthread_mutex_lock(&exit_lock);
  if (exiting) { pthread_mutex_unlock(&exit_lock); return; }
  exiting = 1;
  pthread_mutex_unlock(&exit_lock);
  while (atexit_n > 0) rt_guest_call(c, atexit_fns[--atexit_n], 0, 0, 0, 0);
}

static u64 data_initenv, data_commode, data_fmode, lconv_g, locale_c;
u64 rt_data_import(const char *name) {
  if (!strcmp(name, "msvcrt.dll!__initenv")) return data_initenv;
  if (!strcmp(name, "msvcrt.dll!_commode")) return data_commode;
  if (!strcmp(name, "msvcrt.dll!_fmode")) return data_fmode;
  return 0;
}
void rt_crt_init(void) {
  data_initenv = (uptr)calloc(1, 16); M64(data_initenv) = (uptr)calloc(1, 16);
  data_commode = (uptr)calloc(1, 8); data_fmode = (uptr)calloc(1, 8);
  iob = (uptr)calloc(3, 48);
  for (int i = 0; i < 3; i++) streams[i] = (Stream){iob + 48ull * i, i, 0, 0, -1};
  locale_c = (uptr)strdup("C");
  // struct lconv: ten char* fields, then the monetary chars
  lconv_g = (uptr)calloc(1, 128);
  M64(lconv_g) = (uptr)strdup(".");
  for (int i = 1; i < 10; i++) M64(lconv_g + 8 * i) = (uptr)strdup("");
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
u64 rt_msvcrt_environ(void) { return M64(data_initenv); }
u64 rt_msvcrt_iob(int i) { return iob + 48ull * (u64)i; }
IMP(msvcrt, __getmainargs) {
  int argc; u64 argv = rt_msvcrt_argv(&argc);
  M32(ARG(0)) = (u32)argc; M64(ARG(1)) = argv; M64(ARG(2)) = M64(data_initenv);
  RET(0);
}
// formats to a stream (file != 0) or a buffer; *len: the full length
void rt_msvcrt_format(Cpu *c, u64 file, u64 buf, u64 cap, u64 fmt, u64 va, int *len) {
  Sink o = {GSTR(buf), buf ? cap : 0, 0, file ? stream(file) : 0};
  if (file && !o.out) { *len = -1; return; }
  Args a = {c, 0, va}; format(&o, GSTR(fmt), &a);
  *len = (int)o.len;
}
IMP(msvcrt, __set_app_type) {}
IMP(msvcrt, __setusermatherr) {}
IMP(msvcrt, _initterm) { for (u64 p = ARG(0), e = ARG(1); p < e; p += 8) { u64 f = M64(p); if (f) rt_guest_call(c, f, 0, 0, 0, 0); } }
IMP(msvcrt, atexit) { pthread_mutex_lock(&exit_lock); if (atexit_n < 256) atexit_fns[atexit_n++] = ARG(0); pthread_mutex_unlock(&exit_lock); RET(0); }
IMP(msvcrt, _onexit) { imp_msvcrt_atexit(c); RET(ARG(0)); }
IMP(msvcrt, exit) { u32 code = (u32)ARG(0); run_atexit(c); rt_exit_process(code); }
IMP(msvcrt, _exit) { rt_exit_process((u32)ARG(0)); }
IMP(msvcrt, _cexit) { run_atexit(c); }
IMP(msvcrt, _amsg_exit) { rt_fatal("runtime error R60%02llu", ARG(0)); }
IMP(msvcrt, abort) { rt_fatal("abort() called from 0x%llx", M64(R[RSP])); }
IMP(msvcrt, _assert) { rt_fatal("assertion failed: %s (%s:%u)", GSTR(ARG(0)), GSTR(ARG(1)), (u32)ARG(2)); }
IMP(msvcrt, signal) { RET(0); }
IMP(msvcrt, _errno) { RET(THREAD(c)->teb + TEB_ERRNO); }
IMP(msvcrt, __iob_func) { RET(iob); }
IMP(msvcrt, _beginthreadex) { u32 tid = 0; u64 h = rt_create_thread(c, ARG(1), ARG(2), ARG(3), (ARG(4) & 4) != 0, &tid); if (ARG(5)) M32(ARG(5)) = tid; RET(h); }
IMP(msvcrt, _endthreadex) { rt_thread_exit(c, (u32)ARG(0)); }
IMP(msvcrt, _setjmp) { RET(0); } // longjmp is not supported yet: a setjmp that only ever returns once
IMP(msvcrt, longjmp) { rt_fatal("longjmp is not implemented yet (from 0x%llx)", M64(R[RSP])); }

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
static void set_end(Cpu *c, u64 endp, const char *start, char *end) { if (endp) M64(endp) = (uptr)end; (void)start; }
IMP(msvcrt, strtol) { char *e; long v = strtol(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET((s64)(s32)v); }
IMP(msvcrt, strtoul) { char *e; unsigned long v = strtoul(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET((u32)v); }
IMP(msvcrt, _strtoi64) { char *e; long long v = strtoll(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET(v); }
IMP(msvcrt, _strtoui64) { char *e; unsigned long long v = strtoull(GSTR(ARG(0)), &e, (int)ARG(2)); set_end(c, ARG(1), 0, e); RET(v); }
IMP(msvcrt, strtod) { char *e; double v = strtod(GSTR(ARG(0)), &e); set_end(c, ARG(1), 0, e); X[0] = vzero(); X[0].pd[0] = v; }
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
IMP(msvcrt, ceil) { X[0].pd[0] = ceil(FARG(0)); }
IMP(msvcrt, floor) { X[0].pd[0] = floor(FARG(0)); }
IMP(msvcrt, ldexp) { X[0].pd[0] = ldexp(FARG(0), (s32)ARG(1)); }
IMP(msvcrt, log10) { X[0].pd[0] = log10(FARG(0)); }
IMP(msvcrt, log) { X[0].pd[0] = log(FARG(0)); }
IMP(msvcrt, exp) { X[0].pd[0] = exp(FARG(0)); }
IMP(msvcrt, pow) { X[0].pd[0] = pow(FARG(0), FARG(1)); }
IMP(msvcrt, sqrt) { X[0].pd[0] = sqrt(FARG(0)); }

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
IMP(msvcrt, fopen_s) { u64 g = stream_open(c, GSTR(ARG(1)), GSTR(ARG(2))); M64(ARG(0)) = g; RET(g ? 0 : M32(THREAD(c)->teb + TEB_ERRNO)); }
IMP(msvcrt, _wfopen_s) { char *n = rt_narrow(GWSTR(ARG(1))), *m = rt_narrow(GWSTR(ARG(2))); u64 g = stream_open(c, n, m); free(n); free(m); M64(ARG(0)) = g; RET(g ? 0 : M32(THREAD(c)->teb + TEB_ERRNO)); }
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
IMP(msvcrt, vfprintf) { Stream *s = FILEARG(0); if (!s) { RET(-1); return; } Sink o = {0, 0, 0, s}; Args a = {c, 0, ARG(2)}; format(&o, GSTR(ARG(1)), &a); RET(o.len); }
IMP(msvcrt, fprintf) { Stream *s = FILEARG(0); if (!s) { RET(-1); return; } Sink o = {0, 0, 0, s}; Args a = {c, 2, 0}; format(&o, GSTR(ARG(1)), &a); RET(o.len); }
IMP(msvcrt, printf) { Sink o = {0, 0, 0, &streams[1]}; Args a = {c, 1, 0}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
IMP(msvcrt, vprintf) { Sink o = {0, 0, 0, &streams[1]}; Args a = {c, 0, ARG(1)}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
IMP(msvcrt, _vscprintf) { Sink o = {0}; Args a = {c, 0, ARG(1)}; format(&o, GSTR(ARG(0)), &a); RET(o.len); }
// msvcrt: -1 and no terminator when the text does not fit
IMP(msvcrt, _vsnprintf) {
  Sink o = {GSTR(ARG(0)), ARG(1), 0, 0}; Args a = {c, 0, ARG(3)}; format(&o, GSTR(ARG(2)), &a);
  if (o.len < ARG(1)) { GSTR(ARG(0))[o.len] = 0; RET(o.len); } else RET(-1);
}
IMP(msvcrt, _snprintf) {
  Sink o = {GSTR(ARG(0)), ARG(1), 0, 0}; Args a = {c, 3, 0}; format(&o, GSTR(ARG(2)), &a);
  if (o.len < ARG(1)) { GSTR(ARG(0))[o.len] = 0; RET(o.len); } else RET(-1);
}
IMP(msvcrt, sprintf) { Sink o = {GSTR(ARG(0)), (size_t)-1, 0, 0}; Args a = {c, 2, 0}; format(&o, GSTR(ARG(1)), &a); GSTR(ARG(0))[o.len] = 0; RET(o.len); }
IMP(msvcrt, vsprintf) { Sink o = {GSTR(ARG(0)), (size_t)-1, 0, 0}; Args a = {c, 0, ARG(2)}; format(&o, GSTR(ARG(1)), &a); GSTR(ARG(0))[o.len] = 0; RET(o.len); }

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

u64 rt_random64(void) { u64 v = 0; arc4random_buf(&v, sizeof v); return v; }

// ---- the rest of the C math library (Win64: arguments in xmm0 / xmm1, the result in xmm0; floats in the low lane) ----
// (written out: tools/build-title.sh finds runtime functions by their IMP lines)
IMP(msvcrt, sin) { X[0].pd[0] = sin(X[0].pd[0]); }
IMP(msvcrt, sinf) { X[0].ps[0] = sinf(X[0].ps[0]); }
IMP(msvcrt, cos) { X[0].pd[0] = cos(X[0].pd[0]); }
IMP(msvcrt, cosf) { X[0].ps[0] = cosf(X[0].ps[0]); }
IMP(msvcrt, tan) { X[0].pd[0] = tan(X[0].pd[0]); }
IMP(msvcrt, tanf) { X[0].ps[0] = tanf(X[0].ps[0]); }
IMP(msvcrt, asin) { X[0].pd[0] = asin(X[0].pd[0]); }
IMP(msvcrt, asinf) { X[0].ps[0] = asinf(X[0].ps[0]); }
IMP(msvcrt, acos) { X[0].pd[0] = acos(X[0].pd[0]); }
IMP(msvcrt, acosf) { X[0].ps[0] = acosf(X[0].ps[0]); }
IMP(msvcrt, atan) { X[0].pd[0] = atan(X[0].pd[0]); }
IMP(msvcrt, atanf) { X[0].ps[0] = atanf(X[0].ps[0]); }
IMP(msvcrt, sinh) { X[0].pd[0] = sinh(X[0].pd[0]); }
IMP(msvcrt, sinhf) { X[0].ps[0] = sinhf(X[0].ps[0]); }
IMP(msvcrt, cosh) { X[0].pd[0] = cosh(X[0].pd[0]); }
IMP(msvcrt, coshf) { X[0].ps[0] = coshf(X[0].ps[0]); }
IMP(msvcrt, tanh) { X[0].pd[0] = tanh(X[0].pd[0]); }
IMP(msvcrt, tanhf) { X[0].ps[0] = tanhf(X[0].ps[0]); }
IMP(msvcrt, asinh) { X[0].pd[0] = asinh(X[0].pd[0]); }
IMP(msvcrt, asinhf) { X[0].ps[0] = asinhf(X[0].ps[0]); }
IMP(msvcrt, acosh) { X[0].pd[0] = acosh(X[0].pd[0]); }
IMP(msvcrt, acoshf) { X[0].ps[0] = acoshf(X[0].ps[0]); }
IMP(msvcrt, atanh) { X[0].pd[0] = atanh(X[0].pd[0]); }
IMP(msvcrt, atanhf) { X[0].ps[0] = atanhf(X[0].ps[0]); }
IMP(msvcrt, exp2) { X[0].pd[0] = exp2(X[0].pd[0]); }
IMP(msvcrt, exp2f) { X[0].ps[0] = exp2f(X[0].ps[0]); }
IMP(msvcrt, log2) { X[0].pd[0] = log2(X[0].pd[0]); }
IMP(msvcrt, log2f) { X[0].ps[0] = log2f(X[0].ps[0]); }
IMP(msvcrt, log1p) { X[0].pd[0] = log1p(X[0].pd[0]); }
IMP(msvcrt, log1pf) { X[0].ps[0] = log1pf(X[0].ps[0]); }
IMP(msvcrt, expm1) { X[0].pd[0] = expm1(X[0].pd[0]); }
IMP(msvcrt, expm1f) { X[0].ps[0] = expm1f(X[0].ps[0]); }
IMP(msvcrt, cbrt) { X[0].pd[0] = cbrt(X[0].pd[0]); }
IMP(msvcrt, cbrtf) { X[0].ps[0] = cbrtf(X[0].ps[0]); }
IMP(msvcrt, round) { X[0].pd[0] = round(X[0].pd[0]); }
IMP(msvcrt, roundf) { X[0].ps[0] = roundf(X[0].ps[0]); }
IMP(msvcrt, trunc) { X[0].pd[0] = trunc(X[0].pd[0]); }
IMP(msvcrt, truncf) { X[0].ps[0] = truncf(X[0].ps[0]); }
IMP(msvcrt, rint) { X[0].pd[0] = rint(X[0].pd[0]); }
IMP(msvcrt, rintf) { X[0].ps[0] = rintf(X[0].ps[0]); }
IMP(msvcrt, nearbyint) { X[0].pd[0] = nearbyint(X[0].pd[0]); }
IMP(msvcrt, nearbyintf) { X[0].ps[0] = nearbyintf(X[0].ps[0]); }
IMP(msvcrt, fabs) { X[0].pd[0] = fabs(X[0].pd[0]); }
IMP(msvcrt, fabsf) { X[0].ps[0] = fabsf(X[0].ps[0]); }
IMP(msvcrt, erf) { X[0].pd[0] = erf(X[0].pd[0]); }
IMP(msvcrt, erff) { X[0].ps[0] = erff(X[0].ps[0]); }
IMP(msvcrt, erfc) { X[0].pd[0] = erfc(X[0].pd[0]); }
IMP(msvcrt, erfcf) { X[0].ps[0] = erfcf(X[0].ps[0]); }
IMP(msvcrt, tgamma) { X[0].pd[0] = tgamma(X[0].pd[0]); }
IMP(msvcrt, tgammaf) { X[0].ps[0] = tgammaf(X[0].ps[0]); }
IMP(msvcrt, lgamma) { X[0].pd[0] = lgamma(X[0].pd[0]); }
IMP(msvcrt, lgammaf) { X[0].ps[0] = lgammaf(X[0].ps[0]); }
IMP(msvcrt, floorf) { X[0].ps[0] = floorf(X[0].ps[0]); }
IMP(msvcrt, ceilf) { X[0].ps[0] = ceilf(X[0].ps[0]); }
IMP(msvcrt, sqrtf) { X[0].ps[0] = sqrtf(X[0].ps[0]); }
IMP(msvcrt, expf) { X[0].ps[0] = expf(X[0].ps[0]); }
IMP(msvcrt, logf) { X[0].ps[0] = logf(X[0].ps[0]); }
IMP(msvcrt, log10f) { X[0].ps[0] = log10f(X[0].ps[0]); }
IMP(msvcrt, atan2) { X[0].pd[0] = atan2(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, atan2f) { X[0].ps[0] = atan2f(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, fmod) { X[0].pd[0] = fmod(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, fmodf) { X[0].ps[0] = fmodf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, hypot) { X[0].pd[0] = hypot(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, hypotf) { X[0].ps[0] = hypotf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, fmin) { X[0].pd[0] = fmin(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, fminf) { X[0].ps[0] = fminf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, fmax) { X[0].pd[0] = fmax(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, fmaxf) { X[0].ps[0] = fmaxf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, copysign) { X[0].pd[0] = copysign(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, copysignf) { X[0].ps[0] = copysignf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, remainder) { X[0].pd[0] = remainder(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, remainderf) { X[0].ps[0] = remainderf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, fdim) { X[0].pd[0] = fdim(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, fdimf) { X[0].ps[0] = fdimf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, nextafter) { X[0].pd[0] = nextafter(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, nextafterf) { X[0].ps[0] = nextafterf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, powf) { X[0].ps[0] = powf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, _hypot) { X[0].pd[0] = hypot(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, _hypotf) { X[0].ps[0] = hypotf(X[0].ps[0], X[1].ps[0]); }
IMP(msvcrt, _copysign) { X[0].pd[0] = copysign(X[0].pd[0], X[1].pd[0]); }
IMP(msvcrt, ldexpf) { X[0].ps[0] = ldexpf(X[0].ps[0], (int)R[RDX]); }
IMP(msvcrt, frexp) { int e; X[0].pd[0] = frexp(X[0].pd[0], &e); M32(R[RDX]) = (u32)e; }
IMP(msvcrt, modf) { double ip; X[0].pd[0] = modf(X[0].pd[0], &ip); MF64(R[RDX]) = ip; }
IMP(msvcrt, modff) { float ip; X[0].ps[0] = modff(X[0].ps[0], &ip); MF32(R[RDX]) = ip; }
IMP(msvcrt, lround) { RET((s64)(s32)lround(X[0].pd[0])); }
IMP(msvcrt, lroundf) { RET((s64)(s32)lroundf(X[0].ps[0])); }
IMP(msvcrt, llround) { RET((u64)llround(X[0].pd[0])); }
IMP(msvcrt, lrint) { RET((s64)(s32)lrint(X[0].pd[0])); }
IMP(msvcrt, lrintf) { RET((s64)(s32)lrintf(X[0].ps[0])); }
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
