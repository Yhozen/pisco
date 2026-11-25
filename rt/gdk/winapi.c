// SPDX-License-Identifier: GPL-2.0-or-later
// Small Windows functions the 64-bit runtimes' titles ask for: thread descriptions, environment variables (none),
// toolhelp snapshots (refused), advapi32's LsaOpenPolicy (refused: the caller's fallback runs).
#include "rt.h"

IMP(kernel32, SetThreadDescription) { char *n = rt_narrow(GWSTR(ARG(1))); Object *o = ARG(0) == PSEUDO_THREAD ? THREAD(c)->handle : rt_handle_get(ARG(0)); if (o && o->type == OBJ_THREAD) { free(o->thread->name); o->thread->name = n; } else free(n); RET(0); }
IMP(advapi32, LsaOpenPolicy) { RET(0xC0000002u); } // STATUS_NOT_IMPLEMENTED: the caller's fallback
IMP(kernel32, SetThreadStackGuarantee) { if (ARG(0)) M32(ARG(0)) = 0; RET(1); }
IMP(kernel32, IsThreadAFiber) { RET(0); }
IMP(kernel32, GetEnvironmentVariableW) { SET_LAST_ERROR(203); RET(0); }
IMP(kernel32, GetEnvironmentVariableA) { SET_LAST_ERROR(203); RET(0); }
IMP(kernel32, CreateWaitableTimerExW) { SET_LAST_ERROR(50); RET(0); }
IMP(kernel32, CreateToolhelp32Snapshot) { SET_LAST_ERROR(50); RET(0xFFFFFFFFFFFFFFFFull); }
IMP(kernel32, Module32FirstW) { RET(0); }
IMP(kernel32, Module32NextW) { RET(0); }
