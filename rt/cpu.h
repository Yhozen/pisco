// SPDX-License-Identifier: GPL-2.0-or-later
// Guest CPU state and instruction helpers for lifted x86-64 code, and for lifted 32-bit x86 code.
// Guest address == offset in wasm linear memory (the image is based below 4 GiB).
#pragma once
// The guest's width: the lifter defines GUEST_BITS 32 in the files of a 32-bit title, nothing in a 64-bit one's.
// A 32-bit guest uses r[0..7] (EAX..EDI, zero-extended), fs_base (its TEB), x87 and, when its code has it, x[0..7].
#ifndef GUEST_BITS
#define GUEST_BITS 64
#endif
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
typedef signed char s8; typedef short s16; typedef int s32; typedef long long s64;
typedef __int128 s128; typedef unsigned __int128 u128;

#define VEC(T, N) T __attribute__((vector_size(16), aligned(1), may_alias))
typedef union { VEC(u8,16) b; VEC(u16,8) w; VEC(u32,4) d; VEC(u64,2) q;
                VEC(s8,16) sb; VEC(s16,8) sw; VEC(s32,4) sd; VEC(s64,2) sq;
                VEC(float,4) ps; VEC(double,2) pd; } V;

typedef struct Cpu {
  u64 r[16];
  V x[16];
  u64 gs_base;
  u64 fs_base;    // 32-bit guests: the thread's TEB (FS-prefixed memory operands add it)
  u32 mxcsr;
  u16 fcw, fsw;   // x87 control and status
  u32 ftop;       // x87 stack top (0..7)
  u32 eflags;     // 32-bit guests: the EFLAGS bits pushfd / popfd carry beside the function's flags (AC, ID)
  double st[8];   // x87 registers, kept as doubles (80-bit precision is not kept)
  struct Landing *landing; // the innermost frame an exception can resume in (rt/seh.c)
} Cpu;
// A lifted function with landing pads registers one while it runs. `jb` is a jmp_buf in its frame.
typedef struct Landing { void *jb; u64 entry_rsp; u64 resume; struct Landing *prev; } Landing;
#include <setjmp.h>
#define LANDING(body) do { jmp_buf jb; Landing L = {&jb, c->r[4], 0, c->landing}; c->landing = &L; \
    u64 resume_at = 0; if (setjmp(jb)) resume_at = L.resume; body(c, resume_at); c->landing = L.prev; } while (0)
typedef struct { u8 cf, zf, sf, of, pf, af, df; } Flags; // df: 32-bit code only (64-bit code assumes it clear)

#define R (c->r)
#define X (c->x)
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };

#define UA(T) T __attribute__((aligned(1), may_alias))
typedef UA(u8) u8a; typedef UA(u16) u16a; typedef UA(u32) u32a; typedef UA(u64) u64a;
typedef UA(float) f32a; typedef UA(double) f64a;
typedef V __attribute__((aligned(1), may_alias)) Va;
typedef __UINTPTR_TYPE__ uptr;
// guest address -> host pointer: wasm32 keeps the low 32 bits, wasm64 all 64
#define GP(a) ((uptr)(a))
#define M8(a)  (*(u8a *)GP(a))
#define M16(a) (*(u16a *)GP(a))
#define M32(a) (*(u32a *)GP(a))
#define M64(a) (*(u64a *)GP(a))
#define MF32(a) (*(f32a *)GP(a))
#define MF64(a) (*(f64a *)GP(a))
#define MV(a)  (*(Va *)GP(a))

void rt_trap(u64 addr) __attribute__((noreturn));
void rt_unimpl(u64 addr) __attribute__((noreturn));
void rt_call(Cpu *c, u64 target);
void rt_cpuid(Cpu *c);
u64 rt_rdtsc(void);

#define PUSH64(v) do { R[RSP] -= 8; M64(R[RSP]) = (v); } while (0)
#define POP64() (R[RSP] += 8, M64(R[RSP] - 8))
// 32-bit code: 4-byte stack slots (2-byte ones behind an operand-size prefix); ESP wraps at 4 GiB
#define PUSH32(v) do { u32 push_v_ = (v); R[RSP] = (u32)(R[RSP] - 4); M32(R[RSP]) = push_v_; } while (0)
#define POP32() (R[RSP] = (u32)(R[RSP] + 4), M32(R[RSP] - 4))
#define PUSH16(v) do { u16 push_v_ = (v); R[RSP] = (u32)(R[RSP] - 2); M16(R[RSP]) = push_v_; } while (0)
#define POP16() (R[RSP] = (u32)(R[RSP] + 2), M16(R[RSP] - 2))

static inline u8 parity(u64 v) { return !__builtin_parity((u8)v); }

