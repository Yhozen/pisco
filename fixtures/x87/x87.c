// SPDX-License-Identifier: GPL-2.0-or-later
// A 32-bit title for what old Visual C++ code leans on: the x87 (control word, precision, rounding, the
// transcendental instructions, compares), the direction flag, EFLAGS.ID, a stdcall callback, and instructions
// 64-bit code never uses (pushad, enter, loop, jecxz, rcl, rcr, xlat). It prints one line per check; the
// expected output (expected.txt) is what the same program prints under Wine on a real CPU:
//   tools/oracle-wine/oracle.sh run fixtures out/x87/x87.exe > fixtures/x87/expected.txt
// No C runtime, no SSE (as Visual C++ 7.1 compiled): build.sh has the flags. It never runs cpuid.
typedef unsigned int u32; typedef unsigned long long u64; typedef long long s64;
__declspec(dllimport) void *__stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void __stdcall ExitProcess(u32);

static char line[256]; static u32 at;
static void flush(void) { u32 w; line[at++] = '\n'; WriteFile(GetStdHandle((u32)-11), line, at, &w, 0); at = 0; }
static void str(const char *s) { while (*s) line[at++] = *s++; }
static void hex(u64 v, int digits) { line[at++] = ' '; for (int i = digits - 1; i >= 0; i--) line[at++] = "0123456789abcdef"[(v >> (4 * i)) & 15]; }
static void h32(u32 v) { hex(v, 8); }
static void h64(u64 v) { hex(v, 16); }
static u64 bits(double d) { union { double d; u64 u; } x; x.d = d; return x.u; }

