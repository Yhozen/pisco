// SPDX-License-Identifier: GPL-2.0-or-later
// Direct3D 9 shader bytecode (vs_1_1 to vs_2_x, ps_1_1 to ps_2_x) as WGSL: see d3d9_sm.h for how a translated
// shader meets the pipeline. Two passes: the tokens are decoded into instructions (and what the shader uses is
// counted), then each instruction becomes WGSL statements. Registers are module-scope `var<private>` vec4fs, so the
// subroutines a shader calls (label / call / ret) are WGSL functions that read and write the same registers.
// An instruction computes its whole vec4f result, then writes the components its mask names: `{ let t = ...;
// r0.x = t.x; r0.z = t.z; }`. Behaviour follows the shader model's documentation; where Direct3D defines a
// corner (a zero length in nrm, the absolute value in rsq / log / pow, loop counts capped at 255) it is written so.
#include "d3d9_sm.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- text ----
typedef struct { char *buf; size_t len, cap; int over; } Text;
static void put(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Text *t, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  size_t room = t->len < t->cap ? t->cap - t->len : 0;
  int n = vsnprintf(t->buf ? t->buf + t->len : 0, room, fmt, ap);
  va_end(ap);
  if (n < 0) { t->over = 1; return; }
  if ((size_t)n >= room) t->over = 1;
  t->len += (size_t)n;
}
static size_t refuse(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static size_t refuse(char *out, size_t cap, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  if (cap) vsnprintf(out, cap, fmt, ap);
  va_end(ap);
  return 0;
}

// ---- the bytecode ----
enum {
  OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE, OP_EXP, OP_LOG,
  OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP, OP_LABEL,
  OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF, OP_BREAK, OP_BREAKC,
  OP_MOVA, OP_DEFB, OP_DEFI,
  OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_TEXBEM, OP_TEXBEML, OP_TEXREG2AR, OP_TEXREG2GB, OP_TEXM3x2PAD, OP_TEXM3x2TEX, OP_TEXM3x3PAD,
  OP_TEXM3x3TEX, OP_RESERVED0, OP_TEXM3x3SPEC, OP_TEXM3x3VSPEC, OP_EXPP, OP_LOGP, OP_CND, OP_DEF, OP_TEXREG2RGB, OP_TEXDP3TEX,
  OP_TEXM3x2DEPTH, OP_TEXDP3, OP_TEXM3x3, OP_TEXDEPTH, OP_CMP, OP_BEM, OP_DP2ADD, OP_DSX, OP_DSY, OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP,
  OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF,
};
enum {
  R_TEMP = 0, R_INPUT, R_CONST, R_ADDR_TEX, R_RASTOUT, R_ATTROUT, R_TEXCRDOUT, R_CONSTINT, R_COLOROUT, R_DEPTHOUT, R_SAMPLER,
  R_CONST2, R_CONST3, R_CONST4, R_CONSTBOOL, R_LOOP, R_TEMPFLOAT16, R_MISCTYPE, R_LABEL, R_PREDICATE,
};
enum { MOD_NONE = 0, MOD_NEG, MOD_BIAS, MOD_BIASNEG, MOD_SIGN, MOD_SIGNNEG, MOD_COMP, MOD_X2, MOD_X2NEG, MOD_DZ, MOD_DW, MOD_ABS, MOD_ABSNEG, MOD_NOT };

typedef struct { uint32_t type, num, swz, mod; int rel; uint32_t rel_type, rel_num, rel_comp; } Src;
typedef struct { uint32_t type, num, mask, mod, shift; } Dst;
typedef struct {
  uint32_t op, ctrl; int coissue;
  int has_dst; Dst d;
  int ns; Src s[4];
  int predicated; Src pred;
  uint32_t lit[4];          // def / defi / defb values; dcl: [0] the declaration token
} Ins;

static uint32_t reg_type(uint32_t t) { return ((t >> 28) & 7) | ((t >> 8) & 0x18); }
static Dst dst_of(uint32_t t) { return (Dst){reg_type(t), t & 0x7FF, (t >> 16) & 15, (t >> 20) & 15, (t >> 24) & 15}; }
static Src src_of(uint32_t t) { Src s = {reg_type(t), t & 0x7FF, (t >> 16) & 0xFF, (t >> 24) & 15, (t >> 13) & 1, 0, 0, 0}; return s; }

// parameters of each opcode when the instruction token has no length (shader model 1): a destination, sources
static int params_sm1(uint32_t op, int pixel, int minor, int *has_dst) {
  *has_dst = 1;
  switch (op) {
  case OP_NOP: case OP_RET: case OP_ENDLOOP: case OP_ENDREP: case OP_ELSE: case OP_ENDIF: case OP_BREAK: case OP_PHASE: *has_dst = 0; return 0;
  case OP_CALL: case OP_LABEL: case OP_REP: case OP_IF: case OP_BREAKP: *has_dst = 0; return 1;
  case OP_CALLNZ: case OP_LOOP: case OP_IFC: case OP_BREAKC: *has_dst = 0; return 2;
  case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC: case OP_ABS: case OP_NRM: case OP_MOVA:
  case OP_EXPP: case OP_LOGP: case OP_DSX: case OP_DSY: case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB:
  case OP_TEXM3x2PAD: case OP_TEXM3x2TEX: case OP_TEXM3x3PAD: case OP_TEXM3x3TEX: case OP_TEXM3x3VSPEC: case OP_TEXREG2RGB:
  case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH: case OP_TEXDP3: case OP_TEXM3x3: return 1;
  case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE: case OP_DST:
  case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_POW: case OP_CRS: case OP_BEM: case OP_SETP: case OP_TEXM3x3SPEC: return 2;
  case OP_MAD: case OP_LRP: case OP_SGN: case OP_SINCOS: case OP_CND: case OP_CMP: case OP_DP2ADD: return 3;
  case OP_TEXCOORD: case OP_TEX: return pixel && minor >= 4 ? 1 : 0;   // ps_1_4: texcrd / texld take a source
  case OP_TEXKILL: case OP_TEXDEPTH: return 0;
  default: return -1;
  }
}

typedef struct {
  const SmLinkKey *link; SmInfo *info;
  int pixel, major, minor;
  Ins *ins; size_t n;
  // what the shader uses
  uint32_t temps, out_tex, inputs_read, tex_regs; int writes_pos, writes_fog, writes_d[2], colors;
  int max_const, relative, uses_a0, uses_p0, max_loop_depth, uses_ib;
  uint8_t def_f[8192 / 8]; float defs[8192][4];  // def'd float constants (by number, CONST2..4 folded in)
  uint8_t def_i[16], def_b[16]; int32_t defi[16][4]; uint32_t defb[16];
  // emission
  Text *t; int depth; int loop_depth; char err[200];
} Sm;

static int bit(const uint8_t *set, int i) { return (set[i >> 3] >> (i & 7)) & 1; }
static uint32_t const_num(uint32_t type, uint32_t num) { return num + (type == R_CONST2 ? 2048 : type == R_CONST3 ? 4096 : type == R_CONST4 ? 6144 : 0); }
static int is_const(uint32_t type) { return type == R_CONST || type == R_CONST2 || type == R_CONST3 || type == R_CONST4; }

// ---- pass 1: decode ----
static int decode(Sm *m, const uint32_t *tk, size_t count) {
  size_t cap = 64, i = 1;
  m->ins = malloc(cap * sizeof(Ins)); m->n = 0;
  while (i < count) {
    uint32_t w = tk[i], op = w & 0xFFFF;
    if (w == 0x0000FFFF) return 1;
    if (op == OP_COMMENT) { i += 1 + ((w >> 16) & 0x7FFF); continue; }
    Ins in; memset(&in, 0, sizeof in);
    in.op = op; in.ctrl = (w >> 16) & 0xFF; in.coissue = (w >> 30) & 1; in.predicated = (w >> 28) & 1;
    size_t start = ++i, len;
    if (m->major >= 2) len = (w >> 24) & 15;
    else if (op == OP_DEF) len = 5;
    else if (op == OP_DCL) len = 2;
    else if (op == OP_DEFI) len = 5;
    else if (op == OP_DEFB) len = 2;
    else { int hd, ns = params_sm1(op, m->pixel, m->minor, &hd); if (ns < 0) { snprintf(m->err, sizeof m->err, "opcode %u is not one of shader model 1", op); return 0; } len = (size_t)(hd + ns); }
    if (start + len > count) { snprintf(m->err, sizeof m->err, "an instruction runs past the end of the bytecode"); return 0; }
    size_t p = start, end = start + len;
    if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
      in.has_dst = 1; in.d = dst_of(tk[p++]);
      for (int k = 0; p < end && k < 4; k++) in.lit[k] = tk[p++];
    } else if (op == OP_DCL) {
      in.lit[0] = tk[p++]; in.has_dst = 1; in.d = dst_of(tk[p++]);
    } else {
      int hd = 1, ns;
      if (m->major >= 2) { ns = params_sm1(op, m->pixel, m->minor, &hd); if (op == OP_TEX) ns = 2; if (op == OP_TEXLDD) ns = 4; if (op == OP_TEXLDL) ns = 2; if (op == OP_TEXKILL) { hd = 1; ns = 0; } }
      else ns = params_sm1(op, m->pixel, m->minor, &hd);
      if (ns < 0) { snprintf(m->err, sizeof m->err, "opcode %u is not known", op); return 0; }
      if (hd && p < end) { in.has_dst = 1; in.d = dst_of(tk[p++]); }
      if (in.predicated && p < end) in.pred = src_of(tk[p++]);
      while (p < end && in.ns < 4) {
        Src s = src_of(tk[p++]);
        if (s.rel && m->major >= 2 && p < end) { uint32_t a = tk[p++]; s.rel_type = reg_type(a); s.rel_num = a & 0x7FF; s.rel_comp = (a >> 16) & 3; }
        else if (s.rel) { s.rel_type = R_ADDR_TEX; s.rel_num = 0; s.rel_comp = 0; }   // vs_1_1: a0.x
        in.s[in.ns++] = s;
      }
    }
    i = end;
    if (m->n == cap) { cap *= 2; m->ins = realloc(m->ins, cap * sizeof(Ins)); }
    m->ins[m->n++] = in;
  }
  snprintf(m->err, sizeof m->err, "no end token");
  return 0;
}