#define ARITH(N, U, S, MSB) \
static inline U add##N(Flags *f, U a, U b, u8 ci) { U r = a + b + ci; f->cf = ci ? r <= a : r < a; \
  f->of = (U)((a ^ r) & (b ^ r)) >> MSB; f->af = ((a ^ b ^ r) >> 4) & 1; f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U sub##N(Flags *f, U a, U b, u8 ci) { U r = a - b - ci; f->cf = ci ? a <= b : a < b; \
  f->of = (U)((a ^ b) & (a ^ r)) >> MSB; f->af = ((a ^ b ^ r) >> 4) & 1; f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U logic##N(Flags *f, U r) { f->cf = 0; f->of = 0; f->af = 0; f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U inc##N(Flags *f, U a) { u8 cf = f->cf; U r = add##N(f, a, 1, 0); f->cf = cf; return r; } \
static inline U dec##N(Flags *f, U a) { u8 cf = f->cf; U r = sub##N(f, a, 1, 0); f->cf = cf; return r; } \
static inline U neg##N(Flags *f, U a) { U r = sub##N(f, 0, a, 0); f->cf = a != 0; return r; } \
static inline U shl##N(Flags *f, U a, u8 n) { n &= (N == 64 ? 63 : 31); if (!n) return a; \
  U r = n > MSB ? 0 : (U)(a << n); f->cf = n > MSB + 1 ? 0 : (a >> (MSB + 1 - n)) & 1; f->of = (r >> MSB) ^ f->cf; \
  f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U shr##N(Flags *f, U a, u8 n) { n &= (N == 64 ? 63 : 31); if (!n) return a; \
  U r = n > MSB ? 0 : (U)(a >> n); f->cf = n > MSB + 1 ? 0 : (a >> (n - 1)) & 1; f->of = a >> MSB; \
  f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U sar##N(Flags *f, U a, u8 n) { n &= (N == 64 ? 63 : 31); if (!n) return a; if (n > MSB) n = MSB + 1; \
  U r = n > MSB ? (U)((S)a < 0 ? -1 : 0) : (U)((S)a >> n); f->cf = n > MSB ? (a >> MSB) & 1 : (a >> (n - 1)) & 1; f->of = 0; \
  f->zf = r == 0; f->sf = r >> MSB; f->pf = parity(r); return r; } \
static inline U rol##N(Flags *f, U a, u8 n) { n &= MSB; if (!n) return a; U r = (U)(a << n) | (U)(a >> (MSB + 1 - n)); \
  f->cf = r & 1; f->of = (r >> MSB) ^ f->cf; return r; } \
static inline U ror##N(Flags *f, U a, u8 n) { n &= MSB; if (!n) return a; U r = (U)(a >> n) | (U)(a << (MSB + 1 - n)); \
  f->cf = r >> MSB; f->of = ((r >> MSB) ^ (r >> (MSB - 1))) & 1; return r; }
ARITH(8, u8, s8, 7) ARITH(16, u16, s16, 15) ARITH(32, u32, s32, 31) ARITH(64, u64, s64, 63)
// rotates through carry: the count is taken modulo the operand's bits plus one (OF as for a count of 1)
#define ROTC(N, U, MSB) \
static inline U rcl##N(Flags *f, U a, u8 n) { n &= (N == 64 ? 63 : 31); if (N < 32) n %= N + 1; if (!n) return a; \
  for (u8 k = 0; k < n; k++) { u8 top = (a >> MSB) & 1; a = (U)(a << 1) | (U)f->cf; f->cf = top; } f->of = ((a >> MSB) ^ f->cf) & 1; return a; } \
static inline U rcr##N(Flags *f, U a, u8 n) { n &= (N == 64 ? 63 : 31); if (N < 32) n %= N + 1; if (!n) return a; f->of = ((a >> MSB) ^ f->cf) & 1; \
  for (u8 k = 0; k < n; k++) { u8 low = a & 1; a = (U)(a >> 1) | (U)((U)f->cf << MSB); f->cf = low; } return a; }
ROTC(8, u8, 7) ROTC(16, u16, 15) ROTC(32, u32, 31) ROTC(64, u64, 63)

#define IMUL(N, U, S, W) static inline U imul##N(Flags *f, U a, U b) { W p = (W)(S)a * (W)(S)b; U r = (U)p; \
  f->cf = f->of = (W)(S)r != p; return r; }
IMUL(16, u16, s16, s32) IMUL(32, u32, s32, s64)
// 64x64 -> high 64 without compiler-rt's 128-bit helpers
static inline u64 mulhu64(u64 a, u64 b) { u64 al = (u32)a, ah = a >> 32, bl = (u32)b, bh = b >> 32;
  u64 t = al * bl; u64 m1 = ah * bl + (t >> 32); u64 m2 = al * bh + (u32)m1; return ah * bh + (m1 >> 32) + (m2 >> 32); }
