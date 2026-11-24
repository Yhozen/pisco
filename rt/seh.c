// Windows x64 structured exception handling over the image's own unwind tables (.pdata / .xdata):
// RtlLookupFunctionEntry, RtlVirtualUnwind, RtlCaptureContext, RaiseException's dispatch, RtlUnwindEx,
// RtlRestoreContext, and msvcrt's __C_specific_handler. Language handlers (GCC's and LLVM's
// personality, MSVC's C++ handlers) are guest code or imports; they call back into these.
//
// Resuming at a context: a lifted function that holds landing pads registers a landing (setjmp) with its
// entry stack pointer; rt_restore_context finds the frame's landing, loads the registers and longjmps there.
// The wasm frames in between are dropped by the unwind, as the guest frames are by the new RSP.
#include <setjmp.h>
#include "rt.h"

// CONTEXT (x64) offsets
#define CX_FLAGS 0x30
#define CX_MXCSR 0x34
#define CX_EFLAGS 0x44
#define CX_RAX 0x78
#define CX_RSP 0x98
#define CX_RIP 0xF8
#define CX_FLTSAVE 0x100
#define CX_XMM0 0x1A0
#define CX_SIZE 0x4D0
#define CX_GPR(n) (CX_RAX + 8 * (n)) // rax rcx rdx rbx rsp rbp rsi rdi r8..r15, in register-number order
// EXCEPTION_RECORD
#define ER_CODE 0x00
#define ER_FLAGS 0x04
#define ER_RECORD 0x08
#define ER_ADDRESS 0x10
#define ER_NPARAMS 0x18
#define ER_INFO 0x20
#define ER_SIZE 0x98
// DISPATCHER_CONTEXT
#define DC_CONTROL_PC 0x00
#define DC_IMAGE_BASE 0x08
#define DC_FUNCTION_ENTRY 0x10
#define DC_ESTABLISHER 0x18
#define DC_TARGET_IP 0x20
#define DC_CONTEXT 0x28
#define DC_HANDLER 0x30
#define DC_HANDLER_DATA 0x38
#define DC_HISTORY 0x40
#define DC_SCOPE_INDEX 0x48
#define DC_SIZE 0x50

#define EH_NONCONTINUABLE 1
#define EH_UNWINDING 2
#define EH_EXIT_UNWIND 4
#define EH_TARGET_UNWIND 0x20
#define UNW_FLAG_EHANDLER 1
#define UNW_FLAG_UHANDLER 2
#define UNW_FLAG_CHAININFO 4


// ---- function table: the .pdata of the module holding the address ----
static u64 base_of(u64 pc) { const ModuleDef *m = rt_module_of(pc); return m ? m->base : 0; }
static u64 lookup(u64 pc) {
  const ModuleDef *m = rt_module_of(pc);
  if (!m || !m->pdata) return 0;
  u32 rva = (u32)(pc - m->base), lo = 0, hi = m->pdata_size / 12;
  while (lo < hi) {
    u32 k = (lo + hi) / 2; u64 e = m->pdata + 12ull * k;
    if (rva < M32(e)) hi = k; else if (rva >= M32(e + 4)) lo = k + 1; else return e;
  }
  return 0;
}
IMP(kernel32, RtlLookupFunctionEntry) {
  u64 e = lookup(ARG(0));
  if (e && ARG(1)) M64(ARG(1)) = base_of(ARG(0));
  RET(e);
}

