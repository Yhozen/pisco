// SPDX-License-Identifier: GPL-2.0-or-later
// The Direct3D 9 shader translator (rt/windows-x86/d3d9_sm.c), built natively: shaders assembled here token by token
// cover every opcode, register type, modifier and form of flow control it takes, for each shader model from 1.1 to
// 2.x; each one's WGSL is written to <out dir>/<name>.wgsl for naga (tests/gfx/d3d9-sm.sh). Shaders it must refuse
// are checked to be refused. With files of captured bytecode (an app's shaders, from the oracle: never stored in
// the repository) as further arguments, those are translated too.
//   d3d9-sm <out dir> [bytecode files...]
#include "d3d9_sm.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- a little assembler ----
typedef struct { uint32_t t[1024]; int n, major; } Asm;
enum { TEMP = 0, INPUT = 1, CONST = 2, ADDR = 3, TEXR = 3, RASTOUT = 4, ATTROUT = 5, TEXCRDOUT = 6, CONSTINT = 7, COLOROUT = 8, DEPTHOUT = 9,
       SAMPLER = 10, CONSTBOOL = 14, LOOP = 15, LABEL = 18, PRED = 19 };
#define XYZW 0xE4u
#define XXXX 0x00u
#define YYYY 0x55u
#define ZZZZ 0xAAu
#define WWWW 0xFFu
static uint32_t type_bits(uint32_t ty) { return ((ty & 7) << 28) | (((ty >> 3) & 3) << 11); }
static uint32_t D(uint32_t ty, uint32_t num, uint32_t mask) { return 0x80000000u | type_bits(ty) | (mask << 16) | num; }
static uint32_t Dm(uint32_t ty, uint32_t num, uint32_t mask, uint32_t mod, uint32_t shift) { return D(ty, num, mask) | (mod << 20) | (shift << 24); }
static uint32_t S(uint32_t ty, uint32_t num) { return 0x80000000u | type_bits(ty) | (XYZW << 16) | num; }
static uint32_t Ss(uint32_t ty, uint32_t num, uint32_t swz) { return 0x80000000u | type_bits(ty) | (swz << 16) | num; }
static uint32_t Sm(uint32_t ty, uint32_t num, uint32_t swz, uint32_t mod) { return Ss(ty, num, swz) | (mod << 24); }
static uint32_t Srel(uint32_t ty, uint32_t num) { return S(ty, num) | (1u << 13); }
static uint32_t fbits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static void begin(Asm *a, int pixel, int major, int minor) { a->n = 0; a->major = major; a->t[a->n++] = (pixel ? 0xFFFF0000u : 0xFFFE0000u) | (uint32_t)(major << 8) | (uint32_t)minor; }
// an instruction and its parameter tokens (a count, then the tokens); `ctrl` goes to bits 16-23
static void op_(Asm *a, uint32_t op, uint32_t ctrl, int n, ...) {
  va_list ap; va_start(ap, n);
  a->t[a->n++] = op | (ctrl << 16) | (a->major >= 2 ? (uint32_t)n << 24 : 0);
  for (int i = 0; i < n; i++) a->t[a->n++] = va_arg(ap, uint32_t);
  va_end(ap);
}
#define OP(a, op, ...) op_(a, op, 0, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), __VA_ARGS__)
#define OPC(a, op, ctrl, ...) op_(a, op, ctrl, (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), __VA_ARGS__)
static void op0(Asm *a, uint32_t op) { a->t[a->n++] = op; }
static void def(Asm *a, uint32_t reg, float x, float y, float z, float w) { op_(a, 81, 0, 5, D(CONST, reg, 15), fbits(x), fbits(y), fbits(z), fbits(w)); }
static void dcl(Asm *a, uint32_t decl, uint32_t dst) { op_(a, 31, 0, 2, 0x80000000u | decl, dst); }
static void comment(Asm *a) { a->t[a->n++] = 0xFFFE | (2u << 16); a->t[a->n++] = 0x42424242; a->t[a->n++] = 0x43434343; }
static void end(Asm *a) { a->t[a->n++] = 0x0000FFFF; }
enum { MOV = 1, ADD, SUB, MAD, MUL, RCP, RSQ, DP3, DP4, MIN, MAX, SLT, SGE, EXP, LOG, LIT, DST, LRP, FRC, M4x4, M4x3, M3x4, M3x3, M3x2,
       CALL, CALLNZ, LOOPOP, RET, ENDLOOP, LABELOP, DCL, POW, CRS, SGN, ABS, NRM, SINCOS, REP, ENDREP, IF, IFC, ELSE, ENDIF, BREAK, BREAKC,
       MOVA, DEFB, DEFI, TEXCOORD = 64, TEXKILL, TEX, TEXBEM, EXPP = 78, LOGP, CND, CMP = 88, BEM, DP2ADD, DSX, DSY, TEXLDD, SETP, TEXLDL, BREAKP, PHASE = 0xFFFD };