// the instructions under test, as they are (cdecl, arguments at [esp+4])
u32 get_fcw(void); void set_fcw(u32); int fist_d(double); void fistll_d(double, s64 *);
void div_d(double, double, double *); void add_d(double, double, double *); void mul_d(double, double, double *);
void sqrt_d(double, double *); void rnd_d(double, double *);
void x87_un(int op, double x, double *out); void x87_bin(int op, double y, double x, double *out);
u32 fcmp_i(double a, double b); u32 fcmp_sw(double a, double b); u32 fxam_d(double);
void fild64(const s64 *in, s64 *out); void std_copy(char *dst_last, const char *src_last, u32 n);
u32 eflags_id(void); u32 call_std(void *fn, int a, int b, int c); u32 loop_sum(u32 n); u32 pushad_test(void);
u32 rcl_test(u32 v, u32 carry, u32 n); u32 rcr_test(u32 v, u32 carry, u32 n); u32 enter_test(void);
u32 xlat_test(const char *table, u32 index); u32 adc_test(u32 a, u32 b, u32 c, u32 d); u32 lahf_test(u32 a, u32 b);
__asm__(".intel_syntax noprefix\n.text\n"
  ".globl _get_fcw\n_get_fcw: sub esp, 4\n fnstcw [esp]\n movzx eax, word ptr [esp]\n add esp, 4\n ret\n"
  ".globl _set_fcw\n_set_fcw: fldcw [esp+4]\n ret\n"
  ".globl _fist_d\n_fist_d: fld qword ptr [esp+4]\n fistp dword ptr [esp+4]\n mov eax, [esp+4]\n ret\n"
  ".globl _fistll_d\n_fistll_d: fld qword ptr [esp+4]\n mov eax, [esp+12]\n fistp qword ptr [eax]\n ret\n"
  ".globl _div_d\n_div_d: fld qword ptr [esp+4]\n fdiv qword ptr [esp+12]\n mov eax, [esp+20]\n fstp qword ptr [eax]\n ret\n"
  ".globl _add_d\n_add_d: fld qword ptr [esp+4]\n fadd qword ptr [esp+12]\n mov eax, [esp+20]\n fstp qword ptr [eax]\n ret\n"
  ".globl _mul_d\n_mul_d: fld qword ptr [esp+4]\n fmul qword ptr [esp+12]\n mov eax, [esp+20]\n fstp qword ptr [eax]\n ret\n"
  ".globl _sqrt_d\n_sqrt_d: fld qword ptr [esp+4]\n fsqrt\n mov eax, [esp+12]\n fstp qword ptr [eax]\n ret\n"
  ".globl _rnd_d\n_rnd_d: fld qword ptr [esp+4]\n frndint\n mov eax, [esp+12]\n fstp qword ptr [eax]\n ret\n"
  // x87_un: 0 fsin, 1 fcos, 2 fptan (pushes 1.0, dropped), 3 f2xm1, 4 fabs, 5 fchs
  ".globl _x87_un\n_x87_un: mov ecx, [esp+4]\n fld qword ptr [esp+8]\n"
  " cmp ecx, 0\n jne 1f\n fsin\n jmp 9f\n1: cmp ecx, 1\n jne 2f\n fcos\n jmp 9f\n2: cmp ecx, 2\n jne 3f\n fptan\n fstp st(0)\n jmp 9f\n"
  "3: cmp ecx, 3\n jne 4f\n f2xm1\n jmp 9f\n4: cmp ecx, 4\n jne 5f\n fabs\n jmp 9f\n5: fchs\n"
  "9: mov eax, [esp+16]\n fstp qword ptr [eax]\n ret\n"
  // x87_bin: 0 fpatan = atan2(y, x), 1 fyl2x = y*log2(x), 2 fscale = x*2^trunc(y), 3 fprem = x rem y
  ".globl _x87_bin\n_x87_bin: mov ecx, [esp+4]\n fld qword ptr [esp+8]\n fld qword ptr [esp+16]\n"
  " cmp ecx, 0\n jne 1f\n fpatan\n jmp 9f\n1: cmp ecx, 1\n jne 2f\n fyl2x\n jmp 9f\n2: cmp ecx, 2\n jne 3f\n fscale\n fstp st(1)\n jmp 9f\n"
  "3: fprem\n fstp st(1)\n"
  "9: mov eax, [esp+24]\n fstp qword ptr [eax]\n ret\n"
  ".globl _fcmp_i\n_fcmp_i: fld qword ptr [esp+12]\n fld qword ptr [esp+4]\n fucomip st, st(1)\n fstp st(0)\n pushfd\n pop eax\n and eax, 0x45\n ret\n"
  ".globl _fcmp_sw\n_fcmp_sw: fld qword ptr [esp+12]\n fld qword ptr [esp+4]\n fucompp\n fnstsw ax\n and eax, 0x4500\n ret\n"
  ".globl _fxam_d\n_fxam_d: fld qword ptr [esp+4]\n fxam\n fnstsw ax\n fstp st(0)\n and eax, 0x4700\n ret\n"
  ".globl _fild64\n_fild64: mov eax, [esp+4]\n fild qword ptr [eax]\n mov eax, [esp+8]\n fistp qword ptr [eax]\n ret\n"
  ".globl _std_copy\n_std_copy: push esi\n push edi\n mov edi, [esp+12]\n mov esi, [esp+16]\n mov ecx, [esp+20]\n std\n rep movsb\n cld\n pop edi\n pop esi\n ret\n"
  ".globl _eflags_id\n_eflags_id: pushfd\n pop eax\n mov ecx, eax\n xor eax, 0x200000\n push eax\n popfd\n pushfd\n pop eax\n xor eax, ecx\n shr eax, 21\n and eax, 1\n push ecx\n popfd\n ret\n"
  // call_std: calls a stdcall function; returns its result, plus 0x10000 for each byte the stack is off afterwards
  ".globl _call_std\n_call_std: push ebp\n mov ebp, esp\n push dword ptr [ebp+20]\n push dword ptr [ebp+16]\n push dword ptr [ebp+12]\n call dword ptr [ebp+8]\n"
  " mov ecx, ebp\n sub ecx, esp\n shl ecx, 16\n add eax, ecx\n mov esp, ebp\n pop ebp\n ret\n"
  ".globl _loop_sum\n_loop_sum: mov ecx, [esp+4]\n xor eax, eax\n jecxz .Lsum_done\n.Lsum_next: add eax, ecx\n loop .Lsum_next\n.Lsum_done: ret\n"
  ".globl _pushad_test\n_pushad_test: push ebx\n mov eax, 1\n mov ecx, 2\n mov edx, 3\n mov ebx, 4\n pushad\n mov eax, 100\n mov ecx, 200\n mov edx, 300\n mov ebx, 400\n popad\n"
  " shl ecx, 4\n shl edx, 8\n shl ebx, 12\n or eax, ecx\n or eax, edx\n or eax, ebx\n pop ebx\n ret\n"
  ".globl _rcl_test\n_rcl_test: mov eax, [esp+4]\n mov ecx, [esp+12]\n mov edx, [esp+8]\n neg edx\n rcl eax, cl\n ret\n"
  ".globl _rcr_test\n_rcr_test: mov eax, [esp+4]\n mov ecx, [esp+12]\n mov edx, [esp+8]\n neg edx\n rcr eax, cl\n ret\n"
  ".globl _enter_test\n_enter_test: enter 24, 0\n mov eax, ebp\n sub eax, esp\n leave\n ret\n"
  ".globl _xlat_test\n_xlat_test: push ebx\n mov ebx, [esp+8]\n mov eax, [esp+12]\n xlatb\n movzx eax, al\n pop ebx\n ret\n"
  // adc_test: the high half of (a:b) + (c:d); lahf_test: AH after `cmp a, b`
  ".globl _adc_test\n_adc_test: mov eax, [esp+4]\n mov edx, [esp+8]\n add edx, [esp+16]\n adc eax, [esp+12]\n ret\n"
  ".globl _lahf_test\n_lahf_test: mov ecx, [esp+4]\n xor eax, eax\n cmp ecx, [esp+8]\n lahf\n and eax, 0xd500\n ret\n"
  ".att_syntax\n");

