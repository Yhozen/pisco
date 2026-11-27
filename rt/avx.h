// SPDX-License-Identifier: GPL-2.0-or-later
// AVX, AVX2 and FMA for lifted code (crates/lifter/src/avx.rs). A ymm register is xmm<n> (X[n], the low 128 bits)
// and c->yh[n] (the high 128 bits); a VEX instruction on an xmm register clears the high half. A fused multiply-add
// is a multiply then an add (WebAssembly has no fused one; the last bit of a result can differ), unless
// PISCO_FMA_EXACT: then float ones are exact but for rare double rounding.
// Included by cpu.h once V, Flags and fpred are defined.
#pragma once
static inline V v_addps(V a, V b) { V r; r.ps = a.ps + b.ps; return r; }
static inline V v_subps(V a, V b) { V r; r.ps = a.ps - b.ps; return r; }
static inline V v_mulps(V a, V b) { V r; r.ps = a.ps * b.ps; return r; }
static inline V v_divps(V a, V b) { V r; r.ps = a.ps / b.ps; return r; }
static inline V v_and(V a, V b) { V r; r.q = a.q & b.q; return r; }
static inline V v_or(V a, V b) { V r; r.q = a.q | b.q; return r; }
static inline V v_xor(V a, V b) { V r; r.q = a.q ^ b.q; return r; }
static inline V v_andn(V a, V b) { V r; r.q = ~a.q & b.q; return r; }
static inline V v_paddd(V a, V b) { V r; r.d = a.d + b.d; return r; }
static inline V v_psubd(V a, V b) { V r; r.d = a.d - b.d; return r; }
static inline V v_pcmpeqd(V a, V b) { return FROMV(wasm_i32x4_eq(TOV(a), TOV(b))); }
static inline V v_pcmpeqb(V a, V b) { return FROMV(wasm_i8x16_eq(TOV(a), TOV(b))); }
static inline V v_psllvd(V a, V b) { V r; for (int i = 0; i < 4; i++) r.d[i] = b.d[i] > 31 ? 0 : a.d[i] << b.d[i]; return r; }
static inline V v_movlhps(V a, V b) { V r; r.q[0] = a.q[0]; r.q[1] = b.q[0]; return r; }
static inline V v_blendvps(V a, V b, V m) { V r; for (int i = 0; i < 4; i++) r.d[i] = m.sd[i] < 0 ? b.d[i] : a.d[i]; return r; }
// the 32 predicates of vcmpps: the low three bits as cmpps's, bit 3 the negated-ordering forms (signalling or not is
// the same here)
static inline u8 v_fpred(u8 p, float a, float b) {
  int un = a != a || b != b;
  switch (p & 0xF) {
  case 0x8: return a == b || un;            // EQ_UQ
  case 0x9: return !(a >= b);               // NGE: less or unordered
  case 0xA: return !(a > b);                // NGT
  case 0xB: return 0;                       // FALSE
  case 0xC: return !un && a != b;           // NEQ_OQ
  case 0xD: return a >= b;                  // GE
  case 0xE: return a > b;                   // GT
  case 0xF: return 1;                       // TRUE
  default: return fpred(p & 7, a, b);
  }
}
static inline V v_cmpps(V a, V b, u8 p) { V r; for (int i = 0; i < 4; i++) r.d[i] = v_fpred(p, a.ps[i], b.ps[i]) ? ~0u : 0; return r; }
static inline float v_round1(float x, u8 imm, u32 mxcsr) {
  int mode = imm & 4 ? (int)((mxcsr >> 13) & 3) : imm & 3;
  switch (mode) { case 1: return __builtin_floorf(x); case 2: return __builtin_ceilf(x); case 3: return __builtin_truncf(x); default: return __builtin_rintf(x); }
}
static inline V v_roundps(V a, u8 imm, u32 mxcsr) { V r; for (int i = 0; i < 4; i++) r.ps[i] = v_round1(a.ps[i], imm, mxcsr); return r; }
static inline V v_sqrtps(V a) { V r; for (int i = 0; i < 4; i++) r.ps[i] = __builtin_sqrtf(a.ps[i]); return r; }
static inline V v_rcpps(V a) { V r; r.ps = 1.0f / a.ps; return r; }
static inline V v_cvttps2dq(V a) { V r; for (int i = 0; i < 4; i++) r.sd[i] = cvt_f2i32(a.ps[i]); return r; }
static inline V v_cvtdq2ps(V a) { V r; for (int i = 0; i < 4; i++) r.ps[i] = (float)a.sd[i]; return r; }
static inline V v_movshdup(V a) { V r; r.d[0] = r.d[1] = a.d[1]; r.d[2] = r.d[3] = a.d[3]; return r; }
static inline V v_movddup(V a) { V r; r.q[0] = r.q[1] = a.q[0]; return r; }
static inline V v_splat32(u32 v) { V r; r.d[0] = r.d[1] = r.d[2] = r.d[3] = v; return r; }
static inline V v_from32(u32 v) { V r = vzero(); r.d[0] = v; return r; }
// vinsertps: element (imm >> 6) of b, or b's low element when it comes from memory (from_mem), into element
// (imm >> 4) & 3 of a; then the elements imm & 15 names are cleared
static inline V v_insertps(V a, V b, u8 imm, int from_mem) { V r = a; r.d[(imm >> 4) & 3] = b.d[from_mem ? 0 : imm >> 6]; for (int i = 0; i < 4; i++) if (imm >> i & 1) r.d[i] = 0; return r; }
static inline void v_testps(Flags *f, V a, V b) { u32 z = 0, c = 0; for (int i = 0; i < 4; i++) { z |= a.d[i] & b.d[i]; c |= ~a.d[i] & b.d[i]; } f->zf = !(z >> 31); f->cf = !(c >> 31); f->of = f->sf = f->af = f->pf = 0; }
// FMA: a * b + c with the product and c negated as the form says
#if PISCO_FMA_EXACT
// The product of two floats is exact in a double, so a * b + c rounds once there and once to float: the fused
// result but for the rare double rounding. About a third slower in the raymarching demo, so not the default
// (build with OPT='-O2 -DPISCO_FMA_EXACT=1'; tests/avx.sh does)
static inline float v_fma1(float a, float b, float c, int neg_prod, int neg_c) { double p = (double)a * b; return (float)((neg_prod ? -p : p) + (neg_c ? -(double)c : (double)c)); }
typedef double v_d4 __attribute__((vector_size(32)));
static inline V v_fma(V a, V b, V c, int neg_prod, int neg_c) {
  v_d4 p = __builtin_convertvector(a.ps, v_d4) * __builtin_convertvector(b.ps, v_d4), cc = __builtin_convertvector(c.ps, v_d4);
  V r; r.ps = __builtin_convertvector((neg_prod ? -p : p) + (neg_c ? -cc : cc), typeof(r.ps)); return r;
}
#else
static inline float v_fma1(float a, float b, float c, int neg_prod, int neg_c) { float p = a * b; return (neg_prod ? -p : p) + (neg_c ? -c : c); }
static inline V v_fma(V a, V b, V c, int neg_prod, int neg_c) { V r; r.ps = (neg_prod ? -(a.ps * b.ps) : a.ps * b.ps) + (neg_c ? -c.ps : c.ps); return r; }
#endif
// vpermpd ymm: four doubles from the 256 bits (lo, hi) the immediate picks
static inline void v_permpd(V lo, V hi, u8 imm, V *out_lo, V *out_hi) { u64 s[4] = {lo.q[0], lo.q[1], hi.q[0], hi.q[1]}; out_lo->q[0] = s[imm & 3]; out_lo->q[1] = s[(imm >> 2) & 3]; out_hi->q[0] = s[(imm >> 4) & 3]; out_hi->q[1] = s[(imm >> 6) & 3]; }
// lane-wise integer and double arithmetic (the AVX2 forms of paddb ... divpd, on each 128-bit half)
#define V_LANE2(NAME, F, OP) static inline V NAME(V a, V b) { V r; r.F = a.F OP b.F; return r; }
V_LANE2(v_lane_badd, b, +) V_LANE2(v_lane_wadd, w, +) V_LANE2(v_lane_qadd, q, +) V_LANE2(v_lane_bsub, b, -) V_LANE2(v_lane_wsub, w, -)
V_LANE2(v_lane_qsub, q, -) V_LANE2(v_lane_wmul, w, *) V_LANE2(v_lane_pdadd, pd, +) V_LANE2(v_lane_pdsub, pd, -) V_LANE2(v_lane_pdmul, pd, *)
V_LANE2(v_lane_pddiv, pd, /)
static inline V v_pcmpeqw(V a, V b) { return FROMV(wasm_i16x8_eq(TOV(a), TOV(b))); }
static inline V v_pcmpeqq(V a, V b) { return FROMV(wasm_i64x2_eq(TOV(a), TOV(b))); }
static inline V v_pcmpgtb(V a, V b) { return FROMV(wasm_i8x16_gt(TOV(a), TOV(b))); }
static inline V v_pcmpgtw(V a, V b) { return FROMV(wasm_i16x8_gt(TOV(a), TOV(b))); }
static inline V v_pcmpgtd(V a, V b) { return FROMV(wasm_i32x4_gt(TOV(a), TOV(b))); }
static inline V v_pcmpgtq(V a, V b) { return FROMV(wasm_i64x2_gt(TOV(a), TOV(b))); }
static inline V v_psrlvd(V a, V b) { V r; for (int i = 0; i < 4; i++) r.d[i] = b.d[i] > 31 ? 0 : a.d[i] >> b.d[i]; return r; }
static inline V v_psravd(V a, V b) { V r; for (int i = 0; i < 4; i++) r.sd[i] = a.sd[i] >> (b.d[i] > 31 ? 31 : b.d[i]); return r; }
static inline V v_psllvq(V a, V b) { V r; for (int i = 0; i < 2; i++) r.q[i] = b.q[i] > 63 ? 0 : a.q[i] << b.q[i]; return r; }
static inline V v_psrlvq(V a, V b) { V r; for (int i = 0; i < 2; i++) r.q[i] = b.q[i] > 63 ? 0 : a.q[i] >> b.q[i]; return r; }
static inline V v_haddps(V a, V b) { V r; r.ps[0] = a.ps[0] + a.ps[1]; r.ps[1] = a.ps[2] + a.ps[3]; r.ps[2] = b.ps[0] + b.ps[1]; r.ps[3] = b.ps[2] + b.ps[3]; return r; }
static inline V v_hsubps(V a, V b) { V r; r.ps[0] = a.ps[0] - a.ps[1]; r.ps[1] = a.ps[2] - a.ps[3]; r.ps[2] = b.ps[0] - b.ps[1]; r.ps[3] = b.ps[2] - b.ps[3]; return r; }
static inline V v_haddpd(V a, V b) { V r; r.pd[0] = a.pd[0] + a.pd[1]; r.pd[1] = b.pd[0] + b.pd[1]; return r; }
static inline V v_addsubps(V a, V b) { V r; r.ps[0] = a.ps[0] - b.ps[0]; r.ps[1] = a.ps[1] + b.ps[1]; r.ps[2] = a.ps[2] - b.ps[2]; r.ps[3] = a.ps[3] + b.ps[3]; return r; }
// dpps: the products imm's high four bits pick, summed in the CPU's order, into the elements its low four bits pick
static inline V v_dpps(V a, V b, u8 imm) {
  float p[4]; for (int i = 0; i < 4; i++) p[i] = (imm >> (4 + i) & 1) ? a.ps[i] * b.ps[i] : 0.0f;
  float s = (p[0] + p[1]) + (p[2] + p[3]); V r; for (int i = 0; i < 4; i++) r.ps[i] = (imm >> i & 1) ? s : 0.0f; return r;
}
static inline u8 v_fpredd(u8 p, double a, double b) {
  int un = a != a || b != b;
  switch (p & 0xF) {
  case 0x8: return a == b || un; case 0x9: return !(a >= b); case 0xA: return !(a > b); case 0xB: return 0;
  case 0xC: return !un && a != b; case 0xD: return a >= b; case 0xE: return a > b; case 0xF: return 1;
  default: return fpred(p & 7, a, b);
  }
}
static inline V v_cmppd(V a, V b, u8 p) { V r; for (int i = 0; i < 2; i++) r.q[i] = v_fpredd(p, a.pd[i], b.pd[i]) ? ~0ull : 0; return r; }
static inline double v_round1d(double x, u8 imm, u32 mxcsr) {
  int mode = imm & 4 ? (int)((mxcsr >> 13) & 3) : imm & 3;
  switch (mode) { case 1: return __builtin_floor(x); case 2: return __builtin_ceil(x); case 3: return __builtin_trunc(x); default: return __builtin_rint(x); }
}
static inline V v_roundpd(V a, u8 imm, u32 mxcsr) { V r; for (int i = 0; i < 2; i++) r.pd[i] = v_round1d(a.pd[i], imm, mxcsr); return r; }
static inline V v_sqrtpd(V a) { V r; for (int i = 0; i < 2; i++) r.pd[i] = __builtin_sqrt(a.pd[i]); return r; }
static inline V v_movsldup(V a) { V r; r.d[0] = r.d[1] = a.d[0]; r.d[2] = r.d[3] = a.d[2]; return r; }
// cvtps2dq rounds as MXCSR says (nearest-even unless the title changed it)
static inline V v_cvtps2dq(V a, u32 mxcsr) { V r; for (int i = 0; i < 4; i++) r.sd[i] = cvt_f2i32(v_round1(a.ps[i], 4, mxcsr)); return r; }
static inline V v_blendvpd(V a, V b, V m) { V r; for (int i = 0; i < 2; i++) r.q[i] = (s64)m.q[i] < 0 ? b.q[i] : a.q[i]; return r; }
// vpermilps with a variable control: each element picks one of its own half's four
static inline V v_permilps(V a, V c) { V r; for (int i = 0; i < 4; i++) r.d[i] = a.d[c.d[i] & 3]; return r; }
// vpermd / vpermps: eight dwords of the table (t0, t1) the indices (i0, i1) pick
static inline void v_permd(V i0, V i1, V t0, V t1, V *r0, V *r1) {
  u32 t[8], ix[8]; __builtin_memcpy(t, &t0, 16); __builtin_memcpy(t + 4, &t1, 16); __builtin_memcpy(ix, &i0, 16); __builtin_memcpy(ix + 4, &i1, 16);
  for (int i = 0; i < 4; i++) { r0->d[i] = t[ix[i] & 7]; r1->d[i] = t[ix[4 + i] & 7]; }
}
static inline V v_splat64(u64 v) { V r; r.q[0] = r.q[1] = v; return r; }
static inline V v_splat16(u16 v) { V r; for (int i = 0; i < 8; i++) r.w[i] = v; return r; }
static inline V v_splat8(u8 v) { V r; for (int i = 0; i < 16; i++) r.b[i] = v; return r; }
// vptest on 256 bits: ZF if a & b is zero, CF if ~a & b is
static inline void v_ptest256(Flags *f, V a0, V a1, V b0, V b1) {
  u64 z = (a0.q[0] & b0.q[0]) | (a0.q[1] & b0.q[1]) | (a1.q[0] & b1.q[0]) | (a1.q[1] & b1.q[1]);
  u64 c = (~a0.q[0] & b0.q[0]) | (~a0.q[1] & b0.q[1]) | (~a1.q[0] & b1.q[0]) | (~a1.q[1] & b1.q[1]);
  f->zf = z == 0; f->cf = c == 0; f->of = f->sf = f->af = f->pf = 0;
}
// vtestps / vtestpd: the same on the sign bits only (bit 31 or 63 of each element)
static inline void v_testp256(Flags *f, V a0, V a1, V b0, V b1, int bit) {
  u64 m = bit == 31 ? 0x8000000080000000ull : 0x8000000000000000ull;
  u64 z = ((a0.q[0] & b0.q[0]) | (a0.q[1] & b0.q[1]) | (a1.q[0] & b1.q[0]) | (a1.q[1] & b1.q[1])) & m;
  u64 c = ((~a0.q[0] & b0.q[0]) | (~a0.q[1] & b0.q[1]) | (~a1.q[0] & b1.q[0]) | (~a1.q[1] & b1.q[1])) & m;
  f->zf = z == 0; f->cf = c == 0; f->of = f->sf = f->af = f->pf = 0;
}
static inline u64 v_pdep(u64 src, u64 mask) { u64 r = 0; for (u64 b = 1; mask; b <<= 1) { if (src & b) r |= mask & -mask; mask &= mask - 1; } return r; }
static inline u64 v_pext(u64 src, u64 mask) { u64 r = 0; for (u64 b = 1; mask; b <<= 1) { if (src & mask & -mask) r |= b; mask &= mask - 1; } return r; }
static inline V v_fmapd(V a, V b, V c, int neg_prod, int neg_c) { V r; r.pd = (neg_prod ? -(a.pd * b.pd) : a.pd * b.pd) + (neg_c ? -c.pd : c.pd); return r; }
static inline double v_fma1d(double a, double b, double c, int neg_prod, int neg_c) { double p = a * b; return (neg_prod ? -p : p) + (neg_c ? -c : c); }
