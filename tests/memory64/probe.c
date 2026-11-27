// SPDX-License-Identifier: GPL-2.0-or-later
// Run the lifted x86-64 FNV workload above 5 GiB, with a different byte at the same low 32 bits.
#include "cpu.h"
#include "modules.h"

extern const ModuleDef rt_modules[];
extern const unsigned fn_count;
extern const u64 fn_addrs[];
extern void (*const fn_ptrs[])(Cpu *);

static Cpu cpu;
static u64 guest_stack[128];
static volatile u32 marker;

u64 check(void) {
    const uptr size = 64ull << 20, high = 5ull << 30, low = high - (1ull << 32);
    const uptr pages = (high + size) >> 16;
    uptr before = __builtin_wasm_memory_size(0);
    marker = 17;
    if (before >= pages || __builtin_wasm_memory_grow(0, pages - before) != before) return 1;
    if (__builtin_wasm_memory_size(0) != pages || marker != 17) return 2;
    volatile u8 *twin = (volatile u8 *)low;
    if (*twin || M8(high) || M8(high + size - 1)) return 3;
    *twin = 0x5a;
#ifdef SHARED
    if (__atomic_fetch_add((u32 *)high, 1, __ATOMIC_SEQ_CST) != 0) return 4;
#endif
    for (uptr i = 0; i < size; i++) M8(high + i) = (u8)(i * 131);
    if (*twin != 0x5a || __builtin_wasm_memory_grow(0, 1) != (uptr)-1) return 5;

    cpu.r[RSP] = (uptr)&guest_stack[127];
    cpu.r[RDI] = high;
    cpu.r[RSI] = size;
    for (unsigned i = 0; i < fn_count; i++) {
        if (fn_addrs[i] == rt_modules[0].entry) {
            fn_ptrs[i](&cpu);
            return cpu.r[RAX];
        }
    }
    return 6;
}