static inline u64 mulhs64(u64 a, u64 b) { return mulhu64(a, b) - ((s64)a < 0 ? b : 0) - ((s64)b < 0 ? a : 0); }
static inline u64 imul64(Flags *f, u64 a, u64 b) { u64 r = a * b; f->cf = f->of = mulhs64(a, b) != (u64)((s64)r >> 63); return r; }
static inline void mul64x(Cpu *c, Flags *f, u64 b) { u64 a = c->r[0]; c->r[0] = a * b; c->r[2] = mulhu64(a, b); f->cf = f->of = c->r[2] != 0; }
static inline void imul64x(Cpu *c, Flags *f, u64 b) { u64 a = c->r[0]; c->r[0] = a * b; c->r[2] = mulhs64(a, b); f->cf = f->of = c->r[2] != (u64)((s64)c->r[0] >> 63); }
static inline void div64x(Cpu *c, u64 d, u64 ip) { u64 hi = c->r[2], lo = c->r[0]; if (!d || hi >= d) rt_trap(ip);
  if (!hi) { c->r[0] = lo / d; c->r[2] = lo % d; return; }
  u64 q = 0, r = hi; for (int i = 63; i >= 0; i--) { u8 top = r >> 63; r = (r << 1) | ((lo >> i) & 1); if (top || r >= d) { r -= d; q |= 1ull << i; } }
  c->r[0] = q; c->r[2] = r; }
static inline void idiv64x(Cpu *c, s64 d, u64 ip) { s64 lo = (s64)c->r[0]; if (!d || c->r[2] != (u64)(lo >> 63) || (d == -1 && lo == (s64)(1ull << 63))) rt_trap(ip);
  c->r[0] = (u64)(lo / d); c->r[2] = (u64)(lo % d); }

// SSE helpers
static inline V vzero(void) { V v; v.q = (typeof(v.q)){0, 0}; return v; }
static inline V vfrom64(u64 lo) { V v; v.q = (typeof(v.q)){lo, 0}; return v; }
static inline V pshufd(V a, u8 i) { V r; r.d = (typeof(r.d)){a.d[i & 3], a.d[(i >> 2) & 3], a.d[(i >> 4) & 3], a.d[(i >> 6) & 3]}; return r; }
static inline V pshuflw(V a, u8 i) { V r = a; r.w[0] = a.w[i & 3]; r.w[1] = a.w[(i >> 2) & 3]; r.w[2] = a.w[(i >> 4) & 3]; r.w[3] = a.w[(i >> 6) & 3]; return r; }
static inline V pshufhw(V a, u8 i) { V r = a; r.w[4] = a.w[4 + (i & 3)]; r.w[5] = a.w[4 + ((i >> 2) & 3)]; r.w[6] = a.w[4 + ((i >> 4) & 3)]; r.w[7] = a.w[4 + ((i >> 6) & 3)]; return r; }
static inline V shufps(V a, V b, u8 i) { V r; r.d = (typeof(r.d)){a.d[i & 3], a.d[(i >> 2) & 3], b.d[(i >> 4) & 3], b.d[(i >> 6) & 3]}; return r; }
#define SHIFTV(NAME, FIELD, BITS, OP) static inline V NAME(V a, u64 n) { V r; if (n >= BITS) { return vzero(); } r.FIELD = a.FIELD OP (typeof(a.FIELD[0]))n; return r; }
SHIFTV(psllw, w, 16, <<) SHIFTV(pslld, d, 32, <<) SHIFTV(psllq, q, 64, <<)
SHIFTV(psrlw, w, 16, >>) SHIFTV(psrld, d, 32, >>) SHIFTV(psrlq, q, 64, >>)
static inline V psraw(V a, u64 n) { V r; r.sw = a.sw >> (s16)(n > 15 ? 15 : n); return r; }
static inline V psrad(V a, u64 n) { V r; r.sd = a.sd >> (s32)(n > 31 ? 31 : n); return r; }
static inline V pslldq(V a, u8 n) { V r = vzero(); for (int i = n; i < 16; i++) r.b[i] = a.b[i - n]; return r; }
static inline V psrldq(V a, u8 n) { V r = vzero(); for (int i = 0; i + n < 16; i++) r.b[i] = a.b[i + n]; return r; }
#define UNPACK(NAME, FIELD, N, OFF) static inline V NAME(V a, V b) { V r; for (int i = 0; i < N / 2; i++) { r.FIELD[2 * i] = a.FIELD[OFF + i]; r.FIELD[2 * i + 1] = b.FIELD[OFF + i]; } return r; }
UNPACK(punpcklbw, b, 16, 0) UNPACK(punpcklwd, w, 8, 0) UNPACK(punpckldq, d, 4, 0) UNPACK(punpcklqdq, q, 2, 0)
UNPACK(punpckhbw, b, 16, 8) UNPACK(punpckhwd, w, 8, 4) UNPACK(punpckhdq, d, 4, 2) UNPACK(punpckhqdq, q, 2, 1)
static inline u32 pmovmskb_scalar(V a) { u32 m = 0; for (int i = 0; i < 16; i++) m |= (u32)(a.b[i] >> 7) << i; return m; } // (replaced below)
static inline V pmuludq_scalar(V a, V b) { V r; r.q = (typeof(r.q)){(u64)a.d[0] * b.d[0], (u64)a.d[2] * b.d[2]}; return r; }
static inline V packuswb_scalar(V a, V b) { V r; for (int i = 0; i < 8; i++) { s16 x = a.sw[i], y = b.sw[i]; r.b[i] = x < 0 ? 0 : x > 255 ? 255 : x; r.b[8 + i] = y < 0 ? 0 : y > 255 ? 255 : y; } return r; } // (replaced below)
static inline void fcmp(Flags *f, double a, double b) { f->of = f->sf = f->af = 0;
  if (a != a || b != b) { f->zf = f->pf = f->cf = 1; } else { f->zf = a == b; f->pf = 0; f->cf = a < b; } }
