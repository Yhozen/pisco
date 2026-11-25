// SPDX-License-Identifier: GPL-2.0-or-later
// A 32-bit title that leans on msvcrt.dll and kernel32's interlocked lists the way the DLLs of old Windows games do:
// the C math functions (cdecl: doubles on the stack, the result on the x87 stack), sprintf with doubles and 64-bit
// integers among 32-bit arguments, strtod, and the SList functions on an 8-byte header. It prints one line per check;
// the expected output (expected.txt) is what it prints under Wine:
//   tools/oracle-wine/oracle.sh run fixtures out/crt32/crt32.exe > fixtures/crt32/expected.txt
// No C runtime of its own: the entry point is ours and every C function comes from msvcrt.dll (build.sh).
typedef unsigned int u32; typedef unsigned long long u64; typedef long long s64; typedef unsigned short u16;
__declspec(dllimport) void *__stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(void *, const void *, u32, u32 *, void *);
__declspec(dllimport) void __stdcall ExitProcess(u32);
typedef struct Entry { struct Entry *next; u32 value; } Entry;
typedef union { u64 align; struct { Entry *next; u16 depth, sequence; } s; } Header;
__declspec(dllimport) void __stdcall InitializeSListHead(Header *);
__declspec(dllimport) Entry *__stdcall InterlockedPushEntrySList(Header *, Entry *);
__declspec(dllimport) Entry *__stdcall InterlockedPopEntrySList(Header *);
__declspec(dllimport) Entry *__stdcall InterlockedFlushSList(Header *);
__declspec(dllimport) u16 __stdcall QueryDepthSList(Header *);
__declspec(dllimport) double sin(double); __declspec(dllimport) double cos(double); __declspec(dllimport) double tan(double);
__declspec(dllimport) double atan(double); __declspec(dllimport) double atan2(double, double); __declspec(dllimport) double pow(double, double);
__declspec(dllimport) double sqrt(double); __declspec(dllimport) double floor(double); __declspec(dllimport) double ceil(double);
__declspec(dllimport) double fmod(double, double); __declspec(dllimport) double ldexp(double, int); __declspec(dllimport) double frexp(double, int *);
__declspec(dllimport) double modf(double, double *); __declspec(dllimport) double exp(double); __declspec(dllimport) double log(double);
__declspec(dllimport) double log10(double); __declspec(dllimport) double fabs(double); __declspec(dllimport) double asin(double);
__declspec(dllimport) double acos(double); __declspec(dllimport) double strtod(const char *, char **);
__declspec(dllimport) int sprintf(char *, const char *, ...);
__declspec(dllimport) int _snprintf(char *, u32, const char *, ...);

static void out(const char *s) { u32 n = 0, w; while (s[n]) n++; WriteFile(GetStdHandle((u32)-11), s, n, &w, 0); }
static char buf[512];
static void d1(const char *name, double v) { sprintf(buf, "%s %.15g\n", name, v); out(buf); }
static volatile double one = 1.0, two = 2.0, half = 0.5; // not folded by the compiler

void start(void) {
  d1("sin(1)", sin(one)); d1("cos(1)", cos(one)); d1("tan(1)", tan(one)); d1("atan(2)", atan(two));
  d1("asin(0.5)", asin(half)); d1("acos(0.5)", acos(half)); d1("atan2(1,2)", atan2(one, two)); d1("pow(2,0.5)", pow(two, half));
  d1("sqrt(2)", sqrt(two)); d1("floor(-1.5)", floor(-one - half)); d1("ceil(-1.5)", ceil(-one - half)); d1("fmod(7.5,2)", fmod(7.5 * one, two));
  d1("ldexp(0.75,4)", ldexp(0.75 * one, 4)); d1("exp(1)", exp(one)); d1("log(2)", log(two)); d1("log10(2)", log10(two)); d1("fabs(-2)", fabs(-two));
  { int e = 0; double m = frexp(48 * one, &e); sprintf(buf, "frexp(48) %.15g %d\n", m, e); out(buf); }
  { double ip = 0; double f = modf(3.25 * one, &ip); sprintf(buf, "modf(3.25) %.15g %.15g\n", f, ip); out(buf); }
  { char *end = 0; const char *s = "3.5e2 rest"; double v = strtod(s, &end); sprintf(buf, "strtod %.15g +%d\n", v, (int)(end - s)); out(buf); }
  // the stack holds int, double, long long, int: each read at its own width
  sprintf(buf, "printf %d|%5.2f|%lld|%u|%e|%I64x|%g|%x|%s|%c|%-6d|%+.3f|%%\n", -7, 3.14159 * one, -1234567890123ll, 4000000000u,
          12345.678 * one, 0x123456789abcdefull, 0.0001 * one, 0xbeef, "str", 'Z', 42, two);
  out(buf);
  { int n = _snprintf(buf, 8, "%s", "too long for it"); sprintf(buf, "_snprintf %d\n", n); out(buf); }
  // SList: three pushes, the depth, a pop, a flush
  static Header h; static Entry e[3];
  InitializeSListHead(&h);
  for (u32 i = 0; i < 3; i++) { e[i].value = 10 + i; InterlockedPushEntrySList(&h, &e[i]); }
  sprintf(buf, "slist depth %u\n", (u32)QueryDepthSList(&h)); out(buf);
  Entry *p = InterlockedPopEntrySList(&h); sprintf(buf, "slist pop %u depth %u\n", p ? p->value : 0, (u32)QueryDepthSList(&h)); out(buf);
  Entry *l = InterlockedFlushSList(&h); u32 n = 0, sum = 0; for (; l; l = l->next) { n++; sum += l->value; }
  sprintf(buf, "slist flush %u entries sum %u depth %u empty %d\n", n, sum, (u32)QueryDepthSList(&h), InterlockedPopEntrySList(&h) == 0); out(buf);
  ExitProcess(0);
}