enum { NEG = 1, BIAS, BIASNEG, SIGN, SIGNNEG, COMP, X2, X2NEG, DZ, DW, ABSM, ABSNEG, NOT };
static uint32_t usage(uint32_t u, uint32_t i) { return u | (i << 16); }

static int failures, written;
static void translate(const char *dir, const char *name, const uint32_t *t, size_t n, const SmLinkKey *link, int must_refuse) {
  static char out[1 << 18]; SmInfo info;
  size_t len = sm_wgsl(t, n, link, &info, out, sizeof out);
  if (must_refuse) {
    if (len) { printf("%s: translated, but should have been refused\n", name); failures++; }
    else printf("%s refused: %s\n", name, out);
    return;
  }
  if (!len) { printf("%s FAILED: %s\n", name, out); failures++; return; }
  char path[512]; snprintf(path, sizeof path, "%s/%s.wgsl", dir, name);
  FILE *f = fopen(path, "w"); if (!f) { perror(path); exit(1); }
  fwrite(out, 1, len, f); fclose(f); written++;
  printf("%s.wgsl %s %u.%u floats %u samplers 0x%x inputs 0x%x varyings 0x%x\n", name, info.pixel ? "ps" : "vs", info.major, info.minor, info.float_count, info.samplers, info.inputs, info.varyings);
}
static void run(const char *dir, const char *name, Asm *a, const SmLinkKey *link) { translate(dir, name, a->t, (size_t)a->n, link, 0); }
static void must_refuse(const char *dir, const char *name, Asm *a) { translate(dir, name, a->t, (size_t)a->n, 0, 1); }

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: d3d9-sm <out dir> [bytecode files...]\n"); return 2; }
  const char *dir = argv[1];
  Asm A, *a = &A; SmLinkKey link;

  // vs_1_1: transforms, lighting arithmetic, the matrix forms, a0 indexing, every output
  begin(a, 0, 1, 1); comment(a);
  dcl(a, usage(0, 0), D(INPUT, 0, 15)); dcl(a, usage(3, 0), D(INPUT, 1, 15)); dcl(a, usage(5, 0), D(INPUT, 2, 15)); dcl(a, usage(10, 0), D(INPUT, 3, 15));
  def(a, 90, 0.5f, 1.0f, 2.0f, 255.0f);
  OP(a, M4x4, D(RASTOUT, 0, 15), S(INPUT, 0), S(CONST, 0));
  OP(a, DP3, D(TEMP, 0, 1), S(INPUT, 1), S(CONST, 4));
  OP(a, MAX, D(TEMP, 0, 1), Ss(TEMP, 0, XXXX), Ss(CONST, 90, XXXX));
  OP(a, MUL, D(ATTROUT, 0, 15), Ss(TEMP, 0, XXXX), S(INPUT, 3));
  OP(a, MOV, D(TEXCRDOUT, 0, 3), S(INPUT, 2));
  OP(a, MOV, D(ADDR, 0, 1), Ss(INPUT, 2, ZZZZ));
  OP(a, MOV, D(ATTROUT, 1, 15), Srel(CONST, 10));
  OP(a, RCP, D(TEMP, 1, 1), Ss(CONST, 90, WWWW)); OP(a, RSQ, D(TEMP, 1, 2), Ss(CONST, 90, ZZZZ));
  OP(a, EXP, D(TEMP, 1, 4), Ss(TEMP, 0, XXXX)); OP(a, LOG, D(TEMP, 1, 8), Ss(TEMP, 0, XXXX));
  OP(a, EXPP, D(TEMP, 2, 15), Ss(TEMP, 1, WWWW)); OP(a, LOGP, D(TEMP, 3, 15), Ss(TEMP, 1, WWWW));
  OP(a, LIT, D(TEMP, 4, 15), S(TEMP, 1)); OP(a, DST, D(TEMP, 5, 15), S(TEMP, 2), S(TEMP, 3));
  OP(a, FRC, D(TEMP, 6, 3), S(TEMP, 5)); OP(a, SLT, D(TEMP, 7, 15), S(TEMP, 4), S(TEMP, 5)); OP(a, SGE, D(TEMP, 8, 15), S(TEMP, 4), Sm(TEMP, 5, XYZW, NEG));
  OP(a, MIN, D(TEMP, 9, 15), S(TEMP, 7), S(TEMP, 8)); OP(a, MAD, D(TEMP, 9, 15), S(TEMP, 9), S(CONST, 90), S(TEMP, 6)); OP(a, SUB, D(TEMP, 10, 15), S(TEMP, 9), S(TEMP, 4));
  OP(a, ADD, D(TEMP, 10, 15), S(TEMP, 10), S(TEMP, 2)); OP(a, DP4, D(TEMP, 11, 8), S(TEMP, 10), S(CONST, 11));
  OP(a, M4x3, D(TEMP, 11, 7), S(TEMP, 10), S(CONST, 12)); OP(a, M3x4, D(TEXCRDOUT, 1, 15), S(TEMP, 11), S(CONST, 16));
  OP(a, M3x3, D(TEXCRDOUT, 2, 7), S(INPUT, 1), S(CONST, 20)); OP(a, M3x2, D(TEXCRDOUT, 3, 3), S(INPUT, 1), S(CONST, 24));
  OP(a, MOV, D(RASTOUT, 1, 1), Ss(TEMP, 11, WWWW)); OP(a, MOV, D(RASTOUT, 2, 1), Ss(CONST, 90, YYYY));
  end(a); run(dir, "vs_1_1-arith", a, 0);
  memset(&link, 0, sizeof link); link.input_types[3] = SM_INPUT_UINT; link.input_types[2] = SM_INPUT_SINT;   // UBYTE4 and SHORT4 inputs
  run(dir, "vs_1_1-arith-integer-inputs", a, &link);

  // vs_2_0: def'd integers and booleans, loops with aL indexing, rep, if / else, ifc, break, call, callnz, the new arithmetic
  begin(a, 0, 2, 0);
  dcl(a, usage(0, 0), D(INPUT, 0, 15)); dcl(a, usage(2, 0), D(INPUT, 1, 15)); dcl(a, usage(1, 0), D(INPUT, 2, 15)); dcl(a, usage(6, 0), D(INPUT, 3, 15)); dcl(a, usage(7, 0), D(INPUT, 4, 15));
  def(a, 100, 0.0f, 1.0f, 3.14159265f, -1.0f);
  op_(a, DEFI, 0, 5, D(CONSTINT, 0, 15), 4u, 0u, 1u, 0u);
  op_(a, DEFB, 0, 2, D(CONSTBOOL, 3, 15), 1u);
  OP(a, MOV, D(TEMP, 0, 15), Ss(CONST, 100, XXXX));
  OP(a, LOOPOP, S(LOOP, 0), S(CONSTINT, 0));
    OP(a, MOVA, D(ADDR, 0, 1), Ss(INPUT, 1, XXXX));
    // c30[aL]: a relative source and its address token (the loop register, its x)
    a->t[a->n++] = MAD | (5u << 24); a->t[a->n++] = D(TEMP, 0, 15); a->t[a->n++] = Srel(CONST, 30); a->t[a->n++] = 0x80000000u | type_bits(LOOP);
    a->t[a->n++] = Ss(INPUT, 2, XXXX); a->t[a->n++] = S(TEMP, 0);
    // c40[a0.x]
    a->t[a->n++] = ADD | (4u << 24); a->t[a->n++] = D(TEMP, 0, 15); a->t[a->n++] = Srel(CONST, 40); a->t[a->n++] = 0x80000000u | type_bits(ADDR); a->t[a->n++] = S(TEMP, 0);
  op0(a, ENDLOOP);
  OP(a, REP, S(CONSTINT, 1)); OP(a, ADD, D(TEMP, 0, 15), S(TEMP, 0), Ss(CONST, 100, YYYY)); OPC(a, BREAKC, 2, Ss(TEMP, 0, XXXX), Ss(CONST, 100, ZZZZ)); op0(a, ENDREP);
  OP(a, IF, S(CONSTBOOL, 3)); OP(a, NRM, D(TEMP, 1, 15), S(INPUT, 3)); op0(a, ELSE); OP(a, CRS, D(TEMP, 1, 7), S(INPUT, 3), S(INPUT, 4)); op0(a, ENDIF);
  OP(a, REP, S(CONSTINT, 1)); OP(a, IF, Sm(CONSTBOOL, 5, XXXX, NOT)); op0(a, BREAK); op0(a, ENDIF); op0(a, ENDREP);
  OPC(a, IFC, 4, Ss(TEMP, 0, XXXX), Ss(TEMP, 1, YYYY)); OP(a, SGN, D(TEMP, 2, 15), S(TEMP, 1), S(TEMP, 3), S(TEMP, 4)); op0(a, ENDIF);
  OP(a, POW, D(TEMP, 3, 1), Ss(TEMP, 0, XXXX), Ss(CONST, 100, YYYY)); OP(a, ABS, D(TEMP, 4, 15), S(TEMP, 2));
  OP(a, SINCOS, D(TEMP, 5, 3), Ss(TEMP, 3, XXXX), S(CONST, 101), S(CONST, 102)); OP(a, LRP, D(TEMP, 6, 15), S(TEMP, 5), S(TEMP, 4), S(TEMP, 1));
  OP(a, CALL, S(LABEL, 0)); OP(a, CALLNZ, S(LABEL, 1), S(CONSTBOOL, 2));
  OP(a, M4x4, D(RASTOUT, 0, 15), S(INPUT, 0), S(CONST, 0)); OP(a, MOV, D(ATTROUT, 0, 15), S(TEMP, 6)); OP(a, MOV, D(TEXCRDOUT, 7, 15), S(TEMP, 0));
  op0(a, RET);
  OP(a, LABELOP, S(LABEL, 0)); OP(a, MUL, D(TEMP, 6, 15), S(TEMP, 6), Ss(CONST, 100, YYYY)); op0(a, RET);
  OP(a, LABELOP, S(LABEL, 1)); OP(a, REP, S(CONSTINT, 0)); OP(a, ADD, D(TEMP, 6, 15), S(TEMP, 6), S(TEMP, 0)); op0(a, ENDREP); op0(a, RET);
  end(a); run(dir, "vs_2_0-flow", a, 0);

  // vs_2_x: predication (setp, a predicated write, breakp), nested loops with aL
  begin(a, 0, 2, 1);
  dcl(a, usage(0, 0), D(INPUT, 0, 15));
  OPC(a, SETP, 1, D(PRED, 0, 15), S(INPUT, 0), S(CONST, 4));
  a->t[a->n++] = MOV | (3u << 24) | (1u << 28); a->t[a->n++] = D(TEMP, 0, 15); a->t[a->n++] = Ss(PRED, 0, XYZW); a->t[a->n++] = S(CONST, 5);
  OP(a, LOOPOP, S(LOOP, 0), S(CONSTINT, 2)); OP(a, LOOPOP, S(LOOP, 0), S(CONSTINT, 3));
  OP(a, ADD, D(TEMP, 0, 15), S(TEMP, 0), S(LOOP, 0)); OP(a, BREAKP, Ss(PRED, 0, YYYY)); op0(a, ENDLOOP); op0(a, ENDLOOP);
  OP(a, ADD, D(RASTOUT, 0, 15), S(INPUT, 0), S(TEMP, 0));
  end(a); run(dir, "vs_2_x-predication", a, 0);

  // ps_1_1: tex, texcoord, texkill, the arithmetic with source modifiers and shifts, cnd, constants clamped
  begin(a, 1, 1, 1);
  def(a, 7, 2.0f, 0.5f, -3.0f, 1.0f);
  OP(a, TEX, D(TEXR, 0, 15)); OP(a, TEX, D(TEXR, 1, 15)); OP(a, TEXCOORD, D(TEXR, 2, 15)); OP(a, TEXKILL, D(TEXR, 3, 15));
  OP(a, MUL, D(TEMP, 0, 15), S(TEXR, 0), S(INPUT, 0));
  OP(a, DP3, Dm(TEMP, 1, 15, 1, 0), Sm(TEXR, 1, XYZW, SIGN), Sm(INPUT, 1, XYZW, SIGN));
  OP(a, MAD, Dm(TEMP, 0, 7, 0, 1), S(TEMP, 0), S(TEMP, 1), Sm(TEXR, 2, XYZW, BIAS));
  OP(a, LRP, D(TEMP, 0, 7), Sm(CONST, 7, WWWW, COMP), S(TEMP, 0), S(CONST, 0));
  OP(a, CND, D(TEMP, 0, 8), Ss(TEMP, 1, WWWW), S(CONST, 1), Sm(CONST, 7, XYZW, NEG));
  OP(a, ADD, Dm(TEMP, 0, 15, 0, 15), S(TEMP, 0), Sm(TEXR, 0, XYZW, X2));
  end(a); run(dir, "ps_1_1-tex", a, 0);
  memset(&link, 0, sizeof link); link.fog = 1; link.alpha_test = 7; link.sampler_types[1] = SM_TEX_CUBE;
  run(dir, "ps_1_1-tex-fog-alpha-cube", a, &link);

  // ps_1_4: texld / texcrd with _dz, two phases, cmp
  begin(a, 1, 1, 4);
  OP(a, TEXCOORD, D(TEMP, 1, 7), Sm(TEXR, 1, XYZW, DZ)); OP(a, TEX, D(TEMP, 0, 15), S(TEXR, 0)); OP(a, TEX, D(TEMP, 2, 15), S(TEMP, 1));
  op0(a, PHASE);
  OP(a, CMP, D(TEMP, 0, 15), Sm(TEMP, 2, XYZW, BIAS), S(TEMP, 0), S(CONST, 2));
  end(a); run(dir, "ps_1_4-phase", a, 0);

  // ps_2_0: declarations of every input and sampler kind, texld / texldp / texldb, texkill, the arithmetic, several outputs, depth
  begin(a, 1, 2, 0);
  def(a, 0, 0.5f, 0.25f, 1.0f, 0.0f);
  dcl(a, 0, D(INPUT, 0, 15)); dcl(a, 0, D(INPUT, 1, 7)); dcl(a, 0, D(TEXR, 0, 3)); dcl(a, 0, D(TEXR, 1, 15)); dcl(a, 0, D(TEXR, 7, 15));
  dcl(a, 2u << 27, D(SAMPLER, 0, 15)); dcl(a, 3u << 27, D(SAMPLER, 1, 15)); dcl(a, 4u << 27, D(SAMPLER, 2, 15));
  OP(a, TEX, D(TEMP, 0, 15), S(TEXR, 0), S(SAMPLER, 0));
  OPC(a, TEX, 1, D(TEMP, 1, 15), S(TEXR, 1), S(SAMPLER, 1));
  OPC(a, TEX, 2, D(TEMP, 2, 15), S(TEXR, 7), S(SAMPLER, 2));
  OP(a, TEXKILL, D(TEMP, 2, 15));
  OP(a, DP2ADD, D(TEMP, 3, 1), S(TEMP, 0), S(TEMP, 1), Ss(CONST, 0, XXXX));
  OP(a, CMP, D(TEMP, 4, 15), Sm(TEMP, 3, XXXX, NEG), S(TEMP, 0), S(TEMP, 1));
  OP(a, RCP, D(TEMP, 5, 1), Ss(TEMP, 3, XXXX)); OP(a, RSQ, D(TEMP, 5, 2), Ss(TEMP, 0, YYYY)); OP(a, POW, D(TEMP, 5, 4), Sm(TEMP, 0, ZZZZ, ABSM), Ss(CONST, 0, YYYY));
  OP(a, EXP, D(TEMP, 5, 8), Ss(TEMP, 1, XXXX)); OP(a, LOG, D(TEMP, 6, 1), Ss(TEMP, 1, YYYY)); OP(a, NRM, D(TEMP, 7, 7), S(TEMP, 4)); OP(a, FRC, D(TEMP, 8, 15), S(INPUT, 1));
  OP(a, SINCOS, D(TEMP, 9, 3), Ss(TEMP, 5, XXXX), S(CONST, 1), S(CONST, 2));
  OP(a, MAD, Dm(COLOROUT, 0, 15, 1, 0), S(TEMP, 4), S(INPUT, 0), S(TEMP, 7)); OP(a, MOV, D(COLOROUT, 1, 15), S(TEMP, 9)); OP(a, MOV, D(DEPTHOUT, 0, 1), Ss(TEMP, 5, ZZZZ));
  end(a); run(dir, "ps_2_0-samplers", a, 0);
  memset(&link, 0, sizeof link); link.fog = 1; link.alpha_test = 5;
  run(dir, "ps_2_0-samplers-fog-alpha", a, &link);

  // ps_2_x: derivatives, texldd, flow control with booleans and integers, predication
  begin(a, 1, 2, 1);
  dcl(a, 0, D(TEXR, 0, 15)); dcl(a, 2u << 27, D(SAMPLER, 3, 15));
  OP(a, DSX, D(TEMP, 0, 15), S(TEXR, 0)); OP(a, DSY, D(TEMP, 1, 15), S(TEXR, 0));
  OP(a, TEXLDD, D(TEMP, 2, 15), S(TEXR, 0), S(SAMPLER, 3), S(TEMP, 0), S(TEMP, 1));
  OP(a, IF, S(CONSTBOOL, 0)); OP(a, REP, S(CONSTINT, 0)); OP(a, ADD, D(TEMP, 2, 15), S(TEMP, 2), S(CONST, 3)); OPC(a, BREAKC, 1, Ss(TEMP, 2, XXXX), Ss(CONST, 4, XXXX)); op0(a, ENDREP); op0(a, ENDIF);
  OPC(a, SETP, 3, D(PRED, 0, 15), S(TEMP, 2), S(CONST, 5));
  OP(a, IF, Ss(PRED, 0, ZZZZ)); OP(a, MOV, D(TEMP, 2, 8), Ss(CONST, 5, WWWW)); op0(a, ENDIF);
  OP(a, MOV, D(COLOROUT, 0, 15), S(TEMP, 2));
  end(a); run(dir, "ps_2_x-flow", a, 0);

  // what is refused
  begin(a, 0, 3, 0); end(a); must_refuse(dir, "vs_3_0", a);
  begin(a, 1, 1, 1); OP(a, TEX, D(TEXR, 0, 15)); OP(a, TEXBEM, D(TEXR, 1, 15), S(TEXR, 0)); end(a); must_refuse(dir, "ps_1_1-texbem", a);
  begin(a, 0, 2, 0); OP(a, MOV, D(RASTOUT, 0, 15), S(INPUT, 0)); must_refuse(dir, "no-end", a);
  begin(a, 0, 2, 0); OP(a, 1000, D(TEMP, 0, 15)); end(a); must_refuse(dir, "unknown-op", a);

  // captured bytecode: a file of 32-bit little-endian tokens each
  for (int i = 2; i < argc; i++) {
    FILE *f = fopen(argv[i], "rb"); if (!f) { perror(argv[i]); failures++; continue; }
    static uint32_t buf[1 << 16]; size_t n = fread(buf, 4, sizeof buf / 4, f); fclose(f);
    const char *base = strrchr(argv[i], '/'); base = base ? base + 1 : argv[i];
    char name[256]; snprintf(name, sizeof name, "captured-%s", base);
    char *dot = strrchr(name, '.'); if (dot && dot > name + 9) *dot = 0;
    translate(dir, name, buf, n, 0, 0);
  }
  fprintf(stderr, "%d shaders written, %d failures\n", written, failures);
  return failures ? 1 : 0;
}