static inline u8 fpred(u8 p, double a, double b) { switch (p & 7) { case 0: return a == b; case 1: return a < b; case 2: return a <= b;
  case 3: return a != a || b != b; case 4: return !(a == b); case 5: return !(a < b); case 6: return !(a <= b); default: return a == a && b == b; } }

// ---- atomics (lock prefix, xchg with memory) ----
#define ATOM8(a) ((u8 *)GP(a))
#define ATOM16(a) ((u16 *)GP(a))
#define ATOM32(a) ((u32 *)GP(a))
#define ATOM64(a) ((u64 *)GP(a))
u64 rt_random64(void);

// ---- bit scans: the virtual CPU has no BMI1 / LZCNT, so tzcnt runs as bsf and lzcnt as bsr, as on such CPUs ----
#define BSF(N) static inline u64 bsf##N(Flags *f, u64 dst, u64 src) { src &= (N == 64 ? ~0ull : (1ull << N) - 1); f->zf = src == 0; return src ? (u64)__builtin_ctzll(src) : dst; } \
               static inline u64 bsr##N(Flags *f, u64 dst, u64 src) { src &= (N == 64 ? ~0ull : (1ull << N) - 1); f->zf = src == 0; return src ? (u64)(63 - __builtin_clzll(src)) : dst; }
BSF(16) BSF(32) BSF(64)
static inline u64 popcnt(Flags *f, u64 v) { f->cf = f->of = f->sf = f->af = f->pf = 0; f->zf = v == 0; return (u64)__builtin_popcountll(v); }

