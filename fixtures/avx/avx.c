// SPDX-License-Identifier: GPL-2.0-or-later
// AVX, AVX2, FMA and BMI against the lifter: every result is printed in hex, run natively and lifted
// (tests/avx.sh), and the two outputs must be the same. Inputs come through volatile memory so the
// compiler cannot fold the instructions away.
#include <immintrin.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static volatile float fin[16] = { 1.5f, -2.25f, 3.0f, 0.1f, -0.0f, 1e30f, -7.75f, 2.5f, 0.5f, 4.0f, -1.0f, 9.0f, 3.5f, -0.5f, 100.0f, 1e-3f };
static volatile double din[8] = { 1.5, -2.25, 3.0, 0.1, 2.5, -7.0, 1e300, -0.0 };
static volatile int32_t iin[16] = { 1, -2, 3, 0x7fffffff, -5, 6, 0x12345678, 8, 9, -10, 11, 255, 13, -14, 65536, 16 };
static unsigned n;
static void out(const char *name, const void *p, int bytes) {
  const uint32_t *w = p; printf("%-14s", name);
  for (int i = 0; i < bytes / 4; i++) printf(" %08x", w[i]);
  printf("\n"); n++;
}
#define P8(name, v) do { __m256 t_ = (v); out(name, &t_, 32); } while (0)
#define P4(name, v) do { __m128 t_ = (v); out(name, &t_, 16); } while (0)
#define PD(name, v) do { __m256d t_ = (v); out(name, &t_, 32); } while (0)
#define PI(name, v) do { __m256i t_ = (v); out(name, &t_, 32); } while (0)
#define PX(name, v) do { __m128i t_ = (v); out(name, &t_, 16); } while (0)
#define PS(name, v) do { uint64_t t_ = (uint64_t)(v); out(name, &t_, 8); } while (0)
int main(void) {
  float f[16]; double d[8]; int32_t i32[16];
  for (int i = 0; i < 16; i++) { f[i] = fin[i]; i32[i] = iin[i]; }
  for (int i = 0; i < 8; i++) d[i] = din[i];
  __m256 a = _mm256_loadu_ps(f), b = _mm256_loadu_ps(f + 8);
  __m128 a4 = _mm_loadu_ps(f), b4 = _mm_loadu_ps(f + 4);
  __m256d da = _mm256_loadu_pd(d), db = _mm256_loadu_pd(d + 4);
  __m256i ia = _mm256_loadu_si256((void *)i32), ib = _mm256_loadu_si256((void *)(i32 + 8));
  __m128i xa = _mm256_castsi256_si128(ia), xb = _mm256_castsi256_si128(ib);
  P8("addps", _mm256_add_ps(a, b)); P8("subps", _mm256_sub_ps(a, b)); P8("mulps", _mm256_mul_ps(a, b));
  P8("divps", _mm256_div_ps(a, b)); P8("minps", _mm256_min_ps(a, b)); P8("maxps", _mm256_max_ps(a, b));
  P8("sqrtps", _mm256_sqrt_ps(_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a)));
  P8("andps", _mm256_and_ps(a, b)); P8("andnps", _mm256_andnot_ps(a, b)); P8("orps", _mm256_or_ps(a, b)); P8("xorps", _mm256_xor_ps(a, b));
  P8("fmadd", _mm256_fmadd_ps(a, b, a)); P8("fmsub", _mm256_fmsub_ps(a, b, b)); P8("fnmadd", _mm256_fnmadd_ps(a, b, a)); P8("fnmsub", _mm256_fnmsub_ps(a, b, b));
  P4("fmadd_ss", _mm_fmadd_ss(a4, b4, a4)); P4("fnmadd_ss", _mm_fnmadd_ss(a4, b4, b4)); P4("fmsub_ss", _mm_fmsub_ss(a4, b4, a4));
  P4("addss", _mm_add_ss(a4, b4)); P4("mulss", _mm_mul_ss(a4, b4)); P4("divss", _mm_div_ss(a4, b4)); P4("sqrtss", _mm_sqrt_ss(b4));
  P4("minss", _mm_min_ss(a4, b4)); P4("maxss", _mm_max_ss(a4, b4));
  P8("cmp_lt", _mm256_cmp_ps(a, b, _CMP_LT_OQ)); P8("cmp_le", _mm256_cmp_ps(a, b, _CMP_LE_OQ)); P8("cmp_neq", _mm256_cmp_ps(a, b, _CMP_NEQ_UQ));
  P8("cmp_ge", _mm256_cmp_ps(a, b, _CMP_GE_OQ)); P8("cmp_gt", _mm256_cmp_ps(a, b, _CMP_GT_OQ)); P8("cmp_eq", _mm256_cmp_ps(a, a, _CMP_EQ_OQ));
  P4("cmpss_lt", _mm_cmp_ss(a4, b4, _CMP_LT_OS));
  P8("blendv", _mm256_blendv_ps(a, b, a)); P8("blend", _mm256_blend_ps(a, b, 0x5a));
  P8("shufps", _mm256_shuffle_ps(a, b, 0x1b)); P8("permute", _mm256_permute_ps(a, 0x93)); P8("perm2f128", _mm256_permute2f128_ps(a, b, 0x21));
  P8("unpacklo", _mm256_unpacklo_ps(a, b)); P8("unpackhi", _mm256_unpackhi_ps(a, b));
  P8("movehdup", _mm256_movehdup_ps(a)); P8("moveldup", _mm256_moveldup_ps(a));
  P8("insertf128", _mm256_insertf128_ps(a, b4, 1)); P4("extractf128", _mm256_extractf128_ps(b, 1));
  P4("insertps", _mm_insert_ps(a4, b4, 0x9c)); P4("hadd", _mm_hadd_ps(a4, b4)); P8("hadd256", _mm256_hadd_ps(a, b));
  P4("dpps", _mm_dp_ps(a4, b4, 0xf1));
  P8("bcast_ss", _mm256_broadcast_ss((const float *)&f[3])); P8("bcastps", _mm256_broadcastss_ps(b4));
  P8("round_floor", _mm256_round_ps(a, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC)); P8("round_ceil", _mm256_round_ps(b, _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC));
  P8("round_trunc", _mm256_round_ps(b, _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC)); P8("round_near", _mm256_round_ps(_mm256_mul_ps(a, _mm256_set1_ps(3.3f)), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
  P4("roundss", _mm_round_ss(a4, b4, _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC));
  PI("cvttps2dq", _mm256_cvttps_epi32(_mm256_min_ps(b, _mm256_set1_ps(1e9f)))); PI("cvtps2dq", _mm256_cvtps_epi32(_mm256_min_ps(b, _mm256_set1_ps(1e9f))));
  P8("cvtdq2ps", _mm256_cvtepi32_ps(ia)); PD("cvtps2pd", _mm256_cvtps_pd(b4)); P4("cvtpd2ps", _mm256_cvtpd_ps(db));
  PS("movmskps", _mm256_movemask_ps(a)); PS("movmskpd", _mm256_movemask_pd(da)); PS("pmovmskb", _mm256_movemask_epi8(ia));
  PS("testz", _mm256_testz_si256(ia, ib)); PS("testc", _mm256_testc_si256(ia, ia)); PS("testps", _mm256_testz_ps(a, b));
  PD("addpd", _mm256_add_pd(da, db)); PD("mulpd", _mm256_mul_pd(da, db)); PD("divpd", _mm256_div_pd(da, db)); PD("fmaddpd", _mm256_fmadd_pd(da, db, da));
  PD("permute4x64", _mm256_permute4x64_pd(da, 0x4e)); PD("unpacklopd", _mm256_unpacklo_pd(da, db)); PD("blendpd", _mm256_blend_pd(da, db, 0x6));
  PD("sqrtpd", _mm256_sqrt_pd(_mm256_andnot_pd(_mm256_set1_pd(-0.0), da))); PD("maxpd", _mm256_max_pd(da, db));
  PI("paddd", _mm256_add_epi32(ia, ib)); PI("psubd", _mm256_sub_epi32(ia, ib)); PI("pmulld", _mm256_mullo_epi32(ia, ib));
  PI("pand", _mm256_and_si256(ia, ib)); PI("pandn", _mm256_andnot_si256(ia, ib)); PI("pxor", _mm256_xor_si256(ia, ib)); PI("por", _mm256_or_si256(ia, ib));
  PI("pcmpeqd", _mm256_cmpeq_epi32(ia, ib)); PI("pcmpgtd", _mm256_cmpgt_epi32(ia, ib)); PI("pminsd", _mm256_min_epi32(ia, ib)); PI("pmaxud", _mm256_max_epu32(ia, ib));
  PI("pslld", _mm256_slli_epi32(ia, 3)); PI("psrld", _mm256_srli_epi32(ia, 5)); PI("psrad", _mm256_srai_epi32(ia, 7));
  PI("psllvd", _mm256_sllv_epi32(ia, _mm256_and_si256(ib, _mm256_set1_epi32(31)))); PI("psrlvd", _mm256_srlv_epi32(ia, _mm256_and_si256(ib, _mm256_set1_epi32(31))));
  PI("pshufb", _mm256_shuffle_epi8(ia, ib)); PI("pshufd", _mm256_shuffle_epi32(ia, 0x39)); PI("permd", _mm256_permutevar8x32_epi32(ia, _mm256_and_si256(ib, _mm256_set1_epi32(7))));
  PI("pbroadcastd", _mm256_broadcastd_epi32(xb)); PI("punpcklbw", _mm256_unpacklo_epi8(ia, ib)); PI("packusdw", _mm256_packus_epi32(ia, ib)); PI("packsswb", _mm256_packs_epi16(ia, ib));
  PI("paddusb", _mm256_adds_epu8(ia, ib)); PI("pmaddwd", _mm256_madd_epi16(ia, ib)); PI("pmovzxbd", _mm256_cvtepu8_epi32(xa)); PI("pmovsxwd", _mm256_cvtepi16_epi32(xb));
  PI("palignr", _mm256_alignr_epi8(ia, ib, 5)); PI("inserti128", _mm256_inserti128_si256(ia, xb, 1)); PX("extracti128", _mm256_extracti128_si256(ib, 1));
  PI("pblendvb", _mm256_blendv_epi8(ia, ib, ia)); PI("pblendd", _mm256_blend_epi32(ia, ib, 0x33)); PI("pabsd", _mm256_abs_epi32(ib));
  PX("pinsrd", _mm_insert_epi32(xa, i32[9], 2)); PS("pextrd", _mm_extract_epi32(xb, 3)); PX("movd", _mm_cvtsi32_si128(i32[6]));
  uint64_t x = (uint64_t)i32[6] << 20 | (uint32_t)i32[3], y = (uint32_t)i32[12];
  PS("andn", _andn_u64(x, ~y)); PS("blsr", _blsr_u64(x)); PS("rorx", (x >> 13 | x << 51)); PS("shlx", (x << (y & 63)));
  PS("shrx", (x >> (y & 31))); PS("sarx", (uint64_t)((int64_t)-x >> (y & 63))); PS("bzhi", _bzhi_u64(x, 17)); PS("pdep", _pdep_u64(x, 0xf0f0f0f0f0f0ull));
  PS("pext", _pext_u64(x, 0xff00ff00ff00ull)); PS("popcnt", __builtin_popcountll(x));
  // (no lzcnt or tzcnt: the virtual CPU reports no LZCNT or BMI1, and runs them as bsr and bsf, as such a CPU does)
  PS("mulx", _mulx_u64(x, 0x9e3779b97f4a7c15ull, (unsigned long long *)&y) ^ y);
  printf("%u checks\n", n);
  return 0;
}
