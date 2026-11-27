// SPDX-License-Identifier: GPL-2.0-or-later
// A 32-bit title for MMX, as the games of 2000 to 2005 used it (Quake III's engines, Miles' mixer): every
// operation on 64-bit registers, with values that reach the edges (wrap-around, saturation, sign), the moves
// between MMX, general registers and memory, EMMS before x87 code again, and the SSE instructions that work on
// MMX registers (pshufw, pinsrw, pextrw, pmovmskb, pavgb, pminub, pmaxsw, psadbw, pmulhuw). Written in
// assembly: a compiler of today turns MMX's intrinsics into SSE2. It prints one line
// per check; expected.txt is what the same program prints under Wine on a real CPU:
//   tools/oracle-wine/oracle.sh run fixtures out/mmx/mmx.exe > fixtures/mmx/expected.txt
typedef unsigned int u32; typedef unsigned long long u64;
__declspec(dllimport) void *__stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void __stdcall ExitProcess(u32);

static char line[256]; static u32 at;
static void flush(void) { u32 w; line[at++] = '\n'; WriteFile(GetStdHandle((u32)-11), line, at, &w, 0); at = 0; }
static void str(const char *s) { while (*s) line[at++] = *s++; }
static void h64(u64 v) { line[at++] = ' '; for (int i = 15; i >= 0; i--) line[at++] = "0123456789abcdef"[(v >> (4 * i)) & 15]; }
static void show(const char *name, u64 v) { str(name); h64(v); flush(); }