static int __stdcall callback(int a, int b, int c) { return a * 100 + b * 10 + c; }
__attribute__((noinline)) static void sw(int k) { // dense cases doing different things: a jump table, not a lookup table
  switch (k) {
    case 0: str(" zero"); break;
    case 1: h32(0x11); break;
    case 2: hex(0x22, 2); break;
    case 3: str(" three"); h32(3); break;
    case 4: line[at++] = '4'; break;
    case 5: hex(0x5555, 4); str("!"); break;
    case 6: at += 0; str(" six"); break;
    case 7: h64(7); break;
    default: str(" none"); break;
  }
}
__attribute__((noinline)) static u64 mix(u64 a, u64 b, int s) { return (a * b) ^ (a / (b | 1)) ^ (a % (b | 1)) ^ (a << s) ^ (b >> s); }
__attribute__((noinline)) static s64 smix(s64 a, s64 b) { return a / b + a % b * 3; }
__attribute__((noinline)) static double poly(double x, int n) { double s = 0, t = 1; for (int i = 1; i <= n; i++) { t *= x / i; s += t; } return s; } // e^x - 1, compiled to x87
static s64 scaled(double v) { s64 r; fistll_d(v * 1e9, &r); return r; } // nine decimals: enough to tell a wrong function, not the last bit