// ---- what the shader uses, and the checks that need the whole shader ----
// the location texture coordinates N travel at between the stages (d3d9_sm.h)
static uint32_t tex_loc(uint32_t n) { return n == 0 ? 2 : 3 + n; }
static int is_sampling_ps1(const Sm *m, uint32_t op) { return m->pixel && m->major == 1 && (op == OP_TEX || op == OP_TEXCOORD || op == OP_TEXKILL); }
static int survey(Sm *m) {
  SmInfo *f = m->info;
  int loop_depth = 0;
  for (size_t k = 0; k < m->n; k++) {
    Ins *in = &m->ins[k];
    switch (in->op) {
    case OP_TEXBEM: case OP_TEXBEML: case OP_TEXREG2AR: case OP_TEXREG2GB: case OP_TEXM3x2PAD: case OP_TEXM3x2TEX: case OP_TEXM3x3PAD:
    case OP_TEXM3x3TEX: case OP_TEXM3x3SPEC: case OP_TEXM3x3VSPEC: case OP_TEXREG2RGB: case OP_TEXDP3TEX: case OP_TEXM3x2DEPTH: case OP_TEXDP3:
    case OP_TEXM3x3: case OP_TEXDEPTH: case OP_BEM: case OP_TEXLDL:
      snprintf(m->err, sizeof m->err, "instruction %u (a ps_1_x texture-addressing or shader model 3 operation) is not translated", in->op); return 0;
    case OP_LOOP: case OP_REP: loop_depth++; if (loop_depth > m->max_loop_depth) m->max_loop_depth = loop_depth; if (loop_depth > 4) { snprintf(m->err, sizeof m->err, "loops nested deeper than 4"); return 0; } break;
    case OP_ENDLOOP: case OP_ENDREP: loop_depth--; break;
    case OP_DEF: { uint32_t c = const_num(in->d.type, in->d.num); if (c < 8192) { m->def_f[c >> 3] |= (uint8_t)(1 << (c & 7)); memcpy(m->defs[c], in->lit, 16); } break; }
    case OP_DEFI: if (in->d.num < 16) { m->def_i[in->d.num] = 1; memcpy(m->defi[in->d.num], in->lit, 16); } break;
    case OP_DEFB: if (in->d.num < 16) { m->def_b[in->d.num] = 1; m->defb[in->d.num] = in->lit[0]; } break;
    case OP_DCL: {
      uint32_t dt = in->lit[0];
      if (in->d.type == R_SAMPLER) { if (in->d.num < 16) f->sampler_types[in->d.num] = (uint8_t)((dt >> 27) & 15); }
      else if (!m->pixel && in->d.type == R_INPUT && in->d.num < 16) { f->usage[in->d.num] = (uint8_t)(dt & 31); f->usage_index[in->d.num] = (uint8_t)((dt >> 16) & 15); }
      else if (!m->pixel && in->d.type != R_INPUT) { snprintf(m->err, sizeof m->err, "a vertex shader declares register type %u (shader model 3 outputs)", in->d.type); return 0; }
      break;
    }
    default: break;
    }
    if (in->predicated) m->uses_p0 = 1;
    // destinations
    if (in->has_dst && in->op != OP_DEF && in->op != OP_DEFI && in->op != OP_DEFB && in->op != OP_DCL) {
      Dst *d = &in->d;
      switch (d->type) {
      case R_TEMP: if (d->num >= 32) goto bad_reg; m->temps |= 1u << d->num; break;
      case R_ADDR_TEX: if (m->pixel) { if (d->num >= 8) goto bad_reg; m->tex_regs |= 1u << d->num; } else m->uses_a0 = 1; break;
      case R_RASTOUT: if (d->num == 0) m->writes_pos = 1; else if (d->num == 1) m->writes_fog = 1; else if (d->num == 2) f->writes_psize = 1; else goto bad_reg; break;
      case R_ATTROUT: if (d->num >= 2) goto bad_reg; m->writes_d[d->num] = 1; break;
      case R_TEXCRDOUT: if (m->pixel || d->num >= 8) goto bad_reg; m->out_tex |= 1u << d->num; break;
      case R_COLOROUT: if (!m->pixel || d->num >= 4) goto bad_reg; m->colors |= 1 << d->num; break;
      case R_DEPTHOUT: if (!m->pixel) goto bad_reg; f->writes_depth = 1; break;
      case R_PREDICATE: m->uses_p0 = 1; break;
      default: goto bad_reg;
      }
      if (in->op == OP_TEX && m->pixel && m->major == 1) { // the stage a ps_1_x texture instruction samples
        uint32_t st = in->d.num;
        if (st < 16 && (m->minor < 4 || in->op == OP_TEX)) f->samplers |= (uint16_t)(1u << st);
      }
      if (is_sampling_ps1(m, in->op) && m->minor < 4 && in->d.type == R_ADDR_TEX) m->inputs_read |= 1u << tex_loc(in->d.num); // its coordinates
    }
    // sources
    for (int s = 0; s < in->ns; s++) {
      Src *x = &in->s[s];
      if (x->rel) { m->relative = 1; if (x->rel_type == R_ADDR_TEX) m->uses_a0 = 1; }
      switch (x->type) {
      case R_TEMP: if (x->num >= 32) goto bad_reg; m->temps |= 1u << x->num; break;
      case R_INPUT:
        if (x->num >= 16) goto bad_reg;
        if (m->pixel) { if (x->num >= 2) goto bad_reg; m->inputs_read |= 1u << x->num; }
        else f->inputs |= (uint16_t)(1u << x->num);
        break;
      case R_CONST: case R_CONST2: case R_CONST3: case R_CONST4: { int c = (int)const_num(x->type, x->num); if (c > m->max_const) m->max_const = c; break; }
      case R_ADDR_TEX:
        if (m->pixel) { if (x->num >= 8) goto bad_reg; m->tex_regs |= 1u << x->num; m->inputs_read |= 1u << tex_loc(x->num); }
        else m->uses_a0 = 1;
        break;
      case R_CONSTINT: case R_CONSTBOOL: if (x->num >= 16) goto bad_reg; m->uses_ib = 1; break;
      case R_SAMPLER: if (x->num >= 16) goto bad_reg; f->samplers |= (uint16_t)(1u << x->num); break;
      case R_LOOP: case R_LABEL: case R_PREDICATE: break;
      case R_MISCTYPE: snprintf(m->err, sizeof m->err, "vPos / vFace (shader model 3)"); return 0;
      default: goto bad_src;
      }
    }
    continue;
  bad_reg:
    snprintf(m->err, sizeof m->err, "instruction %u writes or reads a register of type %u that this shader model does not have", in->op, in->d.type); return 0;
  bad_src:
    snprintf(m->err, sizeof m->err, "instruction %u reads a register of type %u that is not translated", in->op, in->s[0].type); return 0;
  }
  if (loop_depth) { snprintf(m->err, sizeof m->err, "unbalanced loops"); return 0; }
  // ps_1_x: t registers are texture stage coordinates (1.1 to 1.3: written by tex / texcoord; 1.4: read by texld / texcrd)
  if (m->pixel && m->major == 1) {
    for (int s = 0; s < 8; s++) if (m->tex_regs & (1u << s)) m->inputs_read |= 1u << tex_loc((uint32_t)s);
    for (size_t k = 0; k < m->n; k++) if (m->ins[k].op == OP_TEX && m->minor >= 4) f->samplers |= (uint16_t)(1u << m->ins[k].d.num); // texld r#: stage #
    for (int s = 0; s < 16; s++) if (f->samplers & (1u << s)) f->sampler_types[s] = m->link && m->link->sampler_types[s] ? m->link->sampler_types[s] : SM_TEX_2D;
    f->colors = 1;
  } else if (m->pixel) f->colors = (uint8_t)m->colors;
  if (m->pixel) for (int s = 0; s < 16; s++) if ((f->samplers & (1u << s)) && f->sampler_types[s] != SM_TEX_2D && f->sampler_types[s] != SM_TEX_CUBE && f->sampler_types[s] != SM_TEX_3D) {
    snprintf(m->err, sizeof m->err, "sampler s%d has texture type %u", s, f->sampler_types[s]); return 0;
  }
  if (!m->pixel && (f->samplers)) { snprintf(m->err, sizeof m->err, "a vertex shader samples textures (shader model 3)"); return 0; }
  return 1;
}

