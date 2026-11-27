// SPDX-License-Identifier: GPL-2.0-or-later
#![no_std]
#[allow(dead_code)]
#[path = "../../fixtures/bench/src/work.rs"]
mod work;

// The lifter gets an x86-64 ELF with this entry point. The probe supplies its SysV arguments.
#[no_mangle]
pub unsafe extern "C" fn hash(bytes: *const u8, len: usize) -> u64 {
    work::fnv(core::slice::from_raw_parts(bytes, len))
}
