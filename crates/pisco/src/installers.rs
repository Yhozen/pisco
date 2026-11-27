// SPDX-License-Identifier: GPL-2.0-or-later
// Two installer formats of the 2000s read without running them: a Wise installer (the setup stub followed by the
// files, each a raw deflate stream with its CRC-32 after it, and a script that names them), and the InstallShield 5
// and 6 cabinets it often carries (data1.hdr: the file table; data<N>.cab: each file in chunks, each chunk a deflate
// stream of its own). Written from the formats as they are found in the files themselves and as unshield (MIT)
// documents InstallShield's; nothing of either program is run, and no code of unshield's is used.
use crate::recipe::R;
use miniz_oxide::inflate::core::{decompress, inflate_flags, DecompressorOxide};
use miniz_oxide::inflate::TINFLStatus;
use std::collections::BTreeMap;

/// The CRC-32 of zip and gzip (reflected 0xEDB88320), which Wise keeps after each stream
pub fn crc32(data: &[u8]) -> u32 {
    static TABLE: std::sync::OnceLock<[u32; 256]> = std::sync::OnceLock::new();
    let t = TABLE.get_or_init(|| {
        let mut t = [0u32; 256];
        for (i, e) in t.iter_mut().enumerate() { let mut c = i as u32; for _ in 0..8 { c = if c & 1 != 0 { 0xEDB8_8320 ^ (c >> 1) } else { c >> 1 }; } *e = c; }
        t
    });
    !data.iter().fold(!0u32, |c, &b| t[((c ^ b as u32) & 0xff) as usize] ^ (c >> 8))
}

/// One raw deflate stream at the start of `input`: what it inflates to and how many bytes of `input` it took;
/// None if those bytes are not a whole deflate stream
fn inflate(input: &[u8], limit: usize) -> Option<(Vec<u8>, usize)> {
    let mut d = DecompressorOxide::new();
    let mut out = vec![0u8; 64 * 1024];
    let (mut ip, mut op) = (0usize, 0usize);
    loop {
        let flags = inflate_flags::TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
        let (st, i, o) = decompress(&mut d, &input[ip..], &mut out, op, flags);
        ip += i; op += o;
        match st {
            TINFLStatus::Done => { out.truncate(op); return Some((out, ip)); }
            TINFLStatus::HasMoreOutput => { if out.len() >= limit { return None; } let n = (out.len() * 2).min(limit.max(out.len() + 1)); out.resize(n, 0); }
            _ => return None,
        }
    }
}
fn u16at(b: &[u8], o: usize) -> R<u16> { b.get(o..o + 2).map(|x| u16::from_le_bytes([x[0], x[1]])).ok_or_else(|| "cut short".to_string()) }
fn u32at(b: &[u8], o: usize) -> R<u32> { b.get(o..o + 4).map(|x| u32::from_le_bytes([x[0], x[1], x[2], x[3]])).ok_or_else(|| "cut short".to_string()) }
fn u64at(b: &[u8], o: usize) -> R<u64> { Ok(u32at(b, o)? as u64 | (u32at(b, o + 4)? as u64) << 32) }
fn cstr(b: &[u8], o: usize) -> R<String> {
    let s = b.get(o..).ok_or("a name out of the file")?;
    let n = s.iter().position(|&c| c == 0).ok_or("a name without its end")?;
    Ok(s[..n].iter().map(|&c| c as char).collect())   // Latin-1, as the installers of the time wrote names
}
/// A relative path from an installer's name for it: `\` becomes `/`; refused if it would leave its folder
fn rel_path(name: &str) -> R<String> {
    let p = name.replace('\\', "/");
    let p = p.trim_matches('/');
    if p.split('/').any(|s| s == ".." || s == "." || s.is_empty() || s.contains(':')) { return Err(format!("{name}: a path that leaves its folder")); }
    Ok(p.to_string())
}

/// The setup stub's size: the end of its last section; what follows is the installer's data
fn pe_end(b: &[u8]) -> R<usize> {
    if b.get(..2) != Some(b"MZ") { return Err("not a Windows program".into()); }
    let pe = u32at(b, 0x3c)? as usize;
    if b.get(pe..pe + 4) != Some(b"PE\0\0") { return Err("not a Windows program".into()); }
    let (n, opt) = (u16at(b, pe + 6)? as usize, u16at(b, pe + 20)? as usize);
    let mut end = 0;
    for i in 0..n { let s = pe + 24 + opt + 40 * i; end = end.max(u32at(b, s + 20)? as usize + u32at(b, s + 16)? as usize); }
    Ok(end)
}

