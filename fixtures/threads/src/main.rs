#![no_std]
#![no_main]
// Threads and synchronisation as a title uses them: our own test code, built as a Windows executable.
use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
#[panic_handler] fn panic(_: &PanicInfo) -> ! { unsafe { ExitProcess(99) } }
type H = isize;
#[link(name = "kernel32", kind = "raw-dylib")]
extern "system" {
    fn GetStdHandle(n: u32) -> H;
    fn WriteFile(h: H, p: *const u8, n: u32, w: *mut u32, o: *mut u8) -> i32;
    fn ExitProcess(c: u32) -> !;
    fn CreateThread(sa: *mut u8, stack: usize, f: extern "system" fn(*mut u8) -> u32, p: *mut u8, flags: u32, tid: *mut u32) -> H;
    fn ResumeThread(h: H) -> u32;
    fn GetExitCodeThread(h: H, code: *mut u32) -> i32;
    fn WaitForSingleObject(h: H, ms: u32) -> u32;
    fn WaitForMultipleObjects(n: u32, hs: *const H, all: i32, ms: u32) -> u32;
    fn CreateEventA(sa: *mut u8, manual: i32, initial: i32, name: *const u8) -> H;
    fn SetEvent(h: H) -> i32;
    fn CreateSemaphoreA(sa: *mut u8, initial: i32, max: i32, name: *const u8) -> H;
    fn ReleaseSemaphore(h: H, n: i32, prev: *mut i32) -> i32;
    fn CloseHandle(h: H) -> i32;
    fn InitializeCriticalSection(cs: *mut [u64; 5]);
    fn EnterCriticalSection(cs: *mut [u64; 5]);
    fn LeaveCriticalSection(cs: *mut [u64; 5]);
    fn TlsAlloc() -> u32;
    fn TlsSetValue(i: u32, v: *mut u8) -> i32;
    fn TlsGetValue(i: u32) -> *mut u8;
    fn Sleep(ms: u32);
    fn GetTickCount() -> u32;
    fn GetCurrentThreadId() -> u32;
    fn ExitThread(code: u32) -> !;
}
fn out(s: &[u8]) { unsafe { let mut w = 0; WriteFile(GetStdHandle(0xFFFF_FFF5), s.as_ptr(), s.len() as u32, &mut w, core::ptr::null_mut()); } }
fn check(name: &[u8], ok: bool) -> u32 { out(if ok { b"[PASS] " } else { b"[FAIL] " }); out(name); out(b"\n"); (!ok) as u32 }

static COUNTER: AtomicU64 = AtomicU64::new(0);
static mut PLAIN: u64 = 0;
static mut CS: [u64; 5] = [0; 5];
static mut TLS: u32 = 0;
static TLS_OK: AtomicU32 = AtomicU32::new(0);
static mut GO: H = 0;
static mut SEM: H = 0;
static TAKEN: AtomicU32 = AtomicU32::new(0);

extern "system" fn worker(p: *mut u8) -> u32 {
    unsafe {
        let me = p as usize as u32;
        WaitForSingleObject(GO, 0xFFFF_FFFF); // all start together
        TlsSetValue(TLS, (me as usize + 1000) as *mut u8);
        for _ in 0..20000 {
            COUNTER.fetch_add(1, Ordering::SeqCst);
            EnterCriticalSection(&raw mut CS);
            let v = core::ptr::read_volatile(&raw const PLAIN); core::ptr::write_volatile(&raw mut PLAIN, v + 1);
            LeaveCriticalSection(&raw mut CS);
        }
        if TlsGetValue(TLS) as usize == me as usize + 1000 { TLS_OK.fetch_add(1, Ordering::SeqCst); }
        if WaitForSingleObject(SEM, 5000) == 0 { TAKEN.fetch_add(1, Ordering::SeqCst); }
        if me == 3 { ExitThread(300 + me) }
        200 + me
    }
}
extern "system" fn sleeper(_: *mut u8) -> u32 { unsafe { Sleep(50); } GetCurrentThreadIdWrap() }
fn GetCurrentThreadIdWrap() -> u32 { unsafe { GetCurrentThreadId() } }

#[no_mangle] pub extern "system" fn start() -> ! { unsafe {
    let mut f = 0;
    InitializeCriticalSection(&raw mut CS);
    TLS = TlsAlloc();
    GO = CreateEventA(core::ptr::null_mut(), 1, 0, core::ptr::null());
    SEM = CreateSemaphoreA(core::ptr::null_mut(), 0, 4, core::ptr::null());
    let mut hs = [0 as H; 4];
    for k in 0..4 { hs[k] = CreateThread(core::ptr::null_mut(), 0, worker, k as *mut u8, if k == 0 { 4 } else { 0 }, core::ptr::null_mut()); }
    f += check(b"four threads created (one suspended)", hs.iter().all(|&h| h != 0));
    f += check(b"a suspended thread resumes", ResumeThread(hs[0]) != 0xFFFF_FFFF);
    SetEvent(GO);
    ReleaseSemaphore(SEM, 4, core::ptr::null_mut());
    let r = WaitForMultipleObjects(4, hs.as_ptr(), 1, 20000);
    f += check(b"WaitForMultipleObjects(all) returns", r == 0);
    f += check(b"lock xadd from four threads: 80000", COUNTER.load(Ordering::SeqCst) == 80000);
    f += check(b"critical section: 80000", core::ptr::read_volatile(&raw const PLAIN) == 80000);
    f += check(b"TlsGetValue per thread", TLS_OK.load(Ordering::SeqCst) == 4);
    f += check(b"semaphore: four takes", TAKEN.load(Ordering::SeqCst) == 4);
    let mut codes = [0u32; 4];
    for k in 0..4 { GetExitCodeThread(hs[k], &mut codes[k]); CloseHandle(hs[k]); }
    f += check(b"exit codes (return and ExitThread)", codes == [200, 201, 202, 303]);
    let t0 = GetTickCount();
    let h = CreateThread(core::ptr::null_mut(), 0, sleeper, core::ptr::null_mut(), 0, core::ptr::null_mut());
    let w = WaitForSingleObject(h, 10);
    let w2 = WaitForSingleObject(h, 5000);
    let dt = GetTickCount().wrapping_sub(t0);
    f += check(b"wait timeout, then Sleep(50) ends", w == 0x102 && w2 == 0 && dt >= 45);
    ExitProcess(f)
} }
#[no_mangle] pub static _fltused: i32 = 0;
#[no_mangle] pub extern "C" fn __CxxFrameHandler3() {}
#[no_mangle] pub unsafe extern "C" fn memcpy(d: *mut u8, s: *const u8, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), *s.add(i)); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memset(d: *mut u8, c: i32, n: usize) -> *mut u8 { let mut i = 0; while i < n { core::ptr::write_volatile(d.add(i), c as u8); i += 1; } d }
#[no_mangle] pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> i32 { let mut i = 0; while i < n { let (x, y) = (core::ptr::read_volatile(a.add(i)), *b.add(i)); if x != y { return x as i32 - y as i32; } i += 1; } 0 }
