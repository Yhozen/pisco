// SPDX-License-Identifier: GPL-2.0-or-later
// Runtime internals shared by the import modules. Include libc headers before this file:
// cpu.h defines the short register macros (R, X) lifted code uses.
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu.h"

#define HOST(name) __attribute__((import_module("host"), import_name(name)))
HOST("fault") __attribute__((noreturn)) void host_fault(u32 kind, u64 addr);
HOST("now_us") double host_now_us(void);
// The title's clock, in microseconds since 1970: the host's, or with SW_CLOCK=<microseconds a frame> a virtual one
// that starts at a fixed date and moves by that step when a frame is presented and by the time asked for on Sleep
// (clock.c). The Win32 time functions of a 32-bit title read it; rt_uptime_us is the same clock since "boot".
u64 rt_now_us(void);
u64 rt_uptime_us(void);
int rt_clock_virtual(void);
void rt_clock_advance(u64 us);       // nothing unless the clock is virtual
void rt_clock_advance_frame(void);   // a frame was presented
void rt_clock_polled(void);          // the title read the clock: a virtual one moves on after many reads in a row
HOST("read_file") u32 host_read_file(const char *name, u32 name_len, u64 offset, u64 len, u64 dst); // from the title's folder
HOST("log") void host_log(const char *text, u32 len);
HOST("gpu_open") void host_gpu_open(u32 ring);            // the GPU back end starts reading this ring (rt/d3d12_cmd.h)

// ---- the lifted image ----
extern const unsigned fn_count; extern const u64 fn_addrs[]; extern void (*const fn_ptrs[])(Cpu *);
#include "modules.h"
extern const ModuleDef rt_modules[]; extern const unsigned rt_module_count;
extern const u64 import_base, import_slots[], import_targets[]; extern const unsigned import_count;
extern const char *const import_names[];
#define image_base (rt_modules[0].base)                    // the title's exe
#define image_stack_reserve (rt_modules[0].stack_reserve)
const ModuleDef *rt_module_of(u64 addr);                   // the lifted image holding addr, or 0
const ModuleDef *rt_module_named(const char *name);       // by file name, case-insensitive, with or without ".dll"
u64 rt_module_export(const ModuleDef *m, const char *name); // from its export directory in guest memory
int rt_module_loaded(const ModuleDef *m);                  // an on-demand DLL counts once LoadLibrary has loaded it
u64 rt_module_load(Cpu *c, const ModuleDef *m);           // LoadLibrary of a lifted image: its base (an on-demand DLL: attached)
int rt_module_free(Cpu *c, const ModuleDef *m);           // FreeLibrary: an on-demand DLL's last reference detaches it

// ---- guest calls ----
// The runtime is built for one guest width (GUEST_BITS, from the title: cpu.h defaults it to 64).
#if GUEST_BITS == 32
// Arguments of a 32-bit call (stdcall or cdecl), as an import sees them: all on the stack, above the return
// address ESP points at. The import returns normally; rt_call then removes the return address and, for a
// stdcall function, its arguments (the sizes come from tools/stdcall-sizes.sh).
#define ARG(n) ((u64)M32(R[RSP] + 4 + 4 * (n)))
// Floating-point arguments and results (the C runtime's math functions): DA(n) is the nth argument when the ones
// before it are doubles (8 bytes each on the stack), FA(n) when they are floats (4 bytes); DA_INT / FA_INT an integer
// or pointer in that place. The result goes on the x87 stack (ST0).
#define DA(n) MF64(R[RSP] + 4 + 8 * (n))
#define FA(n) MF32(R[RSP] + 4 + 4 * (n))
#define DA_INT(n) ((u64)M32(R[RSP] + 4 + 8 * (n)))
#define FA_INT(n) ((u64)M32(R[RSP] + 4 + 4 * (n)))
#define DRET(v) fpush(c, (double)(v))
#define FRET(v) fpush(c, (double)(float)(v))
#define RET(v) (R[RAX] = (u32)(v))
#define RET64(v) do { u64 v_ = (u64)(v); R[RAX] = (u32)v_; R[RDX] = (u32)(v_ >> 32); } while (0)
#define MP(a) M32(a)   // a pointer-sized value in guest memory
#define PTR_SIZE 4
#else
// Arguments of a Win64 call, as an import sees them: RSP points at the return address.
#define ARG(n) ((n) == 0 ? R[RCX] : (n) == 1 ? R[RDX] : (n) == 2 ? R[8] : (n) == 3 ? R[9] : M64(R[RSP] + 8 + 8 * (n)))
// Win64: the nth floating-point argument is in xmm<n> and an integer one in its integer register, whatever came
// before; the result is in xmm0's low lane.
#define DA(n) (X[n].pd[0])
#define FA(n) (X[n].ps[0])
#define DA_INT(n) ARG(n)
#define FA_INT(n) ARG(n)
#define DRET(v) do { double v_ = (v); memset(&X[0], 0, 16); X[0].pd[0] = v_; } while (0)
#define FRET(v) do { float v_ = (v); memset(&X[0], 0, 16); X[0].ps[0] = v_; } while (0)
#define RET(v) (R[RAX] = (u64)(v))
#define RET64(v) RET(v)
#define MP(a) M64(a)
#define PTR_SIZE 8
#endif
#define GSTR(a) ((char *)GP(a))
typedef u16 wchar16;
#define GWSTR(a) ((wchar16 *)GP(a))