// ---- virtual unwind: the caller's context from a frame's ----
static u64 *gpr(u64 ctx, int n) { return (u64 *)GP(ctx + CX_GPR(n)); }
// Returns the language handler (0 for none) for `type` (EHANDLER / UHANDLER), as RtlVirtualUnwind does.
static u64 virtual_unwind(u32 type, u64 pc, u64 fe, u64 ctx, u64 *handler_data, u64 *establisher) {
  u64 ib = base_of(fe);
  u64 info = ib + M32(fe + 8);
  u32 offset = (u32)(pc - ib - M32(fe));
  u8 flags = M8(info) >> 3, prolog = M8(info + 1), count = M8(info + 2), frame = M8(info + 3);
  u8 frame_reg = frame & 15; u32 frame_off = (frame >> 4) * 16;
  // the establisher frame: RSP after the prolog, or the frame register less its offset
  if (frame_reg && offset >= prolog) *establisher = *gpr(ctx, frame_reg) - frame_off;
  else *establisher = *gpr(ctx, 4);
  int machframe = 0;
  for (u32 i = 0; i < count;) {
    u64 code = info + 4 + 2ull * i;
    u8 at = M8(code), op = M8(code + 1) & 15, oi = M8(code + 1) >> 4;
    u32 slots;
    switch (op) { case 1: slots = oi ? 3 : 2; break; case 4: case 8: slots = 2; break; case 5: case 9: slots = 3; break; default: slots = 1; }
    if (offset < prolog && at > offset) { i += slots; continue; } // not executed yet inside the prolog
    u64 *rsp = gpr(ctx, 4);
    switch (op) {
    case 0: *gpr(ctx, oi) = M64(*rsp); *rsp += 8; break;                       // UWOP_PUSH_NONVOL
    case 1: *rsp += oi ? M32(code + 2) : 8u * M16(code + 2); break;            // UWOP_ALLOC_LARGE
    case 2: *rsp += 8u * oi + 8; break;                                         // UWOP_ALLOC_SMALL
    case 3: *rsp = *gpr(ctx, frame_reg) - frame_off; break;                     // UWOP_SET_FPREG
    case 4: *gpr(ctx, oi) = M64(*establisher + 8u * M16(code + 2)); break;      // UWOP_SAVE_NONVOL (from the frame base)
    case 5: *gpr(ctx, oi) = M64(*establisher + M32(code + 2)); break;           // UWOP_SAVE_NONVOL_FAR
    case 8: memcpy((void *)GP(ctx + CX_XMM0 + 16 * oi), (void *)GP(*establisher + 16u * M16(code + 2)), 16); break;
    case 9: memcpy((void *)GP(ctx + CX_XMM0 + 16 * oi), (void *)GP(*establisher + M32(code + 2)), 16); break;
    case 10: // UWOP_PUSH_MACHFRAME
      if (oi) *rsp += 8;
      M64(ctx + CX_RIP) = M64(*rsp); *rsp = M64(*rsp + 24); machframe = 1; break;
    default: break; // 6 (epilog, version 2) carries no state
    }
    i += slots;
  }
  if (flags & UNW_FLAG_CHAININFO) {
    u64 chained = info + 4 + 2ull * ((count + 1) & ~1u);
    u64 dummy;
    return virtual_unwind(type, ib + M32(chained), chained, ctx, handler_data, &dummy) , 0;
  }
  if (!machframe) { u64 *rsp = gpr(ctx, 4); M64(ctx + CX_RIP) = M64(*rsp); *rsp += 8; }
  if ((flags & type) && !(flags & UNW_FLAG_CHAININFO)) {
    u64 h = info + 4 + 2ull * ((count + 1) & ~1u);
    if (handler_data) *handler_data = h + 4;
    return ib + M32(h);
  }
  return 0;
}
// a frame with no function entry is a leaf: its return address is at RSP
static void leaf_unwind(u64 ctx) { u64 *rsp = gpr(ctx, 4); M64(ctx + CX_RIP) = M64(*rsp); *rsp += 8; }

IMP(kernel32, RtlVirtualUnwind) {
  // (HandlerType, ImageBase, ControlPc, FunctionEntry, ContextRecord, HandlerData*, EstablisherFrame*, ContextPointers)
  u64 hd = 0, est = 0;
  u64 h = virtual_unwind((u32)ARG(0), ARG(2), ARG(3), ARG(4), &hd, &est);
  if (ARG(5)) M64(ARG(5)) = hd;
  if (ARG(6)) M64(ARG(6)) = est;
  RET(h);
}

// ---- contexts ----
// The context of a guest call into the runtime: the caller's registers, RIP the return address.
static void capture(Cpu *c, u64 ctx) {
  memset((void *)GP(ctx), 0, CX_SIZE);
  M32(ctx + CX_FLAGS) = 0x10001F; M32(ctx + CX_MXCSR) = c->mxcsr; M32(ctx + CX_EFLAGS) = 0x202;
  for (int n = 0; n < 16; n++) *gpr(ctx, n) = R[n];
  *gpr(ctx, 4) = R[RSP] + 8;
  M64(ctx + CX_RIP) = M64(R[RSP]);
  memcpy((void *)GP(ctx + CX_XMM0), X, 256);
  M32(ctx + CX_FLTSAVE + 24) = c->mxcsr;
}
IMP(kernel32, RtlCaptureContext) { capture(c, ARG(0)); }

__attribute__((noreturn)) static void restore(Cpu *c, u64 ctx);