// operands in memory, so nothing is folded; each operation is written out (a compiler of today makes SSE2 of MMX's intrinsics)
static volatile u64 A = 0x80ff7f0100fe8001ull, B = 0x7f01ff80ff0280ffull, C = 0x0123456789abcdefull, S = 0x0000000000000004ull;
#define OP2(fn, insn) static u64 fn(u64 a, u64 b) { u64 r; __asm__ volatile("movq %1, %%mm0\n\tmovq %2, %%mm1\n\t" insn " %%mm1, %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "m"(a), "m"(b) : "mm0", "mm1"); return r; }
#define OPM(fn, insn) static u64 fn(u64 a, u64 b) { u64 r; __asm__ volatile("movq %1, %%mm0\n\t" insn " %2, %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "m"(a), "m"(b) : "mm0"); return r; }
#define OPI(fn, insn, n) static u64 fn(u64 a) { u64 r; __asm__ volatile("movq %1, %%mm0\n\t" insn " $" #n ", %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "m"(a) : "mm0"); return r; }
OP2(paddb, "paddb") OP2(paddw, "paddw") OP2(paddd, "paddd") OP2(paddsb, "paddsb") OP2(paddsw, "paddsw") OP2(paddusb, "paddusb") OP2(paddusw, "paddusw")
OP2(psubb, "psubb") OP2(psubw, "psubw") OP2(psubd, "psubd") OP2(psubsb, "psubsb") OP2(psubsw, "psubsw") OP2(psubusb, "psubusb") OP2(psubusw, "psubusw")
OP2(pmullw, "pmullw") OP2(pmulhw, "pmulhw") OP2(pmaddwd, "pmaddwd") OP2(pand, "pand") OP2(pandn, "pandn") OP2(por, "por") OP2(pxor, "pxor")
OP2(pcmpeqb, "pcmpeqb") OP2(pcmpeqw, "pcmpeqw") OP2(pcmpeqd, "pcmpeqd") OP2(pcmpgtb, "pcmpgtb") OP2(pcmpgtw, "pcmpgtw") OP2(pcmpgtd, "pcmpgtd")
OP2(packsswb, "packsswb") OP2(packssdw, "packssdw") OP2(packuswb, "packuswb")
OP2(punpcklbw, "punpcklbw") OP2(punpcklwd, "punpcklwd") OP2(punpckldq, "punpckldq") OP2(punpckhbw, "punpckhbw") OP2(punpckhwd, "punpckhwd") OP2(punpckhdq, "punpckhdq")
OP2(psllw_mm, "psllw") OP2(psrlq_mm, "psrlq") OP2(psrad_mm, "psrad")
OPM(paddw_mem, "paddw") OPM(psubusb_mem, "psubusb")
OPI(psllw3, "psllw", 3) OPI(pslld9, "pslld", 9) OPI(psllq20, "psllq", 20) OPI(psrlw3, "psrlw", 3) OPI(psrld9, "psrld", 9) OPI(psrlq20, "psrlq", 20)
OPI(psraw3, "psraw", 3) OPI(psrad9, "psrad", 9) OPI(psllw16, "psllw", 16) OPI(psraw16, "psraw", 16)
OP2(pavgb, "pavgb") OP2(pminub, "pminub") OP2(pmaxsw, "pmaxsw") OP2(psadbw, "psadbw") OP2(pmulhuw, "pmulhuw")
static u64 pshufw1b(u64 a) { u64 r; __asm__ volatile("movq %1, %%mm1\n\tpshufw $0x1b, %%mm1, %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "m"(a) : "mm0", "mm1"); return r; }
static u64 pinsrw2(u64 a, u32 v) { u64 r; __asm__ volatile("movq %1, %%mm0\n\tpinsrw $2, %2, %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "m"(a), "r"(v) : "mm0"); return r; }
static u32 pextrw3(u64 a) { u32 r; __asm__ volatile("movq %1, %%mm0\n\tpextrw $3, %%mm0, %0" : "=r"(r) : "m"(a) : "mm0"); return r; }
static u32 pmovmskb(u64 a) { u32 r; __asm__ volatile("movq %1, %%mm0\n\tpmovmskb %%mm0, %0" : "=r"(r) : "m"(a) : "mm0"); return r; }
static u32 movd_out(u64 a) { u32 r; __asm__ volatile("movq %1, %%mm0\n\tmovd %%mm0, %0" : "=r"(r) : "m"(a) : "mm0"); return r; }
static u64 movd_in(u32 v) { u64 r; __asm__ volatile("pcmpeqb %%mm0, %%mm0\n\tmovd %1, %%mm0\n\tmovq %%mm0, %0" : "=m"(r) : "r"(v) : "mm0"); return r; }
static void emms(void) { __asm__ volatile("emms"); }

void start(void) {
  u64 a = A, b = B, c = C, s = S;
  show("paddb", paddb(a, b)); show("paddw", paddw(a, b)); show("paddd", paddd(a, b));
  show("paddsb", paddsb(a, b)); show("paddsw", paddsw(a, b)); show("paddusb", paddusb(a, b)); show("paddusw", paddusw(a, b));
  show("psubb", psubb(a, b)); show("psubw", psubw(a, b)); show("psubd", psubd(a, b));
  show("psubsb", psubsb(a, b)); show("psubsw", psubsw(a, b)); show("psubusb", psubusb(a, b)); show("psubusw", psubusw(a, b));
  show("pmullw", pmullw(a, c)); show("pmulhw", pmulhw(a, c)); show("pmaddwd", pmaddwd(a, c));
  show("pand", pand(a, c)); show("pandn", pandn(a, c)); show("por", por(a, c)); show("pxor", pxor(a, c));
  show("pcmpeqb", pcmpeqb(a, a)); show("pcmpeqw", pcmpeqw(a, b)); show("pcmpeqd", pcmpeqd(c, c));
  show("pcmpgtb", pcmpgtb(a, b)); show("pcmpgtw", pcmpgtw(a, b)); show("pcmpgtd", pcmpgtd(a, b));
  show("packsswb", packsswb(a, c)); show("packssdw", packssdw(a, c)); show("packuswb", packuswb(a, c));
  show("punpcklbw", punpcklbw(a, c)); show("punpcklwd", punpcklwd(a, c)); show("punpckldq", punpckldq(a, c));
  show("punpckhbw", punpckhbw(a, c)); show("punpckhwd", punpckhwd(a, c)); show("punpckhdq", punpckhdq(a, c));
  show("psllw 3", psllw3(a)); show("pslld 9", pslld9(a)); show("psllq 20", psllq20(a));
  show("psrlw 3", psrlw3(a)); show("psrld 9", psrld9(a)); show("psrlq 20", psrlq20(a));
  show("psraw 3", psraw3(a)); show("psrad 9", psrad9(a)); show("psllw 16", psllw16(c)); show("psraw 16", psraw16(a));
  show("psllw mm", psllw_mm(c, s)); show("psrlq mm", psrlq_mm(c, s)); show("psrad mm", psrad_mm(a, s));
  show("paddw mem", paddw_mem(a, c)); show("psubusb mem", psubusb_mem(a, b));
  show("movd out", movd_out(c)); show("movd in", movd_in(0x89abcdefu));
  show("pshufw", pshufw1b(c)); show("pinsrw", pinsrw2(c, 0xbeef)); show("pextrw", pextrw3(c)); show("pmovmskb", pmovmskb(a));
  show("pavgb", pavgb(a, b)); show("pminub", pminub(a, b)); show("pmaxsw", pmaxsw(a, b)); show("psadbw", psadbw(a, b)); show("pmulhuw", pmulhuw(a, c));
  emms();
  // the x87 after EMMS: its registers are its own again
  volatile double x = 1.5, y = 2.25; double z = x * y + 0.125;
  u64 zb; __builtin_memcpy(&zb, &z, 8); show("x87 after emms", zb);
  ExitProcess(0);
}