// ---- pass 2: WGSL ----
static void flt(char *out, size_t cap, float v) {
  uint32_t b; memcpy(&b, &v, 4);
  if (!isfinite(v)) { snprintf(out, cap, "bitcast<f32>(0x%08xu)", b); return; }
  snprintf(out, cap, "%.9g", (double)v);
  if (!strpbrk(out, ".en")) strncat(out, ".0", cap - strlen(out) - 1);
}
static const char comp_ch[4] = {'x', 'y', 'z', 'w'};
static void indent(Sm *m) { for (int i = 0; i < m->depth; i++) put(m->t, "  "); }

// the register's value as a vec4f, before the swizzle and the modifier
static void reg_value(Sm *m, const Src *x, char *o, size_t cap) {
  switch (x->type) {
  case R_TEMP: snprintf(o, cap, "r%u", x->num); return;
  case R_INPUT: snprintf(o, cap, "v%u", x->num); return;
  case R_ADDR_TEX: if (m->pixel) snprintf(o, cap, "t%u", x->num); else snprintf(o, cap, "vec4f(a0)"); return;
  case R_CONSTINT:
    if (m->def_i[x->num]) snprintf(o, cap, "vec4f(vec4i(%d, %d, %d, %d))", m->defi[x->num][0], m->defi[x->num][1], m->defi[x->num][2], m->defi[x->num][3]);
    else snprintf(o, cap, "vec4f(ib.i[%u])", x->num);
    return;
  case R_LOOP: snprintf(o, cap, "vec4f(f32(aL%d))", m->loop_depth > 0 ? m->loop_depth : 1); return;
  case R_PREDICATE: snprintf(o, cap, "select(vec4f(0.0), vec4f(1.0), p0)"); return;
  case R_CONSTBOOL:
    if (m->def_b[x->num]) snprintf(o, cap, "vec4f(%s)", m->defb[x->num] ? "1.0" : "0.0");
    else snprintf(o, cap, "vec4f(f32(ib.b[%u].x))", x->num);
    return;
  default: break;
  }
  if (is_const(x->type)) {
    uint32_t c = const_num(x->type, x->num);
    const char *clampl = m->pixel && m->major == 1 ? "clamp(" : "", *clampr = m->pixel && m->major == 1 ? ", vec4f(-1.0), vec4f(1.0))" : "";
    if (x->rel) {
      char idx[64];
      if (x->rel_type == R_LOOP) snprintf(idx, sizeof idx, "aL%d + %u", m->loop_depth > 0 ? m->loop_depth : 1, c);
      else snprintf(idx, sizeof idx, "a0.%c + %u", comp_ch[x->rel_comp & 3], c);
      snprintf(o, cap, "%scfr(%s)%s", clampl, idx, clampr); return;
    }
    if (c < 8192 && bit(m->def_f, (int)c)) {
      char a[4][48]; for (int k = 0; k < 4; k++) { float v; memcpy(&v, &m->defs[c][k], 4); flt(a[k], sizeof a[k], v); }
      snprintf(o, cap, "%svec4f(%s, %s, %s, %s)%s", clampl, a[0], a[1], a[2], a[3], clampr); return;
    }
    snprintf(o, cap, "%scf[%u]%s", clampl, c, clampr); return;
  }
  snprintf(o, cap, "vec4f(0.0)");
}
static void swizzle(uint32_t swz, char *o) { // "" for .xyzw
  if (swz == 0xE4) { o[0] = 0; return; }
  o[0] = '.'; for (int k = 0; k < 4; k++) o[1 + k] = comp_ch[(swz >> (2 * k)) & 3]; o[5] = 0;
}
// a source operand: the register, swizzled, with its modifier
static void src_expr(Sm *m, const Src *x, char *o, size_t cap) {
  char v[600], s[8], base[620];
  reg_value(m, x, v, sizeof v); swizzle(x->swz, s);
  snprintf(base, sizeof base, "%s%s", v, s);
  switch (x->mod) {
  case MOD_NEG: snprintf(o, cap, "(-%s)", base); return;
  case MOD_BIAS: snprintf(o, cap, "(%s - vec4f(0.5))", base); return;
  case MOD_BIASNEG: snprintf(o, cap, "(vec4f(0.5) - %s)", base); return;
  case MOD_SIGN: snprintf(o, cap, "(2.0 * %s - vec4f(1.0))", base); return;
  case MOD_SIGNNEG: snprintf(o, cap, "(vec4f(1.0) - 2.0 * %s)", base); return;
  case MOD_COMP: snprintf(o, cap, "(vec4f(1.0) - %s)", base); return;
  case MOD_X2: snprintf(o, cap, "(2.0 * %s)", base); return;
  case MOD_X2NEG: snprintf(o, cap, "(-2.0 * %s)", base); return;
  case MOD_DZ: snprintf(o, cap, "(%s / %s.z)", base, base); return;
  case MOD_DW: snprintf(o, cap, "(%s / %s.w)", base, base); return;
  case MOD_ABS: snprintf(o, cap, "abs(%s)", base); return;
  case MOD_ABSNEG: snprintf(o, cap, "(-abs(%s))", base); return;
  default: snprintf(o, cap, "%s", base); return;
  }
}
// the name a destination register is written through
static int dst_name(Sm *m, const Dst *d, char *o, size_t cap) {
  switch (d->type) {
  case R_TEMP: snprintf(o, cap, "r%u", d->num); return 1;
  case R_ADDR_TEX: if (m->pixel) snprintf(o, cap, "t%u", d->num); else snprintf(o, cap, "a0"); return 1;
  case R_RASTOUT: snprintf(o, cap, "%s", d->num == 0 ? "oPos" : d->num == 1 ? "oFog" : "oPts"); return 1;
  case R_ATTROUT: snprintf(o, cap, "oD%u", d->num); return 1;
  case R_TEXCRDOUT: snprintf(o, cap, "oT%u", d->num); return 1;
  case R_COLOROUT: snprintf(o, cap, "oC%u", d->num); return 1;
  case R_DEPTHOUT: snprintf(o, cap, "oDepth"); return 1;
  case R_PREDICATE: snprintf(o, cap, "p0"); return 1;
  default: return 0;
  }
}
// writes the vec4f `t` through the destination's mask (with its modifiers, the shift of ps_1_x, a predicate)
static void write_dst(Sm *m, const Ins *in, const char *value) {
  const Dst *d = &in->d; char name[32];
  if (!dst_name(m, d, name, sizeof name)) return;
  indent(m); put(m->t, "{\n"); m->depth++;
  // the value, then the shift of ps_1_x (a signed power of two), then saturation
  const char *scale = 0;
  if (d->shift) { int sh = (int)d->shift; if (sh & 8) sh -= 16; scale = sh == 1 ? "2.0" : sh == 2 ? "4.0" : sh == 3 ? "8.0" : sh == -1 ? "0.5" : sh == -2 ? "0.25" : "0.125"; }
  indent(m);
  if (scale) put(m->t, "let t = %s(%s * %s)%s;\n", d->mod & 1 ? "clamp(" : "", value, scale, d->mod & 1 ? ", vec4f(0.0), vec4f(1.0))" : "");
  else put(m->t, "let t = %s%s%s;\n", d->mod & 1 ? "clamp(" : "", value, d->mod & 1 ? ", vec4f(0.0), vec4f(1.0))" : "");
  uint32_t mask = d->mask ? d->mask : 15;
  int is_a0 = !m->pixel && d->type == R_ADDR_TEX, is_p = d->type == R_PREDICATE;
  for (int k = 0; k < 4; k++) {
    if (!(mask & (1u << k))) continue;
    char rhs[64];
    if (is_a0) snprintf(rhs, sizeof rhs, in->op == OP_MOVA ? "i32(floor(t.%c + 0.5))" : "i32(floor(t.%c))", comp_ch[k]);
    else if (is_p) snprintf(rhs, sizeof rhs, "t.%c != 0.0", comp_ch[k]);
    else snprintf(rhs, sizeof rhs, "t.%c", comp_ch[k]);
    indent(m);
    if (in->predicated) {
      int pc = (int)((in->pred.swz >> (2 * k)) & 3);
      put(m->t, "if (%sp0.%c) { %s.%c = %s; }\n", in->pred.mod == MOD_NOT ? "!" : "", comp_ch[pc], name, comp_ch[k], rhs);
    } else put(m->t, "%s.%c = %s;\n", name, comp_ch[k], rhs);
  }
  m->depth--; indent(m); put(m->t, "}\n");
}
static const char *cmp_op(uint32_t c) { static const char *const o[8] = {"", ">", "==", ">=", "<", "!=", "<=", ""}; return o[c & 7]; }
static int scalar_cond(Sm *m, const Ins *in, char *o, size_t cap) { // ifc / breakc / callnz with b# or p0
  char a[1400], b[1400];
  if (in->op == OP_IFC || in->op == OP_BREAKC) {
    if (!*cmp_op(in->ctrl)) return 0;
    src_expr(m, &in->s[0], a, sizeof a); src_expr(m, &in->s[1], b, sizeof b);
    snprintf(o, cap, "(%s).x %s (%s).x", a, cmp_op(in->ctrl), b); return 1;
  }
  const Src *x = &in->s[in->op == OP_CALLNZ ? 1 : 0];
  if (x->type == R_CONSTBOOL) {
    if (m->def_b[x->num]) snprintf(o, cap, "%s", (m->defb[x->num] != 0) != (x->mod == MOD_NOT) ? "true" : "false");
    else snprintf(o, cap, "ib.b[%u].x %s 0u", x->num, x->mod == MOD_NOT ? "==" : "!=");
    return 1;
  }
  if (x->type == R_PREDICATE) { snprintf(o, cap, "%sp0.%c", x->mod == MOD_NOT ? "!" : "", comp_ch[x->swz & 3]); return 1; }
  return 0;
}
static void int_const(Sm *m, uint32_t n, char *o, size_t cap) {
  if (n < 16 && m->def_i[n]) snprintf(o, cap, "vec4i(%d, %d, %d, %d)", m->defi[n][0], m->defi[n][1], m->defi[n][2], m->defi[n][3]);
  else snprintf(o, cap, "ib.i[%u]", n);
}
static const char *tex_type(uint8_t t) { return t == SM_TEX_CUBE ? "texture_cube<f32>" : t == SM_TEX_3D ? "texture_3d<f32>" : "texture_2d<f32>"; }
static const char *coord_swz(uint8_t t) { return t == SM_TEX_2D ? "xy" : "xyz"; }

