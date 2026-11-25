// SPDX-License-Identifier: GPL-2.0-or-later
#![no_std]
#![no_main]
mod work;
use core::panic::PanicInfo;
#[panic_handler] fn panic(_: &PanicInfo) -> ! { unsafe { ExitProcess(99) } }
#[link(name = "kernel32", kind = "raw-dylib")]
extern "system" {
    fn GetStdHandle(n: u32) -> isize;
    fn WriteFile(h: isize, p: *const u8, n: u32, w: *mut u32, o: *mut u8) -> i32;
    fn ExitProcess(c: u32) -> !;
    fn VirtualAlloc(a: *mut u8, n: usize, t: u32, p: u32) -> *mut u8;
    fn QueryPerformanceCounter(c: *mut i64) -> i32;
    fn QueryPerformanceFrequency(f: *mut i64) -> i32;
}
fn out(s: &[u8]) { unsafe { let mut w = 0; WriteFile(GetStdHandle(0xFFFF_FFF5), s.as_ptr(), s.len() as u32, &mut w, core::ptr::null_mut()); } }
fn num(mut v: u64) { let mut b = [0u8; 20]; let mut i = 20; loop { i -= 1; b[i] = b'0' + (v % 10) as u8; v /= 10; if v == 0 { break; } } out(&b[i..]); }
fn hex(v: u64) { let mut b = [0u8; 16]; for i in 0..16 { let d = (v >> (60 - 4 * i)) & 15; b[i] = if d < 10 { b'0' + d as u8 } else { b'a' + d as u8 - 10 }; } out(&b); }
fn now() -> i64 { let mut c = 0; unsafe { QueryPerformanceCounter(&mut c) }; c }
unsafe fn alloc<T>(n: usize) -> &'static mut [T] { let p = VirtualAlloc(core::ptr::null_mut(), n * core::mem::size_of::<T>(), 0x3000, 4); if p.is_null() { ExitProcess(98) } core::slice::from_raw_parts_mut(p as *mut T, n) }
#[no_mangle] pub extern "system" fn start() -> ! { unsafe {
    let mut freq = 0; QueryPerformanceFrequency(&mut freq);
    let mut m = work::Mem { bytes: alloc(work::BYTES), words: alloc(work::WORDS), table: alloc(work::TABLE), vecs: alloc(work::VECS) };
    for k in 0..5 { let t = now(); let r = work::run(k, &mut m); let us = (now() - t) * 1_000_000 / freq;
        out(work::NAMES[k].as_bytes()); out(b"  "); num(us as u64 / 1000); out(b" ms  checksum "); hex(r); out(b"\n"); }
    ExitProcess(0)
} }
#[no_mangle] pub static _fltused: i32 = 0;
#[no_mangle] pub extern "C" fn __CxxFrameHandler3() {}
#[no_mangle] pub unsafe extern "C" fn memcpy(d: *mut u8, s: *const u8, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), *s.add(i)); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memmove(d: *mut u8, s: *const u8, n: usize) -> *mut u8 { if (d as usize) < (s as usize) { memcpy(d, s, n) } else { let mut i = n; while i > 0 { i -= 1; core::ptr::write_volatile(d.add(i), *s.add(i)); } d } }
#[no_mangle] pub unsafe extern "C" fn memset(d: *mut u8, c: i32, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), c as u8); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 { let mut i = 0; while i < n { let (x, y) = (core::ptr::read_volatile(a.add(i)), *b.add(i)); if x != y { return x as i32 - y as i32; } i += 1; } 0 }
