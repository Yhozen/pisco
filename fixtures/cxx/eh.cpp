// SPDX-License-Identifier: GPL-2.0-or-later
// C++ exceptions as a title uses them: our own test code, built with llvm-mingw (libc++, libunwind, SEH).
#include <stdexcept>
#include <cstring>
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
extern "C" __declspec(dllimport) void *__stdcall CreateThread(void *, size_t, unsigned long(__stdcall *)(void *), void *, unsigned long, unsigned long *);
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
static void out(const char *s) { unsigned long w; WriteFile(GetStdHandle((unsigned long)-11), s, (unsigned long)strlen(s), &w, nullptr); }
static int fails;
static void check(const char *name, bool ok) { out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n"); fails += !ok; }

static int dtors;
struct Guard { int id; ~Guard() { dtors += id; } };
struct MyError { int code; };
__attribute__((noinline)) static void deep(int n) { Guard g{1}; if (n == 0) throw MyError{42}; deep(n - 1); }
__attribute__((noinline)) static int through_pointer(void (*f)(int), int n) { Guard g{100}; f(n); return 0; }
__attribute__((noinline)) static void std_throw() { throw std::runtime_error("from libc++"); }
__attribute__((noinline)) static int rethrow() { try { throw 7; } catch (int &e) { e += 1; throw; } }

static unsigned long __stdcall worker(void *p) {
  int *r = (int *)p;
  try { deep(3); } catch (const MyError &e) { *r = e.code; }
  return 0;
}

int main() {
  int caught = 0;
  try { deep(5); } catch (MyError &e) { caught = e.code; }
  check("throw through six frames, caught by type", caught == 42);
  check("destructors ran in every frame", dtors == 6);
  dtors = 0;
  try { through_pointer(deep, 2); } catch (...) { caught = -1; }
  check("catch (...) through a function pointer, destructors 3 + 100", caught == -1 && dtors == 103);
  const char *what = "";
  try { std_throw(); } catch (const std::exception &e) { what = e.what(); }
  check("std::runtime_error caught as std::exception", strcmp(what, "from libc++") == 0);
  int r = 0;
  try { rethrow(); } catch (int e) { r = e; }
  check("rethrow keeps the modified exception", r == 8);
  int nested = 0;
  try { try { throw 1; } catch (int) { throw 2.5; } } catch (double d) { nested = d == 2.5; }
  check("throw from a catch block", nested == 1);
  int in_thread = 0;
  void *h = CreateThread(nullptr, 0, worker, &in_thread, 0, nullptr);
  WaitForSingleObject(h, 0xFFFFFFFF);
  check("an exception inside a thread", in_thread == 42);
  return fails;
}
