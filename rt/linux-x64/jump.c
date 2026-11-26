// SPDX-License-Identifier: GPL-2.0-or-later
// longjmp. A lifted function returns by returning: the guest's return addresses are not jumped to, so the C
// library's own longjmp (restore the stack pointer, jump to the saved address) cannot work. The lifter puts this in
// its place: the registers setjmp saved are loaded, the runtime's stack is unwound, and the guest goes on at the
// address setjmp would have returned to (linux_resume_at, start.c). setjmp itself is the title's: it fills the
// jmp_buf as musl and glibc lay it out for x86-64 (rbx, rbp, r12 to r15, the stack pointer after the return, the
// return address).
#include "linux.h"

void rt_longjmp(Cpu *c) {
  u64 b = R[RDI], sp = M64(b + 48), pc = M64(b + 56);
  R[RAX] = (u32)R[RSI] ? (u32)R[RSI] : 1;
  R[RBX] = M64(b); R[RBP] = M64(b + 8); R[12] = M64(b + 16); R[13] = M64(b + 24); R[14] = M64(b + 32); R[15] = M64(b + 40); R[RSP] = sp;
  if (rt_trace) rt_log("longjmp to 0x%llx", pc);
  linux_resume_at(c, pc);
}
