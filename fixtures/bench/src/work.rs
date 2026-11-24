// CPU workloads shaped like game code; the same source runs natively and lifted.
#[inline(never)] pub fn fib(n: u32) -> u64 { if n < 2 { n as u64 } else { fib(n - 1) + fib(n - 2) } }
#[inline(never)] pub fn fnv(d: &[u8]) -> u64 { let mut h = 0xcbf29ce484222325u64; for &b in d { h ^= b as u64; h = h.wrapping_mul(0x100000001b3); } h }
pub fn fill(d: &mut [u32], mut s: u32) { for x in d.iter_mut() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; *x = s; } }
#[inline(never)] pub fn sort(a: &mut [u32]) { // heapsort: data-dependent branches and loads
    let n = a.len(); if n < 2 { return; }
    let sift = |a: &mut [u32], mut i: usize, n: usize| loop { let l = 2 * i + 1; if l >= n { break; } let mut m = l;
        if l + 1 < n && a[l + 1] > a[l] { m = l + 1; } if a[i] >= a[m] { break; } a.swap(i, m); i = m; };
    for i in (0..n / 2).rev() { sift(a, i, n); }
    for e in (1..n).rev() { a.swap(0, e); sift(a, 0, e); }
}
#[inline(never)] pub fn transform(v: &mut [[f32; 4]], m: &[[f32; 4]; 4]) {
    for p in v.iter_mut() { let q = *p; for r in 0..4 { p[r] = m[r][0] * q[0] + m[r][1] * q[1] + m[r][2] * q[2] + m[r][3] * q[3]; } }
}
#[inline(never)] pub fn gather(t: &[u32], steps: u32) -> u32 { // random access over a large table
    let mask = t.len() as u32 - 1; let mut i = 1u32; let mut acc = 0u32;
    for _ in 0..steps { i = t[(i & mask) as usize] ^ i.wrapping_mul(2654435761); acc = acc.wrapping_add(i); } acc
}
pub struct Mem<'a> { pub bytes: &'a mut [u8], pub words: &'a mut [u32], pub table: &'a mut [u32], pub vecs: &'a mut [[f32; 4]] }
pub const BYTES: usize = 64 << 20; pub const WORDS: usize = 2 << 20; pub const TABLE: usize = 4 << 20; pub const VECS: usize = 1 << 20;
/// Runs one workload and returns a checksum.
pub fn run(k: usize, m: &mut Mem) -> u64 {
    match k {
        0 => fib(32),
        1 => { for (i, b) in m.bytes.iter_mut().enumerate() { *b = (i * 131) as u8; } fnv(m.bytes) }
        2 => { fill(m.words, 12345); sort(m.words); (m.words[0] as u64) << 32 | m.words[m.words.len() - 1] as u64 ^ m.words[m.words.len() / 2] as u64 }
        3 => { for (i, v) in m.vecs.iter_mut().enumerate() { *v = [i as f32, 1.0, 2.0, 1.0]; }
               let mm = [[0.5, 0.1, 0.0, 1.0], [0.0, 0.9, 0.2, 2.0], [0.3, 0.0, 0.7, 3.0], [0.0, 0.0, 0.0, 1.0]];
               for _ in 0..8 { transform(m.vecs, &mm); } let s: f32 = m.vecs.iter().map(|v| v[0] + v[1] + v[2]).sum(); s.to_bits() as u64 }
        _ => { fill(m.table, 777); gather(m.table, 20_000_000) as u64 }
    }
}
pub const NAMES: [&str; 5] = ["calls   fib(32)", "bytes   fnv 64 MB", "sort    heapsort 2M u32", "float   4x4 transform 8x1M", "memory  20M random loads"];
