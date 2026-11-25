// SPDX-License-Identifier: GPL-2.0-or-later
// A DLL "shipped with the game": exports, DllMain, thread_local, and an exception that leaves it.
#include <stdexcept>
static int attached, thread_attached;
thread_local int tl_counter = 100;
extern "C" __declspec(dllexport) int lib_add(int a, int b) { return a + b + attached; }
extern "C" __declspec(dllexport) int lib_attach_count() { return attached * 10 + (thread_attached > 0); }
extern "C" __declspec(dllexport) int lib_tls_bump() { return ++tl_counter; }
extern "C" __declspec(dllexport) void lib_throw(int v) { throw std::runtime_error(v == 7 ? "seven" : "other"); }
extern "C" int __stdcall DllMain(void *, unsigned long reason, void *) {
  if (reason == 1) attached++;
  if (reason == 2) thread_attached++;
  return 1;
}