void start(void) {
  double d; s64 q;
  str("fcw at start"); h32(get_fcw()); flush();
  static const u32 rc[4] = {0x027f, 0x067f, 0x0a7f, 0x0e7f}; // nearest, down, up, toward zero (53-bit precision)
  for (int i = 0; i < 4; i++) { set_fcw(rc[i]); str("fistp rc"); h32(i); h32(fist_d(2.5)); h32(fist_d(-2.5)); h32(fist_d(3.5)); h32(fist_d(-0.5)); rnd_d(2.5, &d); h64(bits(d)); rnd_d(-1.5, &d); h64(bits(d)); flush(); }
  set_fcw(0x027f);
  str("53-bit"); div_d(1, 3, &d); h64(bits(d)); add_d(16777216.0, 1, &d); h64(bits(d)); mul_d(1.1, 1.1, &d); h64(bits(d)); sqrt_d(2, &d); h64(bits(d)); flush();
  set_fcw(0x007f); // 24-bit precision: what Direct3D 9 switches to
  str("24-bit"); div_d(1, 3, &d); h64(bits(d)); add_d(16777216.0, 1, &d); h64(bits(d)); mul_d(1.1, 1.1, &d); h64(bits(d)); sqrt_d(2, &d); h64(bits(d)); flush();
  volatile double px = 0.7; volatile int pn = 12;
  str("24-bit poly"); h64(bits(poly(px, pn))); flush();
  set_fcw(0x027f);
  str("53-bit poly"); h64(bits(poly(px, pn))); flush();
  str("fcw read back"); set_fcw(0x0e7f); h32(get_fcw()); set_fcw(0x027f); h32(get_fcw()); flush();
  str("sin cos tan"); x87_un(0, 1.0, &d); h64(scaled(d)); x87_un(1, 1.0, &d); h64(scaled(d)); x87_un(2, 1.0, &d); h64(scaled(d)); flush();
  str("f2xm1 fabs fchs"); x87_un(3, 0.5, &d); h64(scaled(d)); x87_un(4, -2.25, &d); h64(bits(d)); x87_un(5, 2.25, &d); h64(bits(d)); flush();
  str("atan2 ylog2x scale rem"); x87_bin(0, 1.0, 2.0, &d); h64(scaled(d)); x87_bin(1, 3.0, 10.0, &d); h64(scaled(d)); x87_bin(2, 3.7, 1.5, &d); h64(bits(d)); x87_bin(3, 3.0, 10.5, &d); h64(bits(d)); flush();
  double nan = 0.0; nan = (nan - nan) / (nan - nan);
  str("fucomip"); h32(fcmp_i(1, 2)); h32(fcmp_i(2, 1)); h32(fcmp_i(2, 2)); h32(fcmp_i(nan, 1)); flush();
  str("fucompp sw"); h32(fcmp_sw(1, 2)); h32(fcmp_sw(2, 1)); h32(fcmp_sw(2, 2)); h32(fcmp_sw(nan, 1)); flush();
  str("fxam"); h32(fxam_d(1.5)); h32(fxam_d(-1.5)); h32(fxam_d(0.0)); h32(fxam_d(nan)); h32(fxam_d(1e308 * 10)); flush();
  s64 in = 0x000fedcba9876543ll; fild64(&in, &q); str("fild fistp 64"); h64(q); in = -123456789012345ll; fild64(&in, &q); h64(q); fistll_d(-1e15 - 0.5, &q); h64(q); flush();
  char buf[16] = "0123456789abcde"; std_copy(buf + 12, buf + 9, 8); str("std movsb "); str(buf); flush();
  str("eflags id"); h32(eflags_id()); flush();
  str("stdcall callback"); h32(call_std(callback, 1, 2, 3)); flush();
  str("loop jecxz"); h32(loop_sum(100)); h32(loop_sum(0)); h32(loop_sum(1)); flush();
  str("pushad"); h32(pushad_test()); flush();
  str("rcl rcr"); h32(rcl_test(0x80000001, 1, 1)); h32(rcl_test(0x80000001, 0, 4)); h32(rcr_test(0x80000001, 1, 1)); h32(rcr_test(0x80000001, 0, 9)); flush();
  str("enter"); h32(enter_test()); flush();
  str("xlat adc lahf"); h32(xlat_test("ABCDEFGH", 5)); h32(adc_test(1, 0xffffffff, 2, 1)); h32(lahf_test(1, 2)); h32(lahf_test(2, 2)); h32(lahf_test(3, 2)); flush();
  str("switch"); for (volatile int k = -1; k < 9; k++) sw(k); flush();
  volatile u64 a = 0x0123456789abcdefull, b = 0x00000fedcba98765ull; volatile s64 sa = -1234567890123456789ll, sb = 987654321ll;
  str("64-bit ints"); h64(mix(a, b, 13)); h64(mix(b, a, 37)); h64(smix(sa, sb)); h64(smix(sb, -7)); flush();
  ExitProcess(0);
}
