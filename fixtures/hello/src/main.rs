// SPDX-License-Identifier: GPL-2.0-or-later
#![no_std]
#![no_main]
use core::panic::PanicInfo;
#[panic_handler] fn panic(_: &PanicInfo) -> ! { unsafe { ExitProcess(99) } }

// 32-bit: stdcall symbols are `_Name@N`, kernel32 exports plain names
#[cfg_attr(target_arch = "x86", link(name = "kernel32", kind = "raw-dylib", import_name_type = "undecorated"))]
#[cfg_attr(not(target_arch = "x86"), link(name = "kernel32", kind = "raw-dylib"))]
extern "system" {
    fn GetStdHandle(n: u32) -> isize;
    fn WriteFile(h: isize, p: *const u8, n: u32, w: *mut u32, o: *mut u8) -> i32;
    fn ExitProcess(c: u32) -> !;
    fn VirtualAlloc(a: *mut u8, n: usize, t: u32, p: u32) -> *mut u8;
}
fn out(s: &[u8]) { unsafe { let mut w = 0; WriteFile(GetStdHandle(0xFFFF_FFF5), s.as_ptr(), s.len() as u32, &mut w, core::ptr::null_mut()); } }
fn check(name: &[u8], ok: bool) -> u32 { out(if ok { b"[PASS] " } else { b"[FAIL] " }); out(name); out(b"\n"); (!ok) as u32 }

#[inline(never)] fn fib(n: u32) -> u64 { if n < 2 { n as u64 } else { fib(n - 1) + fib(n - 2) } }
#[inline(never)] fn op(k: u32, a: i64, b: i64) -> i64 {
    match core::hint::black_box(k) { 0 => a + b, 1 => a - b, 2 => a * b, 3 => a / b, 4 => a % b, 5 => a ^ b, 6 => a << (b & 63), 7 => a >> (b & 63), _ => -1 }
}
fn add1(x: u64) -> u64 { x + 1 } fn dbl(x: u64) -> u64 { x * 2 }
#[inline(never)] fn apply(f: fn(u64) -> u64, x: u64) -> u64 { f(x) }
#[inline(never)] fn sort(a: &mut [u32]) { for i in 1..a.len() { let mut j = i; while j > 0 && a[j - 1] > a[j] { a.swap(j - 1, j); j -= 1; } } }
#[inline(never)] fn fsum(n: u32) -> f64 { let mut s = 0.0f64; let mut i = 1; while i <= n { s += 1.0 / (core::hint::black_box(i) as f64); i += 1; } s }
#[inline(never)] fn hash(d: &[u8]) -> u64 { let mut h = 0xcbf29ce484222325u64; for &b in d { h ^= b as u64; h = h.wrapping_mul(0x100000001b3); } h }

#[no_mangle] pub extern "system" fn start() -> ! {
    let mut f = 0;
    f += check(b"fib(20) recursion", fib(core::hint::black_box(20)) == 6765);
    let mut ok = true; let exp = [10i64, 4, 21, 2, 1, 4, 56, 0, -1];
    for k in 0..9 { ok &= op(k, 7, 3) == exp[k as usize]; }
    f += check(b"match / arithmetic", ok);
    let fs: [fn(u64) -> u64; 2] = [add1, dbl];
    f += check(b"indirect calls", apply(fs[core::hint::black_box(0)], 41) == 42 && apply(fs[core::hint::black_box(1)], 21) == 42);
    let mut a = [9u32, 3, 7, 1, 8, 2, 6, 4, 5, 0]; sort(core::hint::black_box(&mut a));
    f += check(b"sort on stack", a == [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]);
    let s = fsum(1000);
    f += check(b"f64 sum", s > 7.4854 && s < 7.4855);
    f += check(b"fnv hash", hash(core::hint::black_box(b"series-web")) == hash(b"series-web") && hash(b"a") == 0xaf63dc4c8601ec8c);
    unsafe {
        let p = VirtualAlloc(core::ptr::null_mut(), 1 << 16, 0x3000, 4);
        let mut okm = !p.is_null();
        if okm { for i in 0..65536usize { *p.add(i) = (i * 7) as u8; } let mut t = 0u64; for i in 0..65536usize { t += *p.add(i) as u64; } okm = t == 8355840; }
        f += check(b"VirtualAlloc memory", okm);
        ExitProcess(f)
    }
}

// As a 32-bit program: the compiler calls the Visual C++ runtime's 64-bit division helpers (arguments on the
// stack, removed by the callee, result in EDX:EAX). No runtime is linked, so they are ours: a thunk each, around
// a shift-and-subtract division that never divides 64-bit values itself.
#[cfg(target_arch = "x86")] mod div64 {
    fn udivmod(mut n: u64, d: u64) -> (u64, u64) {
        let (mut q, mut r, mut i) = (0u64, 0u64, 0);
        while i < 64 { r = (r << 1) | (n >> 63); n <<= 1; q <<= 1; if r >= d { r -= d; q |= 1; } i += 1; }
        (q, r)
    }
    #[no_mangle] extern "C" fn sw_udiv64(a: u64, b: u64) -> u64 { udivmod(a, b).0 }
    #[no_mangle] extern "C" fn sw_urem64(a: u64, b: u64) -> u64 { udivmod(a, b).1 }
    #[no_mangle] extern "C" fn sw_sdiv64(a: i64, b: i64) -> i64 { let q = udivmod(a.unsigned_abs(), b.unsigned_abs()).0 as i64; if (a < 0) != (b < 0) { q.wrapping_neg() } else { q } }
    #[no_mangle] extern "C" fn sw_srem64(a: i64, b: i64) -> i64 { let r = udivmod(a.unsigned_abs(), b.unsigned_abs()).1 as i64; if a < 0 { r.wrapping_neg() } else { r } }
    macro_rules! thunk { ($name:literal, $f:literal) => { core::arch::global_asm!(
        concat!(".globl ", $name), concat!($name, ":"),
        "push dword ptr [esp+16]", "push dword ptr [esp+16]", "push dword ptr [esp+16]", "push dword ptr [esp+16]",
        concat!("call ", $f), "add esp, 16", "ret 16"); } }
    thunk!("__aulldiv", "_sw_udiv64"); thunk!("__aullrem", "_sw_urem64"); thunk!("__alldiv", "_sw_sdiv64"); thunk!("__allrem", "_sw_srem64");
}

#[no_mangle] pub static _fltused: i32 = 0;
#[no_mangle] pub extern "C" fn __CxxFrameHandler3() {}
#[no_mangle] pub unsafe extern "C" fn memcpy(d: *mut u8, s: *const u8, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), *s.add(i)); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memset(d: *mut u8, c: i32, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), c as u8); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 { let mut i = 0; while i < n { let (x, y) = (core::ptr::read_volatile(a.add(i)), *b.add(i)); if x != y { return x as i32 - y as i32; } i += 1; } 0 }