// Calls a guest function with up to four integer arguments and returns RAX (EAX for a 32-bit guest, where the
// arguments go on the stack and it does not matter whether the callee removes them).
u64 rt_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3);

// ---- logging ----
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int rt_trace;
extern void (*rt_exit_hook)(void);   // called once when the process ends, before the host is told (statistics)
void rt_exit_process(u32 code);       // core.c: the hook, then the host ends the process

// ---- memory ----
u64 rt_valloc(u64 size, u64 align); // zeroed guest memory, never freed (VirtualAlloc, stacks, TEBs)
void rt_vfree(u64 addr);
// the console's GPU-visible region (gdk.c): zeroed blocks whose addresses carry a tag above 4 GiB (window 0..3)
u64 rt_gfx_alloc(u64 size, u64 align, u32 window);
int rt_gfx_free(u64 addr);   // 1 when addr starts a block (or an address-space-only reservation)
u8 *rt_phys_page(u64 num);   // XMem physical page `num`: where its bytes are now
// a vtable of `n` methods named "<prefix><names[i]>" in `dll`'s imports (named stubs for the missing ones)
u64 rt_com_vtable(const char *dll, const char *prefix, const char *const *names, unsigned n);

// ---- threads ----
typedef struct Thread {
  Cpu cpu;                 // first: a Cpu* is a Thread*
  u64 teb, stack_lo, stack_hi;
  u32 id;
  struct Object *handle;   // the thread's waitable object
  u64 start, param;
  void *exit_jmp;          // jmp_buf* while the guest runs
  u32 exit_code;
  char *name;              // SetThreadName / SetThreadDescription
  void (*native)(Cpu *, void *); void *native_arg; // a runtime thread that calls guest code (task queue workers)
} Thread;
#define THREAD(c) ((Thread *)(c))
extern Thread *rt_main_thread;
Thread *rt_thread_new(u64 stack_size);
void rt_thread_attach(Thread *t, int reason); // runs the image's TLS callbacks
__attribute__((noreturn)) void rt_thread_exit(Cpu *c, u32 code);

// TEB fields
#if GUEST_BITS == 32
#define TEB_SEH 0x00   // the head of the exception handler chain; -1: empty
#define TEB_STACK_BASE 0x04
#define TEB_STACK_LIMIT 0x08
#define TEB_SELF 0x18
#define TEB_PID 0x20
#define TEB_TID 0x24
#define TEB_TLS_POINTER 0x2C
#define TEB_PEB 0x30
#define TEB_LAST_ERROR 0x34
#define TEB_TLS_SLOTS 0xE10
#define PEB_IMAGE_BASE 0x08
#else
#define TEB_STACK_BASE 0x08
#define TEB_STACK_LIMIT 0x10
#define TEB_SELF 0x30
#define TEB_PID 0x40
#define TEB_TID 0x48
#define TEB_TLS_POINTER 0x58
#define TEB_PEB 0x60
#define TEB_LAST_ERROR 0x68
#define TEB_TLS_SLOTS 0x1480
#define PEB_IMAGE_BASE 0x10
#endif
#define TEB_ERRNO 0x1900 // ours: msvcrt's per-thread errno
#define TEB_SIZE 0x2000
#define SET_LAST_ERROR(e) (M32(THREAD(c)->teb + TEB_LAST_ERROR) = (e))
extern u64 rt_peb;