// ---- dispatch ----
// A handler call is a frame the unwinder must see through, as Windows' dispatcher frames: its return address
// is a sentinel, and walking past it continues from `resume` (the exception's context while dispatching, the
// frame being unwound for a collided unwind).
#define SENTINEL 0x7FFE0000ull
typedef struct { u64 ctx; } HandlerFrame;
static __thread HandlerFrame hframes[64];
static __thread int hframe_n;
static u64 call_handler(Cpu *c, u64 handler, u64 rec, u64 est, u64 ctx, u64 dc, u64 resume) {
  if (hframe_n >= 64) rt_fatal("exception handlers nested too deep");
  int k = hframe_n++;
  hframes[k].ctx = (uptr)aligned_alloc(16, CX_SIZE); memcpy((void *)GP(hframes[k].ctx), (void *)GP(resume), CX_SIZE);
  u64 sp = R[RSP];
  R[RSP] = (R[RSP] - 32) & ~15ull;
  R[RCX] = rec; R[RDX] = est; R[8] = ctx; R[9] = dc;
  PUSH64(SENTINEL + (u64)k);
  rt_call(c, handler);
  R[RSP] = sp;
  hframe_n = k;
  free((void *)GP(hframes[k].ctx));
  return (u32)R[RAX];
}
// after a frame is unwound: a sentinel return address leads back to the context behind the handler call
static void through_handlers(u64 ctx) {
  u64 pc = M64(ctx + CX_RIP);
  if (pc >= SENTINEL && pc < SENTINEL + 64 && (int)(pc - SENTINEL) < hframe_n) memcpy((void *)GP(ctx), (void *)GP(hframes[pc - SENTINEL].ctx), CX_SIZE);
}

// ---- vectored handlers: called before the frames' handlers, the newest first unless added last ----
static u64 vectored[32]; static int vectored_n;
static pthread_mutex_t vectored_lock = PTHREAD_MUTEX_INITIALIZER;
IMP(kernel32, AddVectoredExceptionHandler) {
  pthread_mutex_lock(&vectored_lock);
  if (vectored_n < 32) { if (ARG(0)) { memmove(vectored + 1, vectored, sizeof(u64) * (size_t)vectored_n); vectored[0] = ARG(1); } else vectored[vectored_n] = ARG(1); vectored_n++; }
  pthread_mutex_unlock(&vectored_lock);
  RET(ARG(1)); // the handle is the handler
}
IMP(kernel32, RemoveVectoredExceptionHandler) {
  pthread_mutex_lock(&vectored_lock);
  for (int i = 0; i < vectored_n; i++) if (vectored[i] == ARG(0)) { memmove(vectored + i, vectored + i + 1, sizeof(u64) * (size_t)(vectored_n - i - 1)); vectored_n--; break; }
  pthread_mutex_unlock(&vectored_lock);
  RET(1);
}
IMP(kernel32, AddVectoredContinueHandler) { RET(ARG(1)); }
IMP(kernel32, RemoveVectoredContinueHandler) { RET(1); }

// Returns when a handler answers ExceptionContinueExecution: the raise returns to its caller.
static void dispatch(Cpu *c, u64 rec, u64 orig) {
  if (vectored_n) {
    u64 ptrs = (uptr)calloc(1, 16); M64(ptrs) = rec; M64(ptrs + 8) = orig;
    for (int i = 0; i < vectored_n; i++) if ((s32)rt_guest_call(c, vectored[i], ptrs, 0, 0, 0) == -1) { free((void *)GP(ptrs)); return; } // EXCEPTION_CONTINUE_EXECUTION
    free((void *)GP(ptrs));
  }
  u64 ctx = (uptr)aligned_alloc(16, CX_SIZE), dc = (uptr)calloc(1, DC_SIZE);
  memcpy((void *)GP(ctx), (void *)GP(orig), CX_SIZE);
  for (int depth = 0; depth < 100000; depth++) {
    u64 pc = M64(ctx + CX_RIP);
    if (!pc) break;
    u64 fe = lookup(pc);
    if (!fe) { leaf_unwind(ctx); through_handlers(ctx); continue; }
    u64 frame = (uptr)aligned_alloc(16, CX_SIZE); memcpy((void *)GP(frame), (void *)GP(ctx), CX_SIZE);
    u64 hd = 0, est = 0;
    u64 h = virtual_unwind(UNW_FLAG_EHANDLER, pc, fe, ctx, &hd, &est);
    if (rt_trace > 1) rt_log("  dispatch: pc 0x%llx fn 0x%llx establisher 0x%llx handler 0x%llx -> caller rsp 0x%llx pc 0x%llx", pc, base_of(pc) + M32(fe), est, h, *gpr(ctx, 4), M64(ctx + CX_RIP));
    if (h) {
      M64(dc + DC_CONTROL_PC) = pc; M64(dc + DC_IMAGE_BASE) = base_of(pc); M64(dc + DC_FUNCTION_ENTRY) = fe;
      M64(dc + DC_ESTABLISHER) = est; M64(dc + DC_CONTEXT) = frame; M64(dc + DC_HANDLER) = h; M64(dc + DC_HANDLER_DATA) = hd;
      M32(dc + DC_SCOPE_INDEX) = 0;
      u64 r = call_handler(c, h, rec, est, orig, dc, orig);
      if (r == 0) { free((void *)GP(frame)); return; }                     // ExceptionContinueExecution
      if (r != 1 && rt_trace) rt_log("exception handler returned %llu", r); // 1: ExceptionContinueSearch
    }
    free((void *)GP(frame));
    through_handlers(ctx);
    if (*gpr(ctx, 4) >= THREAD(c)->stack_hi) break;
  }
  rt_fatal("unhandled exception 0x%08x at 0x%llx (thread %u)", M32(rec + ER_CODE), M64(rec + ER_ADDRESS), THREAD(c)->id);
}