/// A Wise installer's files under %MAINDIR% (the folder it installs to), by their path below it. Every stream is
/// inflated and checked against the CRC-32 after it; the script's file records (a size and a CRC-32 before each
/// name) say which stream is which file.
pub fn wise(exe: &[u8]) -> R<Vec<(String, Vec<u8>)>> {
    let start = pe_end(exe)?;
    let crc_after = |out: &[u8], at: usize| -> Option<usize> { let c = crc32(out); (0..8).find(|k| u32at(exe, at + k).ok() == Some(c)) };
    // the first stream: the header before it has no fixed size in the versions seen, so the first offset where a
    // stream with its CRC-32 after it starts is taken
    let first = (start..(start + 0x10000).min(exe.len())).find(|&p| matches!(inflate(&exe[p..], 1 << 24), Some((out, n)) if out.len() > 0 && crc_after(&out, p + n).is_some()))
        .ok_or("a Wise installer's first stream was not found (not a Wise installer?)")?;
    let mut streams: Vec<Vec<u8>> = Vec::new();
    let mut p = first;
    while p + 4 < exe.len() {
        let Some((out, n)) = inflate(&exe[p..], 1 << 31) else { break };
        let Some(k) = crc_after(&out, p + n) else { return Err(format!("Wise: stream {} at {p:#x}: its CRC-32 does not match", streams.len())); };
        p += n + k + 4;
        streams.push(out);
    }
    if exe.len() - p > 16 { return Err(format!("Wise: {} bytes after the last stream ({} streams read)", exe.len() - p, streams.len())); }
    let mut by_sum: BTreeMap<(usize, u32), usize> = BTreeMap::new();
    for (i, s) in streams.iter().enumerate() { by_sum.entry((s.len(), crc32(s))).or_insert(i); }
    // file records: "%MAINDIR%\name\0", the CRC-32 4 bytes before the name, the size 28 bytes before it
    let mut files: BTreeMap<String, (String, usize)> = BTreeMap::new();
    let key = b"%MAINDIR%\\";
    for s in &streams {
        let mut i = 0;
        while let Some(f) = s[i..].windows(key.len()).position(|w| w == key) {
            let at = i + f; i = at + key.len();
            if at < 28 { continue; }
            let (size, crc) = (u32at(s, at - 28)? as usize, u32at(s, at - 4)?);
            let Some(&ix) = by_sum.get(&(size, crc)) else { continue };
            let name = cstr(s, at + key.len())?;
            if name.is_empty() || name.contains('*') { continue; }
            let rel = rel_path(&name)?;
            files.entry(rel.to_ascii_lowercase()).or_insert((rel, ix));
        }
    }
    if files.is_empty() { return Err(format!("Wise: {} streams, but no script names a file in %MAINDIR%", streams.len())); }
    Ok(files.into_values().map(|(rel, ix)| (rel, streams[ix].clone())).collect())
}

/// One file of an InstallShield cabinet set
pub struct IsFile { pub name: String, pub dir: String, pub group: String, pub data: Vec<u8> }

