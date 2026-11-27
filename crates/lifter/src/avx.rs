// SPDX-License-Identifier: GPL-2.0-or-later
//! VEX-encoded instructions: AVX and AVX2 on xmm and ymm registers, FMA3, and the BMI instructions a compiler for a
//! Zen 2 CPU (the PS5's) picks. A ymm register is X[n] (its low half) and c->yh[n] (its high half, rt/avx.h); a VEX
//! instruction that writes an xmm register clears the high half, as the CPU does. None: not handled (the caller
//! counts it and makes it a trap).
use super::*;

fn ymm(i: &Instruction, n: u32) -> bool {
    i.op_kind(n) == OpKind::Register && i.op_register(n).is_ymm()
}
/// the low 128 bits of operand n: a register, or memory
fn lo(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => format!("X[{}]", i.op_register(n).number()),
        _ => format!("MV({})", ea(i)),
    }
}
/// the high 128 bits of a 256-bit operand
fn hi(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => format!("c->yh[{}]", i.op_register(n).number()),
        _ => format!("MV(({})+16)", ea(i)),
    }
}
/// a 32-bit float source: an xmm register's low element, or memory
fn f32src(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => format!("X[{}].ps[0]", i.op_register(n).number()),
        _ => format!("MF32({})", ea(i)),
    }
}
fn imm(i: &Instruction) -> u64 {
    (0..i.op_count()).find(|&n| matches!(i.op_kind(n), OpKind::Immediate8 | OpKind::Immediate8to32 | OpKind::Immediate8to64)).map(|n| i.immediate(n) & 0xFF).unwrap_or(0)
}

