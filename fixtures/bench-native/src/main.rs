// SPDX-License-Identifier: GPL-2.0-or-later
#[path = "../../bench/src/work.rs"] mod work;
fn main() {
    let (mut b, mut w, mut t, mut v) = (vec![0u8; work::BYTES], vec![0u32; work::WORDS], vec![0u32; work::TABLE], vec![[0f32; 4]; work::VECS]);
    let mut m = work::Mem { bytes: &mut b, words: &mut w, table: &mut t, vecs: &mut v };
    for k in 0..5 { let s = std::time::Instant::now(); let r = work::run(k, &mut m); println!("{}  {} ms  checksum {:016x}", work::NAMES[k], s.elapsed().as_millis(), r); }
}
