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

// ---- guest calls ----
// Arguments of a Win64 call, as an import sees them: RSP points at the return address.
#define ARG(n) ((n) == 0 ? R[RCX] : (n) == 1 ? R[RDX] : (n) == 2 ? R[8] : (n) == 3 ? R[9] : M64(R[RSP] + 8 + 8 * (n)))
#define FARG(n) (X[n].pd[0])
#define RET(v) (R[RAX] = (u64)(v))
#define GSTR(a) ((char *)GP(a))
typedef u16 wchar16;
#define GWSTR(a) ((wchar16 *)GP(a))

// Calls a guest function with up to four integer arguments and returns RAX.
u64 rt_guest_call(Cpu *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3);

// ---- logging ----
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
__attribute__((noreturn)) void rt_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int rt_trace;

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
#define TEB_STACK_BASE 0x08
#define TEB_STACK_LIMIT 0x10
#define TEB_SELF 0x30
#define TEB_PID 0x40
#define TEB_TID 0x48
#define TEB_TLS_POINTER 0x58
#define TEB_PEB 0x60
#define TEB_LAST_ERROR 0x68
#define TEB_TLS_SLOTS 0x1480
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
#define PSEUDO_PROCESS 0xFFFFFFFFFFFFFFFFull
#define PSEUDO_THREAD 0xFFFFFFFFFFFFFFFEull
// every wait and every signal go through this lock and condition
extern pthread_mutex_t rt_wait_lock;
extern pthread_cond_t rt_wait_cond;
void rt_signal(void); // after a change that can satisfy a wait, with rt_wait_lock held
u32 rt_wait(Cpu *c, u32 n, const u64 *handles, int all, u32 ms);

// ---- imports ----
typedef void (*ImportFn)(Cpu *c);
#define IMP(dll, name) void imp_##dll##_##name(Cpu *c)
// a data import: the IAT slot gets the address `rt_data_import` returns
u64 rt_data_import(const char *name);
u64 rt_dynamic_import(const char *dll, const char *name); // GetProcAddress
const char *rt_import_name(u64 addr);
u64 rt_stub_import(const char *full);                     // a named address that only logs (unknown vtable slots)

// ---- strings ----
size_t rt_wcslen(const wchar16 *s);
char *rt_narrow(const wchar16 *s);          // malloc'd UTF-8
wchar16 *rt_widen_guest(const char *s);     // guest memory (malloc), UTF-16