// ---- handles ----
enum ObjType { OBJ_NONE, OBJ_EVENT, OBJ_SEMAPHORE, OBJ_MUTEX, OBJ_THREAD, OBJ_FILE, OBJ_MAPPING, OBJ_PROCESS };
typedef struct Object {
  int type, refs;
  int signaled;            // events, threads
  int manual;              // manual-reset event
  s64 count, max;          // semaphores
  Thread *owner; int recursion; // mutexes
  int fd;                  // files: a host descriptor (standard handles), or
  struct VFile *vf;        // a file of the title's file system (vfs.c)
  int delete_on_close;
  u64 size, view;          // mappings
  char name[64];
  Thread *thread;
} Object;
u64 rt_handle_new(Object *o);
Object *rt_handle_get(u64 h);
Object *rt_object_new(int type);
void rt_handle_close(u64 h);
void rt_mapping_free(Object *o);   // kernel32.c: a mapping no handle or view holds, and its bytes (rt_wait_lock held)
#if GUEST_BITS == 32
#define PSEUDO_PROCESS 0xFFFFFFFFull
#define PSEUDO_THREAD 0xFFFFFFFEull
#else
#define PSEUDO_PROCESS 0xFFFFFFFFFFFFFFFFull
#define PSEUDO_THREAD 0xFFFFFFFFFFFFFFFEull
#endif
// every wait and every signal go through this lock and condition
extern pthread_mutex_t rt_wait_lock;
extern pthread_cond_t rt_wait_cond;
void rt_signal(void); // after a change that can satisfy a wait, with rt_wait_lock held
u32 rt_wait(Cpu *c, u32 n, const u64 *handles, int all, u32 ms);

// ---- imports ----
typedef void (*ImportFn)(Cpu *c);
#define IMP(dll, name) void imp_##dll##_##name(Cpu *c)
// ALIAS(dll, "imported name", fn, bytes): an import that is not a C identifier (by ordinal, "#23"; a C++ method,
// "?createSound@System@FMOD@@QAG..."; a name with other characters) is answered by IMP(dll, fn), which removes
// `bytes` of arguments from a 32-bit guest's stack (-1: as the import libraries say for fn). tools/gen-imports.sh
// collects them into the module's alias table; `pisco census` and the progress map read them as well.
#define ALIAS(dll, name, fn, bytes) extern const int rt_alias_unused_
// a data import: the IAT slot gets the address `rt_data_import` returns
u64 rt_data_import(const char *name);
u64 rt_dynamic_import(const char *dll, const char *name); // GetProcAddress
const char *rt_import_name(u64 addr);
u64 rt_stub_import(const char *full);                     // a named address that only logs (unknown vtable slots)

// ---- the title's recipe (recipe.gen.c, from tools/recipe-gen.py) ----
// The virtual CPU's features, one bit each. Without a recipe a title gets all of them.
enum { CPU_FPU = 1, CPU_TSC = 2, CPU_CX8 = 4, CPU_CMOV = 8, CPU_MMX = 0x10, CPU_FXSR = 0x20, CPU_SSE = 0x40, CPU_SSE2 = 0x80, CPU_SSE3 = 0x100,
       CPU_SSSE3 = 0x200, CPU_SSE41 = 0x400, CPU_SSE42 = 0x800, CPU_POPCNT = 0x1000, CPU_CX16 = 0x2000, CPU_PCLMUL = 0x4000 };
extern const unsigned rt_cpu_features;
extern const int rt_heap_fill;   // [quirks] heap_fill, or -1
// registry values: `key` is the full key path ("HKEY_LOCAL_MACHINE\\Software\\..."), a number (REG_DWORD) or a string (REG_SZ)
typedef struct { const char *key, *name, *string; unsigned number; } RegistryValue;
extern const RegistryValue rt_registry[]; extern const unsigned rt_registry_count;

// ---- strings ----
size_t rt_wcslen(const wchar16 *s);
char *rt_narrow(const wchar16 *s);          // malloc'd UTF-8
wchar16 *rt_widen_guest(const char *s);     // guest memory (malloc), UTF-16