/// The files of an InstallShield 5 or 6 cabinet set: `hdr` is data1.hdr, `volume(n)` gives data<n>.cab. Each file
/// with its folder (relative, as the file table has it) and the file group it belongs to. A file the table marks as
/// neither compressed nor in a cabinet is one the setup copies from beside itself: `loose(name, size)` gives it.
/// Only the files of the groups `want` says yes to are read.
pub fn installshield(hdr: &[u8], volume: &dyn Fn(u16) -> R<Vec<u8>>, loose: &dyn Fn(&str, usize) -> R<Vec<u8>>, want: &dyn Fn(&str) -> bool) -> R<Vec<IsFile>> {
    if u32at(hdr, 0)? != 0x2863_5349 { return Err("data1.hdr: not an InstallShield header (no ISc( signature)".into()); }
    let ver = u32at(hdr, 4)?;
    let major = if ver >> 24 == 1 { (ver >> 12) & 0xf } else { (ver & 0xffff) / 100 };
    if !(5..=6).contains(&major) { return Err(format!("data1.hdr: InstallShield {major} (version {ver:#x}); 5 and 6 are read")); }
    let c = u32at(hdr, 12)? as usize;   // the cabinet descriptor
    let ft = c + u32at(hdr, c + 0x0c)? as usize;   // the file table
    let dirs = u32at(hdr, c + 0x1c)? as usize;
    let (count, ft2) = (u32at(hdr, c + 0x28)? as usize, u32at(hdr, c + 0x2c)? as usize);
    let dir_names: Vec<String> = (0..dirs).map(|i| cstr(hdr, ft + u32at(hdr, ft + 4 * i)? as usize)).collect::<R<_>>()?;
    // file groups, from the descriptor's table of 71 lists: a name and the first and last file of each
    let mut groups: Vec<(usize, usize, String)> = Vec::new();
    let mut seen = std::collections::BTreeSet::new();
    for k in 0..71 {
        let mut off = u32at(hdr, c + 0x3e + 4 * k)? as usize;
        while off != 0 && seen.insert(off) {
            let (desc, next) = (u32at(hdr, c + off + 4)? as usize, u32at(hdr, c + off + 8)? as usize);
            let g = c + desc;
            let name = cstr(hdr, c + u32at(hdr, g)? as usize)?;
            let q = g + 4 + if major <= 5 { 0x48 } else { 0x12 };
            groups.push((u32at(hdr, q)? as usize, u32at(hdr, q + 4)? as usize, name));
            off = next;
        }
    }
    let mut vols: BTreeMap<u16, Vec<u8>> = BTreeMap::new();
    let mut out = Vec::new();
    for i in 0..count {
        let o = ft + ft2 + i * 0x57;
        let flags = u16at(hdr, o)?;
        let (size, csize, at) = (u64at(hdr, o + 2)? as usize, u64at(hdr, o + 0x0a)? as usize, u64at(hdr, o + 0x12)? as usize);
        let name_off = u32at(hdr, o + 0x3a)? as usize;
        let di = u16at(hdr, o + 0x3e)? as usize;
        let vol = u16at(hdr, o + 0x55)?;
        if flags & 8 != 0 || name_off == 0 { continue; }   // not a file (a folder's entry, or marked invalid)
        let name = cstr(hdr, ft + name_off)?;
        if name.is_empty() { continue; }
        let group = groups.iter().find(|(a, b, _)| (*a..=*b).contains(&i)).map(|g| g.2.clone()).unwrap_or_default();
        if !want(&group) { continue; }
        if flags & 1 != 0 { return Err(format!("{name}: split across cabinets, which is not read yet")); }
        if flags & 2 != 0 { return Err(format!("{name}: obfuscated, which is not read yet")); }
        let dir = if di < dir_names.len() { dir_names[di].clone() } else { String::new() };
        if flags & 4 == 0 && at == 0 {
            let data = loose(&name, size)?;
            out.push(IsFile { name, dir, group, data });
            continue;
        }
        if !vols.contains_key(&vol) { vols.insert(vol, volume(vol)?); }
        let v = &vols[&vol];
        let raw = v.get(at..at + csize).ok_or(format!("{name}: beyond the end of data{vol}.cab"))?;
        let data = if flags & 4 == 0 { raw.to_vec() } else {
            let mut d = Vec::with_capacity(size);
            let mut p = 0;
            while p < raw.len() {
                let n = u16at(raw, p)? as usize; p += 2;
                let chunk = raw.get(p..p + n).ok_or(format!("{name}: a chunk cut short"))?;
                let (b, _) = inflate(chunk, 1 << 20).ok_or(format!("{name}: a chunk that does not inflate"))?;
                d.extend_from_slice(&b); p += n;
            }
            d
        };
        if data.len() != size { return Err(format!("{name}: {} bytes, the file table says {size}", data.len())); }
        out.push(IsFile { name, dir, group, data });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn crc32_check_value() { assert_eq!(crc32(b"123456789"), 0xCBF4_3926); }
    #[test]
    fn inflate_reports_what_it_took() {
        let packed = miniz_oxide::deflate::compress_to_vec(b"hello hello hello", 6);
        let mut input = packed.clone(); input.extend_from_slice(b"TAIL");
        let (out, n) = inflate(&input, 1 << 20).unwrap();
        assert_eq!(out, b"hello hello hello"); assert_eq!(n, packed.len());
    }
    #[test]
    fn paths_stay_inside() { assert!(rel_path("..\\x").is_err()); assert_eq!(rel_path("redist\\directx8\\a.cab").unwrap(), "redist/directx8/a.cab"); }
}
