// SPDX-License-Identifier: GPL-2.0-or-later
// Both calls start at the same ELF entry and take the same SysV (bytes, length) arguments.
#include "cpu.h"
#include "modules.h"

extern const ModuleDef rt_modules[];
extern const unsigned fn_count;
extern const u64 fn_addrs[];
extern void (*const fn_ptrs[])(Cpu *);

static const u8 text[] = {
#include "fnv-text.inc"
};
static u8 input[64 << 20];
static Cpu cpu;
static u64 stack[128];

__attribute__((import_module("hypervisor"), import_name("run_x86_64")))
extern u64 run_x86_64(const u8 *code, u32 code_len, u32 entry, const u8 *bytes, u32 len);

void rt_unimpl(u64 addr) {
    (void)addr;
    __builtin_trap();
}

u64 check(u32 use_kvm, u32 len) {
    if (len > sizeof input) __builtin_trap();
    for (u32 i = 0; i < len; ++i) input[i] = (u8)(i * 131);

    u64 entry = rt_modules[0].entry;
    if (use_kvm) return run_x86_64(text, sizeof text, (u32)entry, input, len);

    cpu.r[RSP] = (uptr)&stack[127];
    cpu.r[RDI] = (uptr)input;
    cpu.r[RSI] = len;
    for (unsigned i = 0; i < fn_count; ++i) {
        if (fn_addrs[i] == entry) {
            fn_ptrs[i](&cpu);
            return cpu.r[RAX];
        }
    }
    __builtin_trap();
}
