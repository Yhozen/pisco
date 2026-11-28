// SPDX-License-Identifier: GPL-2.0-or-later
// The app: imports gamelib.dll statically and finds it again by name.
#include <stdexcept>
#include <cstring>
extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long);
extern "C" __declspec(dllimport) int __stdcall WriteFile(void *, const void *, unsigned long, unsigned long *, void *);
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryA(const char *);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *, const char *);
extern "C" __declspec(dllimport) void *__stdcall GetModuleHandleA(const char *);
extern "C" __declspec(dllimport) void *__stdcall CreateThread(void *, size_t, unsigned long(__stdcall *)(void *), void *, unsigned long, unsigned long *);
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *, unsigned long);
extern "C" __declspec(dllimport) int lib_add(int, int);
extern "C" __declspec(dllimport) int lib_attach_count();
extern "C" __declspec(dllimport) int lib_tls_bump();
extern "C" __declspec(dllimport) void lib_throw(int);
static void out(const char *s) { unsigned long w; WriteFile(GetStdHandle((unsigned long)-11), s, (unsigned long)strlen(s), &w, nullptr); }
static int fails;
static void check(const char *name, bool ok) { out(ok ? "[PASS] " : "[FAIL] "); out(name); out("\n"); fails += !ok; }
static int thread_tls;
static unsigned long __stdcall worker(void *) { thread_tls = lib_tls_bump() * 1000 + lib_tls_bump(); return 0; }
int main() {
  check("a static import from the game's DLL (DllMain ran first)", lib_add(2, 3) == 6);
  void *h = LoadLibraryA("gamelib.dll");
  check("LoadLibrary returns the module that is already loaded", h && h == GetModuleHandleA("gamelib"));
  auto add = (int (*)(int, int))GetProcAddress(h, "lib_add");
  check("GetProcAddress finds the export", add && add(40, 1) == 42);
  check("thread_local in the DLL, main thread", lib_tls_bump() == 101 && lib_tls_bump() == 102);
  void *t = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
  WaitForSingleObject(t, 0xFFFFFFFF);
  check("thread_local in the DLL, new thread starts from the template", thread_tls == 101102);
  check("DllMain got DLL_THREAD_ATTACH", lib_attach_count() == 11);
  const char *what = "";
  try { lib_throw(7); } catch (const std::exception &e) { what = e.what(); }
  check("an exception thrown in the DLL, caught in the exe", strcmp(what, "seven") == 0);
  return fails;
}