static int emit_ins(Sm *m, const Ins *in, int *in_main, int *in_fn) {
  char a[1400], b[1400], c[1400], e[8192];
  const SmInfo *f = m->info;
  if (in->ns > 0 && in->s[0].type != R_LABEL && in->s[0].type != R_SAMPLER) src_expr(m, &in->s[0], a, sizeof a); else a[0] = 0;
  if (in->ns > 1 && in->s[1].type != R_SAMPLER && in->s[1].type != R_LABEL) src_expr(m, &in->s[1], b, sizeof b); else b[0] = 0;
  if (in->ns > 2) src_expr(m, &in->s[2], c, sizeof c); else c[0] = 0;
  switch (in->op) {
  case OP_NOP: case OP_DCL: case OP_DEF: case OP_DEFI: case OP_DEFB: case OP_PHASE: return 1;
  case OP_MOV: case OP_MOVA: write_dst(m, in, a); return 1;
  case OP_ADD: snprintf(e, sizeof e, "(%s + %s)", a, b); break;
  case OP_SUB: snprintf(e, sizeof e, "(%s - %s)", a, b); break;
  case OP_MUL: snprintf(e, sizeof e, "(%s * %s)", a, b); break;
  case OP_MAD: snprintf(e, sizeof e, "(%s * %s + %s)", a, b, c); break;
  case OP_LRP: snprintf(e, sizeof e, "mix(%s, %s, %s)", c, b, a); break;
  case OP_MIN: snprintf(e, sizeof e, "min(%s, %s)", a, b); break;
  case OP_MAX: snprintf(e, sizeof e, "max(%s, %s)", a, b); break;
  case OP_SLT: snprintf(e, sizeof e, "select(vec4f(0.0), vec4f(1.0), %s < %s)", a, b); break;
  case OP_SGE: snprintf(e, sizeof e, "select(vec4f(0.0), vec4f(1.0), %s >= %s)", a, b); break;
  case OP_DP3: snprintf(e, sizeof e, "vec4f(dot((%s).xyz, (%s).xyz))", a, b); break;
  case OP_DP4: snprintf(e, sizeof e, "vec4f(dot(%s, %s))", a, b); break;
  case OP_DP2ADD: snprintf(e, sizeof e, "vec4f(dot((%s).xy, (%s).xy) + (%s).x)", a, b, c); break;
  case OP_RCP: snprintf(e, sizeof e, "vec4f(1.0 / (%s).w)", a); break;
  case OP_RSQ: snprintf(e, sizeof e, "vec4f(inverseSqrt(abs((%s).w)))", a); break;
  case OP_EXP: snprintf(e, sizeof e, "vec4f(exp2((%s).w))", a); break;
  case OP_LOG: snprintf(e, sizeof e, "vec4f(log2(abs((%s).w)))", a); break;
  case OP_EXPP:
    if (m->major >= 2) snprintf(e, sizeof e, "vec4f(exp2((%s).w))", a);
    else snprintf(e, sizeof e, "smexpp((%s).w)", a);
    break;
  case OP_LOGP:
    if (m->major >= 2) snprintf(e, sizeof e, "vec4f(log2(abs((%s).w)))", a);
    else snprintf(e, sizeof e, "smlogp((%s).w)", a);
    break;
  case OP_POW: snprintf(e, sizeof e, "vec4f(pow(abs((%s).w), (%s).w))", a, b); break;
  case OP_FRC: snprintf(e, sizeof e, "fract(%s)", a); break;
  case OP_ABS: snprintf(e, sizeof e, "abs(%s)", a); break;
  case OP_SGN: snprintf(e, sizeof e, "sign(%s)", a); break;
  case OP_NRM: snprintf(e, sizeof e, "smnrm(%s)", a); break;
  case OP_CRS: snprintf(e, sizeof e, "vec4f(cross((%s).xyz, (%s).xyz), 0.0)", a, b); break;
  case OP_LIT: snprintf(e, sizeof e, "smlit(%s)", a); break;
  case OP_DST: snprintf(e, sizeof e, "vec4f(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b); break;
  case OP_SINCOS: snprintf(e, sizeof e, "vec4f(cos((%s).w), sin((%s).w), 0.0, 0.0)", a, a); break;
  case OP_CMP: snprintf(e, sizeof e, "select(%s, %s, %s >= vec4f(0.0))", c, b, a); break;
  case OP_CND: snprintf(e, sizeof e, "select(%s, %s, %s > vec4f(0.5))", c, b, a); break;
  case OP_DSX: snprintf(e, sizeof e, "dpdx(%s)", a); break;
  case OP_DSY: snprintf(e, sizeof e, "dpdy(%s)", a); break;
  case OP_SETP: { if (!*cmp_op(in->ctrl)) return 0; snprintf(e, sizeof e, "select(vec4f(0.0), vec4f(1.0), %s %s %s)", a, cmp_op(in->ctrl), b); break; }
  case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
    int rows = in->op == OP_M4x4 || in->op == OP_M3x4 ? 4 : in->op == OP_M4x3 || in->op == OP_M3x3 ? 3 : 2, four = in->op == OP_M4x4 || in->op == OP_M4x3;
    size_t o = 0; o += (size_t)snprintf(e + o, sizeof e - o, "vec4f(");
    for (int r = 0; r < 4; r++) {
      if (r < rows) { Src row = in->s[1]; row.num += (uint32_t)r; char rv[1400]; src_expr(m, &row, rv, sizeof rv);
        o += (size_t)snprintf(e + o, sizeof e - o, four ? "dot(%s, %s)" : "dot((%s).xyz, (%s).xyz)", a, rv); }
      else o += (size_t)snprintf(e + o, sizeof e - o, "0.0");
      if (r < 3) o += (size_t)snprintf(e + o, sizeof e - o, ", ");
    }
    snprintf(e + o, sizeof e - o, ")");
    break;
  }
  case OP_TEXCOORD:
    if (m->major == 1 && m->minor < 4) snprintf(e, sizeof e, "vec4f(clamp(tc%u.xyz, vec3f(0.0), vec3f(1.0)), 1.0)", in->d.num);
    else snprintf(e, sizeof e, "vec4f((%s).xyz, 1.0)", a);   // ps_1_4 texcrd
    break;
  case OP_TEX: {
    uint32_t stage; char coords[1400];
    if (m->major == 1 && m->minor < 4) { stage = in->d.num; snprintf(coords, sizeof coords, "tc%u", stage); }
    else if (m->major == 1) { stage = in->d.num; snprintf(coords, sizeof coords, "%s", a); }   // ps_1_4 texld r#, src: stage #
    else { stage = in->s[1].num; snprintf(coords, sizeof coords, "%s", a); }
    if (stage >= 16) return 0;
    uint8_t ty = f->sampler_types[stage]; const char *cs = coord_swz(ty);
    uint32_t mode = m->major >= 2 ? in->ctrl & 3 : 0;
    char sw[8] = ""; if (m->major >= 2) swizzle(in->s[1].swz, sw);
    if (mode == 1) snprintf(e, sizeof e, "textureSample(tex%u, smp%u, ((%s) / (%s).w).%s)%s", stage, stage, coords, coords, cs, sw);
    else if (mode == 2) snprintf(e, sizeof e, "textureSampleBias(tex%u, smp%u, (%s).%s, (%s).w)%s", stage, stage, coords, cs, coords, sw);
    else snprintf(e, sizeof e, "textureSample(tex%u, smp%u, (%s).%s)%s", stage, stage, coords, cs, sw);
    break;
  }
  case OP_TEXLDD: {
    uint32_t stage = in->s[1].num; if (stage >= 16) return 0;
    char gx[1400], gy[1400]; src_expr(m, &in->s[2], gx, sizeof gx); src_expr(m, &in->s[3], gy, sizeof gy);
    uint8_t ty = f->sampler_types[stage]; const char *cs = coord_swz(ty);
    snprintf(e, sizeof e, "textureSampleGrad(tex%u, smp%u, (%s).%s, (%s).%s, (%s).%s)", stage, stage, a, cs, gx, cs, gy, cs);
    break;
  }
  case OP_TEXKILL: {
    char r[40];
    if (m->major == 1 && m->minor < 4 && in->d.type == R_ADDR_TEX) snprintf(r, sizeof r, "tc%u", in->d.num);
    else if (!dst_name(m, &in->d, r, sizeof r)) return 0;
    uint32_t mask = m->major == 1 ? 7 : (in->d.mask ? in->d.mask : 15);
    indent(m); put(m->t, "if (");
    int first = 1; for (int k = 0; k < 4; k++) if (mask & (1u << k)) { put(m->t, "%s%s.%c < 0.0", first ? "" : " || ", r, comp_ch[k]); first = 0; }
    put(m->t, ") { discard; }\n");
    return 1;
  }
  // flow control
  case OP_IF: case OP_IFC: { char cond[3000]; if (!scalar_cond(m, in, cond, sizeof cond)) return 0; indent(m); put(m->t, "if (%s) {\n", cond); m->depth++; return 1; }
  case OP_ELSE: m->depth--; indent(m); put(m->t, "} else {\n"); m->depth++; return 1;
  case OP_ENDIF: m->depth--; indent(m); put(m->t, "}\n"); return 1;
  case OP_BREAK: indent(m); put(m->t, "break;\n"); return 1;
  case OP_BREAKC: case OP_BREAKP: {
    char cond[3000];
    if (in->op == OP_BREAKP) { const Src *x = &in->s[0]; snprintf(cond, sizeof cond, "%sp0.%c", x->mod == MOD_NOT ? "!" : "", comp_ch[x->swz & 3]); }
    else if (!scalar_cond(m, in, cond, sizeof cond)) return 0;
    indent(m); put(m->t, "if (%s) { break; }\n", cond); return 1;
  }
  case OP_REP: case OP_LOOP: {
    char ic[64]; int_const(m, in->s[in->op == OP_LOOP ? 1 : 0].num, ic, sizeof ic);
    int dpt = ++m->loop_depth;
    indent(m); put(m->t, "{\n"); m->depth++;
    indent(m); put(m->t, "let li%d = %s;\n", dpt, ic);
    if (in->op == OP_LOOP) { indent(m); put(m->t, "aL%d = li%d.y;\n", dpt, dpt); }
    indent(m); put(m->t, "for (var k%d: i32 = 0; k%d < min(li%d.x, 255); k%d++) {\n", dpt, dpt, dpt, dpt); m->depth++;
    return 1;
  }
  case OP_ENDREP: case OP_ENDLOOP: {
    int dpt = m->loop_depth--;
    if (in->op == OP_ENDLOOP) { indent(m); put(m->t, "aL%d += li%d.z;\n", dpt, dpt); }
    m->depth--; indent(m); put(m->t, "}\n"); m->depth--; indent(m); put(m->t, "}\n");
    return 1;
  }
  case OP_CALL: indent(m); put(m->t, "l%u();\n", in->s[0].num); return 1;
  case OP_CALLNZ: { char cond[3000]; if (!scalar_cond(m, in, cond, sizeof cond)) return 0; indent(m); put(m->t, "if (%s) { l%u(); }\n", cond, in->s[0].num); return 1; }
  case OP_RET:
    if (m->depth > 1) { indent(m); put(m->t, "return;\n"); return 1; }   // inside a block: an early return
    put(m->t, "}\n"); m->depth = 0; *in_main = 0; *in_fn = 0; return 1;
  case OP_LABEL:
    if (*in_main || *in_fn) put(m->t, "}\n");
    *in_main = 0; *in_fn = 1;
    put(m->t, "fn l%u() {\n", in->s[0].num); m->depth = 1; m->loop_depth = 0;
    return 1;
  default:
    snprintf(m->err, sizeof m->err, "instruction %u is not translated", in->op); return 0;
  }
  write_dst(m, in, e);
  return 1;
}

