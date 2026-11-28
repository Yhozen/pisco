// SPDX-License-Identifier: GPL-2.0-or-later
//! A PS5 app: a native executable of the PlayStation 5 (ELF, type 0xFE10, "SCE dynamic executable"), as the open
//! ps5-payload-sdk and ps5-native-tool make one. Its loadable segments are the "sections", as for a Linux app;
//! like a PE it is position-independent (relative relocations) and imports functions, here from system libraries
//! (PRX) by NID: an 11-character name the PS5 derives from the function's name (AnyPS5's tools/nid_names.py says
//! how). ps5_nids.txt gives the names of the NIDs the runtime answers for (rt/ps5/); an import the table does not
//! know keeps its NID ("libkernel!#<nid>") and becomes a stub that logs its first call.
//! The executable's own relocations hold no addend in the file (RELA): the image keeps the values they make, and the
//! runtime writes them (`rt_reloc_values`, beside the module's relocations).
use super::*;

/// ET_SCE_DYNEXEC: a PS5 or PS4 application's main executable
pub const ET_SCE_DYNEXEC: u16 = 0xFE10;
const DT_SCE_IMPORT_LIB: u64 = 0x6100_0049;
const R_X86_64_GLOB_DAT: u32 = 6;
const R_X86_64_JUMP_SLOT: u32 = 7;
const R_X86_64_RELATIVE: u32 = 8;
const R_X86_64_64: u32 = 1;

fn nid_table() -> BTreeMap<&'static str, &'static str> {
    include_str!("ps5_nids.txt").lines().map(|l| l.trim()).filter(|l| !l.is_empty() && !l.starts_with('#'))
        .filter_map(|l| { let mut w = l.split_whitespace(); Some((w.next()?, w.next()?)) }).collect()
}

/// The index a NID's library or module letter stands for (the PS5's base64 alphabet: A-Z a-z 0-9 + -)
fn letter(s: &str) -> Option<u64> {
    let mut v = 0u64;
    for ch in s.chars() {
        let d = match ch { 'A'..='Z' => ch as u64 - 'A' as u64, 'a'..='z' => ch as u64 - 'a' as u64 + 26, '0'..='9' => ch as u64 - '0' as u64 + 52, '+' => 62, '-' => 63, _ => return None };
        v = v * 64 + d;
    }
    Some(v)
}

pub fn is_ps5(data: &[u8]) -> bool {
    data.len() > 64 && data.starts_with(b"\x7fELF") && u16::from_le_bytes([data[16], data[17]]) == ET_SCE_DYNEXEC
}