IMP(kernel32, RaiseException) {
  u64 rec = (uptr)calloc(1, ER_SIZE), ctx = (uptr)aligned_alloc(16, CX_SIZE);
  M32(rec + ER_CODE) = (u32)ARG(0); M32(rec + ER_FLAGS) = (u32)ARG(1) & EH_NONCONTINUABLE;
  u32 n = (u32)ARG(2); if (n > 15) n = 15;
  M32(rec + ER_NPARAMS) = n;
  for (u32 i = 0; i < n; i++) M64(rec + ER_INFO + 8 * i) = M64(ARG(3) + 8 * i);
  capture(c, ctx);
  M64(rec + ER_ADDRESS) = M64(ctx + CX_RIP);
  if (rt_trace) rt_log("RaiseException 0x%08x from 0x%llx", (u32)ARG(0), M64(ctx + CX_RIP));
  dispatch(c, rec, ctx);
}
IMP(kernel32, RtlRaiseException) {
  u64 rec = ARG(0), ctx = (uptr)aligned_alloc(16, CX_SIZE);
  capture(c, ctx);
  if (!M64(rec + ER_ADDRESS)) M64(rec + ER_ADDRESS) = M64(ctx + CX_RIP);
  dispatch(c, rec, ctx);
}

// RtlUnwindEx(TargetFrame, TargetIp, ExceptionRecord, ReturnValue, ContextRecord, HistoryTable).
// The walk starts at the exception's context (the frames between it and the handler that called us are the
// dispatcher's), calls each frame's unwind handler and stops at the target frame.
// The walk starts at the caller and goes through handler frames as Windows' does through its dispatcher's.
static void unwind_to(Cpu *c, u64 target, u64 target_ip, u64 rec, u64 retval, u64 orig) {
  u64 ctx = (uptr)aligned_alloc(16, CX_SIZE), dc = (uptr)calloc(1, DC_SIZE);
  (void)orig;
  capture(c, ctx);
  if (!rec) { rec = (uptr)calloc(1, ER_SIZE); M32(rec + ER_CODE) = 0xC0000027; M64(rec + ER_ADDRESS) = M64(ctx + CX_RIP); } // STATUS_UNWIND
  M32(rec + ER_FLAGS) |= EH_UNWINDING | (target ? 0 : EH_EXIT_UNWIND);
  for (int depth = 0; depth < 100000; depth++) {
    u64 pc = M64(ctx + CX_RIP);
    if (!pc) rt_fatal("RtlUnwindEx: the target frame 0x%llx is not on the stack", target);
    u64 fe = lookup(pc);
    if (!fe) { leaf_unwind(ctx); through_handlers(ctx); continue; }
    u64 frame = (uptr)aligned_alloc(16, CX_SIZE); memcpy((void *)GP(frame), (void *)GP(ctx), CX_SIZE);
    u64 hd = 0, est = 0;
    u64 h = virtual_unwind(UNW_FLAG_UHANDLER, pc, fe, ctx, &hd, &est);
    if (rt_trace > 1) rt_log("  unwind (target 0x%llx): pc 0x%llx fn 0x%llx establisher 0x%llx handler 0x%llx", target, pc, base_of(pc) + M32(fe), est, h);
    if (est == target) M32(rec + ER_FLAGS) |= EH_TARGET_UNWIND;
    if (h) {
      M64(dc + DC_CONTROL_PC) = pc; M64(dc + DC_IMAGE_BASE) = base_of(pc); M64(dc + DC_FUNCTION_ENTRY) = fe;
      M64(dc + DC_ESTABLISHER) = est; M64(dc + DC_CONTEXT) = frame; M64(dc + DC_HANDLER) = h; M64(dc + DC_HANDLER_DATA) = hd;
      M64(dc + DC_TARGET_IP) = target_ip;
      call_handler(c, h, rec, est, frame, dc, frame);
    }
    if (est == target) {
      // stop at the target frame: its own context, as it was before unwinding it
      M64(frame + CX_RAX) = retval; M64(frame + CX_RIP) = target_ip;
      M32(rec + ER_FLAGS) &= ~(EH_UNWINDING | EH_TARGET_UNWIND);
      restore(c, frame);
    }
    free((void *)GP(frame));
    through_handlers(ctx);
  }
  rt_fatal("RtlUnwindEx: no end");
}
IMP(kernel32, RtlUnwindEx) { unwind_to(c, ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); }
IMP(kernel32, RtlUnwind) { unwind_to(c, ARG(0), ARG(1), ARG(2), ARG(3), 0); }
IMP(kernel32, RtlRestoreContext) { restore(c, ARG(0)); }

