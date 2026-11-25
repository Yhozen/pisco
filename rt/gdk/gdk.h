// SPDX-License-Identifier: GPL-2.0-or-later
// What the GDK's libraries share (rt/gdk/, rt/xbox-series/, rt/windows-x64/): HRESULTs and small helpers.
#pragma once
#include <sched.h>
#include <time.h>
#include "rt.h"
#include "vfs.h"

#define S_OK 0
#define E_NOTIMPL 0x80004001u
#define E_FAIL 0x80004005u
#define E_PENDING 0x8000000Au
#define E_ABORT 0x80004004u
#define E_INVALIDARG 0x80070057u
#define E_GAMEUSER_NO_NETWORK 0x80072EFDu
u64 rt_spawn_native(void (*fn)(Cpu *, void *), void *arg); // process.c: a guest-capable thread running native code
static inline u64 now_us(void) { return (u64)host_now_us(); }
