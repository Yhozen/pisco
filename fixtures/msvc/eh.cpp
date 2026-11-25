// SPDX-License-Identifier: GPL-2.0-or-later
// C++ exceptions with the MSVC ABI (clang, x86_64-pc-windows-msvc): __CxxFrameHandler3, _CxxThrowException.
// Our own test code; no Microsoft SDK or CRT: the entry point is ours, imports come from .def files.
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
extern "C" __declspec(dllimport) void __stdcall ExitProcess(unsigned);
extern "C" __declspec(dllimport) void *__stdcall CreateThread(void *, unsigned long long, unsigned long(__stdcall *)(void *), void *, unsigned long, unsigned long *);
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
static unsigned long len(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { unsigned long w; WriteFile(GetStdHandle((unsigned long)-11), s, len(s), &w, nullptr); }
static int fails;
static void check(const char *name, bool ok) { out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n"); fails += !ok; }

static int dtors, copies;
struct Guard { int id; ~Guard() { dtors += id; } };
struct Base { int code; virtual ~Base() {} virtual int kind() const { return 1; } };
struct Derived : Base { Derived(int c) { code = c; } Derived(const Derived &o) { code = o.code; copies++; } int kind() const override { return 2; } };
__attribute__((noinline)) void deep(int n) { Guard g{1}; if (n == 0) throw Derived(42); deep(n - 1); }
__attribute__((noinline)) int by_value() { try { deep(2); } catch (Derived d) { return d.code + d.kind() * 100; } return 0; }
__attribute__((noinline)) int nested() {
  try { try { throw 1; } catch (int) { Guard g{1000}; throw 2.5; } } catch (double d) { return d == 2.5 ? 1 : 0; }
  return 0;
}
__attribute__((noinline)) int rethrow() { try { try { throw 7; } catch (int &e) { e++; throw; } } catch (int e) { return e; } return 0; }
static unsigned long __stdcall worker(void *p) { try { deep(1); } catch (const Base &b) { *(int *)p = b.code; } return 0; }

extern "C" void start() {
  int code = 0, kind = 0;
  try { deep(4); } catch (const Base &b) { code = b.code; kind = b.kind(); }
  check("throw a derived class through five frames, caught as const Base &", code == 42 && kind == 2);
  check("destructors in every frame (__CxxFrameHandler3 unwind funclets)", dtors == 5);
  dtors = 0; copies = 0;
  check("catch by value copies the object with its copy constructor", by_value() == 242 && copies >= 1 && dtors == 3);
  dtors = 0;
  check("throw from a catch block: its locals destroyed", nested() == 1 && dtors == 1000);
  check("rethrow keeps the modified exception", rethrow() == 8);
  int in_thread = 0;
  void *h = CreateThread(nullptr, 0, worker, &in_thread, 0, nullptr);
  WaitForSingleObject(h, 0xFFFFFFFF);
  check("an exception inside a thread", in_thread == 42);
  ExitProcess(fails);
}