impl Image {
    pub fn from_ps5_elf(data: &[u8]) -> Result<Image, String> {
        let u16at = |o: usize| -> Result<u16, String> { data.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]])).ok_or_else(|| "the file ends early".to_string()) };
        let u32at = |o: usize| -> Result<u32, String> { data.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap())).ok_or_else(|| "the file ends early".to_string()) };
        let u64at = |o: usize| -> Result<u64, String> { data.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap())).ok_or_else(|| "the file ends early".to_string()) };
        if u16at(18)? != 62 { return Err("a PS5 executable for another machine than x86-64".into()); }
        let (entry, phoff, phnum) = (u64at(24)?, u64at(32)? as usize, u16at(56)? as usize);
        // program headers: (type, flags, offset, va, filesz, memsz)
        let mut phdrs = Vec::new();
        for k in 0..phnum {
            let o = phoff + 56 * k;
            phdrs.push((u32at(o)?, u32at(o + 4)?, u64at(o + 8)?, u64at(o + 16)?, u64at(o + 32)?, u64at(o + 40)?));
        }
        let (mut sections, mut raw, mut text) = (Vec::new(), Vec::new(), Vec::new());
        // PT_LOAD, and the segment of the dynamic linking data (PT_SCE_DYNLIBDATA: its relocations, symbols and names,
        // which ps5-native-tool gives a place in the image after the others)
        for &(t, fl, off, va, filesz, memsz) in &phdrs {
            if t != 1 { continue; }
            let bytes = data.get(off as usize..(off + filesz) as usize).ok_or("a segment reaches past the end of the file")?;
            sections.push((va, bytes.to_vec(), memsz.max(filesz)));
            raw.push(off);
            if fl & 1 != 0 { text.push((va, va + memsz)); }
        }
        let base = sections.iter().map(|s| s.0).min().ok_or("no loadable segment")? & !0xFFF;
        let size = ((sections.iter().map(|s| s.0 + s.2).max().unwrap() + 0xFFFF) & !0xFFFF) - base;
        // the dynamic section: where the relocations, the symbols and their names are (addresses in the image)
        let &(_, _, dyn_off, _, dyn_size, _) = phdrs.iter().find(|p| p.0 == 2).ok_or("no dynamic section: not a PS5 executable from ps5-native-tool")?;
        let file_of = |va: u64| -> Option<usize> { phdrs.iter().filter(|p| p.0 == 1).find(|p| va >= p.3 && va < p.3 + p.4).map(|p| (p.2 + va - p.3) as usize) };
        let (mut strtab, mut symtab, mut rela, mut relasz, mut jmprel, mut pltrelsz) = (0, 0, 0, 0, 0, 0);
        let mut libs: BTreeMap<u64, String> = BTreeMap::new();
        let mut lib_names = Vec::new();
        for k in 0..(dyn_size / 16) as usize {
            let (tag, v) = (u64at(dyn_off as usize + 16 * k)?, u64at(dyn_off as usize + 16 * k + 8)?);
            match tag { 0 => break, 5 => strtab = v, 6 => symtab = v, 7 => rela = v, 8 => relasz = v, 0x17 => jmprel = v, 2 => pltrelsz = v,
                DT_SCE_IMPORT_LIB => lib_names.push(v), _ => {} }
        }
        let st = file_of(strtab).ok_or("the string table is outside the image")?;
        let cstr = |o: usize| -> String { let s = &data[st + o..]; String::from_utf8_lossy(&s[..s.iter().position(|&b| b == 0).unwrap_or(0)]).into_owned() };
        for v in lib_names { libs.insert(v >> 48, cstr((v & 0xFFFF_FFFF) as usize).to_ascii_lowercase()); }
        let nids = nid_table();
        let sy = file_of(symtab).ok_or("the symbol table is outside the image")?;
        let mut img = Image { bits: 64, base, entry, sections, text, pdata: BTreeMap::new(), unwind: BTreeMap::new(), imports: Vec::new(), relocs: Vec::new(), raw, orig_base: base,
            dirs: Vec::new(), stack_reserve: 1 << 20, fixed: false, size, is_dll: false, exports: Vec::new(), ranges: vec![(base, base + size)], elf: false, seeds: Vec::new(), jumps: BTreeMap::new(),
            ps5: true, machine: false, named: BTreeMap::new(), patched: BTreeSet::new(), reloc_values: Vec::new() };
        let mut tables = vec![(rela, relasz)];
        if jmprel != rela && pltrelsz != 0 { tables.push((jmprel, pltrelsz)); }
        for (at, n) in tables {
            if n == 0 { continue; }
            let o = file_of(at).ok_or("the relocations are outside the image")?;
            for k in 0..(n / 24) as usize {
                let (off, info, addend) = (u64at(o + 24 * k)?, u64at(o + 24 * k + 8)?, u64at(o + 24 * k + 16)?);
                let (ty, sym) = ((info & 0xFFFF_FFFF) as u32, (info >> 32) as usize);
                let symbol = || -> Result<String, String> { Ok(cstr(u32at(sy + 24 * sym)? as usize)) };
                match ty {
                    R_X86_64_RELATIVE => { img.relocs.push(off); img.reloc_values.push(addend); img.put64(off, addend); }
                    R_X86_64_GLOB_DAT | R_X86_64_JUMP_SLOT | R_X86_64_64 => {
                        let full = symbol()?;
                        let mut parts = full.split('#');
                        let nid = parts.next().unwrap_or("");
                        let lib = parts.next().and_then(letter).and_then(|l| libs.get(&l).cloned()).unwrap_or_else(|| "libunknown".into());
                        let name = nids.get(nid).map(|n| n.to_string()).unwrap_or_else(|| format!("#{nid}"));
                        if addend != 0 { return Err(format!("import {full} with an addend")); }
                        img.imports.push((off, format!("{lib}!{name}")));
                    }
                    0 => {}
                    t => return Err(format!("relocation type {t} not supported in a PS5 executable")),
                }
            }
        }
        // the functions: each FDE of the unwind tables starts one (.eh_frame_hdr's sorted table names them all)
        if let Some(&(_, _, off, va, _, _)) = phdrs.iter().find(|p| p.0 == 0x6474_E550) {
            let d = data.get(off as usize..).ok_or("the unwind table is outside the file")?;
            let (fde_enc, table_enc) = (d[2], d[3]);
            let mut p = 4usize;
            let ptr = |p: &mut usize, enc: u8| -> Option<u64> { let field = va + *p as u64; let v = encoded(d, p, enc)?; Some(if enc & 0x70 == 0x10 { field.wrapping_add(v) } else if enc & 0x70 == 0x30 { va.wrapping_add(v) } else { v }) };
            let _eh_frame = ptr(&mut p, d[1]);
            let count = ptr(&mut p, fde_enc).unwrap_or(0);
            for _ in 0..count {
                let Some(start) = ptr(&mut p, table_enc) else { break };
                let _ = ptr(&mut p, table_enc);
                if img.is_code(start) { img.seeds.push(start); }
            }
        }
        img.seeds.sort();
        img.seeds.dedup();
        Ok(img)
    }
    fn put64(&mut self, va: u64, v: u64) {
        if let Some((a, d, _)) = self.sections.iter_mut().find(|(a, d, _)| va >= *a && va + 8 <= *a + d.len() as u64) {
            let o = (va - *a) as usize;
            d[o..o + 8].copy_from_slice(&v.to_le_bytes());
        }
    }
}
