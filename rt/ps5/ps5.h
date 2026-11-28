// SPDX-License-Identifier: GPL-2.0-or-later
// The PS5 runtime (base/ps5): what a native PS5 executable finds of its system. The app is built as ps5-payload-sdk
// and ps5-native-tool build homebrew; its calls into the system's libraries (libkernel, libSceLibcInternal,
// libSceVideoOut, libScePad, libSceAudioOut, ...) are answered here, in the System V calling convention: arguments in
// rdi, rsi, rdx, rcx, r8, r9, the result in rax. AnyPS5 (github.com/boykopovar/AnyPS5, core/libs/prx) is the
// reference for what these functions do; nothing of Sony's is used.
#pragma once
#include <errno.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>
#include "rt.h"

#define A0 R[RDI]
#define A1 R[RSI]
#define A2 R[RDX]
#define A3 R[RCX]
#define A4 R[8]
#define A5 R[9]
#define SA(n) M64(R[RSP] + 8 + 8 * (n))   // the seventh argument on: on the stack above the return address

// the PS5's error codes (SCE_KERNEL_ERROR_*): 0x8002xxxx, the low bits an errno
#define SCE_KERNEL_ERROR(e) (0x80020000u | (e))

// ---- threads (start.c) ----
typedef struct Ps5Thread {
  Cpu cpu;                 // first: a Cpu* is a Ps5Thread*
  u64 entry, arg, result, stack;
  pthread_t pt;
  char name[32];
} Ps5Thread;
u64 ps5_thread_create(u64 entry, u64 arg, const char *name);   // its handle: the Ps5Thread's address
int ps5_thread_join(u64 handle, u64 *result);
void ps5_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1);         // a guest function, from the runtime
__attribute__((noreturn)) void ps5_exit(Cpu *c, int status);

// ---- the screen (videoout.c) ----
void ps5_video_flush(void);   // what was submitted is shown before the process ends

// ---- the host's input block (rt/gdk/gameinput.c has its layout) ----
HOST("input_open") void host_input_open(u32 block);