// ---- resuming ----
// The entry RSP of the function the context is in: unwind it once; the caller's RSP is entry RSP + 8.
static u64 entry_rsp(u64 ctx) {
  u64 tmp = (uptr)aligned_alloc(16, CX_SIZE); memcpy((void *)GP(tmp), (void *)GP(ctx), CX_SIZE);
  u64 pc = M64(tmp + CX_RIP), fe = lookup(pc), hd, est;
  if (fe) virtual_unwind(0, pc, fe, tmp, &hd, &est); else leaf_unwind(tmp);
  u64 e = *gpr(tmp, 4) - 8;
  free((void *)GP(tmp));
  return e;
}
static void restore(Cpu *c, u64 ctx) {
  Thread *t = THREAD(c);
  u64 want = entry_rsp(ctx), pc = M64(ctx + CX_RIP);
  Landing *l = c->landing;
  while (l && l->entry_rsp != want) l = l->prev;
  if (!l) rt_fatal("cannot resume at 0x%llx: its frame (entry RSP 0x%llx) holds no landing", pc, want);
  for (int n = 0; n < 16; n++) R[n] = *gpr(ctx, n);
  memcpy(X, (void *)GP(ctx + CX_XMM0), 256);
  c->mxcsr = M32(ctx + CX_MXCSR);
  c->landing = l;
  l->resume = pc;
  if (rt_trace) rt_log("resume at 0x%llx (entry RSP 0x%llx)", pc, want);
  longjmp(*(jmp_buf *)l->jb, 1);
}

// ---- __C_specific_handler: C's __try / __except / __finally (scope tables) ----
// (ExceptionRecord, EstablisherFrame, ContextRecord, DispatcherContext)
IMP(msvcrt, __C_specific_handler) {
  u64 rec = ARG(0), est = ARG(1), dc = ARG(3);
  u64 table = M64(dc + DC_HANDLER_DATA), ib = M64(dc + DC_IMAGE_BASE);
  u32 n = M32(table), rva = (u32)(M64(dc + DC_CONTROL_PC) - ib);
  u32 flags = M32(rec + ER_FLAGS);
  for (u32 i = M32(dc + DC_SCOPE_INDEX); i < n; i++) {
    u64 e = table + 4 + 16ull * i;
    u32 begin = M32(e), end = M32(e + 4), handler = M32(e + 8), jump = M32(e + 12);
    if (rva < begin || rva >= end) continue;
    if (!(flags & (EH_UNWINDING | EH_EXIT_UNWIND))) {
      if (!jump) continue; // a __finally: only on unwinding
      s32 r = 1; // EXCEPTION_EXECUTE_HANDLER
      if (handler != 1) {
        u64 ptrs = (uptr)calloc(1, 16); M64(ptrs) = rec; M64(ptrs + 8) = M64(dc + DC_CONTEXT);
        r = (s32)rt_guest_call(c, ib + handler, ptrs, est, 0, 0);
      }
      if (r < 0) { RET(0); return; } // EXCEPTION_CONTINUE_EXECUTION
      if (r > 0) { unwind_to(c, est, ib + jump, rec, M32(rec + ER_CODE), ARG(2)); }
    } else {
      if (jump) { if ((flags & EH_TARGET_UNWIND) && M64(dc + DC_TARGET_IP) == ib + jump) break; continue; }
      M32(dc + DC_SCOPE_INDEX) = i + 1;
      rt_guest_call(c, ib + handler, 1, est, 0, 0); // a __finally block, abnormal termination
    }
  }
  RET(1); // ExceptionContinueSearch
}