// ---- x87, with doubles ----
double log2(double); double exp2(double); double log1p(double); double expm1(double); double ldexp(double, int);
#define ST(i) (c->st[(c->ftop + (i)) & 7])
static inline void fpush(Cpu *c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(Cpu *c) { c->ftop = (c->ftop + 1) & 7; }
static inline double f80_load(u64 a) {
  u64 m = M64(a); u16 se = M16(a + 8); int e = se & 0x7FFF; int neg = se >> 15; double v;
  if (e == 0 && m == 0) v = 0.0;
  else if (e == 0x7FFF) v = (m << 1) ? __builtin_nan("") : __builtin_inf();
  else v = ldexp((double)m, e - 16383 - 63);
  return neg ? -v : v;
}
static inline void f80_store(u64 a, double v) {
  u64 bits; __builtin_memcpy(&bits, &v, 8);
  int neg = bits >> 63; int e = (bits >> 52) & 0x7FF; u64 frac = bits & ((1ull << 52) - 1); u64 m; int ee;
  if (e == 0 && frac == 0) { m = 0; ee = 0; }
  else if (e == 0x7FF) { m = (1ull << 63) | (frac << 11); ee = 0x7FFF; }
  else if (e == 0) { int sh = __builtin_clzll(frac) - 11; m = frac << (11 + sh); ee = 1 - 1023 + 16383 - sh; }
  else { m = (1ull << 63) | (frac << 11); ee = e - 1023 + 16383; }
  M64(a) = m; M16(a + 8) = (u16)(ee | (neg << 15));
}
static inline void fcom_sw(Cpu *c, double a, double b) {
  c->fsw &= ~0x4500;
  if (a != a || b != b) c->fsw |= 0x4500; else if (a < b) c->fsw |= 0x100; else if (a == b) c->fsw |= 0x4000;
}
static inline u16 fstsw(Cpu *c) { return (u16)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }
static inline double fround(Cpu *c, double v) {
  switch ((c->fcw >> 10) & 3) { case 0: return __builtin_rint(v); case 1: return __builtin_floor(v); case 2: return __builtin_ceil(v); default: return __builtin_trunc(v); }
}
static inline void fxam(Cpu *c) {
  double v = ST(0); c->fsw &= ~0x4700;
  if (__builtin_signbit(v)) c->fsw |= 0x200;
  if (v != v) c->fsw |= 0x100; else if (__builtin_isinf(v)) c->fsw |= 0x500; else if (v == 0) c->fsw |= 0x4000; else c->fsw |= 0x400;
}
static inline s64 fist(Cpu *c, double v, int bits) {
  double r = fround(c, v); double lim = bits == 64 ? 9223372036854775808.0 : bits == 32 ? 2147483648.0 : 32768.0;
  if (!(r >= -lim && r < lim)) return bits == 64 ? (s64)0x8000000000000000ull : bits == 32 ? (s64)(s32)0x80000000u : (s64)(s16)0x8000;
  return (s64)r;
}

// ---- 32-bit code: EFLAGS as a whole, string operations with a direction, the rest of x87 ----
// pushfd / popfd. The arithmetic flags and DF are the function's; AC and ID round-trip through the CPU state
// (programs toggle them to tell a 486 from a 386 and to see whether CPUID exists).
static inline u32 eflags_get(Cpu *c, const Flags *f) {
  return (u32)f->cf | 2u | (u32)f->pf << 2 | (u32)f->af << 4 | (u32)f->zf << 6 | (u32)f->sf << 7 | 0x200u | (u32)f->df << 10 | (u32)f->of << 11 | (c->eflags & 0x240000u);
}
static inline void eflags_set(Cpu *c, Flags *f, u32 v) {
  f->cf = v & 1; f->pf = (v >> 2) & 1; f->af = (v >> 4) & 1; f->zf = (v >> 6) & 1; f->sf = (v >> 7) & 1; f->df = (v >> 10) & 1; f->of = (v >> 11) & 1;
  c->eflags = v & 0x240000u;
}
// rep movs / rep stos: ECX elements of `size` bytes, upwards or (DF set) downwards from ESI / EDI
static inline void rep_movs(Cpu *c, u8 df, u32 size) {
  u32 n = (u32)R[RCX], di = (u32)R[RDI], si = (u32)R[RSI], bytes = n * size;
  if (!df) {
    if (di - si >= bytes) __builtin_memmove((void *)GP(di), (void *)GP(si), bytes); // what is read is not written first
    else if (size == 1) for (u32 k = 0; k < n; k++) M8(di + k) = M8(si + k);
    else if (size == 2) for (u32 k = 0; k < n; k++) M16(di + 2 * k) = M16(si + 2 * k);
    else for (u32 k = 0; k < n; k++) M32(di + 4 * k) = M32(si + 4 * k);
    di += bytes; si += bytes;
  } else {
    if (size == 1) for (u32 k = 0; k < n; k++) M8(di - k) = M8(si - k);
    else if (size == 2) for (u32 k = 0; k < n; k++) M16(di - 2 * k) = M16(si - 2 * k);
    else for (u32 k = 0; k < n; k++) M32(di - 4 * k) = M32(si - 4 * k);
    di -= bytes; si -= bytes;
  }
  R[RCX] = 0; R[RDI] = di; R[RSI] = si;
}
static inline void rep_stos(Cpu *c, u8 df, u32 size) {
  u32 n = (u32)R[RCX], di = (u32)R[RDI], bytes = n * size;
  if (!df) {
    if (size == 1) __builtin_memset((void *)GP(di), (u8)R[0], n);
    else if (size == 2) for (u32 k = 0; k < n; k++) M16(di + 2 * k) = (u16)R[0];
    else for (u32 k = 0; k < n; k++) M32(di + 4 * k) = (u32)R[0];
    di += bytes;
  } else {
    if (size == 1) for (u32 k = 0; k < n; k++) M8(di - k) = (u8)R[0];
    else if (size == 2) for (u32 k = 0; k < n; k++) M16(di - 2 * k) = (u16)R[0];
    else for (u32 k = 0; k < n; k++) M32(di - 4 * k) = (u32)R[0];
    di -= bytes;
  }
  R[RCX] = 0; R[RDI] = di;
}
double sin(double); double cos(double); double tan(double); double atan2(double, double);
double fmod(double, double); double remainder(double, double); double frexp(double, int *);
// precision control: with PC = 00 (24 bits; Direct3D 9 selects it) add, sub, mul, div and sqrt round to float;
// 53 and 64 bits both stay doubles
static inline double fprec(Cpu *c, double v) { return (c->fcw & 0x300) ? v : (double)(float)v; }
// fprem / fprem1: the remainder is complete at once (C2 = 0); C0, C3, C1 are the quotient's low three bits
static inline void fprem(Cpu *c, int nearest) {
  double a = ST(0), b = ST(1), q = a / b; q = nearest ? __builtin_rint(q) : __builtin_trunc(q);
  u64 n = (q > -9.0e18 && q < 9.0e18) ? (u64)(q < 0 ? -q : q) : 0;
  ST(0) = nearest ? remainder(a, b) : fmod(a, b);
  c->fsw = (u16)((c->fsw & ~0x4700) | ((n & 4) ? 0x100 : 0) | ((n & 2) ? 0x4000 : 0) | ((n & 1) ? 0x200 : 0));
}
static inline void fxtract(Cpu *c) {
  double v = ST(0); int e = 0;
  if (v == 0) { ST(0) = -__builtin_inf(); fpush(c, v); }
  else if (v != v || __builtin_isinf(v)) { ST(0) = v != v ? v : __builtin_inf(); fpush(c, v); }
  else { double m = frexp(v, &e); ST(0) = (double)(e - 1); fpush(c, m * 2); }
}
// the 28-byte environment and the 108-byte state of 32-bit protected mode. Which registers are empty is not
// tracked: the tag word says none is. Instruction and operand pointers are zero.
static inline void fstenv(Cpu *c, u64 a) {
  M32(a) = 0xFFFF0000u | c->fcw; M32(a + 4) = 0xFFFF0000u | fstsw(c); M32(a + 8) = 0xFFFF0000u;
  M32(a + 12) = 0; M32(a + 16) = 0; M32(a + 20) = 0; M32(a + 24) = 0xFFFF0000u;
}
static inline void fldenv(Cpu *c, u64 a) { u16 sw = M16(a + 4); c->fcw = M16(a); c->fsw = sw & ~0x3800; c->ftop = (sw >> 11) & 7; }
static inline void fsave(Cpu *c, u64 a) { fstenv(c, a); for (int i = 0; i < 8; i++) f80_store(a + 28 + 10 * i, ST(i)); c->ftop = 0; c->fcw = 0x37F; c->fsw = 0; }
static inline void frstor(Cpu *c, u64 a) { fldenv(c, a); for (int i = 0; i < 8; i++) ST(i) = f80_load(a + 28 + 10 * i); }

// ---- more SSE, on WebAssembly SIMD where it has the same operation ----
#include <wasm_simd128.h>
static inline v128_t TOV(V a) { v128_t r; __builtin_memcpy(&r, &a, 16); return r; }
static inline V FROMV(v128_t a) { V r; __builtin_memcpy(&r, &a, 16); return r; }
#define SAT(T, LO, HI) static inline T sat_##T(s64 v) { return (T)(v < LO ? LO : v > HI ? HI : v); }
SAT(s8, -128, 127) SAT(u8, 0, 255) SAT(s16, -32768, 32767) SAT(u16, 0, 65535)
#define W2(NAME, OP) static inline V NAME(V a, V b) { return FROMV(OP(TOV(a), TOV(b))); }
W2(packsswb, wasm_i8x16_narrow_i16x8) W2(packssdw, wasm_i16x8_narrow_i32x4)
W2(packusdw, wasm_u16x8_narrow_i32x4)
W2(pmaddwd, wasm_i32x4_dot_i16x8)
W2(packuswb, wasm_u8x16_narrow_i16x8)
static inline u32 pmovmskb(V a) { return (u32)wasm_i8x16_bitmask(TOV(a)); }
static inline V pmuludq(V a, V b) { return FROMV(wasm_u64x2_extmul_low_u32x4(wasm_i32x4_shuffle(TOV(a), TOV(a), 0, 2, 0, 2), wasm_i32x4_shuffle(TOV(b), TOV(b), 0, 2, 0, 2))); }
W2(pminsw, wasm_i16x8_min) W2(pmaxsw, wasm_i16x8_max) W2(pminub, wasm_u8x16_min) W2(pmaxub, wasm_u8x16_max)
W2(pminsb, wasm_i8x16_min) W2(pmaxsb, wasm_i8x16_max) W2(pminuw, wasm_u16x8_min) W2(pmaxuw, wasm_u16x8_max)
W2(pminsd, wasm_i32x4_min) W2(pmaxsd, wasm_i32x4_max) W2(pminud, wasm_u32x4_min) W2(pmaxud, wasm_u32x4_max)
W2(paddsb, wasm_i8x16_add_sat) W2(psubsb, wasm_i8x16_sub_sat) W2(paddsw, wasm_i16x8_add_sat) W2(psubsw, wasm_i16x8_sub_sat)
W2(paddusb, wasm_u8x16_add_sat) W2(psubusb, wasm_u8x16_sub_sat) W2(paddusw, wasm_u16x8_add_sat) W2(psubusw, wasm_u16x8_sub_sat)
W2(pavgb, wasm_u8x16_avgr) W2(pavgw, wasm_u16x8_avgr) W2(pmulld, wasm_i32x4_mul)
W2(pcmpeqq, wasm_i64x2_eq) W2(pcmpgtq, wasm_i64x2_gt)
static inline V pmulhw(V a, V b) { v128_t x = TOV(a), y = TOV(b);
  return FROMV(wasm_i16x8_narrow_i32x4(wasm_i32x4_shr(wasm_i32x4_extmul_low_i16x8(x, y), 16), wasm_i32x4_shr(wasm_i32x4_extmul_high_i16x8(x, y), 16))); }
static inline V pmulhuw(V a, V b) { v128_t x = TOV(a), y = TOV(b);
  return FROMV(wasm_u16x8_narrow_i32x4(wasm_u32x4_shr(wasm_u32x4_extmul_low_u16x8(x, y), 16), wasm_u32x4_shr(wasm_u32x4_extmul_high_u16x8(x, y), 16))); }
// u8 x s8, pairs summed with signed saturation
static inline V pmaddubsw(V a, V b) { v128_t x = TOV(a), y = TOV(b);
  v128_t lo = wasm_i16x8_mul(wasm_u16x8_extend_low_u8x16(x), wasm_i16x8_extend_low_i8x16(y));
  v128_t hi = wasm_i16x8_mul(wasm_u16x8_extend_high_u8x16(x), wasm_i16x8_extend_high_i8x16(y));
  v128_t even = wasm_i16x8_shuffle(lo, hi, 0, 2, 4, 6, 8, 10, 12, 14), odd = wasm_i16x8_shuffle(lo, hi, 1, 3, 5, 7, 9, 11, 13, 15);
  return FROMV(wasm_i16x8_add_sat(even, odd)); }
static inline V psadbw(V a, V b) { v128_t x = TOV(a), y = TOV(b);
  v128_t d = wasm_v128_or(wasm_u8x16_sub_sat(x, y), wasm_u8x16_sub_sat(y, x));
  v128_t s = wasm_u32x4_extadd_pairwise_u16x8(wasm_u16x8_extadd_pairwise_u8x16(d));
  return FROMV(wasm_i64x2_make((s64)(wasm_u32x4_extract_lane(s, 0) + wasm_u32x4_extract_lane(s, 1)), (s64)(wasm_u32x4_extract_lane(s, 2) + wasm_u32x4_extract_lane(s, 3)))); }
static inline V pabs_b(V a) { return FROMV(wasm_i8x16_abs(TOV(a))); }
static inline V pabs_w(V a) { return FROMV(wasm_i16x8_abs(TOV(a))); }
static inline V pabs_d(V a) { return FROMV(wasm_i32x4_abs(TOV(a))); }
static inline V pshufb(V a, V m) { return FROMV(wasm_i8x16_swizzle(TOV(a), wasm_v128_and(TOV(m), wasm_i8x16_splat((s8)0x8F)))); }
#define LANEOP(NAME, F, T, N, EXPR) static inline V NAME(V a, V b) { V r; for (int i = 0; i < N; i++) { T x = a.F[i], y = b.F[i]; r.F[i] = (EXPR); } return r; }
LANEOP(minps, ps, float, 4, x < y ? x : y) LANEOP(maxps, ps, float, 4, x > y ? x : y)
LANEOP(minpd, pd, double, 2, x < y ? x : y) LANEOP(maxpd, pd, double, 2, x > y ? x : y)
static inline V palignr(V dst, V src, u8 n) { u8 t[32]; __builtin_memcpy(t, &src, 16); __builtin_memcpy(t + 16, &dst, 16); V r; for (int i = 0; i < 16; i++) r.b[i] = n + i < 32 ? t[n + i] : 0; return r; }
static inline V pblendw(V a, V b, u8 m) { V r; for (int i = 0; i < 8; i++) r.w[i] = (m >> i) & 1 ? b.w[i] : a.w[i]; return r; }
static inline V blendps(V a, V b, u8 m) { V r; for (int i = 0; i < 4; i++) r.d[i] = (m >> i) & 1 ? b.d[i] : a.d[i]; return r; }
static inline V pblendvb(V a, V b, V m) { return FROMV(wasm_v128_bitselect(TOV(b), TOV(a), wasm_i8x16_shr(TOV(m), 7))); }
static inline V shufpd(V a, V b, u8 i) { V r; r.q[0] = a.q[i & 1]; r.q[1] = b.q[(i >> 1) & 1]; return r; }
static inline V vlow64(u64 lo) { return vfrom64(lo); }
#define PMOVX(NAME, OP) static inline V NAME(V s) { return FROMV(OP(TOV(s))); }
PMOVX(pmovsxbw, wasm_i16x8_extend_low_i8x16) PMOVX(pmovsxwd, wasm_i32x4_extend_low_i16x8) PMOVX(pmovsxdq, wasm_i64x2_extend_low_i32x4)
PMOVX(pmovzxbw, wasm_u16x8_extend_low_u8x16) PMOVX(pmovzxwd, wasm_u32x4_extend_low_u16x8) PMOVX(pmovzxdq, wasm_u64x2_extend_low_u32x4)
static inline V pmovsxbd(V s) { return pmovsxwd(pmovsxbw(s)); }
static inline V pmovsxbq(V s) { return pmovsxdq(pmovsxwd(pmovsxbw(s))); }
static inline V pmovsxwq(V s) { return pmovsxdq(pmovsxwd(s)); }
static inline V pmovzxbd(V s) { return pmovzxwd(pmovzxbw(s)); }
static inline V pmovzxbq(V s) { return pmovzxdq(pmovzxwd(pmovzxbw(s))); }
static inline V pmovzxwq(V s) { return pmovzxdq(pmovzxwd(s)); }
static inline u32 movmskps(V a) { return (u32)wasm_i32x4_bitmask(TOV(a)); }
static inline u32 movmskpd(V a) { return (u32)wasm_i64x2_bitmask(TOV(a)); }
// SSE4.2 string compares (pcmp[ei]str[im]), Intel SDM vol. 2 "PCMPxSTRx": the intermediate result IntRes2 as a
// bit mask, flags set. la / lb: explicit lengths (absolute value, saturated), or -1 for implicit (up to a zero element).
static inline s64 pcmp_elem(V v, int i, int fmt) { switch (fmt) { case 0: return v.b[i]; case 1: return v.w[i]; case 2: return v.sb[i]; default: return v.sw[i]; } }
static inline u32 pcmpstr(Flags *f, V a, V b, s64 la, s64 lb, u8 imm) {
  int fmt = imm & 3, n = fmt & 1 ? 8 : 16, agg = (imm >> 2) & 3, pol = (imm >> 4) & 3;
  if (la < 0) { la = 0; while (la < n && pcmp_elem(a, (int)la, fmt)) la++; } else { if (la < 0) la = -la; if (la > n) la = n; }
  if (lb < 0) { lb = 0; while (lb < n && pcmp_elem(b, (int)lb, fmt)) lb++; } else { if (lb < 0) lb = -lb; if (lb > n) lb = n; }
  u32 r = 0;
  for (int j = 0; j < n; j++) {
    int bit = 0, vb = j < lb;
    switch (agg) {
    case 0: for (int i = 0; i < la && vb && !bit; i++) bit = pcmp_elem(a, i, fmt) == pcmp_elem(b, j, fmt); break;     // equal any
    case 1: for (int i = 0; i + 1 < la && vb && !bit; i += 2) bit = pcmp_elem(b, j, fmt) >= pcmp_elem(a, i, fmt) && pcmp_elem(b, j, fmt) <= pcmp_elem(a, i + 1, fmt); break; // ranges
    case 2: { int va = j < la; bit = va && vb ? pcmp_elem(a, j, fmt) == pcmp_elem(b, j, fmt) : !va && !vb; break; } // equal each
    default: bit = 1; for (int k = 0; k < n - j && bit; k++) { int va = k < la, vbk = j + k < lb; if (!va) break; bit = vbk && pcmp_elem(a, k, fmt) == pcmp_elem(b, j + k, fmt); } // equal ordered
    }
    r |= (u32)bit << j;
  }
  u32 valid = lb >= 32 ? 0xFFFFFFFFu : (1u << lb) - 1, all = (1u << n) - 1;
  if (pol == 1) r = ~r & all; else if (pol == 3) r ^= valid;
  f->cf = r != 0; f->zf = lb < n; f->sf = la < n; f->of = r & 1; f->af = f->pf = 0;
  return r;
}
static inline u32 pcmpstri(u32 r, u8 imm) { int n = imm & 1 ? 8 : 16; return !r ? (u32)n : imm & 0x40 ? 31 - (u32)__builtin_clz(r) : (u32)__builtin_ctz(r); }
static inline V pcmpstrm(u32 r, u8 imm) {
  V m; m.q[0] = m.q[1] = 0;
  if (!(imm & 0x40)) { m.d[0] = r; return m; }
  for (int j = 0; j < (imm & 1 ? 8 : 16); j++) if (r >> j & 1) { if (imm & 1) m.w[j] = 0xFFFF; else m.b[j] = 0xFF; }
  return m;
}
static inline void ptest(Flags *f, V a, V b) { f->zf = ((a.q[0] & b.q[0]) | (a.q[1] & b.q[1])) == 0; f->cf = ((~a.q[0] & b.q[0]) | (~a.q[1] & b.q[1])) == 0; f->of = f->sf = f->af = f->pf = 0; }
static inline s32 cvt_f2i32(double v) { return (v >= -2147483648.0 && v < 2147483648.0) ? (s32)v : (s32)0x80000000; }
static inline s64 cvt_f2i64(double v) { return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (s64)v : (s64)0x8000000000000000ull; }