static void emit_decls(Sm *m) {
  Text *t = m->t; SmInfo *f = m->info; int g = m->pixel;
  put(t, "diagnostic(off, derivative_uniformity);\n");
  put(t, "struct SmFixed { half_pixel: vec4f, fog_color: vec4f, misc: vec4f, point: vec4f }\n");
  put(t, "@group(%d) @binding(0) var<uniform> fx: SmFixed;\n", g);
  put(t, "@group(%d) @binding(1) var<uniform> cf: array<vec4f, %u>;\n", g, f->float_count);
  if (f->int_bool) put(t, "struct SmIntBool { i: array<vec4i, 16>, b: array<vec4u, 16> }\n@group(%d) @binding(2) var<uniform> ib: SmIntBool;\n", g);
  for (int s = 0; s < 16; s++) if (f->samplers & (1u << s))
    put(t, "@group(1) @binding(%d) var tex%d: %s;\n@group(1) @binding(%d) var smp%d: sampler;\n", 3 + 2 * s, s, tex_type(f->sampler_types[s]), 4 + 2 * s, s);
  for (int r = 0; r < 32; r++) if (m->temps & (1u << r)) put(t, "var<private> r%d: vec4f;\n", r);
  if (m->uses_a0) put(t, "var<private> a0: vec4i;\n");
  if (m->uses_p0) put(t, "var<private> p0: vec4<bool>;\n");
  for (int d = 1; d <= m->max_loop_depth; d++) put(t, "var<private> aL%d: i32;\n", d);
  if (m->relative) {
    // a relative read: a def'd constant wins over the block, as in Direct3D; out of range reads 0
    put(t, "fn cfr(i: i32) -> vec4f {\n  switch (i) {\n");
    for (int c = 0; c < 8192; c++) if (bit(m->def_f, c)) {
      char a[4][48]; for (int k = 0; k < 4; k++) { float v; memcpy(&v, &m->defs[c][k], 4); flt(a[k], sizeof a[k], v); }
      put(t, "    case %d: { return vec4f(%s, %s, %s, %s); }\n", c, a[0], a[1], a[2], a[3]);
    }
    put(t, "    default: { if (i < 0 || i >= %u) { return vec4f(0.0); } return cf[i]; }\n  }\n}\n", f->float_count);
  }
  put(t, "fn smlit(s: vec4f) -> vec4f {\n  let d = max(s.x, 0.0);\n  let p = clamp(s.w, -128.0, 128.0);\n"
         "  return vec4f(1.0, d, select(0.0, pow(max(s.y, 0.0), p), s.x > 0.0), 1.0);\n}\n");
  put(t, "fn smnrm(s: vec4f) -> vec4f {\n  let d = dot(s.xyz, s.xyz);\n  return select(s * inverseSqrt(d), vec4f(0.0), d == 0.0);\n}\n");
  put(t, "fn smexpp(w: f32) -> vec4f {\n  let i = floor(w);\n  return vec4f(exp2(i), w - i, exp2(w), 1.0);\n}\n");
  put(t, "fn smlogp(w: f32) -> vec4f {\n  let a = abs(w);\n  if (a == 0.0) { return vec4f(-bitcast<f32>(0x7f800000u), 1.0, -bitcast<f32>(0x7f800000u), 1.0); }\n"
         "  let e = floor(log2(a));\n  return vec4f(e, a / exp2(e), log2(a), 1.0);\n}\n");
  if (!m->pixel) {
    for (int v = 0; v < 16; v++) if (f->inputs & (1u << v)) put(t, "var<private> v%d: vec4f;\n", v);
    put(t, "var<private> oPos: vec4f;\nvar<private> oFog: vec4f;\nvar<private> oPts: vec4f;\nvar<private> oD0: vec4f;\nvar<private> oD1: vec4f;\n");
    for (int k = 0; k < 8; k++) put(t, "var<private> oT%d: vec4f;\n", k);
  } else {
    for (int v = 0; v < 2; v++) if (m->inputs_read & (1u << v)) put(t, "var<private> v%d: vec4f;\n", v);
    for (int k = 0; k < 8; k++) if (m->tex_regs & (1u << k)) put(t, "var<private> t%d: vec4f;\n", k);
    if (m->major == 1 && m->minor < 4) for (int k = 0; k < 8; k++) if (m->tex_regs & (1u << k)) put(t, "var<private> tc%d: vec4f;\n", k);
    if (m->major >= 2 || m->minor >= 4) for (int k = 0; k < 4; k++) if (m->colors & (1 << k)) put(t, "var<private> oC%d: vec4f;\n", k);
    if (f->writes_depth) put(t, "var<private> oDepth: vec4f;\n");
  }
}