pub fn avx(i: &Instruction) -> Option<String> {
    use Mnemonic as M;
    let m = i.mnemonic();
    let d = if i.op_count() > 0 && i.op_kind(0) == OpKind::Register { i.op0_register().number() } else { 0 };
    let wide = (0..i.op_count()).any(|n| ymm(i, n)) && !matches!(m, M::Vextractf128 | M::Vinsertf128);
    // the high half of the destination after a 128-bit result
    let clear = format!("c->yh[{d}]=vzero();");
    // dst = fun(op1, op2[, extra]) lane by lane, 128 or 256 bits
    let bin = |fun: &str, extra: &str| -> String {
        if wide {
            format!("{{V a0={},b0={},a1={},b1={};X[{d}]={fun}(a0,b0{extra});c->yh[{d}]={fun}(a1,b1{extra});}}", lo(i, 1), lo(i, 2), hi(i, 1), hi(i, 2))
        } else {
            format!("{{V a0={},b0={};X[{d}]={fun}(a0,b0{extra});{clear}}}", lo(i, 1), lo(i, 2))
        }
    };
    // dst = fun(op1[, extra])
    let un = |fun: &str, extra: &str| -> String {
        if wide {
            format!("{{V a0={},a1={};X[{d}]={fun}(a0{extra});c->yh[{d}]={fun}(a1{extra});}}", lo(i, 1), hi(i, 1))
        } else {
            format!("{{V a0={};X[{d}]={fun}(a0{extra});{clear}}}", lo(i, 1))
        }
    };
    // scalar single: element 0 = expr(x: op1[0], y: op2), elements 1..3 from op1
    let ss = |expr: &str| -> String {
        format!("{{V a=X[{}];float x=a.ps[0],y={};a.ps[0]=({expr});X[{d}]=a;{clear}}}", i.op_register(1).number(), f32src(i, 2))
    };
    let s = match m {
        M::Vzeroupper | M::Vzeroall => {
            if m == M::Vzeroall { "for(int k=0;k<16;k++){X[k]=vzero();c->yh[k]=vzero();}".into() } else { "for(int k=0;k<16;k++)c->yh[k]=vzero();".into() }
        }
        // ---- moves ----
        M::Vmovaps | M::Vmovups | M::Vmovdqa | M::Vmovdqu | M::Vmovapd | M::Vmovupd => {
            if i.op_kind(0) == OpKind::Memory {
                if ymm(i, 1) { format!("MV({e})=X[{s}];MV(({e})+16)=c->yh[{s}];", e = ea(i), s = i.op_register(1).number()) } else { format!("MV({})=X[{}];", ea(i), i.op_register(1).number()) }
            } else if wide {
                format!("{{V a0={},a1={};X[{d}]=a0;c->yh[{d}]=a1;}}", lo(i, 1), hi(i, 1))
            } else {
                format!("X[{d}]={};{clear}", lo(i, 1))
            }
        }
        M::Vmovss => match (i.op_count(), i.op_kind(0)) {
            (2, OpKind::Memory) => format!("MF32({})=X[{}].ps[0];", ea(i), i.op_register(1).number()),
            (2, _) => format!("X[{d}]=v_from32(M32({}));{clear}", ea(i)),
            _ => format!("{{V a=X[{}];a.d[0]=X[{}].d[0];X[{d}]=a;{clear}}}", i.op_register(1).number(), i.op_register(2).number()),
        },
        M::Vmovsd => match (i.op_count(), i.op_kind(0)) {
            (2, OpKind::Memory) => format!("M64({})=X[{}].q[0];", ea(i), i.op_register(1).number()),
            (2, _) => format!("X[{d}]=vfrom64(M64({}));{clear}", ea(i)),
            _ => format!("{{V a=X[{}];a.q[0]=X[{}].q[0];X[{d}]=a;{clear}}}", i.op_register(1).number(), i.op_register(2).number()),
        },
        M::Vmovd | M::Vmovq => {
            let b = if m == M::Vmovq { 64 } else { 32 };
            if i.op_kind(0) == OpKind::Register && i.op0_register().is_xmm() {
                let src = match i.op_kind(1) {
                    OpKind::Register if i.op_register(1).is_xmm() => format!("X[{}].q[0]", i.op_register(1).number()),
                    OpKind::Register => rreg(i.op_register(1)),
                    _ => format!("M{b}({})", ea(i)),
                };
                format!("X[{d}]=vfrom64((u64)(u{b})({src}));{clear}")
            } else if i.op_kind(0) == OpKind::Memory {
                format!("M{b}({})=X[{}].{}[0];", ea(i), i.op_register(1).number(), if b == 64 { "q" } else { "d" })
            } else {
                wreg(i.op0_register(), &format!("X[{}].{}[0]", i.op_register(1).number(), if b == 64 { "q" } else { "d" }))
            }
        }
        M::Vmovlps | M::Vmovlpd if i.op_kind(0) == OpKind::Memory => format!("M64({})=X[{}].q[0];", ea(i), i.op_register(1).number()),
        M::Vmovlps | M::Vmovlpd => format!("{{V a=X[{}];a.q[0]=M64({});X[{d}]=a;{clear}}}", i.op_register(1).number(), ea(i)),
        M::Vmovhps | M::Vmovhpd if i.op_kind(0) == OpKind::Memory => format!("M64({})=X[{}].q[1];", ea(i), i.op_register(1).number()),
        M::Vmovhps | M::Vmovhpd => format!("{{V a=X[{}];a.q[1]=M64({});X[{d}]=a;{clear}}}", i.op_register(1).number(), ea(i)),
        M::Vmovlhps => bin("v_movlhps", ""),
        M::Vmovhlps => format!("{{V a=X[{}];a.q[0]=X[{}].q[1];X[{d}]=a;{clear}}}", i.op_register(1).number(), i.op_register(2).number()),
        M::Vbroadcastss | M::Vpbroadcastd => {
            let v = if i.op_kind(1) == OpKind::Memory { format!("M32({})", ea(i)) } else { format!("X[{}].d[0]", i.op_register(1).number()) };
            if wide { format!("{{V a=v_splat32({v});X[{d}]=a;c->yh[{d}]=a;}}") } else { format!("X[{d}]=v_splat32({v});{clear}") }
        }
        M::Vmovddup => {
            let v = if i.op_kind(1) == OpKind::Memory { format!("vfrom64(M64({}))", ea(i)) } else { format!("X[{}]", i.op_register(1).number()) };
            format!("X[{d}]=v_movddup({v});{clear}")
        }
        M::Vmovshdup => un("v_movshdup", ""),
        M::Vinsertps => {
            let (src, mem) = if i.op_kind(2) == OpKind::Memory { (format!("v_from32(M32({}))", ea(i)), 1) } else { (format!("X[{}]", i.op_register(2).number()), 0) };
            format!("{{V a=X[{}],b={src};X[{d}]=v_insertps(a,b,{},{mem});{clear}}}", i.op_register(1).number(), imm(i))
        }
        M::Vextractps | M::Vpextrd => {
            let v = format!("X[{}].d[{}]", i.op_register(1).number(), imm(i) & 3);
            if i.op_kind(0) == OpKind::Memory { format!("M32({})={v};", ea(i)) } else { wreg(i.op0_register(), &v) }
        }
        M::Vinsertf128 | M::Vinserti128 => {
            let src = lo(i, 2);
            let (s1, h1) = (format!("X[{}]", i.op_register(1).number()), format!("c->yh[{}]", i.op_register(1).number()));
            if imm(i) & 1 != 0 { format!("{{V a={s1},b={src};X[{d}]=a;c->yh[{d}]=b;}}") } else { format!("{{V b={src},h={h1};X[{d}]=b;c->yh[{d}]=h;}}") }
        }
        M::Vextractf128 | M::Vextracti128 => {
            let r = i.op_register(1).number();
            let v = if imm(i) & 1 != 0 { format!("c->yh[{r}]") } else { format!("X[{r}]") };
            if i.op_kind(0) == OpKind::Memory { format!("MV({})={v};", ea(i)) } else { format!("X[{d}]={v};{clear}") }
        }
        M::Vpermpd => format!("{{V a0={},a1={},r0,r1;v_permpd(a0,a1,{},&r0,&r1);X[{d}]=r0;c->yh[{d}]=r1;}}", lo(i, 1), hi(i, 1), imm(i)),
        M::Vpmovzxbd | M::Vpmovzxbw | M::Vpmovzxwd | M::Vpmovzxbq | M::Vpmovzxwq | M::Vpmovzxdq | M::Vpmovsxbd | M::Vpmovsxbw | M::Vpmovsxwd | M::Vpmovsxdq if !wide => {
            let src = if i.op_kind(1) == OpKind::Memory { format!("vlow64(M64({}))", ea(i)) } else { format!("X[{}]", i.op_register(1).number()) };
            format!("X[{d}]={}({src});{clear}", format!("{m:?}").to_lowercase().trim_start_matches('v'))
        }
        // ---- packed arithmetic, logic and shuffles ----
        M::Vaddps => bin("v_addps", ""),
        M::Vsubps => bin("v_subps", ""),
        M::Vmulps => bin("v_mulps", ""),
        M::Vdivps => bin("v_divps", ""),
        M::Vminps => bin("minps", ""),
        M::Vmaxps => bin("maxps", ""),
        M::Vandps | M::Vandpd | M::Vpand => bin("v_and", ""),
        M::Vorps | M::Vorpd | M::Vpor => bin("v_or", ""),
        M::Vxorps | M::Vxorpd | M::Vpxor => bin("v_xor", ""),
        M::Vandnps | M::Vandnpd | M::Vpandn => bin("v_andn", ""),
        M::Vpaddd => bin("v_paddd", ""),
        M::Vpsubd => bin("v_psubd", ""),
        M::Vpcmpeqd => bin("v_pcmpeqd", ""),
        M::Vpcmpeqb => bin("v_pcmpeqb", ""),
        M::Vpsllvd => bin("v_psllvd", ""),
        M::Vunpcklps => bin("punpckldq", ""),
        M::Vunpckhps => bin("punpckhdq", ""),
        M::Vpunpckhqdq => bin("punpckhqdq", ""),
        M::Vpunpcklqdq => bin("punpcklqdq", ""),
        M::Vpacksswb => bin("packsswb", ""),
        M::Vpshufb => bin("pshufb", ""),
        M::Vshufps => bin("shufps", &format!(",{}", imm(i))),
        M::Vshufpd if !wide => bin("shufpd", &format!(",{}", imm(i))),
        M::Vshufpd => format!("{{V a0={},b0={},a1={},b1={};X[{d}]=shufpd(a0,b0,{});c->yh[{d}]=shufpd(a1,b1,{});}}", lo(i, 1), lo(i, 2), hi(i, 1), hi(i, 2), imm(i) & 3, (imm(i) >> 2) & 3),
        M::Vcmpps => bin("v_cmpps", &format!(",{}", imm(i))),
        M::Vsqrtps => un("v_sqrtps", ""),
        M::Vrcpps => un("v_rcpps", ""),
        M::Vrsqrtps => un("v_sqrtps", "") .replace("v_sqrtps(a0)", "v_rcpps(v_sqrtps(a0))").replace("v_sqrtps(a1)", "v_rcpps(v_sqrtps(a1))"),
        M::Vroundps => un("v_roundps", &format!(",{},c->mxcsr", imm(i))),
        M::Vcvttps2dq => un("v_cvttps2dq", ""),
        M::Vcvtdq2ps => un("v_cvtdq2ps", ""),
        M::Vpshufd | M::Vpermilps if i.op_count() == 3 && matches!(i.op_kind(2), OpKind::Immediate8) => un("pshufd", &format!(",{}", imm(i))),
        M::Vpshuflw => un("pshuflw", &format!(",{}", imm(i))),
        M::Vpshufhw => un("pshufhw", &format!(",{}", imm(i))),
        M::Vpsrad | M::Vpslld | M::Vpsrld | M::Vpsllw | M::Vpsrlw | M::Vpsraw | M::Vpsllq | M::Vpsrlq | M::Vpslldq | M::Vpsrldq if i.op_count() == 3 && matches!(i.op_kind(2), OpKind::Immediate8) => {
            un(format!("{m:?}").to_lowercase().trim_start_matches('v'), &format!(",{}", imm(i)))
        }
        // ---- scalar single ----
        M::Vaddss => ss("x+y"),
        M::Vsubss => ss("x-y"),
        M::Vmulss => ss("x*y"),
        M::Vdivss => ss("x/y"),
        M::Vminss => ss("x<y?x:y"),
        M::Vmaxss => ss("x>y?x:y"),
        M::Vsqrtss => ss("__builtin_sqrtf(y)"),
        M::Vrcpss => ss("1.0f/y"),
        M::Vrsqrtss => ss("1.0f/__builtin_sqrtf(y)"),
        M::Vroundss => ss(&format!("v_round1(y,{},c->mxcsr)", imm(i))),
        M::Vcmpss => format!("{{V a=X[{}];float x=a.ps[0],y={};a.d[0]=v_fpred({},x,y)?~0u:0;X[{d}]=a;{clear}}}", i.op_register(1).number(), f32src(i, 2), imm(i)),
        M::Vcvtsi2ss => {
            let src = if i.op_kind(2) == OpKind::Memory { format!("(s{b})M{b}({})", ea(i), b = i.memory_size().size() * 8) } else { let r = i.op_register(2); format!("(s{b}){}", rreg(r), b = r.size() * 8) };
            format!("{{V a=X[{}];a.ps[0]=(float)({src});X[{d}]=a;{clear}}}", i.op_register(1).number())
        }
        M::Vcvtss2sd => format!("{{V a=X[{}];a.pd[0]=(double){};X[{d}]=a;{clear}}}", i.op_register(1).number(), f32src(i, 2)),
        M::Vcvttss2si | M::Vcvtss2si => {
            let v = if m == M::Vcvtss2si { format!("__builtin_rint((double){})", f32src(i, 1)) } else { format!("(double){}", f32src(i, 1)) };
            wreg(i.op0_register(), &format!("cvt_f2i{}({v})", i.op0_register().size() * 8))
        }
        M::Vucomiss | M::Vcomiss => format!("fcmp(&f,X[{}].ps[0],{});", i.op_register(0).number(), f32src(i, 1)),
        M::Vptest if !wide => format!("ptest(&f,X[{}],{});", d, lo(i, 1)),
        M::Vtestps if !wide => format!("v_testps(&f,X[{}],{});", d, lo(i, 1)),
        M::Vmovmskps if !ymm(i, 1) => wreg(i.op0_register(), &format!("movmskps(X[{}])", i.op_register(1).number())),
        M::Vpmovmskb if !ymm(i, 1) => wreg(i.op0_register(), &format!("pmovmskb(X[{}])", i.op_register(1).number())),
        // ---- FMA3: the digits say which operands are multiplied (132: 1*3+2, 213: 2*1+3, 231: 2*3+1) ----
        _ if format!("{m:?}").starts_with("Vfm") || format!("{m:?}").starts_with("Vfnm") => {
            let name = format!("{m:?}").to_lowercase();
            let (neg_prod, rest) = if let Some(r) = name.strip_prefix("vfn") { (1, r.to_string()) } else { (0, name.trim_start_matches("vf").to_string()) };
            let neg_c = u8::from(rest.starts_with("msub"));
            if !(rest.starts_with("madd") || rest.starts_with("msub")) { return None; }
            let form = &rest[4..7];
            let (ps, scalar) = (rest.ends_with("ps") || rest.ends_with("pd"), rest.ends_with("ss") || rest.ends_with("sd"));
            if !ps && !scalar { return None; }
            let dbl = rest.ends_with("pd") || rest.ends_with("sd");
            let fun = if dbl { "v_fmapd" } else { "v_fma" };
            let order: [u32; 3] = match form { "132" => [0, 2, 1], "213" => [1, 0, 2], "231" => [1, 2, 0], _ => return None };
            if scalar {
                if dbl {
                    let op = |n: u32| if n == 0 { format!("X[{d}].pd[0]") } else if i.op_kind(n) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(n).number()) };
                    format!("{{double a={},b={},cc={};X[{d}].pd[0]=v_fma1d(a,b,cc,{neg_prod},{neg_c});{clear}}}", op(order[0]), op(order[1]), op(order[2]))
                } else {
                let op = |n: u32| if n == 0 { format!("X[{d}].ps[0]") } else { f32src(i, n) };
                format!("{{float a={},b={},cc={};X[{d}].ps[0]=v_fma1(a,b,cc,{neg_prod},{neg_c});{clear}}}", op(order[0]), op(order[1]), op(order[2]))
                }
            } else if wide {
                format!("{{V a0={},b0={},c0={},a1={},b1={},c1={};X[{d}]={fun}(a0,b0,c0,{neg_prod},{neg_c});c->yh[{d}]={fun}(a1,b1,c1,{neg_prod},{neg_c});}}",
                    lo(i, order[0]), lo(i, order[1]), lo(i, order[2]), hi(i, order[0]), hi(i, order[1]), hi(i, order[2]))
            } else {
                format!("{{V a0={},b0={},c0={};X[{d}]={fun}(a0,b0,c0,{neg_prod},{neg_c});{clear}}}", lo(i, order[0]), lo(i, order[1]), lo(i, order[2]))
            }
        }
        // ---- more in-lane AVX2 integer and double ops: the SSE helper (rt/cpu.h) or an expression on each half ----
        M::Vpaddb | M::Vpaddw | M::Vpaddq | M::Vpsubb | M::Vpsubw | M::Vpsubq | M::Vpmullw | M::Vaddpd | M::Vsubpd | M::Vmulpd | M::Vdivpd => {
            let (fld, op) = match m {
                M::Vpaddb => ("b", "+"), M::Vpaddw => ("w", "+"), M::Vpaddq => ("q", "+"), M::Vpsubb => ("b", "-"), M::Vpsubw => ("w", "-"), M::Vpsubq => ("q", "-"),
                M::Vpmullw => ("w", "*"), M::Vaddpd => ("pd", "+"), M::Vsubpd => ("pd", "-"), M::Vmulpd => ("pd", "*"), _ => ("pd", "/"),
            };
            bin(&format!("v_lane_{fld}{}", match op { "+" => "add", "-" => "sub", "*" => "mul", _ => "div" }), "")
        }
        M::Vpcmpeqw | M::Vpcmpeqq | M::Vpcmpgtb | M::Vpcmpgtw | M::Vpcmpgtd | M::Vpcmpgtq => bin(&format!("v_{}", format!("{m:?}").to_lowercase().trim_start_matches('v')), ""),
        M::Vpunpcklbw | M::Vpunpcklwd | M::Vpunpckldq | M::Vpunpckhbw | M::Vpunpckhwd | M::Vpunpckhdq | M::Vpmuludq | M::Vpackuswb
        | M::Vpmulhw | M::Vpmulhuw | M::Vpmaddwd | M::Vpmaddubsw | M::Vpminsw | M::Vpmaxsw | M::Vpminub | M::Vpmaxub | M::Vpminsb | M::Vpmaxsb | M::Vpminuw | M::Vpmaxuw
        | M::Vpminsd | M::Vpmaxsd | M::Vpminud | M::Vpmaxud | M::Vpaddsb | M::Vpsubsb | M::Vpaddsw | M::Vpsubsw | M::Vpaddusb | M::Vpsubusb | M::Vpaddusw | M::Vpsubusw
        | M::Vpavgb | M::Vpavgw | M::Vpmulld | M::Vpsadbw | M::Vpackssdw | M::Vpackusdw | M::Vminpd | M::Vmaxpd => bin(format!("{m:?}").to_lowercase().trim_start_matches('v'), ""),
        M::Vunpcklpd => bin("punpcklqdq", ""),
        M::Vunpckhpd => bin("punpckhqdq", ""),
        M::Vpsrlvd => bin("v_psrlvd", ""),
        M::Vpsravd => bin("v_psravd", ""),
        M::Vpsllvq => bin("v_psllvq", ""),
        M::Vpsrlvq => bin("v_psrlvq", ""),
        M::Vhaddps => bin("v_haddps", ""),
        M::Vhaddpd => bin("v_haddpd", ""),
        M::Vhsubps => bin("v_hsubps", ""),
        M::Vaddsubps => bin("v_addsubps", ""),
        M::Vdpps => bin("v_dpps", &format!(",{}", imm(i))),
        M::Vcmppd => bin("v_cmppd", &format!(",{}", imm(i))),
        M::Vpalignr => bin("palignr", &format!(",{}", imm(i))),
        M::Vpblendw => bin("pblendw", &format!(",{}", imm(i))),
        M::Vpabsb => un("pabs_b", ""),
        M::Vpabsw => un("pabs_w", ""),
        M::Vpabsd => un("pabs_d", ""),
        M::Vsqrtpd => un("v_sqrtpd", ""),
        M::Vmovsldup => un("v_movsldup", ""),
        M::Vroundpd => un("v_roundpd", &format!(",{},c->mxcsr", imm(i))),
        M::Vcvtps2dq => un("v_cvtps2dq", ",c->mxcsr"),
        M::Vpermilpd if i.op_count() == 3 && matches!(i.op_kind(2), OpKind::Immediate8) => {
            if wide { format!("{{V a0={},a1={};X[{d}]=shufpd(a0,a0,{});c->yh[{d}]=shufpd(a1,a1,{});}}", lo(i, 1), hi(i, 1), imm(i) & 3, (imm(i) >> 2) & 3) } else { un("shufpd", "") .replace("shufpd(a0)", &format!("shufpd(a0,a0,{})", imm(i) & 3)) }
        }
        M::Vpermilps => bin("v_permilps", ""),
        M::Vpsrad | M::Vpslld | M::Vpsrld | M::Vpsllw | M::Vpsrlw | M::Vpsraw | M::Vpsllq | M::Vpsrlq if i.op_count() == 3 => {
            // the count from an xmm register or memory's low 64 bits
            let n = if i.op_kind(2) == OpKind::Memory { format!("M64({})", ea(i)) } else { format!("X[{}].q[0]", i.op_register(2).number()) };
            let f = format!("{m:?}").to_lowercase();
            if wide { format!("{{u64 n={n};V a0={},a1={};X[{d}]={f}(a0,n);c->yh[{d}]={f}(a1,n);}}", lo(i, 1), hi(i, 1), f = f.trim_start_matches('v')) } else { format!("{{u64 n={n};V a0={};X[{d}]={}(a0,n);{clear}}}", lo(i, 1), f.trim_start_matches('v')) }
        }
        M::Vblendps | M::Vblendpd | M::Vpblendd => {
            // one immediate bit an element: blendps on dwords, two dwords an element for pd
            let k = imm(i);
            let (m0, m1) = match m {
                M::Vblendpd => { let e = |b: u64| (b & 1) * 3 | ((b >> 1) & 1) * 12; (e(k), e(k >> 2)) }
                _ => (k & 15, k >> 4),
            };
            if wide { format!("{{V a0={},b0={},a1={},b1={};X[{d}]=blendps(a0,b0,{m0});c->yh[{d}]=blendps(a1,b1,{m1});}}", lo(i, 1), lo(i, 2), hi(i, 1), hi(i, 2)) } else { bin("blendps", &format!(",{m0}")) }
        }
        M::Vblendvps | M::Vblendvpd | M::Vpblendvb => {
            let f = match m { M::Vblendvps => "v_blendvps", M::Vblendvpd => "v_blendvpd", _ => "pblendvb" };
            if wide { format!("{{V a0={},b0={},m0={},a1={},b1={},m1={};X[{d}]={f}(a0,b0,m0);c->yh[{d}]={f}(a1,b1,m1);}}", lo(i, 1), lo(i, 2), lo(i, 3), hi(i, 1), hi(i, 2), hi(i, 3)) } else { format!("{{V a0={},b0={},m={};X[{d}]={f}(a0,b0,m);{clear}}}", lo(i, 1), lo(i, 2), lo(i, 3)) }
        }
        // ---- across the two halves ----
        M::Vperm2f128 | M::Vperm2i128 => {
            let k = imm(i);
            let pick = |s: u64| -> String { if s & 8 != 0 { "vzero()".into() } else { ["a0", "a1", "b0", "b1"][(s & 3) as usize].into() } };
            format!("{{V a0={},a1={},b0={},b1={};V r0={},r1={};X[{d}]=r0;c->yh[{d}]=r1;}}", lo(i, 1), hi(i, 1), lo(i, 2), hi(i, 2), pick(k), pick(k >> 4))
        }
        M::Vpermd | M::Vpermps => format!("{{V i0={},i1={},t0={},t1={},r0,r1;v_permd(i0,i1,t0,t1,&r0,&r1);X[{d}]=r0;c->yh[{d}]=r1;}}", lo(i, 1), hi(i, 1), lo(i, 2), hi(i, 2)),
        M::Vpermq => format!("{{V a0={},a1={},r0,r1;v_permpd(a0,a1,{},&r0,&r1);X[{d}]=r0;c->yh[{d}]=r1;}}", lo(i, 1), hi(i, 1), imm(i)),
        M::Vbroadcastsd | M::Vpbroadcastq => {
            let v = if i.op_kind(1) == OpKind::Memory { format!("M64({})", ea(i)) } else { format!("X[{}].q[0]", i.op_register(1).number()) };
            if wide { format!("{{V a=v_splat64({v});X[{d}]=a;c->yh[{d}]=a;}}") } else { format!("X[{d}]=v_splat64({v});{clear}") }
        }
        M::Vpbroadcastb | M::Vpbroadcastw => {
            let (b, f) = if m == M::Vpbroadcastb { (8, "b") } else { (16, "w") };
            let v = if i.op_kind(1) == OpKind::Memory { format!("M{b}({})", ea(i)) } else { format!("X[{}].{f}[0]", i.op_register(1).number()) };
            let e = format!("v_splat{b}({v})");
            if wide { format!("{{V a={e};X[{d}]=a;c->yh[{d}]=a;}}") } else { format!("X[{d}]={e};{clear}") }
        }
        M::Vbroadcastf128 | M::Vbroadcasti128 => format!("{{V a=MV({});X[{d}]=a;c->yh[{d}]=a;}}", ea(i)),
        M::Vpmovzxbd | M::Vpmovzxbw | M::Vpmovzxwd | M::Vpmovzxbq | M::Vpmovzxwq | M::Vpmovzxdq | M::Vpmovsxbd | M::Vpmovsxbw | M::Vpmovsxwd | M::Vpmovsxdq | M::Vpmovsxbq | M::Vpmovsxwq => {
            // ymm: the high half from the source bytes past the ones the low half took
            let name = format!("{m:?}").to_lowercase();
            let f = name.trim_start_matches('v');
            let (sz, dz) = (match &f[6..7] { "b" => 1, "w" => 2, _ => 4 }, match &f[7..8] { "w" => 2, "d" => 4, _ => 8 });
            let src = if i.op_kind(1) == OpKind::Memory {
                match i.memory_size().size() { 16 => format!("MV({})", ea(i)), 8 => format!("vlow64(M64({}))", ea(i)), 4 => format!("v_from32(M32({}))", ea(i)), _ => format!("v_from32((u32)M16({}))", ea(i)) }
            } else { format!("X[{}]", i.op_register(1).number()) };
            format!("{{V s={src};V r0={f}(s),r1={};X[{d}]=r0;c->yh[{d}]=r1;}}", if wide { format!("{f}(psrldq(s,{}))", 16 * sz / dz) } else { "vzero()".into() })
        }
        M::Vcvtps2pd => {
            let src = if i.op_kind(1) == OpKind::Memory { if wide { format!("MV({})", ea(i)) } else { format!("vlow64(M64({}))", ea(i)) } } else { format!("X[{}]", i.op_register(1).number()) };
            format!("{{V s={src},r0,r1=vzero();r0.pd[0]=s.ps[0];r0.pd[1]=s.ps[1];{}X[{d}]=r0;c->yh[{d}]=r1;}}", if wide { "r1.pd[0]=s.ps[2];r1.pd[1]=s.ps[3];" } else { "" })
        }
        M::Vcvtpd2ps | M::Vcvttpd2dq | M::Vcvtpd2dq => {
            // four doubles (ymm source) or two into the low elements
            let w = i.op_kind(1) == OpKind::Register && i.op_register(1).is_ymm() || i.op_kind(1) == OpKind::Memory && i.memory_size().size() == 32;
            let cv = match m { M::Vcvtpd2ps => "r.ps[k]=(float)s[k];", M::Vcvttpd2dq => "r.sd[k]=cvt_f2i32(s[k]);", _ => "r.sd[k]=cvt_f2i32(__builtin_rint(s[k]));" };
            format!("{{V a0={},a1={};double s[4]={{a0.pd[0],a0.pd[1],a1.pd[0],a1.pd[1]}};V r=vzero();for(int k=0;k<{};k++){cv}X[{d}]=r;{clear}}}", lo(i, 1), if w { hi(i, 1) } else { "vzero()".into() }, if w { 4 } else { 2 })
        }
        M::Vcvtdq2pd => {
            let src = if i.op_kind(1) == OpKind::Memory { if wide { format!("MV({})", ea(i)) } else { format!("vlow64(M64({}))", ea(i)) } } else { format!("X[{}]", i.op_register(1).number()) };
            format!("{{V s={src},r0,r1=vzero();r0.pd[0]=s.sd[0];r0.pd[1]=s.sd[1];{}X[{d}]=r0;c->yh[{d}]=r1;}}", if wide { "r1.pd[0]=s.sd[2];r1.pd[1]=s.sd[3];" } else { "" })
        }
        M::Vmovmskps | M::Vmovmskpd | M::Vpmovmskb => {
            let (f, sh) = match m { M::Vmovmskps => ("movmskps", 4), M::Vmovmskpd => ("movmskpd", 2), _ => ("pmovmskb", 16) };
            let r = i.op_register(1).number();
            wreg(i.op0_register(), &format!("((u32){f}(X[{r}])|(u32){f}(c->yh[{r}])<<{sh})"))
        }
        M::Vptest => format!("v_ptest256(&f,X[{d}],c->yh[{d}],{},{});", lo(i, 1), hi(i, 1)),
        M::Vtestps | M::Vtestpd => format!("v_testp256(&f,X[{d}],c->yh[{d}],{},{},{});", lo(i, 1), if wide { hi(i, 1) } else { "vzero()".into() }, if m == M::Vtestps { 31 } else { 63 }),
        M::Vpinsrb | M::Vpinsrw | M::Vpinsrd | M::Vpinsrq => {
            let (f, b, n) = match m { M::Vpinsrq => ("q", 64, 1), M::Vpinsrd => ("d", 32, 3), M::Vpinsrw => ("w", 16, 7), _ => ("b", 8, 15) };
            let src = if i.op_kind(2) == OpKind::Memory { format!("M{b}({})", ea(i)) } else { rreg(i.op_register(2)) };
            format!("{{V a=X[{}];a.{f}[{}]=(u{b})({src});X[{d}]=a;{clear}}}", i.op_register(1).number(), imm(i) & n)
        }
        M::Vpextrb | M::Vpextrw | M::Vpextrq => {
            let (f, b, n) = match m { M::Vpextrq => ("q", 64, 1), M::Vpextrw => ("w", 16, 7), _ => ("b", 8, 15) };
            let v = format!("X[{}].{f}[{}]", i.op_register(1).number(), imm(i) & n);
            if i.op_kind(0) == OpKind::Memory { format!("M{b}({})={v};", ea(i)) } else { wreg(i.op0_register(), &format!("(u64)(u{b}){v}")) }
        }
        // ---- scalar double ----
        M::Vaddsd | M::Vsubsd | M::Vmulsd | M::Vdivsd | M::Vminsd | M::Vmaxsd | M::Vsqrtsd | M::Vroundsd => {
            let y = if i.op_kind(2) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(2).number()) };
            let e = match m { M::Vaddsd => "x+y".to_string(), M::Vsubsd => "x-y".into(), M::Vmulsd => "x*y".into(), M::Vdivsd => "x/y".into(), M::Vminsd => "x<y?x:y".into(), M::Vmaxsd => "x>y?x:y".into(), M::Vsqrtsd => "__builtin_sqrt(y)".into(), _ => format!("v_round1d(y,{},c->mxcsr)", imm(i)) };
            format!("{{V a=X[{}];double x=a.pd[0],y={y};a.pd[0]=({e});X[{d}]=a;{clear}}}", i.op_register(1).number())
        }
        M::Vcmpsd => {
            let y = if i.op_kind(2) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(2).number()) };
            format!("{{V a=X[{}];double x=a.pd[0],y={y};a.q[0]=v_fpredd({},x,y)?~0ull:0;X[{d}]=a;{clear}}}", i.op_register(1).number(), imm(i))
        }
        M::Vcvtsd2ss => {
            let y = if i.op_kind(2) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(2).number()) };
            format!("{{V a=X[{}];a.ps[0]=(float){y};X[{d}]=a;{clear}}}", i.op_register(1).number())
        }
        M::Vcvtsi2sd => {
            let src = if i.op_kind(2) == OpKind::Memory { format!("(s{b})M{b}({})", ea(i), b = i.memory_size().size() * 8) } else { let r = i.op_register(2); format!("(s{b}){}", rreg(r), b = r.size() * 8) };
            format!("{{V a=X[{}];a.pd[0]=(double)({src});X[{d}]=a;{clear}}}", i.op_register(1).number())
        }
        M::Vcvttsd2si | M::Vcvtsd2si => {
            let x = if i.op_kind(1) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(1).number()) };
            let v = if m == M::Vcvtsd2si { format!("__builtin_rint({x})") } else { x };
            wreg(i.op0_register(), &format!("cvt_f2i{}({v})", i.op0_register().size() * 8))
        }
        M::Vucomisd | M::Vcomisd => {
            let x = if i.op_kind(1) == OpKind::Memory { format!("MF64({})", ea(i)) } else { format!("X[{}].pd[0]", i.op_register(1).number()) };
            format!("fcmp(&f,X[{}].pd[0],{x});", i.op_register(0).number())
        }
        M::Mulx => {
            let b = i.op0_register().size() * 8;
            let rdx = rreg(if b == 64 { Register::RDX } else { Register::EDX });
            let src = super::rd(i, 2);
            if b == 64 {
                format!("{{u64 s={src},a={rdx};u64 l=a*s,h=mulhu64(a,s);{}{}}}", wreg(i.op_register(1), "l"), wreg(i.op0_register(), "h"))
            } else {
                format!("{{u64 p=(u64)(u32){rdx}*(u32){src};{}{}}}", wreg(i.op_register(1), "(u32)p"), wreg(i.op0_register(), "(u32)(p>>32)"))
            }
        }
        M::Pdep | M::Pext => {
            let b = i.op0_register().size() * 8;
            format!("{{u{b} r={}(({}),({}));{}}}", if m == M::Pdep { "v_pdep" } else { "v_pext" }, rreg(i.op_register(1)), super::rd(i, 2), wreg(i.op0_register(), "r"))
        }
        M::Bzhi => {
            let b = i.op0_register().size() * 8;
            format!("{{u{b} s={};u32 n=(u32)({})&255;u{b} r=n>={b}?s:(s&(((u{b})1<<n)-1));f.zf=r==0;f.sf=r>>{};f.cf=n>{};f.of=0;{}}}", super::rd(i, 1), rreg(i.op_register(2)), b - 1, b - 1, wreg(i.op0_register(), "r"))
        }
        M::Blsi | M::Blsmsk => {
            let b = i.op0_register().size() * 8;
            let e = if m == M::Blsi { format!("s&(u{b})(-s)") } else { format!("s^(u{b})(s-1)") };
            format!("{{u{b} s={};u{b} r={e};f.zf=r==0;f.sf=r>>{};f.cf=s!=0;f.of=0;{}}}", super::rd(i, 1), b - 1, wreg(i.op0_register(), "r"))
        }
        // ---- BMI1 / BMI2 (general registers) ----
        M::Andn => {
            let b = i.op0_register().size() * 8;
            wreg(i.op0_register(), &format!("logic{b}(&f,(u{b})(~{}&{}))", rreg(i.op_register(1)), super::rd(i, 2)))
        }
        M::Blsr => {
            let b = i.op0_register().size() * 8;
            format!("{{u{b} s={};u{b} r=s&(u{b})(s-1);f.zf=r==0;f.sf=r>>{};f.cf=s==0;f.of=0;{}}}", super::rd(i, 1), b - 1, wreg(i.op0_register(), "r"))
        }
        M::Rorx => {
            let b = i.op0_register().size() * 8;
            let n = imm(i) as u32 % b as u32;
            format!("{{u{b} s={};{}}}", super::rd(i, 1), wreg(i.op0_register(), &if n == 0 { "s".to_string() } else { format!("(u{b})((s>>{n})|(s<<{}))", b as u32 - n) }))
        }
        M::Shlx | M::Shrx | M::Sarx => {
            let b = i.op0_register().size() * 8;
            let n = format!("({}&{})", rreg(i.op_register(2)), b - 1);
            let v = match m { M::Shlx => format!("(u{b})(s<<{n})"), M::Shrx => format!("(u{b})(s>>{n})"), _ => format!("(u{b})((s{b})s>>{n})") };
            format!("{{u{b} s={};{}}}", super::rd(i, 1), wreg(i.op0_register(), &v))
        }
        _ => return None,
    };
    Some(s)
}
