// SPDX-License-Identifier: GPL-2.0-only
//! shader-census <title folder>: every shader container in a title's files, wherever it is stored
//! (packs, material files, the executable), with the stages, programs and RDNA2 instructions they use.
//! It reads the app's files and prints counts only; nothing is written.
use shaders::census::{scan, Census};
use std::path::Path;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let Some(dir) = args.get(1) else {
        eprintln!("usage: shader-census <title folder>");
        std::process::exit(2);
    };
    let root = Path::new(dir);
    let mut c = Census::default();
    scan(&mut c, root, root);
    println!("{} files, {:.1} MB read", c.files, c.bytes as f64 / 1e6);
    println!("{} shader containers: {} console (RDNA2 code), {} with PC DXIL, {} console ones not parsed", c.containers, c.console, c.with_dxil, c.unparsed);
    println!("{} console programs, by stage:", c.programs);
    for (s, n) in &c.stages {
        println!("  {n:7}  {s}");
    }
    let files: Vec<_> = { let mut v: Vec<_> = c.per_file.iter().collect(); v.sort_by(|a, b| b.1.cmp(a.1)); v };
    println!("where they are (top 10):");
    for (f, n) in files.iter().take(10) {
        println!("  {n:7}  {f}");
    }
    let sorted = c.stats.sorted();
    println!("{} distinct RDNA2 instructions; most used:", sorted.len());
    for (m, n) in sorted.iter().take(40) {
        println!("  {n:9}  {m}");
    }
}