size_t sm_length(const uint32_t *tk, size_t max) {
  for (size_t i = 1; i < max;) {
    if (tk[i] == 0x0000FFFF) return i + 1;
    if ((tk[i] & 0xFFFF) == OP_COMMENT) { i += 1 + ((tk[i] >> 16) & 0x7FFF); continue; }
    i++;
  }
  return 0;
}
void sm_version_name(uint32_t v, char *out, size_t cap) {
  unsigned major = (v >> 8) & 0xFF, minor = v & 0xFF;
  const char *kind = (v >> 16) == 0xFFFE ? "vs" : (v >> 16) == 0xFFFF ? "ps" : "?s";
  if (minor == 1 && major == 2) snprintf(out, cap, "%s_2_x", kind);   // 2.1 is how 2_x / 2_a / 2_b are numbered
  else snprintf(out, cap, "%s_%u_%u", kind, major, minor);
}

size_t sm_wgsl(const uint32_t *tk, size_t count, const SmLinkKey *link, SmInfo *info, char *out, size_t cap) {
  static const SmLinkKey none;
  if (!link) link = &none;
  memset(info, 0, sizeof *info);
  if (count < 2) return refuse(out, cap, "no bytecode");
  uint32_t v = tk[0]; int pixel = (v >> 16) == 0xFFFF;
  if ((v >> 16) != 0xFFFE && !pixel) return refuse(out, cap, "0x%08x is no shader version token", v);
  int major = (int)((v >> 8) & 0xFF), minor = (int)(v & 0xFF);
  if (major < 1 || major > 2 || (major == 1 && minor > (pixel ? 4 : 1))) return refuse(out, cap, "shader model %d.%d (%s) is not translated", major, minor, pixel ? "pixel" : "vertex");
  if (link->point && !pixel) return refuse(out, cap, "a point list drawn with a vertex shader");
  Sm *m = calloc(1, sizeof *m);
  m->link = link; m->info = info; m->pixel = pixel; m->major = major; m->minor = minor; m->max_const = -1;
  info->pixel = (uint8_t)pixel; info->major = (uint8_t)major; info->minor = (uint8_t)minor;
  for (int i = 0; i < 16; i++) info->usage[i] = 0xFF;
  size_t result = 0;
  if (!decode(m, tk, count) || !survey(m)) { result = refuse(out, cap, "%s", m->err); goto done; }
  if (link->fog && pixel) m->inputs_read |= 1u << 3;
  info->float_count = (uint16_t)(m->relative ? (pixel ? 224 : 256) : (m->max_const + 1 > 0 ? m->max_const + 1 : 1));
  if (m->max_const + 1 > info->float_count) info->float_count = (uint16_t)(m->max_const + 1);
  info->relative = (uint8_t)m->relative; info->int_bool = (uint8_t)m->uses_ib;
  if (!pixel) for (int r = 0; r < 16; r++) if ((info->inputs & (1u << r)) && info->usage[r] == 0xFF) { info->usage[r] = r == 0 ? SM_USAGE_POSITION : SM_USAGE_TEXCOORD; info->usage_index[r] = 0; }
  Text t = {out, 0, cap, 0}; m->t = &t;
  emit_decls(m);
  // the body: main, then the subroutines
  put(&t, "fn shader_main() {\n"); m->depth = 1;
  int in_main = 1, in_fn = 0;
  for (size_t k = 0; k < m->n; k++) {
    if (!emit_ins(m, &m->ins[k], &in_main, &in_fn)) { result = refuse(out, cap, "%s", m->err[0] ? m->err : "an instruction could not be written"); goto done; }
  }
  if (in_main || in_fn) put(&t, "}\n");
  if (!pixel) {
    put(&t, "struct VIn {\n");
    int any = 0; for (int r = 0; r < 16; r++) if (info->inputs & (1u << r)) { put(&t, "  @location(%d) v%d: vec4f,\n", r, r); any = 1; }
    if (!any) put(&t, "  @builtin(vertex_index) index: u32,\n");
    put(&t, "}\nstruct VOut {\n  @builtin(position) position: vec4f,\n  @location(0) diffuse: vec4f,\n  @location(1) specular: vec4f,\n"
            "  @location(2) tex: vec4f,\n  @location(3) fog: f32,\n");
    for (int k = 1; k < 8; k++) put(&t, "  @location(%d) tex%d: vec4f,\n", (int)tex_loc((uint32_t)k), k);
    put(&t, "}\n@vertex fn main(vin: VIn) -> VOut {\n");
    for (int r = 0; r < 16; r++) if (info->inputs & (1u << r)) put(&t, "  v%d = vin.v%d;\n", r, r);
    put(&t, "  shader_main();\n  var o: VOut;\n  o.position = vec4f(oPos.xy + fx.half_pixel.xy * oPos.w, oPos.zw);\n"
            "  o.diffuse = clamp(oD0, vec4f(0.0), vec4f(1.0));\n  o.specular = clamp(oD1, vec4f(0.0), vec4f(1.0));\n  o.tex = oT0;\n"
            "  o.fog = clamp(oFog.x, 0.0, 1.0);\n");
    for (int k = 1; k < 8; k++) put(&t, "  o.tex%d = oT%d;\n", k, k);
    put(&t, "  return o;\n}\n");
    info->varyings = 0x7FF;
  } else {
    put(&t, "struct FIn {\n");
    int any = 0;
    if (m->inputs_read & 1) { put(&t, "  @location(0) diffuse: vec4f,\n"); any = 1; }
    if (m->inputs_read & 2) { put(&t, "  @location(1) specular: vec4f,\n"); any = 1; }
    if (m->inputs_read & 8) { put(&t, "  @location(3) fog: f32,\n"); any = 1; }
    for (int k = 0; k < 8; k++) if (m->inputs_read & (1u << (int)tex_loc((uint32_t)k))) { put(&t, "  @location(%d) tex%d: vec4f,\n", (int)tex_loc((uint32_t)k), k); any = 1; }
    if (!any) put(&t, "  @builtin(position) frag: vec4f,\n");
    put(&t, "}\nstruct FOut {\n");
    int ps1 = major == 1 && minor < 4, colors = info->colors ? info->colors : 1;
    for (int k = 0; k < 4; k++) if (colors & (1 << k)) put(&t, "  @location(%d) c%d: vec4f,\n", k, k);
    if (info->writes_depth) put(&t, "  @builtin(frag_depth) depth: f32,\n");
    put(&t, "}\n@fragment fn main(fin: FIn) -> FOut {\n");
    if (m->inputs_read & 1) put(&t, "  v0 = fin.diffuse;\n");
    if (m->inputs_read & 2) put(&t, "  v1 = fin.specular;\n");
    for (int k = 0; k < 8; k++) if (m->tex_regs & (1u << k)) {
      if (ps1) put(&t, "  tc%d = fin.tex%d;\n", k, k);
      else put(&t, "  t%d = fin.tex%d;\n", k, k);
    }
    put(&t, "  shader_main();\n  var o: FOut;\n");
    const char *c0 = major == 1 ? "r0" : "oC0";
    if ((major == 1 && !(m->temps & 1)) || (major >= 2 && !(m->colors & 1))) c0 = "vec4f(0.0)";
    put(&t, "  var color = %s;\n", c0);
    if (link->alpha_test) {
      static const char *const cmpf[9] = {"", "false", "a8 < fx.misc.x", "a8 == fx.misc.x", "a8 <= fx.misc.x", "a8 > fx.misc.x", "a8 != fx.misc.x", "a8 >= fx.misc.x", "true"};
      if (link->alpha_test > 8) { result = refuse(out, cap, "alpha test %u", link->alpha_test); goto done; }
      put(&t, "  let a8 = floor(clamp(color.a, 0.0, 1.0) * 255.0 + 0.5);\n  if (!(%s)) { discard; }\n", cmpf[link->alpha_test]);
    }
    if (link->fog) put(&t, "  color = vec4f(mix(fx.fog_color.rgb, color.rgb, clamp(fin.fog, 0.0, 1.0)), color.a);\n");
    put(&t, "  o.c0 = color;\n");
    for (int k = 1; k < 4; k++) if (colors & (1 << k)) put(&t, "  o.c%d = oC%d;\n", k, k);
    if (info->writes_depth) put(&t, "  o.depth = oDepth.x;\n");
    put(&t, "  return o;\n}\n");
    info->varyings = (uint16_t)m->inputs_read;
  }
  if (t.over) { result = refuse(out, cap, "the WGSL is longer than %zu bytes", cap); goto done; }
  result = t.len;
done:
  free(m->ins); free(m);
  return result;
}
