// SPDX-License-Identifier: GPL-2.0-or-later
//! Static recompiler: an x86-64 (PE32+) or 32-bit x86 (PE32) image, or a statically linked x86-64 ELF -> C (one
//! function per guest function),
//! to be compiled to WebAssembly. Also the census: what an image uses that the lifter does not handle yet.

use iced_x86::{CodeSize, ConditionCode, Decoder, DecoderOptions, FlowControl, Formatter, Instruction, InstructionInfoFactory, IntelFormatter, Mnemonic, OpAccess, OpKind, Register};
use object::read::pe::{ImageNtHeaders, PeFile, PeFile32, PeFile64};
use object::{LittleEndian as LE, Object, ObjectSection};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

mod avx;
mod ps5;

/// Guest addresses handed out for imports (the IAT slots hold these).
const IMPORT_BASE: u64 = 0x0F80_0000;

#[derive(Clone)]
struct Image {
    bits: u32, // 64 (PE32+) or 32 (PE32): the width of a pointer, a stack slot, an import slot, a relocation
    base: u64,
    entry: u64,
    sections: Vec<(u64, Vec<u8>, u64)>, // va, file bytes, virtual size
    raw: Vec<u64>,                      // each section's offset in the file
    dirs: Vec<(u64, u64)>,              // data directories (rva, size)
    stack_reserve: u64,
    fixed: bool, // IMAGE_FILE_RELOCS_STRIPPED
    orig_base: u64,
    text: Vec<(u64, u64)>,              // executable ranges
    pdata: BTreeMap<u64, u64>,          // function begin -> end
    unwind: BTreeMap<u64, u64>,         // function begin -> its UNWIND_INFO
    imports: Vec<(u64, String)>,        // IAT slot va, "dll!name"
    relocs: Vec<u64>,                   // addresses of absolute pointers (`bits` wide)
    size: u64,                          // SizeOfImage
    is_dll: bool,
    exports: Vec<(String, u32, u64)>,   // name ("" if by ordinal only), ordinal, VA (forwarders left out)
    /// in a merged view of several images: each image's (base, end)
    ranges: Vec<(u64, u64)>,
    /// an ELF (a Linux title): it has no imports and no exception table
    elf: bool,
    /// an ELF: the function starts its symbols and its unwind tables (.eh_frame) name
    seeds: Vec<u64>,
    /// a machine's image: the symbols its runtime and the lifter look for (common_startup_64, early_top_pgt,
    /// __switch_to_asm), as (address, size)
    named: BTreeMap<String, (u64, u64)>,
    /// a machine's image: immediates the kernel patches at boot (Linux's runtime constants: the sections
    /// runtime_ptr_* and runtime_shift_*), by the address of the immediate; they are read from memory
    patched: BTreeSet<u64>,
    /// an ELF: the C library's setjmp (1), sigsetjmp (2) and longjmp (3), by their symbols
    jumps: BTreeMap<u64, u8>,
    /// a PS5 title (ps5.rs): imports from the system's libraries by NID, relocations whose values the image keeps
    ps5: bool,
    /// --machine: the image of a machine's own software (an operating system's kernel, rt/microvm-x64): it stays at
    /// its link addresses, which are not addresses of the module's memory (the machine has page tables)
    machine: bool,
    /// a PS5 title: the value each relocation (`relocs`, in the same order) makes, at the image's base
    reloc_values: Vec<u64>,
}

impl Image {
    fn load(data: &[u8], machine: bool) -> Result<Image, String> {
        if ps5::is_ps5(data) {
            return Image::from_ps5_elf(data);
        }
        if data.starts_with(b"\x7fELF") {
            return Image::from_elf(data, machine);
        }
        match PeFile64::parse(data) {
            Ok(pe) => Image::from_pe(&pe, 64),
            Err(e) => match PeFile32::parse(data) {
                Ok(pe) => Image::from_pe(&pe, 32),
                Err(_) => Err(e.to_string()),
            },
        }
    }
    fn from_pe<Pe: ImageNtHeaders>(pe: &PeFile<Pe>, bits: u32) -> Result<Image, String> {
        let base = pe.relative_address_base();
        let opt = pe.nt_headers().optional_header();
        let entry = base + u64::from(object::read::pe::ImageOptionalHeader::address_of_entry_point(opt));
        let mut sections = Vec::new();
        let mut raw = Vec::new();
        let mut text = Vec::new();
        for s in pe.sections() {
            let hdr = s.pe_section();
            let va = s.address();
            let vsize = u64::from(hdr.virtual_size.get(LE));
            sections.push((va, s.data().map_err(|e| e.to_string())?.to_vec(), vsize));
            raw.push(u64::from(hdr.pointer_to_raw_data.get(LE)));
            if hdr.characteristics.get(LE) & 0x2000_0000 != 0 {
                text.push((va, va + vsize));
            }
        }
        let dirs: Vec<(u64, u64)> = (0..16).map(|n| pe.data_directory(n).map(|d| (u64::from(d.virtual_address.get(LE)), u64::from(d.size.get(LE)))).unwrap_or((0, 0))).collect();
        let stack_reserve = object::read::pe::ImageOptionalHeader::size_of_stack_reserve(opt);
        let mut relocs = Vec::new();
        if let Some(dir) = pe.data_directory(5) {
            let (rva, size) = (u64::from(dir.virtual_address.get(LE)), u64::from(dir.size.get(LE)));
            let find = |va: u64| sections.iter().find(|(a, d, _)| va >= *a && va < *a + d.len() as u64).map(|(a, d, _)| d[(va - a) as usize..].to_vec());
            let blob = find(base + rva).unwrap_or_default();
            let mut at = 0usize;
            while at + 8 <= (size as usize).min(blob.len()) {
                let page = u32::from_le_bytes(blob[at..at + 4].try_into().unwrap()) as u64;
                let len = u32::from_le_bytes(blob[at + 4..at + 8].try_into().unwrap()) as usize;
                if len < 8 { break; }
                for k in (at + 8..at + len).step_by(2) {
                    let e = u16::from_le_bytes([blob[k], blob[k + 1]]);
                    match (e >> 12, bits) {
                        (0, _) => {}
                        (10, 64) | (3, 32) => relocs.push(base + page + u64::from(e & 0xFFF)), // DIR64; HIGHLOW
                        (t, _) => return Err(format!("relocation type {t} not supported")),
                    }
                }
                at += len;
            }
        }
        let size = u64::from(object::read::pe::ImageOptionalHeader::size_of_image(opt));
        let characteristics = pe.nt_headers().file_header().characteristics.get(LE);
        let mut img = Image { bits, base, entry, sections, text, pdata: BTreeMap::new(), unwind: BTreeMap::new(), imports: Vec::new(), relocs, raw, orig_base: base, dirs, stack_reserve,
            fixed: characteristics & 1 != 0, size, is_dll: characteristics & 0x2000 != 0, exports: Vec::new(), ranges: vec![(base, base + size)], elf: false, seeds: Vec::new(), jumps: BTreeMap::new(), ps5: false, machine: false, named: BTreeMap::new(), patched: BTreeSet::new(), reloc_values: Vec::new() };
        if let Some(Some(table)) = pe.export_table().ok() {
            if let Ok(list) = table.exports() {
                for e in list {
                    if let object::read::pe::ExportTarget::Address(rva) = e.target {
                        let name = e.name.map(|n| String::from_utf8_lossy(n).into_owned()).unwrap_or_default();
                        img.exports.push((name, e.ordinal, base + u64::from(rva)));
                    }
                }
            }
        }
        // exception table: 12-byte RUNTIME_FUNCTION entries (x64 only: 32-bit x86 has none)
        if let Some(dir) = pe.data_directory(3).filter(|_| bits == 64) {
            let (rva, size) = (u64::from(dir.virtual_address.get(LE)), u64::from(dir.size.get(LE)));
            for i in 0..size / 12 {
                let at = base + rva + i * 12;
                if let (Some(b), Some(e), Some(u)) = (img.u32(at), img.u32(at + 4), img.u32(at + 8)) {
                    if b != 0 {
                        img.pdata.insert(base + u64::from(b), base + u64::from(e));
                        img.unwind.insert(base + u64::from(b), base + u64::from(u & !1));
                    }
                }
            }
        }
        if let Some(table) = pe.import_table().map_err(|e| e.to_string())? {
            let mut descs = table.descriptors().map_err(|e| e.to_string())?;
            while let Some(d) = descs.next().map_err(|e| e.to_string())? {
                let dll = String::from_utf8_lossy(table.name(d.name.get(LE)).map_err(|e| e.to_string())?).to_lowercase();
                let first = if d.original_first_thunk.get(LE) != 0 { d.original_first_thunk.get(LE) } else { d.first_thunk.get(LE) };
                let mut thunks = table.thunks(first).map_err(|e| e.to_string())?;
                let mut slot = base + u64::from(d.first_thunk.get(LE));
                while let Some(t) = thunks.next::<Pe>().map_err(|e| e.to_string())? {
                    let name = match table.import::<Pe>(t).map_err(|e| e.to_string())? {
                        object::read::pe::Import::Name(_, n) => String::from_utf8_lossy(n).into_owned(),
                        object::read::pe::Import::Ordinal(o) => format!("#{o}"),
                    };
                    img.imports.push((slot, format!("{dll}!{name}")));
                    slot += u64::from(bits / 8);
                }
            }
        }
        Ok(img)
    }
    /// A Linux title: a statically linked x86-64 ELF that is not position-independent. Its loadable segments are the
    /// "sections"; the functions are found from its symbols and its unwind tables, then as in a PE (call targets,
    /// code addresses in data).
    fn from_elf(data: &[u8], machine: bool) -> Result<Image, String> {
        use object::elf;
        use object::read::elf::{ElfFile64, FileHeader, ProgramHeader};
        use object::{ObjectSymbol, SymbolKind};
        let file = ElfFile64::<object::Endianness>::parse(data).map_err(|e| e.to_string())?;
        let (h, en) = (file.elf_header(), file.endian());
        if h.e_machine(en) != elf::EM_X86_64 { return Err("an ELF for another machine than x86-64".into()); }
        let phdrs = h.program_headers(en, data).map_err(|e| e.to_string())?;
        if phdrs.iter().any(|p| matches!(p.p_type(en), elf::PT_INTERP | elf::PT_DYNAMIC)) || h.e_type(en) != elf::ET_EXEC {
            return Err("a dynamically linked or position-independent ELF: a Linux title is linked with -static -no-pie".into());
        }
        let (mut sections, mut raw, mut text) = (Vec::new(), Vec::new(), Vec::new());
        for p in phdrs.iter().filter(|p| p.p_type(en) == elf::PT_LOAD) {
            let (va, off, filesz, memsz) = (p.p_vaddr(en), p.p_offset(en), p.p_filesz(en), p.p_memsz(en));
            let bytes = data.get(off as usize..(off + filesz) as usize).ok_or("a segment reaches past the end of the file")?;
            sections.push((va, bytes.to_vec(), memsz));
            raw.push(off);
            if p.p_flags(en) & elf::PF_X != 0 { text.push((va, va + memsz)); }
        }
        // an image in the upper half of the address space is a machine's own (a kernel), said so or not
        let machine = machine || sections.iter().any(|s| s.0 >> 47 != 0);
        if machine {
            // a kernel's executable segment holds its read-only data too: its code is what its sections say is code
            use object::{Object, ObjectSection, SectionFlags};
            text = file.sections().filter(|s| matches!(s.flags(), SectionFlags::Elf { sh_flags } if sh_flags & u64::from(elf::SHF_EXECINSTR) != 0)).map(|s| (s.address(), s.address() + s.size())).collect();
        }
        let base = sections.iter().map(|s| s.0).min().ok_or("no loadable segment")? & !0xFFF;
        let size = ((sections.iter().map(|s| s.0 + s.2).max().unwrap() + 0xFFF) & !0xFFF) - base;
        let mut img = Image { bits: 64, base, entry: h.e_entry(en), sections, text, pdata: BTreeMap::new(), unwind: BTreeMap::new(), imports: Vec::new(), relocs: Vec::new(), raw, orig_base: base,
            dirs: Vec::new(), stack_reserve: 8 << 20, fixed: true, size, is_dll: false, exports: Vec::new(), ranges: vec![(base, base + size)], elf: true, seeds: Vec::new(), jumps: BTreeMap::new(), ps5: false, machine, named: BTreeMap::new(), patched: BTreeSet::new(), reloc_values: Vec::new() };
        // symbols: functions, and the labels of hand-written assembly (no type)
        for sym in file.symbols() {
            let a = sym.address();
            if machine && (matches!(sym.name().unwrap_or(""), "common_startup_64" | "early_top_pgt" | "__switch_to_asm" | "early_idt_handler_array" | "irq_entries_start" | "spurious_entries_start")
                || sym.name().unwrap_or("").starts_with("__SCT__")) {
                img.named.insert(sym.name().unwrap_or("").to_string(), (a, sym.size()));
            }
            if !img.is_code(a) || !matches!(sym.kind(), SymbolKind::Text | SymbolKind::Unknown | SymbolKind::Label) { continue; }
            img.seeds.push(a);
            match sym.name().unwrap_or("") {
                "setjmp" | "_setjmp" | "__setjmp" | "___setjmp" => { img.jumps.insert(a, 1); }
                "sigsetjmp" | "__sigsetjmp" => { img.jumps.insert(a, 2); }
                "longjmp" | "_longjmp" | "__longjmp" => { img.jumps.insert(a, 3); }
                _ => {}
            }
        }
        // .eh_frame: each FDE begins a function (what is left of the functions when the symbols are stripped)
        if let Some(sec) = file.section_by_name(".eh_frame") {
            let (va, d) = (sec.address(), sec.data().map_err(|e| e.to_string())?);
            let mut cies: BTreeMap<usize, u8> = BTreeMap::new(); // a CIE's offset -> how its FDEs encode addresses
            let mut at = 0usize;
            while at + 8 <= d.len() {
                let len = u32::from_le_bytes(d[at..at + 4].try_into().unwrap()) as usize;
                if len == 0 || len == 0xFFFF_FFFF || at + 4 + len > d.len() { break; }
                let id = u32::from_le_bytes(d[at + 4..at + 8].try_into().unwrap()) as usize;
                let mut p = at + 8;
                if id == 0 {
                    let mut enc = 0u8; // absolute 8-byte addresses unless the augmentation says otherwise
                    let version = d[p];
                    p += 1;
                    let aug: Vec<u8> = d[p..].iter().copied().take_while(|&b| b != 0).collect();
                    p += aug.len() + 1;
                    let ok = (|| {
                        uleb(d, &mut p)?; uleb(d, &mut p)?; // the alignment factors
                        if version == 1 { p += 1 } else { uleb(d, &mut p)?; }
                        if aug.first() == Some(&b'z') {
                            uleb(d, &mut p)?;
                            for c in &aug[1..] {
                                match c {
                                    b'R' => { enc = *d.get(p)?; p += 1; }
                                    b'L' => p += 1,
                                    b'P' => { let e = *d.get(p)?; p += 1; encoded(d, &mut p, e)?; }
                                    _ => {}
                                }
                            }
                        }
                        Some(())
                    })();
                    if ok.is_some() { cies.insert(at, enc); }
                } else if let Some(&enc) = (at + 4).checked_sub(id).and_then(|c| cies.get(&c)) {
                    let field = va + p as u64;
                    if let Some(v) = encoded(d, &mut p, enc) {
                        let a = if enc & 0x70 == 0x10 { field.wrapping_add(v) } else { v };
                        if img.is_code(a) { img.seeds.push(a); }
                    }
                }
                at += 4 + len;
            }
        }
        if machine {
            // Linux's runtime constants: each entry the address of an immediate, relative to the entry
            use object::{Object, ObjectSection};
            for sec in file.sections().filter(|s| s.name().is_ok_and(|n| n.starts_with("runtime_ptr_") || n.starts_with("runtime_shift_"))) {
                let (va, d) = (sec.address(), sec.data().map_err(|e| e.to_string())?);
                for (k, e) in d.chunks_exact(4).enumerate() {
                    img.patched.insert((va + 4 * k as u64).wrapping_add(i32::from_le_bytes(e.try_into().unwrap()) as i64 as u64));
                }
            }
            // a kernel's exception table (Linux's __ex_table: {insn, fixup, data}, each relative to its own field): the
            // fix-up addresses are entered by address when a fault is fixed up, so each is a function's start
            if let Some(sec) = file.section_by_name("__ex_table") {
                let (va, d) = (sec.address(), sec.data().map_err(|e| e.to_string())?);
                for (k, e) in d.chunks_exact(12).enumerate() {
                    let fixup = (va + 12 * k as u64 + 4).wrapping_add(i32::from_le_bytes(e[4..8].try_into().unwrap()) as i64 as u64);
                    if img.is_code(fixup) { img.seeds.push(fixup); }
                }
            }
        }
        img.seeds.sort();
        img.seeds.dedup();
        // a label of hand-written assembly has no size: it reaches to the next function
        let seeds = img.seeds.clone();
        for (a, n) in img.named.values_mut() {
            if *n == 0 { *n = seeds.iter().find(|&&s| s > *a).map(|s| s - *a).unwrap_or(4096); }
        }
        // Linux's arrays of interrupt entry stubs: the IDT points at each stub, which no symbol names
        for (name, stride) in [("early_idt_handler_array", 9u64), ("irq_entries_start", 8), ("spurious_entries_start", 8)] {
            if let Some(&(a, n)) = img.named.get(name) { for k in 0..n / stride { img.seeds.push(a + k * stride); } }
        }
        img.seeds.sort();
        img.seeds.dedup();
        Ok(img)
    }
    /// Moves the image to `to`: every address it holds and every address we keep.
    fn rebase(&mut self, to: u64) -> Result<(), String> {
        let d = to.wrapping_sub(self.base);
        if d == 0 { return Ok(()); }
        if self.fixed { return Err("the image was linked without relocations (/FIXED) and cannot be moved".into()); }
        let w = (self.bits / 8) as usize; // a relocation patches a pointer of the image's width
        for &r in &self.relocs.clone() {
            let (sa, data, _) = self.sections.iter_mut().find(|(a, d, _)| r >= *a && r + w as u64 <= *a + d.len() as u64).ok_or(format!("relocation at 0x{r:x} outside the file data"))?;
            let o = (r - *sa) as usize;
            let mut b = [0u8; 8];
            b[..w].copy_from_slice(&data[o..o + w]);
            let v = u64::from_le_bytes(b).wrapping_add(d);
            data[o..o + w].copy_from_slice(&v.to_le_bytes()[..w]);
        }
        let mv = |a: &mut u64| *a = a.wrapping_add(d);
        mv(&mut self.base); mv(&mut self.entry);
        self.sections.iter_mut().for_each(|s| mv(&mut s.0));
        self.text.iter_mut().for_each(|t| { mv(&mut t.0); mv(&mut t.1) });
        self.pdata = self.pdata.iter().map(|(b, e)| (b.wrapping_add(d), e.wrapping_add(d))).collect();
        self.unwind = self.unwind.iter().map(|(b, u)| (b.wrapping_add(d), u.wrapping_add(d))).collect();
        self.imports.iter_mut().for_each(|i| mv(&mut i.0));
        self.relocs.iter_mut().for_each(mv);
        self.reloc_values.iter_mut().for_each(mv);
        self.seeds.iter_mut().for_each(mv);
        self.jumps = self.jumps.iter().map(|(a, k)| (a.wrapping_add(d), *k)).collect();
        self.exports.iter_mut().for_each(|e| mv(&mut e.2));
        self.ranges.iter_mut().for_each(|r| { mv(&mut r.0); mv(&mut r.1) });
        Ok(())
    }
    /// The base of the image `va` is in (RVAs in an image's tables are relative to it).
    fn base_of(&self, va: u64) -> u64 {
        self.ranges.iter().find(|(b, e)| va >= *b && va < *e).map(|r| r.0).unwrap_or(self.base)
    }
    fn is_base(&self, va: u64) -> bool {
        self.ranges.iter().any(|r| r.0 == va)
    }
    fn bytes(&self, va: u64) -> Option<&[u8]> {
        self.sections.iter().find(|(a, d, _)| va >= *a && va < *a + d.len() as u64).map(|(a, d, _)| &d[(va - a) as usize..])
    }
    fn u8(&self, va: u64) -> Option<u8> {
        self.bytes(va).and_then(|b| b.first().copied())
    }
    fn u32(&self, va: u64) -> Option<u32> {
        self.bytes(va).filter(|b| b.len() >= 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
    }
    fn is_code(&self, va: u64) -> bool {
        self.text.iter().any(|(a, e)| va >= *a && va < *e)
    }
}

struct Func {
    insns: BTreeMap<u64, Instruction>,
    leaders: BTreeSet<u64>,
    tables: BTreeMap<u64, Vec<u64>>, // indirect jmp address -> targets
    pads: BTreeSet<u64>,             // landing pads: where an exception resumes in this function
    /// 32-bit code: the bytes of the jump tables read (address and index tables), as (start, end)
    data: Vec<(u64, u64)>,
    /// 32-bit code: decoding met bytes that are no instruction, or left the code
    bad: bool,
    /// 32-bit code: soft entries that sit inside this function. Each is one more way into its body
    alts: BTreeSet<u64>,
    /// where a call or a system call returns to: a Linux title's function can be entered there (`c->resume`), as a
    /// forked process and a longjmp go on from a return address
    rets: BTreeSet<u64>,
}
impl Func {
    /// 32-bit code: `ip` is in one of the function's own jump tables. Decoding went on past a call that does not
    /// return (a throw) into the table that follows it: what it read there is no code, and it may cover the next
    /// function's first bytes (MOHAA's gamex86.dll has a function right after such a table)
    fn in_data(&self, ip: u64) -> bool {
        self.data.iter().any(|&(a, e)| a <= ip && ip < e)
    }
}

/// A near branch or call with its target in the instruction (far ones exist in 32-bit code).
fn is_near(i: &Instruction) -> bool {
    matches!(i.op_kind(0), OpKind::NearBranch16 | OpKind::NearBranch32 | OpKind::NearBranch64)
}

/// The bound of a jump table's index: `cmp I,N` then ja / jae (N+1 or N entries) among the instructions before
/// the load (`prev`, closest first); I may reach the load through copies, or be masked by an `and`.
fn table_bound(prev: &[&Instruction], load_ip: u64, index: Register) -> Option<u64> {
    let mut bound = None;
    let mut names = vec![index];
    let mut mems: Vec<(Register, Register, u32, u64)> = Vec::new(); // memory the index was loaded from
    let mut seen_load = false;
    let same_mem = |i: &Instruction, m: &(Register, Register, u32, u64)| i.memory_base() == m.0 && i.memory_index() == m.1 && i.memory_index_scale() == m.2 && i.memory_displacement64() == m.3;
    // the index's copies first (a copy may come before the compare), then the compare on any of them
    for _ in 0..3 {
        let mut seen = false;
        for i in prev {
            if i.ip() == load_ip { seen = true; continue; }
            if !seen { continue; }
            if matches!(i.mnemonic(), Mnemonic::Mov | Mnemonic::Movzx | Mnemonic::Movsxd | Mnemonic::Movsx) && i.op_kind(0) == OpKind::Register && names.contains(&i.op0_register().full_register()) {
                if i.op_kind(1) == OpKind::Register && !names.contains(&i.op_register(1).full_register()) { names.push(i.op_register(1).full_register()); }
                else if i.op_kind(1) == OpKind::Memory { mems.push((i.memory_base(), i.memory_index(), i.memory_index_scale(), i.memory_displacement64())); }
            }
        }
    }
    for (k, i) in prev.iter().enumerate() {
        if i.ip() == load_ip { seen_load = true; continue; }
        if !seen_load { continue; }
        let on_index = i.op_kind(0) == OpKind::Register && names.contains(&i.op0_register().full_register());
        let on_mem = i.op_kind(0) == OpKind::Memory && mems.iter().any(|m| same_mem(i, m));
        if i.mnemonic() == Mnemonic::And && on_index && is_imm(i.op_kind(1)) && i.immediate(1) < 4096 {
            bound = Some(i.immediate(1) + 1); // `and I, 2^k-1`: the index is masked into range
            break;
        }
        if i.mnemonic() == Mnemonic::Cmp && (on_index || on_mem) && is_imm(i.op_kind(1)) {
            // the branch after it says whether N is in range
            let after = if k > 0 { Some(prev[k - 1].mnemonic()) } else { None };
            bound = Some(match after { Some(Mnemonic::Jae) | Some(Mnemonic::Jb) => i.immediate(1), _ => i.immediate(1) + 1 });
            break;
        }
    }
    bound
}

/// A `switch` compiled to `jmp reg`. Forms seen:
/// - relative: `lea B,[rip+T] ... movsxd R,[B+I*4]; add R,B; jmp R` (LLVM, GCC; B may be set far earlier);
/// - image-relative (MSVC): `lea B,[__ImageBase] ... mov R32,[B+I*4+rva]; add R,B; jmp R`.
/// The bound comes from the `cmp I,N; ja` before it; without one, entries are read while they land in the function.
/// The bound of a jump table whose block is reached by a branch from far away (GCC puts the table's block at the end
/// of the function: `cmp I,N; jbe <block>` near its start): the compare before a conditional branch to the block.
fn table_bound_by_branch(insns: &BTreeMap<u64, Instruction>, load: &Instruction, index: Register) -> Option<u64> {
    // the block's start: back from the load while the instruction before falls through to the next one
    let mut start = load.ip();
    for (_, i) in insns.range(..load.ip()).rev() {
        if i.next_ip() != start || !matches!(i.flow_control(), FlowControl::Next | FlowControl::Call) { break; }
        start = i.ip();
    }
    for (_, j) in insns.iter() {
        if j.flow_control() != FlowControl::ConditionalBranch || j.near_branch_target() != start { continue; }
        let prev: Vec<&Instruction> = std::iter::once(load).chain(std::iter::once(j)).chain(insns.range(..j.ip()).rev().take(16).map(|(_, i)| i)).collect();
        if let Some(b) = table_bound(&prev, load.ip(), index) { return Some(b); }
    }
    None
}
/// A `switch` of code that is not position-independent (a static Linux program): `jmp [I*8+T]`, T a table of
/// absolute addresses in data. Bounded by the `cmp I,N; ja` before it, else read while the entries land between the
/// function's start and the next function a symbol names.
fn jump_table_abs64(img: &Image, insns: &BTreeMap<u64, Instruction>, jmp: &Instruction, entry: u64) -> Option<Vec<u64>> {
    if jmp.op0_kind() != OpKind::Memory || jmp.memory_base() != Register::None || jmp.memory_index() == Register::None || jmp.memory_index_scale() != 8 || !matches!(jmp.segment_prefix(), Register::None | Register::DS) {   // (DS: `notrack`, CET's)
        return None;
    }
    let table = jmp.memory_displacement64();
    if img.is_code(table) { return None; }
    let prev: Vec<&Instruction> = std::iter::once(jmp).chain(insns.range(..jmp.ip()).rev().take(32).map(|(_, i)| i)).collect();
    let bound = table_bound(&prev, jmp.ip(), jmp.memory_index().full_register()).or_else(|| table_bound_by_branch(insns, jmp, jmp.memory_index().full_register()));
    let end = img.seeds.iter().copied().find(|&s| s > entry).unwrap_or(u64::MAX);
    let mut out = Vec::new();
    for k in 0..bound.unwrap_or(1024).min(4096) {
        let Some(b) = img.bytes(table + 8 * k) else { break };
        if b.len() < 8 { break; }
        let t = u64::from_le_bytes(b[..8].try_into().unwrap());
        if !img.is_code(t) || (bound.is_none() && (t < entry || t >= end)) { if bound.is_some() { return None; } break; }
        out.push(t);
    }
    if out.is_empty() { None } else { Some(out) }
}
fn jump_table(img: &Image, insns: &BTreeMap<u64, Instruction>, jmp: &Instruction, range: Option<(u64, u64)>) -> Option<Vec<u64>> {
    let target = jmp.op0_register().full_register();
    let prev: Vec<&Instruction> = insns.range(..jmp.ip()).rev().take(64).map(|(_, i)| i).collect();
    // the load of the entry into R (or its 32-bit part) and the register it was rebased with
    let mut load = None;
    let mut add_base = None;
    for i in &prev {
        if i.op_count() >= 1 && i.op_kind(0) == OpKind::Register && i.op0_register().full_register() == target {
            match i.mnemonic() {
                Mnemonic::Add if i.op_kind(1) == OpKind::Register && add_base.is_none() && load.is_none() => add_base = Some(i.op_register(1).full_register()),
                Mnemonic::Movsxd | Mnemonic::Mov if i.op_kind(1) == OpKind::Memory && i.memory_index() != Register::None && i.memory_index_scale() == 4 => {
                    load = Some(**i);
                    break;
                }
                _ => {}
            }
        }
    }
    let load = load?;
    let base_reg = load.memory_base().full_register();
    let index = load.memory_index().full_register();
    // where the base register points: the closest `lea base,[rip+X]` before the load in this function
    let lea_of = |r: Register| -> Option<u64> {
        let mut found = None;
        for (&ip, i) in insns.range(..load.ip()).rev() {
            let _ = ip;
            if i.mnemonic() == Mnemonic::Lea && i.op0_register().full_register() == r && i.is_ip_rel_memory_operand() {
                found = Some(i.ip_rel_memory_address());
                break;
            }
        }
        found.or_else(|| insns.values().find(|i| i.mnemonic() == Mnemonic::Lea && i.op0_register().full_register() == r && i.is_ip_rel_memory_operand()).map(|i| i.ip_rel_memory_address()))
    };
    let base = lea_of(base_reg)?;
    let (table, rebase) = if img.is_base(base) {
        (base + load.memory_displacement64(), base) // RVAs, rebased by the image base
    } else {
        (base.wrapping_add(load.memory_displacement64()), add_base.and_then(lea_of).unwrap_or(base))
    };
    // the index computed from a compared register by non-decreasing steps (MSVC's memset / memcpy:
    // `cmp R8,0x80; jae loop; lea R9,[R8+0xF]; and R9,-16; mov R11,R9; shr R11,4; mov R11D,[R10+R11*4+rva]`):
    // the compared maximum carried forward to the load. Tried first: it follows the value live at the load,
    // where the backward search below can pick a compare of an older value of the same register.
    let sim_at = |at: u64, index: Register| -> Option<u64> {
        let fwd: Vec<&Instruction> = prev.iter().rev().copied().collect(); // in program order, up to the jump
        let at_load = fwd.iter().position(|i| i.ip() == at)?;
        for c in (0..at_load).rev() {
            let i = fwd[c];
            if i.mnemonic() != Mnemonic::Cmp || i.op_kind(0) != OpKind::Register || !is_imm(i.op_kind(1)) || c + 1 >= at_load { continue; }
            let max = match fwd[c + 1].mnemonic() { Mnemonic::Jae | Mnemonic::Jb => i.immediate(1).wrapping_sub(1), Mnemonic::Ja | Mnemonic::Jbe => i.immediate(1), _ => continue };
            let mut vals: std::collections::HashMap<Register, u64> = std::collections::HashMap::new();
            vals.insert(i.op0_register().full_register(), max);
            for j in &fwd[c + 2..at_load] {
                if j.op_count() == 0 || j.op_kind(0) != OpKind::Register { continue; }
                let d = j.op0_register().full_register();
                let src = |n: u32| if j.op_kind(n) == OpKind::Register { vals.get(&j.op_register(n).full_register()).copied() } else { None };
                let v = match j.mnemonic() {
                    Mnemonic::Mov | Mnemonic::Movzx | Mnemonic::Movsxd => src(1),
                    Mnemonic::Lea if j.memory_index() == Register::None => vals.get(&j.memory_base().full_register()).map(|b| b.wrapping_add(j.memory_displacement64())),
                    Mnemonic::And if is_imm(j.op_kind(1)) => vals.get(&d).map(|x| x & j.immediate(1)),
                    Mnemonic::Shr if is_imm(j.op_kind(1)) => vals.get(&d).map(|x| x >> (j.immediate(1) & 63)),
                    Mnemonic::Add if is_imm(j.op_kind(1)) => vals.get(&d).map(|x| x.wrapping_add(j.immediate(1))),
                    _ => None,
                };
                match v { Some(v) => { vals.insert(d, v); } None => { vals.remove(&d); } }
            }
            return vals.get(&index).copied().filter(|&v| v < 4096).map(|v| v + 1);
        }
        None
    };
    // MSVC's two-level switch: the index is a byte of an index table, `movzx I,byte [B+J+rva]` with the same image
    // base (`cmp J,N; ja default; movsxd J; movzx eax,[rdx+rax+itab]; mov ecx,[rdx+rax*4+tab]; add rcx,rdx; jmp rcx`):
    // the table has as many entries as the largest byte among the index table's J+1 says. Tried first: a compare
    // found for I itself would be J's, the index table's length
    let mut bound = None;
    if img.is_base(base) {
        let def = prev.iter().filter(|i| i.ip() < load.ip()).find(|i| i.op_count() >= 1 && i.op_kind(0) == OpKind::Register && i.op0_register().full_register() == index);
        if let Some(m) = def.filter(|m| m.mnemonic() == Mnemonic::Movzx && m.op_kind(1) == OpKind::Memory && m.memory_size() == iced_x86::MemorySize::UInt8
            && m.memory_base().full_register() == base_reg && m.memory_index() != Register::None && m.memory_index_scale() == 1) {
            let j = m.memory_index().full_register();
            if let Some(nj) = sim_at(m.ip(), j).or_else(|| table_bound(&prev, m.ip(), j)) {
                let itab = base + m.memory_displacement64();
                let max = (0..nj).filter_map(|k| img.u8(itab + k)).max();
                if let Some(mx) = max { bound = Some(u64::from(mx) + 1); }
            }
        }
    }
    let bound = bound.or_else(|| sim_at(load.ip(), index)).or_else(|| table_bound(&prev, load.ip(), index)).or_else(|| table_bound_by_branch(insns, &load, index));
    if bound.is_none() && std::env::var_os("LIFTER_DEBUG").is_some() { eprintln!("table without bound: jmp at 0x{:x}, load at 0x{:x}", jmp.ip(), load.ip()); }
    let bound = Some(bound?); // a table with no bound is not trusted: its end is unknown
    // a target may be in another part of the function (GCC's .cold): any code will do
    let _ = range;
    let inside = |t: u64| img.is_code(t);
    let mut out = Vec::new();
    for k in 0..bound.unwrap_or(4096).min(4096) {
        let Some(e) = img.u32(table + k * 4) else { break };
        let t = if load.mnemonic() == Mnemonic::Movsxd { rebase.wrapping_add(e as i32 as i64 as u64) } else { rebase.wrapping_add(u64::from(e)) };
        if !inside(t) {
            if bound.is_some() { return None; }
            break;
        }
        out.push(t);
    }
    (!out.is_empty()).then_some(out)
}

/// Where a value is while it is followed backwards through a function (registers as full registers).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Loc {
    Reg(Register),
    Mem(Register, Register, u32, u64), // base, index, scale, displacement
}
fn loc_of(i: &Instruction, n: u32) -> Option<Loc> {
    match i.op_kind(n) {
        OpKind::Register if i.op_register(n).is_gpr() => Some(Loc::Reg(i.op_register(n).full_register())),
        OpKind::Memory if i.segment_prefix() == Register::None => Some(Loc::Mem(i.memory_base(), i.memory_index(), i.memory_index_scale(), i.memory_displacement64())),
        _ => None,
    }
}
/// The most values an index may take for its table to be read.
const MAX_VALUES: usize = 4096;
type Pending = Vec<(Loc, BTreeSet<i64>)>;

/// 32-bit code: the values a register can hold at an indirect jump, found by walking the function backwards
/// along its control flow: `and I,mask`, `cmp I,N` + `ja` / `jb`, `sub I,N` + `jb` (the hand-written memcpy
/// indexes its tables with -N..-1), copies, constants, and index tables (`movzx I, byte ptr [J+table]`).
/// A way in that says nothing is left out: the jump keeps its run-time path (`rt_call`) for what was missed.
struct Values<'a> {
    img: &'a Image,
    f: &'a Func,
    sources: &'a BTreeMap<u64, Vec<u64>>, // a branch target -> the branches and table jumps that go there
    info: InstructionInfoFactory,
    budget: u32,
    seen: BTreeSet<(u64, Loc)>,
    data: Vec<(u64, u64)>, // index tables read on the way
}

impl Values<'_> {
    fn written(&mut self, i: &Instruction) -> Vec<Register> {
        self.info.info(i).used_registers().iter().filter(|u| matches!(u.access(), OpAccess::Write | OpAccess::CondWrite | OpAccess::ReadWrite | OpAccess::ReadCondWrite)).map(|u| u.register().full_register()).collect()
    }
    /// The values of `loc` just before the instruction at `ip` runs. `pend`: what later compares say about other places.
    fn before(&mut self, ip: u64, loc: Loc, pend: &Pending, depth: u32) -> Option<BTreeSet<i64>> {
        if let Some((_, s)) = pend.iter().find(|(l, _)| *l == loc) {
            return Some(s.clone());
        }
        if depth > 12 || self.budget == 0 || !self.seen.insert((ip, loc)) {
            return None;
        }
        let f = self.f;
        let mut edges: Vec<(u64, bool)> = Vec::new();
        if let Some((&p, i)) = f.insns.range(..ip).next_back() {
            if i.next_ip() == ip && matches!(i.flow_control(), FlowControl::Next | FlowControl::Call | FlowControl::IndirectCall | FlowControl::ConditionalBranch) {
                edges.push((p, false));
            }
        }
        if let Some(v) = self.sources.get(&ip) {
            edges.extend(v.iter().map(|&p| (p, true)));
        }
        let fork = u32::from(edges.len() > 1);
        let mut out = BTreeSet::new();
        let mut any = false;
        for (p, taken) in edges {
            if let Some(v) = self.after(p, taken, loc, pend, depth + fork) {
                out.extend(v);
                any = true;
            }
        }
        (any && out.len() <= MAX_VALUES).then_some(out)
    }
    /// The values of `loc` once the instruction at `p` ran and control went on (`taken`: along its branch).
    fn after(&mut self, p: u64, taken: bool, loc: Loc, pend: &Pending, depth: u32) -> Option<BTreeSet<i64>> {
        if self.budget == 0 {
            return None;
        }
        self.budget -= 1;
        let f = self.f;
        let i = &f.insns[&p];
        match i.flow_control() {
            FlowControl::ConditionalBranch => {
                let mut pend = pend.clone();
                if let Some((x, set)) = self.branch_says(i, taken) {
                    if x == loc {
                        return Some(set);
                    }
                    pend.push((x, set));
                }
                self.before(p, loc, &pend, depth)
            }
            FlowControl::UnconditionalBranch | FlowControl::IndirectBranch => self.before(p, loc, pend, depth),
            FlowControl::Call if is_near(i) && i.near_branch_target() == i.next_ip() => {
                // `call $+5` only pushes the address after it: position-independent code learns where it is so
                let top = Loc::Mem(Register::ESP, Register::None, 1, 0);
                if loc == top {
                    return Some([i.next_ip() as i64].into());
                }
                let back = |l: Loc| match l {
                    Loc::Mem(Register::ESP, x, sc, d) => Loc::Mem(Register::ESP, x, sc, u64::from((d as u32).wrapping_sub(4))),
                    l => l,
                };
                let pend: Pending = pend.iter().filter(|(l, _)| *l != top).map(|(l, s)| (back(*l), s.clone())).collect();
                self.before(p, back(loc), &pend, depth)
            }
            FlowControl::Call | FlowControl::IndirectCall => {
                // a call keeps EBX, ESI, EDI, EBP and (we assume) memory that is not addressed from ESP
                let lost = |l: &Loc| matches!(l, Loc::Reg(Register::RAX | Register::RCX | Register::RDX) | Loc::Mem(Register::ESP, ..));
                if lost(&loc) {
                    return None;
                }
                let pend: Pending = pend.iter().filter(|(l, _)| !lost(l)).cloned().collect();
                self.before(p, loc, &pend, depth)
            }
            FlowControl::Next => self.step(i, loc, pend, depth),
            _ => None,
        }
    }
    /// What the conditional branch `i` says on one of its edges: `cmp X,N` + below -> X in 0..N-1;
    /// `sub X,N` + below (a borrow) -> X in -N..-1.
    fn branch_says(&mut self, i: &Instruction, taken: bool) -> Option<(Loc, BTreeSet<i64>)> {
        use ConditionCode as C;
        let or_equal = match (i.condition_code(), taken) {
            (C::b, true) | (C::ae, false) => false,
            (C::be, true) | (C::a, false) => true,
            _ => return None,
        };
        // the instruction that set the flags: the closest one before the branch, across moves
        let f = self.f;
        let mut at = i.ip();
        let mut between: Vec<Register> = Vec::new();
        for _ in 0..32 {
            let (&q, qi) = f.insns.range(..at).next_back()?;
            if qi.next_ip() != at || qi.flow_control() != FlowControl::Next || self.sources.contains_key(&at) {
                return None;
            }
            if qi.rflags_modified() != 0 {
                let x = loc_of(qi, 0)?;
                if opsize(qi, 0) != 4 || qi.op_count() != 2 {
                    return None;
                }
                let touched = match x {
                    Loc::Reg(r) => between.contains(&r),
                    Loc::Mem(b, ix, _, _) => (b != Register::None && between.contains(&b.full_register())) || (ix != Register::None && between.contains(&ix.full_register())),
                };
                // the bound: an immediate, or a register that holds a constant (`mov edi,7 ... cmp eax,edi`; `push 15; pop ecx`)
                let bound = if is_imm(qi.op_kind(1)) {
                    i64::from(qi.immediate(1) as u32)
                } else if qi.mnemonic() == Mnemonic::Cmp && qi.op_kind(1) == OpKind::Register && qi.op_register(1).size() == 4 {
                    let v = self.before(q, Loc::Reg(qi.op_register(1).full_register()), &Vec::new(), 8)?;
                    if v.len() != 1 {
                        return None;
                    }
                    *v.first()?
                } else {
                    return None;
                };
                let n = bound + i64::from(or_equal);
                if touched || !(0..=MAX_VALUES as i64).contains(&n) {
                    return None;
                }
                return match qi.mnemonic() {
                    Mnemonic::Cmp => Some((x, (0..n).collect())),
                    Mnemonic::Sub => Some((x, (0..n).map(|v| v - bound).collect())),
                    _ => None,
                };
            }
            between.extend(self.written(qi));
            at = q;
        }
        None
    }
    /// Backwards across a plain instruction: it leaves the value alone, copies it, or computes it.
    fn step(&mut self, i: &Instruction, loc: Loc, pend: &Pending, depth: u32) -> Option<BTreeSet<i64>> {
        use Mnemonic as M;
        let (ip, m) = (i.ip(), i.mnemonic());
        let w = self.written(i);
        let acc0 = self.info.info(i).op0_access();
        let writes0 = i.op_count() > 0 && matches!(acc0, OpAccess::Write | OpAccess::CondWrite | OpAccess::ReadWrite | OpAccess::ReadCondWrite);
        let imm1 = i.op_count() == 2 && is_imm(i.op_kind(1));
        // places addressed from ESP move when ESP does
        let sp: Option<i64> = if matches!(m, M::Add | M::Sub) && i.op_kind(0) == OpKind::Register && i.op0_register() == Register::ESP && imm1 {
            let n = i64::from(i.immediate(1) as u32 as i32);
            Some(if m == M::Add { n } else { -n })
        } else if i.stack_pointer_increment() != 0 {
            Some(i64::from(i.stack_pointer_increment()))
        } else if w.contains(&Register::RSP) {
            None
        } else {
            Some(0)
        };
        let wide = |n: u32| match i.op_kind(n) {
            OpKind::Register => i.op_register(n).size() == 4,
            OpKind::Memory => i.memory_size().size() == 4,
            _ => false,
        };
        let dst = if i.op_count() > 0 { loc_of(i, 0) } else { None };
        let copy: Option<(Loc, Loc)> = if m == M::Mov && i.op_count() == 2 && wide(0) && wide(1) { loc_of(i, 0).zip(loc_of(i, 1)) } else { None };
        // a place named after the instruction, as the place that held its value before; None: the instruction made the value
        let top = Loc::Mem(Register::ESP, Register::None, 1, 0);
        let carry = |l: Loc| -> Option<Loc> {
            if let Some((x, y)) = copy {
                if l == x {
                    return Some(y);
                }
            }
            if m == M::Push && l == top {
                return if i.op_kind(0) == OpKind::Register && i.op0_register().size() == 4 { Some(Loc::Reg(i.op0_register().full_register())) } else { None };
            }
            if m == M::Pop && i.op_kind(0) == OpKind::Register && i.op0_register().size() == 4 && l == Loc::Reg(i.op0_register().full_register()) {
                return Some(top); // what was on top of the stack
            }
            match l {
                Loc::Reg(r) => (!w.contains(&r)).then_some(l),
                Loc::Mem(b, x, sc, d) => {
                    if (writes0 && dst == Some(l)) || (x != Register::None && w.contains(&x.full_register())) {
                        None
                    } else if b == Register::ESP {
                        sp.map(|delta| Loc::Mem(b, x, sc, u64::from((d as u32).wrapping_add(delta as u32))))
                    } else {
                        (b == Register::None || !w.contains(&b.full_register())).then_some(l)
                    }
                }
            }
        };
        let pend: Pending = pend.iter().filter_map(|(l, s)| carry(*l).map(|l| (l, s.clone()))).collect();
        if let Some(l) = carry(loc) {
            return self.before(ip, l, &pend, depth);
        }
        let Loc::Reg(r) = loc else {
            if m == M::Push && loc == top && is_imm(i.op_kind(0)) {
                return Some([i64::from(i.immediate(0) as u32)].into());
            }
            return (m == M::Mov && dst == Some(loc) && imm1 && wide(0)).then(|| [i64::from(i.immediate(1) as u32)].into());
        };
        if !(i.op_count() > 0 && i.op_kind(0) == OpKind::Register && i.op0_register().full_register() == r) {
            return None; // written on the side (pop, xchg, mul, ...)
        }
        let full = wide(0);
        let n = if imm1 { i.immediate(1) as u32 } else { 0 };
        let same = i.op_count() == 2 && i.op_kind(1) == OpKind::Register && i.op_register(1) == i.op0_register();
        let map = |s: BTreeSet<i64>, g: &dyn Fn(i64) -> i64| -> BTreeSet<i64> { s.into_iter().map(g).collect() };
        match m {
            M::Mov if full && imm1 => Some([i64::from(n)].into()),
            M::Xor | M::Sub if full && same => Some([0].into()),
            M::And if full && imm1 && (n as usize) < MAX_VALUES => Some((0..=i64::from(n)).collect()),
            M::And if full && i.op_kind(1) == OpKind::Register => {
                let s = self.before(ip, loc, &pend, depth)?;
                (*s.first()? >= 0).then(|| (0..=*s.last().unwrap()).collect())
            }
            // an index table: `movzx I, byte ptr [J+table]`, or `mov I8, [J+table]` into a register that was cleared
            M::Movzx | M::Mov if i.op_kind(1) == OpKind::Memory && i.memory_size().size() == 1 && (full || (m == M::Mov && i.op0_register().size() == 1 && !high8(i.op0_register()))) => {
                let (b, x, table) = (i.memory_base(), i.memory_index(), i.memory_displacement64());
                let j = if x == Register::None { b } else if b == Register::None && i.memory_index_scale() == 1 { x } else { Register::None };
                if j == Register::None || j.size() != 4 || self.img.bytes(table).is_none() || i.segment_prefix() != Register::None {
                    return (m == M::Movzx).then(|| (0..=255).collect());
                }
                let s = self.before(ip, Loc::Reg(j.full_register()), &pend, depth)?;
                if m == M::Mov {
                    // the rest of the register must be clear (`xor I,I` before, or I is the small index itself)
                    let rest = if j.full_register() == r { s.clone() } else { self.before(ip, loc, &pend, depth)? };
                    if !rest.iter().all(|v| (0..=255).contains(v)) {
                        return None;
                    }
                }
                let mut out = BTreeSet::new();
                for v in &s {
                    out.insert(i64::from(*self.img.bytes((table as i64 + v) as u64 & 0xFFFF_FFFF)?.first()?));
                }
                let (lo, hi) = (*s.first()?, *s.last()?);
                self.data.push(((table as i64 + lo) as u64, (table as i64 + hi + 1) as u64));
                Some(out)
            }
            M::Movzx if full && opsize(i, 1) == 1 => Some((0..=255).collect()),
            // a position-independent table (LLVM): `call $+5; pop R ... add R,[I*4+table]; jmp R`, where the table
            // holds each target's distance from the address R was loaded with
            M::Add if full && i.op_kind(1) == OpKind::Memory && i.memory_size().size() == 4 && i.memory_base() == Register::None && i.memory_index() != Register::None && i.memory_index_scale() == 4 && i.segment_prefix() == Register::None => {
                let table = i.memory_displacement64() as i64;
                let bases = self.before(ip, loc, &pend, depth)?;
                let index = self.before(ip, Loc::Reg(i.memory_index().full_register()), &pend, depth)?;
                if bases.len() * index.len() > MAX_VALUES {
                    return None;
                }
                let mut out = BTreeSet::new();
                for &k in &index {
                    let e = self.img.u32((table + 4 * k) as u64 & 0xFFFF_FFFF)?;
                    out.extend(bases.iter().map(|b| (b + i64::from(e as i32)) & 0xFFFF_FFFF));
                }
                self.data.push(((table + 4 * index.first()?) as u64 & 0xFFFF_FFFF, (table + 4 * index.last()? + 4) as u64 & 0xFFFF_FFFF));
                Some(out)
            }
            M::Add | M::Sub if full && imm1 => {
                let d = i64::from(n as i32) * if m == M::Add { 1 } else { -1 };
                Some(map(self.before(ip, loc, &pend, depth)?, &|v| v + d))
            }
            M::Inc | M::Dec if full => {
                let d = if m == M::Inc { 1 } else { -1 };
                Some(map(self.before(ip, loc, &pend, depth)?, &|v| v + d))
            }
            M::Lea if full && i.memory_index() == Register::None && i.memory_base().size() == 4 => {
                let d = i64::from(i.memory_displacement64() as u32 as i32);
                Some(map(self.before(ip, Loc::Reg(i.memory_base().full_register()), &pend, depth)?, &|v| v + d))
            }
            M::Neg if full => Some(map(self.before(ip, loc, &pend, depth)?, &|v| -v)),
            M::Shr | M::Shl if full && imm1 && n < 32 => {
                let s = self.before(ip, loc, &pend, depth)?;
                (*s.first()? >= 0).then(|| map(s, &|v| if m == M::Shr { v >> n } else { v << n }))
            }
            _ => None,
        }
    }
}

/// 32-bit code: the targets of the indirect jumps `pending` of `f`, each with the table bytes read for it.
/// - `jmp dword ptr [I*4+table]`: a table of code addresses (Visual C++ puts it after the function), read for
///   the values the index can take; an entry that is no code address is left out (memcpy's LeadVec-4);
/// - the same behind `movzx I, byte ptr [J+index_table]`: the bound is the index table's;
/// - `jmp reg`: the code addresses the register was loaded with in this function, or the position-independent
///   table LLVM makes (`call $+5; pop R; add R,[I*4+table]; jmp R`).
/// A table whose index has no bound is not trusted and stays an indirect jump.
fn jump_tables32(img: &Image, f: &Func, pending: &[u64]) -> Vec<(u64, Vec<u64>, Vec<(u64, u64)>)> {
    let debug = std::env::var_os("LIFTER_DEBUG").is_some();
    let mut sources: BTreeMap<u64, Vec<u64>> = BTreeMap::new();
    for (&ip, i) in &f.insns {
        if matches!(i.flow_control(), FlowControl::ConditionalBranch | FlowControl::UnconditionalBranch) && is_near(i) {
            sources.entry(i.near_branch_target()).or_default().push(ip);
        }
    }
    for (&ip, ts) in &f.tables {
        for t in ts.iter().collect::<BTreeSet<_>>() {
            sources.entry(*t).or_default().push(ip);
        }
    }
    let mut out = Vec::new();
    for &ip in pending {
        let jmp = &f.insns[&ip];
        let mut v = Values { img, f, sources: &sources, info: InstructionInfoFactory::new(), budget: 3000, seen: BTreeSet::new(), data: Vec::new() };
        let mut targets: Vec<u64> = Vec::new();
        if jmp.op0_kind() == OpKind::Register {
            let Some(s) = v.before(ip, Loc::Reg(jmp.op0_register().full_register()), &Vec::new(), 0) else { continue };
            if !s.iter().all(|&t| t >= 0 && img.is_code(t as u64)) {
                continue;
            }
            targets.extend(s.iter().map(|&t| t as u64));
        } else if is_table_jump(jmp) {
            let table = jmp.memory_displacement64() as i64;
            let index = jmp.memory_index().full_register();
            let set = v.before(ip, Loc::Reg(index), &Vec::new(), 0).or_else(|| {
                // the 64-bit lifter's rule: the closest `cmp I,N` or `and I,mask` before the jump, in address order
                let prev: Vec<&Instruction> = f.insns.range(..=ip).rev().take(65).map(|(_, i)| i).collect();
                let n = table_bound(&prev, ip, index)?;
                if debug { eprintln!("table at 0x{ip:x}: bound {n} by address order"); }
                Some((0..n.min(MAX_VALUES as u64) as i64).collect())
            });
            let Some(set) = set.filter(|s| !s.is_empty()) else {
                if debug { eprintln!("table without bound: jmp at 0x{ip:x}"); }
                continue;
            };
            for &k in &set {
                let at = (table + 4 * k) as u64 & 0xFFFF_FFFF;
                match img.u32(at).map(u64::from).filter(|&t| img.is_code(t)) {
                    Some(t) => targets.push(t),
                    None => if debug { eprintln!("table at 0x{ip:x}: entry {k} (0x{at:x}) is no code address"); },
                }
            }
            v.data.push(((table + 4 * set.first().unwrap()) as u64 & 0xFFFF_FFFF, (table + 4 * set.last().unwrap() + 4) as u64 & 0xFFFF_FFFF));
        } else {
            continue; // through a pointer in memory: a tail call (an import, a virtual method, a function pointer)
        }
        targets.sort();
        targets.dedup();
        if !targets.is_empty() {
            out.push((ip, targets, v.data));
        }
    }
    out
}
/// A machine's task switch: `mov rsp, [mem]` in __switch_to_asm. Each task has a host stack of its own
/// (rt/microvm-x64/run.c): the runtime switches them there.
fn is_switch(img: &Image, i: &Instruction) -> bool {
    img.machine && i.mnemonic() == Mnemonic::Mov && i.op_count() == 2 && i.op_kind(0) == OpKind::Register && i.op0_register() == Register::RSP && i.op_kind(1) == OpKind::Memory
        && img.named.get("__switch_to_asm").is_some_and(|&(a, n)| i.ip() >= a && i.ip() < a + n.max(1))
}
/// `jmp dword ptr [I*4+table]`: the form of a `switch` in 32-bit code.
fn is_table_jump(i: &Instruction) -> bool {
    i.op0_kind() == OpKind::Memory && i.memory_base() == Register::None && i.memory_index() != Register::None && i.memory_index_scale() == 4 && i.segment_prefix() == Register::None
}

/// Decodes the function at `entry` by following its control flow. `entries` are the entries that cut a function:
/// a jump to one of them leaves this function (it becomes a call). Returns the function, the entries it refers
/// to by a call or a jump (hard ones) and, in 32-bit code, the code addresses it holds as immediates (soft ones).
fn decode_func(img: &Image, entry: u64, entries: &BTreeSet<u64>) -> (Func, BTreeSet<u64>, BTreeSet<u64>) {
    let m32 = img.bits == 32;
    let range_end = img.pdata.get(&entry).copied();
    let mut f = Func { insns: BTreeMap::new(), leaders: BTreeSet::new(), tables: BTreeMap::new(), pads: BTreeSet::new(), data: Vec::new(), bad: false, alts: BTreeSet::new(), rets: BTreeSet::new() };
    let mut calls = BTreeSet::new();
    let mut taken = BTreeSet::new();
    let mut refs = BTreeSet::new();
    let mut pending: Vec<u64> = Vec::new(); // 32-bit code: indirect jumps, resolved once what leads to them is decoded
    let mut work = vec![entry];
    f.leaders.insert(entry);
    let outside = |t: u64| (t != entry && entries.contains(&t)) || range_end.is_some_and(|e| t < entry || t >= e);
    for pad in landing_pads(img, entry) {
        if outside(pad) {
            calls.insert(pad); // in another part of the function (GCC's .cold): an entry of its own
        } else {
            f.leaders.insert(pad);
            work.push(pad);
        }
        f.pads.insert(pad);
    }
    for _round in 0..16 {
        while let Some(start) = work.pop() {
            let mut ip = start;
            loop {
                if f.insns.contains_key(&ip) {
                    break;
                }
                if m32 && !img.is_code(ip) {
                    f.bad = true;
                    break;
                }
                let Some(bytes) = img.bytes(ip) else { f.bad = true; break };
                let mut d = Decoder::with_ip(img.bits, &bytes[..bytes.len().min(16)], ip, DecoderOptions::NONE);
                let ins = d.decode();
                if ins.is_invalid() {
                    f.bad = true;
                    break;
                }
                f.insns.insert(ip, ins);
                if ins.mnemonic() == Mnemonic::Lea && ins.is_ip_rel_memory_operand() && img.is_code(ins.ip_rel_memory_address()) {
                    taken.insert(ins.ip_rel_memory_address());
                }
                // 32-bit code has no RIP-relative `lea`: an address is taken as an immediate (`push offset f`, `mov [x], offset f`)
                if m32 && matches!(ins.mnemonic(), Mnemonic::Push | Mnemonic::Mov) {
                    for n in 0..ins.op_count() {
                        if ins.op_kind(n) == OpKind::Immediate32 && img.is_code(u64::from(ins.immediate32())) {
                            refs.insert(u64::from(ins.immediate32()));
                        }
                    }
                }
                let next = ins.next_ip();
                match ins.flow_control() {
                    FlowControl::Next => {}
                    FlowControl::Call if ins.mnemonic() == Mnemonic::Syscall => { f.leaders.insert(next); f.rets.insert(next); } // the runtime's (rt_syscall): it returns here
                    FlowControl::Call => {
                        if !m32 {
                            calls.insert(ins.near_branch_target());
                        } else if is_near(&ins) && ins.near_branch_target() != next {
                            calls.insert(ins.near_branch_target()); // (`call $+5` only pushes its return address)
                            if is_setjmp_thunk(img, ins.near_branch_target()) { f.pads.insert(next); work.push(next); }   // (a longjmp resumes there)
                        }
                        f.leaders.insert(next);
                        f.rets.insert(next);
                        if !m32 && is_import_thunk(img, ins.near_branch_target(), "_CxxThrowException") { calls.extend(throw_info_functions(img, &f.insns, &ins)); }
                    }
                    FlowControl::IndirectCall => {
                        f.leaders.insert(next);
                        f.rets.insert(next);
                        // 32-bit `call [__imp__setjmp3]`: a longjmp resumes after the call, in this frame (rt/msvcrt.c);
                        // its return address is a landing pad
                        if m32 && ins.op0_kind() == OpKind::Memory && ins.memory_base() == Register::None && ins.memory_index() == Register::None {
                            let slot = ins.memory_displacement64();
                            if is_setjmp_slot(img, slot) {
                                f.pads.insert(next);
                                work.push(next);
                            }
                        }
                        if !m32 && ins.is_ip_rel_memory_operand() && img.imports.iter().any(|(slot, n)| *slot == ins.ip_rel_memory_address() && n.ends_with("!_CxxThrowException")) {
                            calls.extend(throw_info_functions(img, &f.insns, &ins));
                        }
                    }
                    FlowControl::ConditionalBranch => {
                        let t = ins.near_branch_target();
                        if m32 && outside(t) {
                            calls.insert(t); // emitted as a call: its code is not part of this function
                        } else {
                            f.leaders.insert(t);
                            work.push(t);
                        }
                        f.leaders.insert(next);
                    }
                    FlowControl::UnconditionalBranch => {
                        if m32 && !is_near(&ins) {
                            break; // a far jump: not lifted
                        }
                        let t = ins.near_branch_target();
                        if outside(t) {
                            calls.insert(t);
                        } else {
                            f.leaders.insert(t);
                            work.push(t);
                        }
                        break;
                    }
                    FlowControl::IndirectBranch => {
                        if m32 {
                            pending.push(ip);
                        } else {
                            let found = if ins.op0_kind() == OpKind::Register { jump_table(img, &f.insns, &ins, range_end.map(|e| (entry, e))) }
                                else if img.elf { jump_table_abs64(img, &f.insns, &ins, entry) } else { None };
                            if let Some(ts) = found {
                                if ins.op0_kind() == OpKind::Memory { let t = ins.memory_displacement64(); f.data.push((t, t + 8 * ts.len() as u64)); }   // (its slots are no code pointers: lift)
                                for &t in &ts {
                                    if outside(t) {
                                        calls.insert(t);
                                    } else {
                                        f.leaders.insert(t);
                                        work.push(t);
                                    }
                                }
                                f.tables.insert(ip, ts);
                            }
                        }
                        break;
                    }
                    // (one before another function's start, as a BUG()'s ud2 or the int3s between functions, ends this one)
                    FlowControl::Interrupt | FlowControl::Exception if img.machine && matches!(ins.mnemonic(), Mnemonic::Int3 | Mnemonic::Int | Mnemonic::Ud2 | Mnemonic::Into) => {
                        // (and int3 followed by int3 is padding: the fill between functions and up to an alignment)
                        if entries.contains(&next) || !img.is_code(next) || ins.mnemonic() == Mnemonic::Int3 && img.u8(next) == Some(0xCC) { break; }
                        // a handler may also come back there by a return, or past the 5-byte call or jump an int3 stands
                        // for (Linux emulates one at an int3 while it patches code, and so does its boot's self-test)
                        f.leaders.insert(next); f.rets.insert(next);
                        if ins.mnemonic() == Mnemonic::Int3 { f.leaders.insert(ip + 5); f.rets.insert(ip + 5); }
                    }
                    _ => break, // ret, int3, ud2, ...
                }
                // a machine's task switch (__switch_to_asm's load of the next task's stack): a new task starts after it
                if is_switch(img, &ins) { f.leaders.insert(next); f.rets.insert(next); }
                if m32 && outside(next) {
                    break; // runs into another function: emitted as a call to it
                }
                ip = next;
            }
        }
        if pending.is_empty() {
            break;
        }
        // 32-bit code: the jump tables, now that the code leading to each indirect jump is decoded; their targets
        // are decoded in turn, which may show more ways into a jump
        let mut data = Vec::new();
        for (ip, ts, d) in jump_tables32(img, &f, &pending) {
            data.extend(d);
            if f.tables.get(&ip) == Some(&ts) {
                continue;
            }
            for &t in &ts {
                if outside(t) {
                    calls.insert(t);
                } else if !f.insns.contains_key(&t) || !f.leaders.contains(&t) {
                    f.leaders.insert(t);
                    work.push(t);
                }
            }
            f.tables.insert(ip, ts);
        }
        f.data = data;
        if work.is_empty() {
            break;
        }
    }
    calls.extend(taken.into_iter().filter(|t| !f.insns.contains_key(t)));
    (f, calls, refs)
}

struct Emit<'a> {
    trace_pc: bool,                    // --trace-pc: each block stores its address in rt_last_pc (the place a trap was)
    img: &'a Image,
    unimpl: BTreeMap<String, u32>,
    /// AVX-and-later instructions: lifted as traps (the virtual CPU does not report AVX, so code that checks
    /// CPUID never runs them; the rest of their function still runs)
    avx: u32,
    /// `jmp reg` with no table found: a tail call through a pointer, or a switch the lifter missed
    unresolved: u32,
    /// the entries that cut a function: a jump to one is a call. Every entry in 64-bit code, the hard ones in 32-bit code
    cut: &'a BTreeSet<u64>,
    /// 32-bit code: the instructions already counted (an instruction can be in several functions there)
    counted: BTreeSet<u64>,
    /// 32-bit code: the indirect jumps left unresolved, and the count of jumps through a pointer in memory
    unresolved_at: BTreeSet<u64>,
    pointer_jumps: BTreeSet<u64>,
}

/// An instruction of 32-bit code (the decoder's mode is kept in each instruction).
fn is32(i: &Instruction) -> bool {
    i.code_size() == CodeSize::Code32
}

fn gpr(r: Register) -> usize {
    r.full_register().number()
}
fn high8(r: Register) -> bool {
    matches!(r, Register::AH | Register::CH | Register::DH | Register::BH)
}
fn rreg(r: Register) -> String {
    let n = gpr(r);
    match r.size() {
        8 => format!("R[{n}]"),
        4 => format!("(u32)R[{n}]"),
        2 => format!("(u16)R[{n}]"),
        _ if high8(r) => format!("(u8)(R[{n}]>>8)"),
        _ => format!("(u8)R[{n}]"),
    }
}
fn wreg(r: Register, v: &str) -> String {
    let n = gpr(r);
    match r.size() {
        8 => format!("R[{n}]=(u64)({v});"),
        4 => format!("R[{n}]=(u32)({v});"),
        2 => format!("R[{n}]=(R[{n}]&~0xFFFFull)|(u16)({v});"),
        _ if high8(r) => format!("R[{n}]=(R[{n}]&~0xFF00ull)|((u64)(u8)({v})<<8);"),
        _ => format!("R[{n}]=(R[{n}]&~0xFFull)|(u8)({v});"),
    }
}
/// The address of the memory operand of a 32-bit instruction, as a 32-bit number: the sum wraps at 4 GiB
/// whatever the registers' upper halves hold. FS and GS add the thread's segment bases.
fn ea32(i: &Instruction) -> String {
    let (b, x, d) = (i.memory_base(), i.memory_index(), i.memory_displacement64());
    let a16 = b.size() == 2 || x.size() == 2; // 16-bit addressing (prefix 67h): the sum wraps at 64 KiB
    let seg = match i.segment_prefix() {
        Register::FS => "c->fs_base",
        Register::GS => "c->gs_base",
        _ => "",
    };
    let mut s = String::new();
    if b != Register::None {
        write!(s, "R[{}]", gpr(b)).unwrap();
    }
    if x != Register::None {
        write!(s, "{}R[{}]*{}", if s.is_empty() { "" } else { "+" }, gpr(x), i.memory_index_scale()).unwrap();
    }
    if s.is_empty() && seg.is_empty() {
        return format!("(0x{d:x}ull)");
    }
    let off = if a16 { i64::from(d as u16 as i16) } else { i64::from(d as u32 as i32) };
    if off != 0 || s.is_empty() {
        write!(s, "{}0x{:x}", if off < 0 { "-" } else if s.is_empty() { "" } else { "+" }, off.unsigned_abs()).unwrap();
    }
    if a16 {
        s = format!("(u16)({s})");
    }
    if !seg.is_empty() {
        write!(s, " + {seg}").unwrap();   // (spaced: `0x4e+c->fs_base` would be one number token to C, an exponent)
    }
    format!("((u32)({s}))")
}
fn ea(i: &Instruction) -> String {
    if is32(i) {
        return ea32(i);
    }
    if i.is_ip_rel_memory_operand() {
        return format!("0x{:x}ull", i.ip_rel_memory_address());
    }
    let mut s = String::new();
    if i.memory_base() != Register::None {
        write!(s, "R[{}]", gpr(i.memory_base())).unwrap();
    }
    if i.memory_index() != Register::None {
        write!(s, "{}R[{}]*{}", if s.is_empty() { "" } else { "+" }, gpr(i.memory_index()), i.memory_index_scale()).unwrap();
    }
    let d = i.memory_displacement64();
    if d != 0 || s.is_empty() {
        write!(s, "{}0x{:x}ull", if s.is_empty() { "" } else { "+" }, d).unwrap();
    }
    if i.segment_prefix() == Register::GS {
        s.push_str("+c->gs_base");
    }
    if i.segment_prefix() == Register::FS {
        s.push_str("+c->fs_base"); // a Linux thread's pointer (thread-local storage, the stack protector's canary)
    }
    format!("({s})")
}
fn is_imm(k: OpKind) -> bool {
    !matches!(k, OpKind::Register | OpKind::Memory)
}
/// Size in bytes of integer operand `n`.
fn opsize(i: &Instruction, n: u32) -> usize {
    match i.op_kind(n) {
        OpKind::Register => i.op_register(n).size(),
        OpKind::Memory => i.memory_size().size(),
        _ => opsize(i, 0),
    }
}
fn rd(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => rreg(i.op_register(n)),
        OpKind::Memory => format!("M{}({})", opsize(i, n) * 8, ea(i)),
        _ => format!("(u{})0x{:x}ull", opsize(i, 0) * 8, i.immediate(n)),
    }
}
fn wr(i: &Instruction, n: u32, v: &str) -> String {
    match i.op_kind(n) {
        OpKind::Register => wreg(i.op_register(n), v),
        _ => format!("M{}({})=({});", opsize(i, n) * 8, ea(i), v),
    }
}
fn xmm(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => format!("X[{}]", i.op_register(n).number()),
        _ => format!("MV({})", ea(i)),
    }
}
fn cond(cc: ConditionCode) -> &'static str {
    match cc {
        ConditionCode::o => "f.of",
        ConditionCode::no => "!f.of",
        ConditionCode::b => "f.cf",
        ConditionCode::ae => "!f.cf",
        ConditionCode::e => "f.zf",
        ConditionCode::ne => "!f.zf",
        ConditionCode::be => "(f.cf|f.zf)",
        ConditionCode::a => "!(f.cf|f.zf)",
        ConditionCode::s => "f.sf",
        ConditionCode::ns => "!f.sf",
        ConditionCode::p => "f.pf",
        ConditionCode::np => "!f.pf",
        ConditionCode::l => "(f.sf!=f.of)",
        ConditionCode::ge => "(f.sf==f.of)",
        ConditionCode::le => "(f.zf|(f.sf!=f.of))",
        ConditionCode::g => "!(f.zf|(f.sf!=f.of))",
        ConditionCode::None => "1",
    }
}

/// MMX: eight 64-bit registers of their own (c->mm[], rt/cpu.h). On a real CPU they are the x87 registers' mantissas;
/// here they are apart from them, which code that keeps to the rule (EMMS between MMX and x87 code) cannot tell.
/// Each operation is the SSE2 one on the low half of a vector, where the two agree; the unpacks of the high halves
/// and the packs are arranged so that they do. None: not an MMX instruction.
fn mmx(i: &Instruction) -> Option<String> {
    use Mnemonic as M;
    let m = i.mnemonic();
    let is_mm = |n: u32| n < i.op_count() && i.op_kind(n) == OpKind::Register && i.op_register(n).is_mm();
    if !(0..i.op_count()).any(is_mm) && !matches!(m, M::Emms | M::Femms) { return None; }
    if i.encoding() == iced_x86::EncodingKind::D3NOW { return None; }   // 3DNow!: not reported by the virtual CPU
    let reg = |n: u32| i.op_register(n).number();
    // an operand as 64 bits: an MMX or XMM register's (low) bits, memory, or a general register
    let src = |n: u32| -> String {
        match i.op_kind(n) {
            OpKind::Register if i.op_register(n).is_mm() => format!("c->mm[{}]", reg(n)),
            OpKind::Register if i.op_register(n).is_xmm() => format!("X[{}].q[0]", reg(n)),
            OpKind::Register => format!("(u64){}", rreg(i.op_register(n))),
            OpKind::Memory => format!("M64({})", ea(i)),
            _ => format!("{}ull", i.immediate(n)),
        }
    };
    let d = if i.op_count() > 0 && is_mm(0) { reg(0) } else { 0 };
    // d = f(d, s) on vectors whose low halves are the MMX registers
    let vop = |e: &str| format!("{{V a=vfrom64(c->mm[{d}]),s=vfrom64({});V r={e};c->mm[{d}]=r.q[0];}}", src(1));
    let lanes = |fld: &str, op: &str| vop(&format!("({{V t;t.{fld}=a.{fld}{op}s.{fld};t;}})"));
    let cmp = |fld: &str, op: &str| vop(&format!("({{V t;t.{fld}=(typeof(t.{fld}))(a.{fld}{op}s.{fld});t;}})"));
    let named = |f: &str| vop(&format!("{f}(a,s)"));
    let high = |f: &str| vop(&format!("{f}(vfrom64(a.q[0]>>32),vfrom64(s.q[0]>>32))"));
    let pack = |f: &str| vop(&format!("{f}(punpcklqdq(a,s),vzero())"));
    let shift = |f: &str| {
        let n = if is_imm(i.op_kind(1)) { format!("{}", i.immediate(1)) } else { src(1) };
        format!("{{V r={f}(vfrom64(c->mm[{d}]),{n});c->mm[{d}]=r.q[0];}}")
    };
    Some(match m {
        M::Emms | M::Femms => String::new(),
        M::Movd => if is_mm(0) { format!("c->mm[{d}]=(u32)({});", if i.op_kind(1) == OpKind::Memory { format!("M32({})", ea(i)) } else { rreg(i.op_register(1)) }) }
            else if i.op_kind(0) == OpKind::Memory { format!("M32({})=(u32)c->mm[{}];", ea(i), reg(1)) }
            else { wreg(i.op_register(0), &format!("(u32)c->mm[{}]", reg(1))) },
        M::Movq | M::Movntq => if is_mm(0) { format!("c->mm[{d}]={};", src(1)) }
            else if i.op_kind(0) == OpKind::Memory { format!("M64({})=c->mm[{}];", ea(i), reg(1)) }
            else if i.op_register(0).is_xmm() { format!("X[{}]=vfrom64(c->mm[{}]);", reg(0), reg(1)) }
            else { wreg(i.op_register(0), &format!("c->mm[{}]", reg(1))) },
        M::Movq2dq => format!("X[{}]=vfrom64(c->mm[{}]);", reg(0), reg(1)),
        M::Movdq2q => format!("c->mm[{d}]=X[{}].q[0];", reg(1)),
        M::Pxor => format!("c->mm[{d}]^={};", src(1)),
        M::Pand => format!("c->mm[{d}]&={};", src(1)),
        M::Por => format!("c->mm[{d}]|={};", src(1)),
        M::Pandn => format!("c->mm[{d}]=~c->mm[{d}]&{};", src(1)),
        M::Paddb => lanes("b", "+"), M::Paddw => lanes("w", "+"), M::Paddd => lanes("d", "+"), M::Paddq => lanes("q", "+"),
        M::Psubb => lanes("b", "-"), M::Psubw => lanes("w", "-"), M::Psubd => lanes("d", "-"), M::Psubq => lanes("q", "-"),
        M::Pmullw => lanes("w", "*"),
        M::Pcmpeqb => cmp("b", "=="), M::Pcmpeqw => cmp("w", "=="), M::Pcmpeqd => cmp("d", "=="),
        M::Pcmpgtb => cmp("sb", ">"), M::Pcmpgtw => cmp("sw", ">"), M::Pcmpgtd => cmp("sd", ">"),
        M::Paddsb | M::Paddsw | M::Paddusb | M::Paddusw | M::Psubsb | M::Psubsw | M::Psubusb | M::Psubusw | M::Pmulhw | M::Pmulhuw
        | M::Pmaddwd | M::Pminub | M::Pmaxub | M::Pminsw | M::Pmaxsw | M::Pavgb | M::Pavgw | M::Psadbw | M::Pmuludq => named(&format!("{m:?}").to_lowercase()),
        M::Punpcklbw | M::Punpcklwd | M::Punpckldq => named(&format!("{m:?}").to_lowercase()),
        M::Punpckhbw => high("punpcklbw"), M::Punpckhwd => high("punpcklwd"), M::Punpckhdq => high("punpckldq"),
        M::Packsswb | M::Packssdw | M::Packuswb => pack(&format!("{m:?}").to_lowercase()),
        M::Psllw | M::Pslld | M::Psllq | M::Psrlw | M::Psrld | M::Psrlq | M::Psraw | M::Psrad => shift(&format!("{m:?}").to_lowercase()),
        M::Pshufw => {
            let k = i.immediate(2);
            let parts: Vec<String> = (0..4).map(|j| format!("(((s>>{})&0xFFFFull)<<{})", 16 * ((k >> (2 * j)) & 3), 16 * j)).collect();
            format!("{{u64 s={};c->mm[{d}]={};}}", src(1), parts.join("|"))
        }
        M::Pinsrw => {
            let k = 16 * (i.immediate(2) & 3);
            let v = if i.op_kind(1) == OpKind::Memory { format!("M16({})", ea(i)) } else { rreg(i.op_register(1)) };
            format!("c->mm[{d}]=(c->mm[{d}]&~(0xFFFFull<<{k}))|((u64)(u16)({v})<<{k});")
        }
        M::Pextrw => wreg(i.op_register(0), &format!("(u32)(u16)(c->mm[{}]>>{})", reg(1), 16 * (i.immediate(2) & 3))),
        M::Pmovmskb => wreg(i.op_register(0), &format!("pmovmskb(vfrom64(c->mm[{}]))&0xFF", reg(1))),
        M::Cvtpi2ps => format!("{{u64 s={};X[{x}].ps[0]=(float)(s32)s;X[{x}].ps[1]=(float)(s32)(s>>32);}}", src(1), x = reg(0)),
        M::Cvttps2pi | M::Cvtps2pi => {
            let s = if i.op_kind(1) == OpKind::Memory { format!("vfrom64(M64({}))", ea(i)) } else { format!("X[{}]", reg(1)) };
            let (l, r) = if m == M::Cvtps2pi { ("__builtin_rintf(", ")") } else { ("", "") };
            format!("{{V v={s};c->mm[{d}]=(u64)(u32)cvt_f2i32({l}v.ps[0]{r})|((u64)(u32)cvt_f2i32({l}v.ps[1]{r})<<32);}}")
        }
        _ => return Some(String::from("\u{1}")),   // not lifted: the caller counts it
    })
}

impl Emit<'_> {
    fn insn(&mut self, i: &Instruction, f: &Func, entries: &BTreeSet<u64>, entry: u64) -> String {
        use Mnemonic as M;
        let m = i.mnemonic();
        if let Some(s) = mmx(i) {
            return if s == "\u{1}" { self.not_lifted(i) } else { s };
        }
        if is32(i) {
            if let Some(s) = self.insn32(i, f, entries, entry) {
                return s;
            }
        }
        if self.img.machine {
            if let Some(s) = self.machine_insn(i) {
                return s;
            }
        }
        if !is32(i) {
            if let Some(s) = self.foreign64(i) {
                return s;
            }
        }
        if i.encoding() != iced_x86::EncodingKind::Legacy && i.encoding() != iced_x86::EncodingKind::D3NOW {
            if let Some(s) = avx::avx(i) {
                return s;
            }
            self.avx += 1;
            *self.unimpl.entry(format!("{:?}", i.code())).or_default() += 1;
            return format!("rt_unimpl(0x{:x}ull); /* AVX or later */", i.ip());
        }
        let bits = if i.op_count() > 0 && matches!(i.op_kind(0), OpKind::Register | OpKind::Memory) && !i.op0_register().is_xmm() { opsize(i, 0) * 8 } else { 64 };
        let cut = self.cut;
        let goto = |t: u64| -> String {
            if f.insns.contains_key(&t) && f.leaders.contains(&t) && (t == entry || !cut.contains(&t)) {
                format!("goto L_{t:x};")
            } else if entries.contains(&t) {
                format!("f_{t:x}(c);return;")
            } else {
                format!("rt_call(c,0x{t:x}ull);return;") // no lifted code there: the runtime reports it if reached
            }
        };
        let xr = |n: u32| i.op_kind(n) == OpKind::Register && i.op_register(n).is_xmm();
        // scalar float source: an xmm register's low element or memory
        let fsrc = |n: u32, t: &str| -> String {
            if xr(n) {
                format!("X[{}].{}[0]", i.op_register(n).number(), if t == "f64" { "pd" } else { "ps" })
            } else {
                format!("M{}({})", if t == "f64" { "F64" } else { "F32" }, ea(i))
            }
        };
        if let Some(s) = extra(i, self.img.elf, self.img.machine) {
            return s;
        }
        let simple_v: Option<(&str, &str)> = match m {
            M::Pxor | M::Xorps | M::Xorpd => Some(("q", "^")),
            M::Pand | M::Andps | M::Andpd => Some(("q", "&")),
            M::Por | M::Orps | M::Orpd => Some(("q", "|")),
            M::Paddb => Some(("b", "+")),
            M::Paddw => Some(("w", "+")),
            M::Paddd => Some(("d", "+")),
            M::Paddq => Some(("q", "+")),
            M::Psubb => Some(("b", "-")),
            M::Psubw => Some(("w", "-")),
            M::Psubd => Some(("d", "-")),
            M::Psubq => Some(("q", "-")),
            M::Pmullw => Some(("w", "*")),
            M::Addps => Some(("ps", "+")),
            M::Subps => Some(("ps", "-")),
            M::Mulps => Some(("ps", "*")),
            M::Divps => Some(("ps", "/")),
            M::Addpd => Some(("pd", "+")),
            M::Subpd => Some(("pd", "-")),
            M::Mulpd => Some(("pd", "*")),
            M::Divpd => Some(("pd", "/")),
            _ => None,
        };
        if let Some((fld, op)) = simple_v {
            return format!("{{V s={};X[{d}].{fld}=X[{d}].{fld}{op}s.{fld};}}", xmm(i, 1), d = i.op0_register().number());
        }
        let cmp_v: Option<(&str, &str, &str)> = match m {
            M::Pcmpeqb => Some(("b", "b", "==")),
            M::Pcmpeqw => Some(("w", "w", "==")),
            M::Pcmpeqd => Some(("d", "d", "==")),
            M::Pcmpgtb => Some(("sb", "sb", ">")),
            M::Pcmpgtw => Some(("sw", "sw", ">")),
            M::Pcmpgtd => Some(("sd", "sd", ">")),
            _ => None,
        };
        if let Some((dst, fld, op)) = cmp_v {
            return format!("{{V s={};X[{d}].{dst}=(typeof(s.{dst}))(X[{d}].{fld}{op}s.{fld});}}", xmm(i, 1), d = i.op0_register().number());
        }
        let fn_v = match m {
            M::Punpcklbw => "punpcklbw",
            M::Punpcklwd => "punpcklwd",
            M::Punpckldq | M::Unpcklps => "punpckldq",
            M::Punpcklqdq | M::Unpcklpd | M::Movlhps => "punpcklqdq",
            M::Punpckhbw => "punpckhbw",
            M::Punpckhwd => "punpckhwd",
            M::Punpckhdq | M::Unpckhps => "punpckhdq",
            M::Punpckhqdq | M::Unpckhpd => "punpckhqdq",
            M::Pmuludq => "pmuludq",
            M::Packuswb => "packuswb",
            _ => "",
        };
        if !fn_v.is_empty() {
            return format!("X[{d}]={fn_v}(X[{d}],{});", xmm(i, 1), d = i.op0_register().number());
        }
        let shift_v = match m {
            M::Psllw => "psllw",
            M::Pslld => "pslld",
            M::Psllq => "psllq",
            M::Psrlw => "psrlw",
            M::Psrld => "psrld",
            M::Psrlq => "psrlq",
            M::Psraw => "psraw",
            M::Psrad => "psrad",
            M::Pslldq => "pslldq",
            M::Psrldq => "psrldq",
            _ => "",
        };
        if !shift_v.is_empty() {
            let n = if is_imm(i.op_kind(1)) { format!("{}", i.immediate(1)) } else { format!("({}).q[0]", xmm(i, 1)) };
            return format!("X[{d}]={shift_v}(X[{d}],{n});", d = i.op0_register().number());
        }
        let scalar_f: Option<(&str, &str)> = match m {
            M::Addsd => Some(("f64", "+")),
            M::Subsd => Some(("f64", "-")),
            M::Mulsd => Some(("f64", "*")),
            M::Divsd => Some(("f64", "/")),
            M::Addss => Some(("f32", "+")),
            M::Subss => Some(("f32", "-")),
            M::Mulss => Some(("f32", "*")),
            M::Divss => Some(("f32", "/")),
            _ => None,
        };
        if let Some((t, op)) = scalar_f {
            let fld = if t == "f64" { "pd" } else { "ps" };
            return format!("X[{d}].{fld}[0]=X[{d}].{fld}[0]{op}{};", fsrc(1, t), d = i.op0_register().number());
        }
        match m {
            M::Nop | M::Endbr64 | M::Pause => String::new(),
            M::Mov => wr(i, 0, &rd(i, 1)),
            M::Movzx => wr(i, 0, &rd(i, 1)),
            M::Movsx | M::Movsxd => wr(i, 0, &format!("(s{})(s{}){}", bits, opsize(i, 1) * 8, rd(i, 1))),
            M::Lea => wr(i, 0, &ea(i)),
            M::Add | M::Sub | M::Adc | M::Sbb | M::Cmp => {
                let (op, ci) = match m {
                    M::Add => ("add", "0"),
                    M::Adc => ("add", "f.cf"),
                    M::Sbb => ("sub", "f.cf"),
                    _ => ("sub", "0"),
                };
                let e = format!("{op}{bits}(&f,{},{},{ci})", rd(i, 0), rd(i, 1));
                if m == M::Cmp { format!("(void){e};") } else { wr(i, 0, &e) }
            }
            M::And | M::Or | M::Xor | M::Test => {
                let op = match m {
                    M::Or => "|",
                    M::Xor => "^",
                    _ => "&",
                };
                let e = format!("logic{bits}(&f,{}{op}{})", rd(i, 0), rd(i, 1));
                if m == M::Test { format!("(void){e};") } else { wr(i, 0, &e) }
            }
            M::Not => wr(i, 0, &format!("~{}", rd(i, 0))),
            M::Neg => wr(i, 0, &format!("neg{bits}(&f,{})", rd(i, 0))),
            M::Inc => wr(i, 0, &format!("inc{bits}(&f,{})", rd(i, 0))),
            M::Dec => wr(i, 0, &format!("dec{bits}(&f,{})", rd(i, 0))),
            M::Shl | M::Shr | M::Sar | M::Rol | M::Ror => {
                let op = format!("{m:?}").to_lowercase();
                let n = if i.op_count() == 1 { "1".to_string() } else { format!("(u8){}", rd(i, 1).replace(&format!("(u{bits})"), "")) };
                wr(i, 0, &format!("{op}{bits}(&f,{},{n})", rd(i, 0)))
            }
            M::Imul if i.op_count() >= 2 => {
                let (a, b) = if i.op_count() == 3 { (rd(i, 1), format!("(u{bits})0x{:x}ull", i.immediate(2))) } else { (rd(i, 0), rd(i, 1)) };
                wr(i, 0, &format!("imul{bits}(&f,{a},{b})"))
            }
            M::Mul if bits == 64 => format!("mul64x(c,&f,{});", rd(i, 0)),
            M::Mul if bits == 32 => format!("{{u64 p=(u64)(u32)R[0]*{};R[0]=(u32)p;R[2]=p>>32;f.cf=f.of=R[2]!=0;}}", rd(i, 0)),
            M::Imul if bits == 64 => format!("imul64x(c,&f,{});", rd(i, 0)),
            M::Div if bits == 64 => format!("div64x(c,{},0x{:x}ull);", rd(i, 0), i.ip()),
            M::Div if bits == 32 => format!("{{u64 n=(R[2]<<32)|(u32)R[0];u32 d={};if(!d)rt_trap(0x{:x});R[0]=(u32)(n/d);R[2]=(u32)(n%d);}}", rd(i, 0), i.ip()),
            M::Idiv if bits == 64 => format!("idiv64x(c,(s64){},0x{:x}ull);", rd(i, 0), i.ip()),
            M::Idiv if bits == 32 => format!("{{s64 n=(s64)((R[2]<<32)|(u32)R[0]);s32 d=(s32){};if(!d)rt_trap(0x{:x});R[0]=(u32)(n/d);R[2]=(u32)(n%d);}}", rd(i, 0), i.ip()),
            M::Cdq => "R[2]=(u32)((s32)R[0]>>31);".into(),
            M::Cqo => "R[2]=(u64)((s64)R[0]>>63);".into(),
            M::Cdqe => "R[0]=(u64)(s64)(s32)R[0];".into(),
            M::Cwde => "R[0]=(u32)(s32)(s16)R[0];".into(),
            M::Xchg => format!("{{u{bits} t={};{}{}}}", rd(i, 0), wr(i, 0, &rd(i, 1)), wr(i, 1, "t")),
            M::Push => format!("{{u64 t={};PUSH64(t);}}", if is_imm(i.op_kind(0)) { format!("0x{:x}ull", i.immediate(0)) } else { rd(i, 0) }),
            M::Pop => format!("{{u64 t=POP64();{}}}", wr(i, 0, "t")),
            M::Call if self.img.machine => {
                // a machine's image: a callee that comes back on another stack, from another return address than this
                // call's, is a task switch the runtime did not see (one that moved the stack on purpose, as Linux's
                // error_entry, returned through this call's own address)
                let call = if i.op_kind(0) == OpKind::NearBranch64 && entries.contains(&i.near_branch_target()) { format!("f_{:x}(c);", i.near_branch_target()) }
                    else if i.op_kind(0) == OpKind::NearBranch64 { format!("rt_call(c,0x{:x}ull);", i.near_branch_target()) } else { "rt_call(c,t);".into() };
                let target = if i.op_kind(0) == OpKind::NearBranch64 { String::new() } else { format!("u64 t={};", rd(i, 0)) };
                format!("{{{target}u64 sp_=R[RSP];PUSH64(0x{:x}ull);{call}if(__builtin_expect(R[RSP]!=sp_,0))rt_check_return(c,0x{:x}ull,0x{:x}ull,sp_);}}", i.next_ip(), i.ip(), i.next_ip())
            }
            M::Call if i.op_kind(0) == OpKind::NearBranch64 && entries.contains(&i.near_branch_target()) => format!("PUSH64(0x{:x}ull);f_{:x}(c);", i.next_ip(), i.near_branch_target()),
            M::Call if i.op_kind(0) == OpKind::NearBranch64 => format!("PUSH64(0x{:x}ull);rt_call(c,0x{:x}ull);", i.next_ip(), i.near_branch_target()),
            M::Call => format!("{{u64 t={};PUSH64(0x{:x}ull);rt_call(c,t);}}", rd(i, 0), i.next_ip()),
            M::Ret => format!("R[RSP]+={};return;", 8 + if i.op_count() == 1 { i.immediate(0) } else { 0 }),
            M::Jmp if i.op_kind(0) == OpKind::NearBranch64 => goto(i.near_branch_target()),
            M::Jmp => {
                let mut s = format!("{{u64 t={};", rd(i, 0));
                if i.op_kind(0) == OpKind::Register && !f.tables.contains_key(&i.ip()) {
                    self.unresolved += 1;
                    write!(s, "/*unresolved jmp at 0x{:x}*/", i.ip()).unwrap();
                }
                if let Some(ts) = f.tables.get(&i.ip()) {
                    s.push_str("switch(t){");
                    for t in ts.iter().collect::<BTreeSet<_>>() {
                        if f.insns.contains_key(t) && f.leaders.contains(t) {
                            write!(s, "case 0x{t:x}ull:goto L_{t:x};").unwrap();
                        } else if entries.contains(t) {
                            write!(s, "case 0x{t:x}ull:f_{t:x}(c);return;").unwrap();
                        } // else: no instruction starts there, the table was read too far; the runtime reports it if taken
                    }
                    s.push('}');
                }
                s.push_str("rt_call(c,t);return;}");
                s
            }
            _ if i.flow_control() == FlowControl::ConditionalBranch && i.condition_code() != ConditionCode::None => {
                format!("if({}){{{}}}", cond(i.condition_code()), goto(i.near_branch_target()))
            }
            _ if format!("{m:?}").starts_with("Set") && i.condition_code() != ConditionCode::None => wr(i, 0, cond(i.condition_code())),
            _ if format!("{m:?}").starts_with("Cmov") => {
                // a 32-bit cmov clears the upper half even when the condition is false
                format!("{{u{bits} t={}?{}:{};{}}}", cond(i.condition_code()), rd(i, 1), rd(i, 0), wr(i, 0, "t"))
            }
            M::Int3 | M::Ud2 | M::Hlt => format!("rt_trap(0x{:x}ull);", i.ip()),
            M::Syscall => format!("rt_syscall(c,0x{:x}ull);", i.next_ip()), // a Linux title's way to the system (rt/linux-x64/syscall.c)
            // SSE moves
            M::Movaps | M::Movups | M::Movapd | M::Movupd | M::Movdqa | M::Movdqu => format!("{}={};", xmm(i, 0), xmm(i, 1)),
            M::Movd | M::Movq => {
                let b = if m == M::Movq { 64 } else { 32 };
                if xr(0) {
                    let src = if xr(1) { format!("X[{}].q[0]", i.op_register(1).number()) } else if i.op_kind(1) == OpKind::Memory { format!("M{b}({})", ea(i)) } else { rreg(i.op_register(1)) };
                    format!("X[{}]=vfrom64((u{b}){src});", i.op_register(0).number())
                } else {
                    let v = format!("(u{b})X[{}].q[0]", i.op_register(1).number());
                    if i.op_kind(0) == OpKind::Memory { format!("M{b}({})={v};", ea(i)) } else { wreg(i.op_register(0), &v) }
                }
            }
            M::Movsd | M::Movss if i.op_count() == 2 => {
                let (t, fld, mm) = if m == M::Movsd { ("f64", "pd", "MF64") } else { ("f32", "ps", "MF32") };
                let _ = t;
                if xr(0) && xr(1) {
                    format!("X[{}].{fld}[0]=X[{}].{fld}[0];", i.op_register(0).number(), i.op_register(1).number())
                } else if xr(0) {
                    format!("{{V t=vzero();t.{fld}[0]={mm}({});X[{}]=t;}}", ea(i), i.op_register(0).number())
                } else {
                    format!("{mm}({})=X[{}].{fld}[0];", ea(i), i.op_register(1).number())
                }
            }
            M::Pshufd => format!("X[{}]=pshufd({},{});", i.op0_register().number(), xmm(i, 1), i.immediate(2)),
            M::Pshuflw => format!("X[{}]=pshuflw({},{});", i.op0_register().number(), xmm(i, 1), i.immediate(2)),
            M::Pshufhw => format!("X[{}]=pshufhw({},{});", i.op0_register().number(), xmm(i, 1), i.immediate(2)),
            M::Shufps => format!("X[{d}]=shufps(X[{d}],{},{});", xmm(i, 1), i.immediate(2), d = i.op0_register().number()),
            M::Pandn | M::Andnps | M::Andnpd => format!("{{V s={};X[{d}].q=~X[{d}].q&s.q;}}", xmm(i, 1), d = i.op0_register().number()),
            M::Pinsrw => format!("X[{}].w[{}]=(u16){};", i.op0_register().number(), i.immediate(2) & 7, if i.op_kind(1) == OpKind::Memory { format!("M16({})", ea(i)) } else { rreg(i.op_register(1)) }),
            M::Pextrw => wreg(i.op_register(0), &format!("(u32)X[{}].w[{}]", i.op_register(1).number(), i.immediate(2) & 7)),
            M::Pmovmskb => wreg(i.op_register(0), &format!("pmovmskb(X[{}])", i.op_register(1).number())),
            M::Cvtsi2sd | M::Cvtsi2ss => {
                let sb = opsize(i, 1) * 8;
                let (t, fld) = if m == M::Cvtsi2sd { ("double", "pd") } else { ("float", "ps") };
                format!("X[{}].{fld}[0]=({t})(s{sb}){};", i.op0_register().number(), rd(i, 1))
            }
            M::Cvtsd2ss => format!("X[{}].ps[0]=(float){};", i.op0_register().number(), fsrc(1, "f64")),
            M::Cvtss2sd => format!("X[{}].pd[0]=(double){};", i.op0_register().number(), fsrc(1, "f32")),
            M::Sqrtsd => format!("X[{}].pd[0]=__builtin_sqrt({});", i.op0_register().number(), fsrc(1, "f64")),
            M::Sqrtss => format!("X[{}].ps[0]=__builtin_sqrtf({});", i.op0_register().number(), fsrc(1, "f32")),
            M::Rsqrtss => format!("X[{}].ps[0]=1.0f/__builtin_sqrtf({});", i.op0_register().number(), fsrc(1, "f32")),
            M::Rcpss => format!("X[{}].ps[0]=1.0f/({});", i.op0_register().number(), fsrc(1, "f32")),
            M::Ucomisd | M::Comisd => format!("fcmp(&f,X[{}].pd[0],{});", i.op0_register().number(), fsrc(1, "f64")),
            M::Ucomiss | M::Comiss => format!("fcmp(&f,X[{}].ps[0],{});", i.op0_register().number(), fsrc(1, "f32")),
            M::Cmpsd if i.op_count() == 3 => format!("X[{d}].q[0]=fpred({},X[{d}].pd[0],{})?~0ull:0;", i.immediate(2), fsrc(1, "f64"), d = i.op0_register().number()),
            M::Cmpss if i.op_count() == 3 => format!("X[{d}].d[0]=fpred({},X[{d}].ps[0],{})?~0u:0;", i.immediate(2), fsrc(1, "f32"), d = i.op0_register().number()),
            _ => self.not_lifted(i),
        }
    }

    /// 64-bit code: an instruction with an operand that is no general, vector or x87 register (the emitters below
    /// number a register without looking at its kind: `mov cr3, rax` would write RBX). In a program, a segment
    /// register reads as the selector the system keeps there and takes a write without effect (the bases are the
    /// runtime's: c->fs_base, c->gs_base). Control, debug, MMX and mask registers, and every such operand in a
    /// machine's image (rt/microvm-x64 will give them their meaning), are not lifted.
    fn foreign64(&mut self, i: &Instruction) -> Option<String> {
        use Mnemonic as M;
        let odd = |r: Register| !(r.is_gpr() || r.is_xmm() || r.is_ymm() || r.is_st());
        let regs: Vec<Register> = (0..i.op_count()).filter(|&n| i.op_kind(n) == OpKind::Register).map(|n| i.op_register(n)).filter(|r| odd(*r)).collect();
        if regs.is_empty() {
            return None;
        }
        if self.img.machine || !regs.iter().all(|r| r.is_segment_register()) {
            return Some(self.not_lifted(i));
        }
        // the selectors of a 64-bit user program: Windows (FS 0x53, the others its data segment) or Linux
        let pe = !self.img.elf && !self.img.ps5;
        let sel = |r: Register| -> u32 {
            match r { Register::CS => 0x33, Register::SS => 0x2B, Register::FS if pe => 0x53, _ if pe => 0x2B, _ => 0 }
        };
        let seg = |n: u32| i.op_kind(n) == OpKind::Register && i.op_register(n).is_segment_register();
        Some(match i.mnemonic() {
            M::Mov if seg(1) => wr(i, 0, &format!("0x{:x}u", sel(i.op_register(1)))),
            M::Mov if seg(0) => format!("(void)({});", rd(i, 1)),
            M::Push if seg(0) => format!("PUSH64(0x{:x}ull);", sel(i.op_register(0))),
            M::Pop if seg(0) => "R[RSP]+=8;".into(),
            _ => self.not_lifted(i),
        })
    }

    /// A machine's image (rt/microvm-x64): the instructions of the system, each a call of the runtime's machine.
    /// None: not one of them (the emitters shared with programs lift it).
    fn machine_insn(&mut self, i: &Instruction) -> Option<String> {
        use Mnemonic as M;
        let (ip, next) = (i.ip(), i.next_ip());
        let reg = |n: u32| i.op_kind(n) == OpKind::Register;
        let ctl = |r: Register| r.is_cr() || r.is_dr();
        let seg_index = |r: Register| match r { Register::ES => 0, Register::CS => 1, Register::SS => 2, Register::DS => 3, Register::FS => 4, _ => 5 };
        let port = || if reg(0) && i.op0_register() == Register::DX || i.op_count() > 1 && reg(1) && i.op_register(1) == Register::DX && i.mnemonic() == M::In { "(u16)R[RDX]".to_string() }
            else { format!("0x{:x}", if i.mnemonic() == M::In { i.immediate(1) } else { i.immediate(0) }) };
        // an immediate the kernel patches at boot (a runtime constant): the value it holds now
        if i.mnemonic() == M::Mov && i.op_count() == 2 && i.op_kind(1) == OpKind::Immediate64 && self.img.patched.contains(&(next - 8)) {
            return Some(wr(i, 0, &format!("M64(0x{:x}ull)", next - 8)));
        }
        if matches!(i.mnemonic(), M::Shr | M::Shl | M::Sar) && i.op_count() == 2 && i.op_kind(1) == OpKind::Immediate8 && self.img.patched.contains(&(next - 1)) {
            let bits = opsize(i, 0) * 8;
            return Some(wr(i, 0, &format!("{}{bits}(&f,{},(u8)M8(0x{:x}ull))", format!("{:?}", i.mnemonic()).to_lowercase(), rd(i, 0), next - 1)));
        }
        // far jumps and calls (to real mode, to another segment): not the machine's
        if i.is_jmp_far() || i.is_jmp_far_indirect() || i.is_call_far() || i.is_call_far_indirect() { return Some(self.not_lifted(i)); }
        Some(match i.mnemonic() {
            M::Cli => "mc_rflags&=~0x200ull;".into(),
            M::Sti => "rt_sti(c);".into(),
            M::Hlt => format!("rt_hlt(c,0x{next:x}ull);"),
            M::Pushfq => "PUSH64(flags_get64(&f));".into(),
            M::Popfq => "flags_set64(c,&f,POP64());".into(),
            M::Iretq => "rt_iret(c);return;".into(),
            M::Sysretq => "rt_sysret(c);return;".into(),
            M::Swapgs => "rt_swapgs(c);".into(),
            M::Wrmsr => "rt_wrmsr(c,ip_);".into(),
            M::Rdmsr => "rt_rdmsr(c,ip_);".into(),
            M::Syscall => format!("rt_syscall_m(c,0x{next:x}ull,&f);"),
            M::Int3 => format!("rt_exc(c,3,0,0x{next:x}ull,0x{next:x}ull);"),
            M::Int => format!("rt_exc(c,0x{:x},0,0x{next:x}ull,0x{next:x}ull);", i.immediate(0) & 0xFF),
            M::Ud2 => format!("rt_exc(c,6,0,0x{ip:x}ull,0x{next:x}ull);"),
            M::Invlpg => format!("rt_invlpg(c,{});", ea(i)),
            M::Lgdt | M::Lidt | M::Sgdt | M::Sidt => format!("rt_{}(c,{});", format!("{:?}", i.mnemonic()).to_lowercase(), ea(i)),
            M::Ltr => format!("rt_ltr(c,(u16){});", rd(i, 0)),
            M::Str => wr(i, 0, "rt_str(c)"),
            M::Lldt => String::new(),
            M::Sldt => wr(i, 0, "0"),
            M::In => wr(i, 0, &format!("rt_in(c,{},{})", port(), i.op_register(0).size())),
            M::Out => format!("rt_out(c,{},{},{});", port(), i.op_register(1).size(), rreg(i.op_register(1))),
            M::Wbinvd | M::Invd | M::Clflush | M::Clflushopt | M::Clwb | M::Clts | M::Verw | M::Monitor | M::Mwait | M::Monitorx | M::Mwaitx | M::Serialize | M::Xsetbv | M::Lfence | M::Mfence | M::Sfence => {
                if matches!(i.mnemonic(), M::Lfence | M::Mfence | M::Sfence) { return None; }
                String::new()
            }
            M::Rdpmc => "R[RAX]=0;R[RDX]=0;".into(),
            M::Fxsave64 | M::Fxsave => format!("fxsave(c,{});", ea(i)),
            // one CPU: compared and exchanged in two halves (no other CPU can see between them)
            M::Cmpxchg16b => format!("{{u64 a_={};u64 lo=M64(a_),hi=M64(a_+8);f.zf=lo==R[RAX]&&hi==R[RDX];if(f.zf){{MW64(a_,R[RBX]);MW64(a_+8,R[RCX]);}}else{{R[RAX]=lo;R[RDX]=hi;}}}}", ea(i)),
            M::Fxrstor64 | M::Fxrstor => format!("fxrstor(c,{});", ea(i)),
            M::Rdfsbase => wr(i, 0, "c->fs_base"),
            M::Rdgsbase => wr(i, 0, "c->gs_base"),
            M::Wrfsbase => format!("c->fs_base={};", rd(i, 0)),
            M::Wrgsbase => format!("c->gs_base={};", rd(i, 0)),
            M::Mov if is_switch(self.img, i) => format!("{{u64 v_={};rt_switch(c,v_,0x{next:x}ull);}}", rd(i, 1)),
            M::Mov if reg(0) && ctl(i.op0_register()) => {
                let r = i.op0_register();
                if r.is_cr() { format!("rt_wcr(c,{},{});", r.number(), rd(i, 1)) } else { String::new() }
            }
            M::Mov if reg(1) && ctl(i.op_register(1)) => {
                let r = i.op_register(1);
                wr(i, 0, &if r.is_cr() { format!("rt_rcr(c,{})", r.number()) } else { "0".into() })
            }
            M::Mov if reg(0) && i.op0_register().is_segment_register() => format!("rt_wseg(c,{},(u16){});", seg_index(i.op0_register()), rd(i, 1)),
            M::Mov if reg(1) && i.op_register(1).is_segment_register() => wr(i, 0, &format!("rt_rseg(c,{})", seg_index(i.op_register(1)))),
            _ => return None,
        })
    }

    /// An instruction the lifter has no code for: counted, and a stop if it is ever reached.
    fn not_lifted(&mut self, i: &Instruction) -> String {
        let mut text = String::new();
        IntelFormatter::new().format(i, &mut text);
        if !is32(i) || self.counted.insert(i.ip()) {
            *self.unimpl.entry(format!("{:?}", i.mnemonic())).or_default() += 1;
        }
        format!("rt_unimpl(0x{:x}ull); /* {text} */", i.ip())
    }

    /// 32-bit code: what differs from 64-bit code (4-byte stack slots, the direction flag, FS, addresses that wrap
    /// at 4 GiB) and what only 32-bit titles use. None: the emitters shared with 64-bit code handle it.
    /// 3DNow is not modelled: the virtual CPU of a 32-bit title does not report it (MMX: mmx(), above).
    fn insn32(&mut self, i: &Instruction, f: &Func, entries: &BTreeSet<u64>, entry: u64) -> Option<String> {
        use Mnemonic as M;
        let m = i.mnemonic();
        let (ip, next) = (i.ip(), i.next_ip());
        // segment registers in a flat address space: saving and restoring DS, ES and SS around a routine (Miles' mixer
        // does) moves the stack and nothing else; a pushed selector is the one Windows gives them (0x23). FS and GS
        // are left to the general case below, as are moves into segment registers.
        if matches!(m, M::Push | M::Pop) && i.op_count() == 1 && i.op_kind(0) == OpKind::Register && matches!(i.op_register(0), Register::DS | Register::ES | Register::SS) {
            let slot = i.stack_pointer_increment().unsigned_abs();
            return Some(if m == M::Push { format!("PUSH{b}(0x23);", b = slot * 8) } else { format!("R[RSP]=(u32)(R[RSP]+{slot});") });
        }
        let foreign = (0..i.op_count()).any(|n| i.op_kind(n) == OpKind::Register && { let r = i.op_register(n); !(r.is_gpr() || r.is_xmm() || r.is_st()) });
        let far = i.is_call_far() || i.is_call_far_indirect() || i.is_jmp_far() || i.is_jmp_far_indirect() || i.op_kind(0) == OpKind::NearBranch16;
        if foreign || far || i.encoding() == iced_x86::EncodingKind::D3NOW || matches!(m, M::Emms | M::Femms | M::Rdrand | M::Rdseed) {
            return Some(self.not_lifted(i));
        }
        let bits = if i.op_count() > 0 && matches!(i.op_kind(0), OpKind::Register | OpKind::Memory) { opsize(i, 0) * 8 } else { 32 };
        let cut = self.cut;
        let goto = |t: u64| -> String {
            if f.insns.contains_key(&t) && f.leaders.contains(&t) && (t == entry || !cut.contains(&t)) {
                format!("goto L_{t:x};")
            } else if entries.contains(&t) {
                format!("f_{t:x}(c);return;")
            } else {
                format!("rt_call(c,0x{t:x}u);return;") // no lifted code there: the runtime reports it if reached
            }
        };
        let slot = i.stack_pointer_increment().unsigned_abs() as usize; // bytes pushed or popped
        let count = || if i.op_count() == 1 { "1".to_string() } else { format!("(u8){}", rd(i, 1).replace(&format!("(u{bits})"), "")) };
        let s = match m {
            M::Endbr32 | M::Fnop => String::new(),
            // ---- the stack: 4-byte slots (2-byte ones with an operand-size prefix) ----
            M::Push if slot == 4 || slot == 2 => {
                let v = if is_imm(i.op_kind(0)) { format!("(u{})0x{:x}ull", slot * 8, i.immediate(0)) } else { rd(i, 0) };
                format!("{{u{b} t={v};PUSH{b}(t);}}", b = slot * 8)
            }
            M::Pop if slot == 4 || slot == 2 => format!("{{u{b} t=POP{b}();{}}}", wr(i, 0, "t"), b = slot * 8),
            M::Call if is_near(i) => {
                let t = i.near_branch_target();
                if t == next {
                    format!("PUSH32(0x{next:x}u);") // `call $+5`: only pushes its return address
                } else if entries.contains(&t) {
                    format!("PUSH32(0x{next:x}u);f_{t:x}(c);")
                } else {
                    format!("PUSH32(0x{next:x}u);rt_call(c,0x{t:x}u);")
                }
            }
            M::Call => format!("{{u32 t={};PUSH32(0x{next:x}u);rt_call(c,t);}}", rd(i, 0)),
            M::Ret => format!("R[RSP]=(u32)(R[RSP]+{});return;", 4 + if i.op_count() == 1 { i.immediate(0) } else { 0 }),
            M::Jmp if is_near(i) => goto(i.near_branch_target()),
            M::Jmp => {
                let mut s = format!("{{u32 t={};", rd(i, 0));
                if let Some(ts) = f.tables.get(&ip) {
                    s.push_str("switch(t){");
                    for &t in ts.iter().collect::<BTreeSet<_>>() {
                        if (f.insns.contains_key(&t) && f.leaders.contains(&t)) || entries.contains(&t) {
                            write!(s, "case 0x{t:x}u:{}", goto(t)).unwrap();
                        } // else: no instruction starts there; the runtime reports it if taken
                    }
                    s.push('}');
                } else if i.op0_kind() == OpKind::Register || is_table_jump(i) {
                    self.unresolved_at.insert(ip);
                    write!(s, "/*unresolved jmp at 0x{ip:x}*/").unwrap();
                } else {
                    self.pointer_jumps.insert(ip); // a tail call through an import slot, a vtable, a function pointer
                }
                s.push_str("rt_call(c,t);return;}");
                s
            }
            M::Leave => "R[RSP]=(u32)R[RBP];R[RBP]=POP32();".into(),
            M::Enter => {
                let (size, level) = (i.immediate(0) & 0xFFFF, i.immediate(1) & 31);
                let mut s = String::from("{PUSH32((u32)R[RBP]);u32 fp=(u32)R[RSP];");
                for _ in 1..level {
                    s.push_str("R[RBP]=(u32)(R[RBP]-4);PUSH32(M32(R[RBP]));");
                }
                if level > 0 {
                    s.push_str("PUSH32(fp);");
                }
                write!(s, "R[RBP]=fp;R[RSP]=(u32)(R[RSP]-{size});}}").unwrap();
                s
            }
            M::Pushad => "{u32 sp=(u32)R[RSP];PUSH32((u32)R[0]);PUSH32((u32)R[1]);PUSH32((u32)R[2]);PUSH32((u32)R[3]);PUSH32(sp);PUSH32((u32)R[5]);PUSH32((u32)R[6]);PUSH32((u32)R[7]);}".into(),
            M::Popad => "R[7]=POP32();R[6]=POP32();R[5]=POP32();R[RSP]=(u32)(R[RSP]+4);R[3]=POP32();R[2]=POP32();R[1]=POP32();R[0]=POP32();".into(),
            // EFLAGS: the arithmetic flags and DF from the function's flags, AC and ID (CPU detection toggles them) from the CPU
            M::Pushfd => "{u32 t=eflags_get(c,&f);PUSH32(t);}".into(),
            M::Popfd => "{u32 t=POP32();eflags_set(c,&f,t);}".into(),
            // ---- loops on ECX ----
            M::Loop | M::Loope | M::Loopne | M::Jecxz => {
                if m != M::Jecxz && !format!("{:?}", i.code()).ends_with("_ECX") {
                    return Some(self.not_lifted(i)); // counts in CX
                }
                let t = goto(i.near_branch_target());
                match m {
                    M::Jecxz => format!("if(!(u32)R[RCX]){{{t}}}"),
                    M::Loop => format!("R[RCX]=(u32)(R[RCX]-1);if((u32)R[RCX]){{{t}}}"),
                    M::Loope => format!("R[RCX]=(u32)(R[RCX]-1);if((u32)R[RCX]&&f.zf){{{t}}}"),
                    _ => format!("R[RCX]=(u32)(R[RCX]-1);if((u32)R[RCX]&&!f.zf){{{t}}}"),
                }
            }
            M::Jcxz => return Some(self.not_lifted(i)),
            // ---- string operations: count in ECX, direction from DF ----
            M::Cld => "f.df=0;".into(),
            M::Std => "f.df=1;".into(),
            _ if i.is_string_instruction() => {
                let (kind, size) = match m {
                    M::Movsb => ("movs", 1), M::Movsw => ("movs", 2), M::Movsd => ("movs", 4),
                    M::Stosb => ("stos", 1), M::Stosw => ("stos", 2), M::Stosd => ("stos", 4),
                    M::Lodsb => ("lods", 1), M::Lodsw => ("lods", 2), M::Lodsd => ("lods", 4),
                    M::Cmpsb => ("cmps", 1), M::Cmpsw => ("cmps", 2), M::Cmpsd => ("cmps", 4),
                    M::Scasb => ("scas", 1), M::Scasw => ("scas", 2), M::Scasd => ("scas", 4),
                    _ => return Some(self.not_lifted(i)), // ins, outs
                };
                // 16-bit addressing (CX, SI, DI) and FS / GS sources are not lifted
                if (0..i.op_count()).any(|n| matches!(i.op_kind(n), OpKind::MemorySegSI | OpKind::MemoryESDI)) || matches!(i.segment_prefix(), Register::FS | Register::GS) {
                    return Some(self.not_lifted(i));
                }
                let (n, u) = (size, size * 8);
                let (di, si) = ("R[RDI]=(u32)(R[RDI]+d);", "R[RSI]=(u32)(R[RSI]+d);");
                let one = match kind {
                    "stos" => format!("M{u}(R[RDI])=(u{u})R[0];{di}"),
                    "movs" => format!("M{u}(R[RDI])=M{u}(R[RSI]);{di}{si}"),
                    "lods" => format!("{}{si}", wreg(reg_of(size), &format!("M{u}(R[RSI])"))),
                    "cmps" => format!("(void)sub{u}(&f,M{u}(R[RSI]),M{u}(R[RDI]),0);{di}{si}"),
                    _ => format!("(void)sub{u}(&f,(u{u})R[0],M{u}(R[RDI]),0);{di}"),
                };
                let dir = format!("s32 d=f.df?-{n}:{n};");
                let dec = "R[RCX]=(u32)(R[RCX]-1);";
                let rep = i.has_rep_prefix() || i.has_repne_prefix();
                match kind {
                    "movs" | "stos" if rep => format!("rep_{kind}(c,f.df,{n});"),
                    "lods" if rep => format!("{{{dir}while((u32)R[RCX]){{{one}{dec}}}}}"),
                    _ if i.has_repe_prefix() => format!("{{{dir}while((u32)R[RCX]){{{one}{dec}if(!f.zf)break;}}}}"),
                    _ if i.has_repne_prefix() => format!("{{{dir}while((u32)R[RCX]){{{one}{dec}if(f.zf)break;}}}}"),
                    _ => format!("{{{dir}{one}}}"),
                }
            }
            M::Xlatb => "R[0]=(R[0]&~0xFFull)|M8((u32)(R[RBX]+(u8)R[0]));".into(),
            // ---- integer forms the 64-bit titles did not need ----
            M::Rcl | M::Rcr => wr(i, 0, &format!("{}{bits}(&f,{},{})", if m == M::Rcl { "rcl" } else { "rcr" }, rd(i, 0), count())),
            M::Imul if i.op_count() == 1 && bits == 8 => format!("{{s16 p=(s16)(s8)R[0]*(s8){};R[0]=(R[0]&~0xFFFFull)|(u16)p;f.cf=f.of=p!=(s8)p;}}", rd(i, 0)),
            M::Imul if i.op_count() == 1 && bits == 16 => format!("{{s32 p=(s32)(s16)R[0]*(s16){};R[0]=(R[0]&~0xFFFFull)|(u16)p;R[2]=(R[2]&~0xFFFFull)|(u16)(p>>16);f.cf=f.of=p!=(s16)p;}}", rd(i, 0)),
            M::Idiv if bits == 8 => format!("{{s16 n=(s16)R[0];s8 d=(s8){};if(!d)rt_trap(0x{ip:x});R[0]=(R[0]&~0xFFFFull)|(u16)((u8)(n%d)<<8|(u8)(n/d));}}", rd(i, 0)),
            M::Idiv if bits == 16 => format!("{{s32 n=(s32)(((u32)(u16)R[2]<<16)|(u16)R[0]);s16 d=(s16){};if(!d)rt_trap(0x{ip:x});R[0]=(R[0]&~0xFFFFull)|(u16)(n/d);R[2]=(R[2]&~0xFFFFull)|(u16)(n%d);}}", rd(i, 0)),
            M::Bt | M::Bts | M::Btr | M::Btc if i.op_kind(0) == OpKind::Memory && i.op_kind(1) == OpKind::Register => {
                // a register bit offset reaches past the operand, in both directions
                let off = format!("(s{bits})R[{}]", gpr(i.op_register(1)));
                let addr = format!("(u32)({}+(u32)(({off})>>{})*{})", ea(i), if bits == 32 { 5 } else { 4 }, bits / 8);
                let n = format!("u32 n=({off})&{};", bits - 1);
                match m {
                    M::Bt => format!("{{{n}f.cf=(M{bits}({addr})>>n)&1;}}"),
                    _ if i.has_lock_prefix() => {
                        let (op, arg) = match m { M::Bts => ("or", format!("(u{bits})(1u<<n)")), M::Btr => ("and", format!("(u{bits})~(1u<<n)")), _ => ("xor", format!("(u{bits})(1u<<n)")) };
                        format!("{{{n}u{bits} o=__atomic_fetch_{op}(ATOM{bits}({addr}),{arg},5);f.cf=(o>>n)&1;}}")
                    }
                    _ => {
                        let new = match m { M::Bts => "v|(1u<<n)", M::Btr => "v&~(1u<<n)", _ => "v^(1u<<n)" };
                        format!("{{{n}u{bits} v=M{bits}({addr});f.cf=(v>>n)&1;M{bits}({addr})=(u{bits})({new});}}")
                    }
                }
            }
            // ---- x87: precision control, and what the 64-bit titles did not use ----
            M::Fadd | M::Faddp | M::Fiadd | M::Fsub | M::Fsubp | M::Fisub | M::Fsubr | M::Fsubrp | M::Fisubr | M::Fmul | M::Fmulp | M::Fimul | M::Fdiv | M::Fdivp | M::Fidiv | M::Fdivr | M::Fdivrp | M::Fidivr => farith(i, true),
            M::Fsqrt => "ST(0)=fprec(c,__builtin_sqrt(ST(0)));".into(),
            // (`fxch st(i)` decodes with two operands, st(0) first: the shared emitter exchanges st(0) with itself)
            M::Fxch => format!("{{double t=ST(0);ST(0)=ST({n});ST({n})=t;}}", n = if i.op_count() > 0 { sti(i.op_register(i.op_count() - 1)) } else { 1 }),
            M::Fsin => "ST(0)=sin(ST(0));c->fsw&=~0x400;".into(),
            M::Fcos => "ST(0)=cos(ST(0));c->fsw&=~0x400;".into(),
            M::Fsincos => "{double v=ST(0);ST(0)=sin(v);fpush(c,cos(v));c->fsw&=~0x400;}".into(),
            M::Fptan => "ST(0)=tan(ST(0));fpush(c,1.0);c->fsw&=~0x400;".into(),
            M::Fpatan => "{double r=atan2(ST(1),ST(0));fpop(c);ST(0)=r;}".into(),
            M::Fxtract => "fxtract(c);".into(),
            M::Fprem => "fprem(c,0);".into(),
            M::Fprem1 => "fprem(c,1);".into(),
            M::Fnstenv | M::Fstenv if i.memory_size().size() == 28 => format!("fstenv(c,{});c->fcw|=0x3F;", ea(i)),
            M::Fldenv if i.memory_size().size() == 28 => format!("fldenv(c,{});", ea(i)),
            M::Fnsave | M::Fsave if i.memory_size().size() == 108 => format!("fsave(c,{});", ea(i)),
            M::Frstor if i.memory_size().size() == 108 => format!("frstor(c,{});", ea(i)),
            M::Fcmovb | M::Fcmove | M::Fcmovbe | M::Fcmovu | M::Fcmovnb | M::Fcmovne | M::Fcmovnbe | M::Fcmovnu => {
                let on = match m { M::Fcmovb => "f.cf", M::Fcmove => "f.zf", M::Fcmovbe => "(f.cf|f.zf)", M::Fcmovu => "f.pf", M::Fcmovnb => "!f.cf", M::Fcmovne => "!f.zf", M::Fcmovnbe => "!(f.cf|f.zf)", _ => "!f.pf" };
                format!("if({on})ST(0)=ST({});", sti(i.op_register(i.op_count() - 1)))
            }
            M::Rsqrtss | M::Rcpss => {
                let src = if i.op_kind(1) == OpKind::Register { format!("X[{}].ps[0]", i.op_register(1).number()) } else { format!("MF32({})", ea(i)) };
                format!("X[{}].ps[0]=1.0f/{};", i.op0_register().number(), if m == M::Rcpss { src } else { format!("__builtin_sqrtf({src})") })
            }
            M::Fdecstp => "c->ftop=(c->ftop-1)&7;".into(),
            M::Fincstp => "c->ftop=(c->ftop+1)&7;".into(),
            M::Ffreep => "fpop(c);".into(),
            _ => return None,
        };
        Some(s)
    }

    fn func(&mut self, entry: u64, f: &Func, entries: &BTreeSet<u64>) -> String {
        if !f.insns.contains_key(&entry) {
            // nothing decodes there (a code-looking value in data): reached only by mistake
            return format!("void f_{entry:x}(Cpu*__restrict c){{rt_trap(0x{entry:x}ull);}}\n");
        }
        let mut s = if !f.alts.is_empty() {
            // soft entries inside this function: one body, entered at the function's start or at one of them
            let mut w = format!("static void b_{entry:x}(Cpu*__restrict c,u32 at);\nvoid f_{entry:x}(Cpu*__restrict c){{b_{entry:x}(c,0);}}\n");
            for a in &f.alts {
                writeln!(w, "void f_{a:x}(Cpu*__restrict c){{b_{entry:x}(c,0x{a:x}u);}}").unwrap();
            }
            write!(w, "static void b_{entry:x}(Cpu*__restrict c,u32 at){{Flags f={{0}};(void)f;switch(at){{").unwrap();
            for a in &f.alts {
                write!(w, "case 0x{a:x}u:goto L_{a:x};").unwrap();
            }
            writeln!(w, "}}goto L_{entry:x};").unwrap();
            w
        } else if f.pads.is_empty() && self.img.elf {
            // a Linux title: entered at a return address when c->resume says so (rt/linux-x64/start.c, a forked process
            // and a longjmp go on from there)
            let mut w = format!("void f_{entry:x}(Cpu*__restrict c){{Flags f={{0}};(void)f;if(__builtin_expect(c->resume!=0,0)){{u64 at=c->resume;c->resume=0;switch(at){{");
            for r in f.rets.iter().filter(|r| f.insns.contains_key(r) && f.leaders.contains(r)) {
                write!(w, "case 0x{r:x}ull:goto L_{r:x};").unwrap();
            }
            writeln!(w, "}}rt_unimpl(at);}}goto L_{entry:x};").unwrap();
            w
        } else if f.pads.is_empty() {
            format!("void f_{entry:x}(Cpu*__restrict c){{Flags f={{0}};(void)f;goto L_{entry:x};\n")
        } else {
            // the landing: an exception resumes here (rt_restore_context); the body starts at the landing pad
            let mut w = format!("static void b_{entry:x}(Cpu*__restrict c,u64 resume);\nvoid f_{entry:x}(Cpu*__restrict c){{LANDING(b_{entry:x});}}\n");
            write!(w, "static void b_{entry:x}(Cpu*__restrict c,u64 resume){{Flags f={{0}};(void)f;if(resume){{switch(resume){{").unwrap();
            for p in &f.pads {
                if f.insns.contains_key(p) && f.leaders.contains(p) { // (a pad lifted as an entry of its own goes to the default)
                    write!(w, "case 0x{p:x}ull:goto L_{p:x};").unwrap();
                }
            }
            w.push_str("default:rt_call(c,resume);return;}}\n");
            write!(w, "goto L_{entry:x};\n").unwrap();
            w
        };
        if self.img.machine {
            // the locals the memory macros of a machine's image use (rt/cpu.h): the address of the instruction that
            // runs, and the TLB of the code's privilege: the kernel's above the canonical hole, the user's below it
            let first = s.find("Flags f={0};(void)f;").expect("a function's flags");
            s.insert_str(first + "Flags f={0};(void)f;".len(), &format!("u64 ip_=0x{entry:x}ull;(void)ip_;TlbEntry*const tlb_={};(void)tlb_;", if entry >> 47 != 0 { "mmu_k" } else { "mmu_u" }));
        }
        let mut prev_next = 0;
        for (&ip, i) in &f.insns {
            // a gap in the decoded code: the block before it must not run into this one
            if prev_next != 0 && prev_next != ip {
                writeln!(s, "rt_trap(0x{prev_next:x}ull);").unwrap();
            }
            if f.leaders.contains(&ip) {
                writeln!(s, "L_{ip:x}:;").unwrap();
                if self.trace_pc { writeln!(s, "rt_pc(c,0x{ip:x}u);").unwrap(); }
            }
            let mut body = self.insn(i, f, entries, entry);
            if self.img.machine && !body.is_empty() {
                // the instruction's address, for the fault it may raise; its stores as calls (MW64...); an
                // interrupt is taken at the function's start and at each jump back (rt_poll)
                let back = matches!(i.flow_control(), FlowControl::UnconditionalBranch | FlowControl::ConditionalBranch) && is_near(i) && i.near_branch_target() <= ip;
                body = format!("ip_=0x{ip:x}ull;{}{}", if ip == entry || back { format!("POLL(0x{ip:x}ull);") } else { String::new() }, machine_stores(&body));
            }
            if !body.is_empty() {
                writeln!(s, "{body}").unwrap();
            }
            prev_next = match i.flow_control() {
                FlowControl::Next | FlowControl::Call | FlowControl::IndirectCall | FlowControl::ConditionalBranch => i.next_ip(),
                _ => 0,
            };
            // fallthrough into code this function does not hold (another function's entry)
            if prev_next != 0 && !f.insns.contains_key(&prev_next) {
                if entries.contains(&prev_next) {
                    writeln!(s, "f_{prev_next:x}(c);return;").unwrap();
                } else {
                    writeln!(s, "rt_trap(0x{prev_next:x}ull);").unwrap(); // runs off decoded code: not reached by real code
                }
                prev_next = 0;
            }
        }
        s.push_str("}\n");
        s
    }
}

/// A machine's image: each store of an instruction's C (`M64(a)=(v);`, reads and writes being one macro in a
/// program) as a call (`MW64(a,(v));`), so that the MMU knows a write and a device's register sees it.
fn machine_stores(s: &str) -> String {
    const NAMES: [&str; 7] = ["MF32", "MF64", "M8", "M16", "M32", "M64", "MV"];
    let b = s.as_bytes();
    let close = |from: usize| -> usize { // the index of the ')' closing the '(' before `from`
        let mut depth = 1; let mut k = from;
        while k < b.len() { match b[k] { b'(' => depth += 1, b')' => { depth -= 1; if depth == 0 { return k; } } _ => {} } k += 1; }
        panic!("unbalanced parentheses in {s}");
    };
    let mut out = String::new(); let mut k = 0;
    while k < b.len() {
        let at = NAMES.iter().find(|n| s[k..].starts_with(&format!("{n}(")) && (k == 0 || !(b[k - 1].is_ascii_alphanumeric() || b[k - 1] == b'_')));
        let Some(name) = at else { out.push(b[k] as char); k += 1; continue };
        let open = k + name.len() + 1; let end = close(open);
        let inner = machine_stores(&s[open..end]);
        if b.get(end + 1) == Some(&b'=') && b.get(end + 2) != Some(&b'=') {
            // the value: up to the `;` (or the end of the enclosing group) at this depth
            let mut depth = 0i32; let mut e = end + 2;
            while e < b.len() { match b[e] { b'(' | b'{' | b'[' => depth += 1, b')' | b'}' | b']' => { if depth == 0 { break; } depth -= 1; } b';' if depth == 0 => break, _ => {} } e += 1; }
            out.push_str(&format!("MW{}({inner},({}))", &name[1..], machine_stores(&s[end + 2..e])));
            k = e;
        } else {
            out.push_str(&format!("{name}({inner})"));
            k = end + 1;
        }
    }
    out
}

/// One byte of state per byte of the executable sections: 0 nothing known, 1 the first byte of an instruction,
/// 2 the rest of an instruction, 3 a jump table.
struct Cover {
    lo: u64,
    map: Vec<u8>,
}
impl Cover {
    fn new(img: &Image) -> Cover {
        let lo = img.text.iter().map(|t| t.0).min().unwrap_or(0);
        let hi = img.text.iter().map(|t| t.1).max().unwrap_or(0);
        Cover { lo, map: vec![0; (hi - lo) as usize] }
    }
    fn get(&self, va: u64) -> u8 {
        if va < self.lo { 0 } else { self.map.get((va - self.lo) as usize).copied().unwrap_or(0) }
    }
    fn set(&mut self, va: u64, v: u8) {
        if va >= self.lo {
            if let Some(b) = self.map.get_mut((va - self.lo) as usize) {
                *b = v;
            }
        }
    }
    fn add(&mut self, f: &Func) {
        for (&ip, i) in &f.insns {
            if f.in_data(ip) {
                continue;
            }
            self.set(ip, 1);
            for a in ip + 1..i.next_ip() {
                if self.get(a) != 1 {
                    self.set(a, 2);
                }
            }
        }
        for &(a, e) in &f.data {
            for x in a..e {
                if self.get(x) == 0 {
                    self.set(x, 3);
                }
            }
        }
    }
    /// An instruction of `f` starts inside an instruction already here: one of the two is not code.
    fn clashes(&self, f: &Func) -> bool {
        f.insns.keys().any(|&a| self.get(a) == 2)
    }
}

/// The length of the padding instruction at the start of `b`, if it is one: `int3`, `nop`, `mov r,r`, `lea r,[r+0]`.
/// (`mov edi,edi` before `push ebp; mov ebp,esp` is a hot-patchable function's first instruction, not padding.)
fn padding(bits: u32, b: &[u8], ip: u64) -> Option<u64> {
    if b.starts_with(&[0x8B, 0xFF, 0x55, 0x8B, 0xEC]) {
        return None;
    }
    let i = Decoder::with_ip(bits, &b[..b.len().min(16)], ip, DecoderOptions::NONE).decode();
    let same = |r: Register| i.op_kind(0) == OpKind::Register && i.op0_register() == r && r.size() == 4;
    let pad = match i.mnemonic() {
        Mnemonic::Int3 | Mnemonic::Nop => true,
        Mnemonic::Mov => i.op_kind(1) == OpKind::Register && same(i.op_register(1)),
        Mnemonic::Lea => same(i.memory_base()) && i.memory_index() == Register::None && i.memory_displacement64() == 0,
        _ => false,
    };
    pad.then(|| i.len() as u64)
}

/// What the function discovery of a 32-bit title found.
struct Found {
    hard: BTreeSet<u64>,
    soft: BTreeSet<u64>,
    funcs: BTreeMap<u64, Func>,
    /// soft candidates left out: inside an instruction of the code reached from hard entries, or not code
    inside: usize,
    not_code: usize,
    /// soft entries found by their first bytes in code nothing referred to
    by_pattern: usize,
}

/// Function discovery without an exception table (32-bit code). Flags are local to a lifted function and a branch
/// to another entry is a call, so a wrong entry inside a function would silently lose flags. Therefore:
/// - hard entries cut functions: the entry points, the exports, the targets of direct calls;
/// - soft entries are lifted as functions of their own but cut nothing: code addresses in data (vtables, callbacks)
///   and in immediates (`push offset f`), and typical function starts in the code nothing else reached. The code of
///   a soft entry may also be part of another function.
/// A soft candidate inside an instruction of the code reached from hard entries, or that does not decode as code,
/// is no entry (a number that looks like an address).
fn discover32(img: &Image, roots: &BTreeSet<u64>, forced: &BTreeSet<u64>) -> Found {
    let mut hard = roots.clone();
    let slots: BTreeSet<u64> = img.imports.iter().map(|i| i.0).collect();
    let mut cand: BTreeSet<u64> = BTreeSet::new();
    for (va, d, _) in &img.sections {
        if img.is_code(*va) {
            continue;
        }
        for (k, w) in d.chunks_exact(4).enumerate() {
            let t = u64::from(u32::from_le_bytes(w.try_into().unwrap()));
            if img.is_code(t) && !slots.contains(&(va + 4 * k as u64)) {
                cand.insert(t);
            }
        }
    }
    // An image that merges its read-only data into its code section (D3DX's .text holds its import table and the C
    // runtime's tables of initializers): a relocated pointer there is a candidate as well. Pointers that are part of
    // an instruction (`push offset f`) are found by decoding too; a candidate inside known code is rejected below.
    // (every relocation that is not an import slot: the stretch of the code section that holds data is not "code")
    for &r in &img.relocs {
        if slots.contains(&r) {
            continue;
        }
        if let Some(t) = img.u32(r).map(u64::from) {
            if img.is_code(t) {
                cand.insert(t);
            }
        }
    }
    let in_data = cand.clone();
    cand.extend(forced.iter().copied());
    let mut soft: BTreeSet<u64> = BTreeSet::new();
    let mut rejected: BTreeMap<u64, bool> = BTreeMap::new(); // -> it is inside an instruction
    let mut patterns: BTreeSet<u64> = BTreeSet::new();
    let mut tried: BTreeSet<u64> = BTreeSet::new();
    let mut reconsidered: BTreeSet<u64> = BTreeSet::new();
    let mut funcs: BTreeMap<u64, Func> = BTreeMap::new();
    loop {
        loop {
            let before = (hard.len(), soft.len(), cand.len(), rejected.len());
            funcs.clear();
            // everything reached from the hard entries
            let mut work: Vec<u64> = hard.iter().rev().copied().collect();
            while let Some(e) = work.pop() {
                if funcs.contains_key(&e) {
                    continue;
                }
                let (f, calls, refs) = decode_func(img, e, &hard);
                for t in calls {
                    if img.is_code(t) && hard.insert(t) {
                        work.push(t);
                    }
                }
                cand.extend(refs);
                funcs.insert(e, f);
            }
            let mut known = Cover::new(img);
            for f in funcs.values() {
                known.add(f);
            }
            // the soft candidates against that code
            for &s in &cand.clone() {
                // a candidate turned down for lying inside an instruction of code that is no longer known (that code
                // was found from an entry since dropped) is looked at again, once
                if rejected.get(&s) == Some(&true) && known.get(s) != 2 && reconsidered.insert(s) {
                    rejected.remove(&s);
                }
                if hard.contains(&s) || rejected.contains_key(&s) {
                    continue;
                }
                if known.get(s) == 2 && !forced.contains(&s) {
                    rejected.insert(s, true);
                    soft.remove(&s);
                    continue;
                }
                // in a jump table or index table of known code: data
                if known.get(s) == 3 && !forced.contains(&s) {
                    rejected.insert(s, false);
                    soft.remove(&s);
                    continue;
                }
                let (f, calls, refs) = decode_func(img, s, &hard);
                if !forced.contains(&s) && (!f.insns.contains_key(&s) || (known.get(s) != 1 && (f.bad || known.clashes(&f) || !starts_as_code(&f, s)))) {
                    rejected.insert(s, false);
                    soft.remove(&s);
                    continue;
                }
                soft.insert(s);
                hard.extend(calls.into_iter().filter(|&t| img.is_code(t) && known.get(t) != 2));
                cand.extend(refs);
                funcs.insert(s, f);
            }
            soft.retain(|s| !hard.contains(s));
            // two soft functions that read the same bytes as different instructions: the one whose entry is inside
            // an instruction of the other is a number that looked like an address (unless that other one is doubted too)
            let inside_of = |skip: &BTreeSet<u64>| -> BTreeSet<u64> {
                let mut inner = Cover::new(img);
                for (e, f) in funcs.iter().filter(|(e, _)| soft.contains(e) && !skip.contains(e)) {
                    let _ = e;
                    for (&ip, i) in f.insns.iter().filter(|(ip, _)| !f.in_data(**ip)) {
                        (ip + 1..i.next_ip()).for_each(|a| inner.set(a, 2));
                    }
                    for &(a, e) in &f.data {
                        (a..e).for_each(|x| inner.set(x, 2));   // its tables: data
                    }
                }
                soft.iter().copied().filter(|s| inner.get(*s) == 2 && !forced.contains(s)).collect()
            };
            let doubted = inside_of(&BTreeSet::new());
            for s in inside_of(&doubted) {
                rejected.insert(s, true);
                soft.remove(&s);
                funcs.remove(&s);
            }
            if (hard.len(), soft.len(), cand.len(), rejected.len()) == before {
                break;
            }
        }
        let new = gap_scan(img, &funcs, &hard, &mut tried);
        if new.is_empty() {
            break;
        }
        patterns.extend(new.iter().copied());
        cand.extend(new);
    }
    if std::env::var_os("LIFTER_DEBUG").is_some() {
        let mut known = Cover::new(img);
        for (e, f) in &funcs {
            if hard.contains(e) {
                known.add(f);
            }
        }
        for s in &soft {
            let f = &funcs[s];
            eprintln!("soft 0x{s:x}: {} instructions, {}{}{}{}", f.insns.len(), if known.get(*s) == 1 { "in a hard function" } else { "on its own" }, if f.bad { ", runs into bytes that are no code" } else { "" }, if patterns.contains(s) { ", by pattern" } else { "" }, if in_data.contains(s) { ", in data" } else { "" });
        }
        for (s, inside) in &rejected {
            let by = if *inside { funcs.iter().find_map(|(e, f)| f.insns.values().find(|i| i.ip() < *s && *s < i.next_ip()).map(|i| format!(" (of 0x{:x}'s instruction at 0x{:x}{})", e, i.ip(), if hard.contains(e) { ", a hard entry" } else { "" }))).unwrap_or_default() } else { String::new() };
            eprintln!("no entry 0x{s:x}: {}{by}", if *inside { "inside an instruction" } else { "does not decode as code" });
        }
    }
    // a soft entry inside another function's code is one more way into that function's body, not a copy of its
    // code: first the hard functions, then the soft ones, the largest first (one that holds another comes before it)
    let mut owner: std::collections::HashMap<u64, u64> = std::collections::HashMap::new();
    for (&e, f) in funcs.iter().filter(|(e, _)| hard.contains(e)) {
        for &a in f.insns.keys() {
            owner.entry(a).or_insert(e);
        }
    }
    let mut order: Vec<u64> = soft.iter().copied().collect();
    order.sort_by_key(|s| (std::cmp::Reverse(funcs[s].insns.len()), *s));
    for s in order {
        match owner.get(&s).copied() {
            Some(body) => {
                funcs.remove(&s);
                let f = funcs.get_mut(&body).unwrap();
                f.alts.insert(s);
                f.leaders.insert(s);
            }
            None => {
                for &a in funcs[&s].insns.keys() {
                    owner.entry(a).or_insert(s);
                }
            }
        }
    }
    Found {
        by_pattern: patterns.iter().filter(|p| soft.contains(p)).count(),
        inside: rejected.values().filter(|&&v| v).count(),
        not_code: rejected.values().filter(|&&v| !v).count(),
        hard,
        soft,
        funcs,
    }
}

/// Function starts in the bytes of the executable sections that no function reached: `push ebp; mov ebp,esp`
/// (also behind `mov edi,edi`) and `push -1; push offset handler` anywhere; after known code or padding also
/// `sub esp,N` and anything on a 16-byte boundary. A start counts when what follows decodes as a function that
/// fits the code already known. Jump tables found on the way are skipped like code.
fn gap_scan(img: &Image, funcs: &BTreeMap<u64, Func>, hard: &BTreeSet<u64>, tried: &mut BTreeSet<u64>) -> Vec<u64> {
    let mut cover = Cover::new(img);
    for f in funcs.values() {
        cover.add(f);
    }
    let mut found = Vec::new();
    for &(start, end) in &img.text {
        let mut p = start;
        let mut edge = true; // p follows known code, a table or padding
        while p < end {
            if cover.get(p) != 0 {
                p += 1;
                edge = true;
                continue;
            }
            let Some(b) = img.bytes(p) else { break };
            // (a padding instruction of several bytes only where instructions are known to start: inside
            // bytes that are not understood, any two bytes may look like one)
            if let Some(n) = padding(img.bits, b, p).filter(|&n| n == 1 || edge) {
                p += n;
                edge = true;
                continue;
            }
            let frame = b.starts_with(&[0x55, 0x8B, 0xEC]) || b.starts_with(&[0x8B, 0xFF, 0x55, 0x8B, 0xEC])
                || (b.len() >= 7 && b.starts_with(&[0x6A, 0xFF, 0x68]) && img.is_code(u64::from(u32::from_le_bytes([b[3], b[4], b[5], b[6]]))));
            let weak = edge && (b.starts_with(&[0x83, 0xEC]) || b.starts_with(&[0x81, 0xEC]) || p % 16 == 0);
            if (frame || weak) && tried.insert(p) {
                let (f, _, _) = decode_func(img, p, hard);
                if f.insns.contains_key(&p) && !f.bad && !cover.clashes(&f) && (frame || plausible(&f)) {
                    cover.add(&f);
                    found.push(p);
                    continue;
                }
            }
            p += 1;
            edge = false;
        }
    }
    found
}
/// What decoded from a guessed start looks like a compiled function: it returns or jumps away, and holds
/// nothing a Windows program does not do (port I/O, privileged or segment instructions, `add [eax],al` runs).
fn plausible(f: &Func) -> bool {
    f.insns.values().any(|i| matches!(i.flow_control(), FlowControl::Return | FlowControl::UnconditionalBranch | FlowControl::IndirectBranch)) && !f.insns.values().any(odd)
}
/// An instruction no compiled Windows program holds: what tables and text look like when read as code.
fn odd(i: &Instruction) -> bool {
    use Mnemonic as M;
    matches!(i.mnemonic(), M::In | M::Out | M::Insb | M::Insw | M::Insd | M::Outsb | M::Outsw | M::Outsd | M::Hlt | M::Cli | M::Sti | M::Iret | M::Iretd | M::Retf | M::Int1 | M::Into
        | M::Bound | M::Arpl | M::Les | M::Lds | M::Aaa | M::Aas | M::Aam | M::Aad | M::Daa | M::Das | M::Salc)
        || i.is_call_far() || i.is_jmp_far() || i.is_call_far_indirect() || i.is_jmp_far_indirect()
        || (0..i.op_count()).any(|n| i.op_kind(n) == OpKind::Register && i.op_register(n).is_segment_register())
        || (i.len() == 2 && i.mnemonic() == M::Add && i.op_kind(0) == OpKind::Memory && i.memory_base() == Register::EAX && i.op_kind(1) == OpKind::Register && i.op_register(1) == Register::AL && i.memory_displacement64() == 0)
}
/// The first instructions at a soft candidate, in a straight line: none of them is one only data decodes to.
/// (The whole function is not asked: a function may end in a call that does not return, with a table behind it.)
fn starts_as_code(f: &Func, entry: u64) -> bool {
    if f.insns.get(&entry).is_some_and(|i| i.mnemonic() == Mnemonic::Int3) {
        return false; // the padding between two functions
    }
    // hand-written routines that save DS and ES (Miles' mixer) push and pop them in pairs and return: those
    // pushes and pops are not data read as code
    let seg = |i: &Instruction, m: Mnemonic, r: Register| i.mnemonic() == m && i.op_count() == 1 && i.op_kind(0) == OpKind::Register && i.op_register(0) == r;
    let count = |m: Mnemonic, r: Register| f.insns.values().filter(|i| seg(i, m, r)).count();
    // (in pairs taken together: `push ds; pop es` copies DS into ES)
    let saves = f.insns.values().any(|i| i.flow_control() == FlowControl::Return)
        && [Register::DS, Register::ES].iter().map(|&r| count(Mnemonic::Push, r)).sum::<usize>() == [Register::DS, Register::ES].iter().map(|&r| count(Mnemonic::Pop, r)).sum::<usize>();
    let mut ip = entry;
    for _ in 0..24 {
        let Some(i) = f.insns.get(&ip) else { break };
        if odd(i) && !(saves && [Register::DS, Register::ES].iter().any(|&r| seg(i, Mnemonic::Push, r) || seg(i, Mnemonic::Pop, r))) {
            return false;
        }
        if !matches!(i.flow_control(), FlowControl::Next | FlowControl::Call | FlowControl::IndirectCall | FlowControl::ConditionalBranch) {
            break;
        }
        ip = i.next_ip();
    }
    true
}

fn main() {
    // (the backwards walk to a jump table's bound is recursive: give it room whatever the function's length)
    let done = std::thread::Builder::new().stack_size(256 << 20).spawn(lift).expect("thread").join();
    if done.is_err() {
        std::process::exit(101);
    }
}

fn lift() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: lifter <image.exe> <out.c> [--split N] [--base hex] [--entry hex]... [--builtin name.dll]... [--dll-dir <folder>=<guest folder>]... [--load <file>=<name>]... [--trace-pc] [--machine] [--program <elf>]...");
        std::process::exit(2);
    }
    let exe = std::path::Path::new(&args[1]);
    let dir = exe.parent().unwrap_or(std::path::Path::new("."));
    let listing: Vec<String> = std::fs::read_dir(dir).map(|d| d.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    // --builtin name.dll: a DLL the title ships that is not lifted (the runtime answers for it: a recipe's choice)
    let builtin: Vec<&String> = args.iter().enumerate().filter(|(_, a)| *a == "--builtin").filter_map(|(i, _)| args.get(i + 1)).collect();
    // --dll-dir <folder>=<guest folder>: where else an imported DLL is looked for after the exe's folder (an image's
    // C:\WINDOWS\system32, given as c:/WINDOWS/system32): one found there is lifted with the title under its guest path
    let dll_dirs: Vec<(std::path::PathBuf, String, Vec<String>)> = args.iter().enumerate().filter(|(_, a)| *a == "--dll-dir").filter_map(|(i, _)| args.get(i + 1)).map(|a| {
        let (host, guest) = a.split_once('=').expect("--dll-dir <folder>=<guest folder>");
        let list = std::fs::read_dir(host).map(|d| d.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
        (std::path::PathBuf::from(host), guest.trim_end_matches('/').to_string(), list)
    }).collect();
    // --load <file>=<name>: a DLL the title loads itself (LoadLibrary), which nothing imports: lifted with the title
    // under its name (its path in the title's folder, "main/gamex86.dll"), loaded on demand (rt/process.c). The
    // DLLs only such a DLL imports are loaded on demand too.
    let loads: Vec<(String, std::path::PathBuf)> = args.iter().enumerate().filter(|(_, a)| *a == "--load").filter_map(|(i, _)| args.get(i + 1)).map(|a| {
        let (host, name) = a.split_once('=').expect("--load <file>=<name>");
        (name.replace('\\', "/"), std::path::PathBuf::from(host))
    }).collect();
    let mut mods: Vec<(String, Image)> = Vec::new();
    let mut eager = 0usize;   // the modules the exe's imports reach: mods[..eager]
    let mut queue = vec![(exe.file_name().unwrap().to_string_lossy().into_owned(), exe.to_path_buf())];
    // --program <elf>: a machine's image (a kernel) with the programs it will run (static ELFs at their own addresses,
    // as its initramfs has them): lifted into the same module, and found by address when the kernel runs them
    for p in args.iter().enumerate().filter(|(_, a)| *a == "--program").filter_map(|(i, _)| args.get(i + 1)) {
        let path = std::path::PathBuf::from(p);
        queue.insert(0, (path.file_name().unwrap().to_string_lossy().into_owned(), path));
    }
    let mut pending_loads = loads.clone(); pending_loads.reverse();
    loop {
        let Some((name, path)) = queue.pop().or_else(|| { if eager == 0 { eager = mods.len(); } pending_loads.pop() }) else { break };
        let base_name = name.rsplit('/').next().unwrap_or(&name).to_string();
        if mods.iter().any(|(n, _)| n.rsplit('/').next().unwrap_or(n).eq_ignore_ascii_case(&base_name)) { continue; }
        if mods.iter().any(|(n, _)| n.eq_ignore_ascii_case(&name)) { continue; }
        let data = std::fs::read(&path).expect("read image");
        let img = Image::load(&data, args.iter().any(|a| a == "--machine")).unwrap_or_else(|e| panic!("{name}: {e}"));
        for (_, imp) in &img.imports {
            let dll = imp.split('!').next().unwrap();
            if builtin.iter().any(|b| b.eq_ignore_ascii_case(dll)) { continue; }
            // service libraries the GDK ships beside a title (HTTP, curl, PIX markers) stay the runtime's: offline,
            // their calls fail, nothing leaves the machine
            const RUNTIME: [&str; 3] = ["libhttpclient.dll", "xcurl.dll", "winpixeventruntime.dll"];
            if RUNTIME.contains(&dll.to_ascii_lowercase().as_str()) { continue; }
            if let Some(file) = listing.iter().find(|f| f.eq_ignore_ascii_case(dll)) {
                queue.push((file.clone(), dir.join(file))); // a DLL the title ships: lifted with it
            } else if let Some((host, guest, file)) = dll_dirs.iter().find_map(|(h, g, l)| l.iter().find(|f| f.eq_ignore_ascii_case(dll)).map(|f| (h, g, f))) {
                queue.push((format!("{guest}/{file}"), host.join(file))); // a DLL of the image's system folder
            }
        }
        mods.push((name, img));
    }
    if eager == 0 { eager = mods.len(); }
    // --machine, or an image in the upper half of the address space: the image is a machine's (a kernel,
    // rt/microvm-x64), and its addresses are the machine's virtual ones
    let machine = mods.iter().any(|(_, m)| m.machine);
    if machine {
        // the kernel first (its tables, its symbols), then its programs; none may overlap another
        if !mods.iter().all(|(_, m)| m.elf) { panic!("a machine's image and its programs are ELFs"); }
        let k = mods.iter().position(|(_, m)| m.machine).unwrap(); let kernel = mods.remove(k); mods.insert(0, kernel);
        for (i, (a, ma)) in mods.iter().enumerate() { for (b, mb) in &mods[i + 1..] {
            if ma.base < mb.base + mb.size && mb.base < ma.base + ma.size { panic!("{b} overlaps {a} (one module holds programs at their own addresses)"); } } }
    }
    let bits = mods[0].1.bits;
    if let Some((name, m)) = mods.iter().find(|(_, m)| m.bits != bits) {
        panic!("{name}: a {}-bit image in a {bits}-bit title (32- and 64-bit images cannot be lifted together)", m.bits);
    }
    // where each image goes in guest memory (see docs/memory.md): the exe at 0x10000000 (or --base), the DLLs after it
    let first = args.iter().position(|a| a == "--base").map(|i| u64::from_str_radix(args[i + 1].trim_start_matches("0x"), 16).expect("--base hex"));
    let after = |m: &Image| (m.base + m.size + 0x10_FFFF) & !0xF_FFFF; // a 1 MiB gap between images
    if machine {
        // (nothing to place: no address of the image is an address of the module's memory)
    } else if first.is_none() && mods.iter().any(|(_, m)| m.fixed) {
        // an image linked without relocations stays where it is; the others go after the highest of those
        let mut next = 0;
        for (k, (name, m)) in mods.iter().enumerate().filter(|(_, (_, m))| m.fixed) {
            if m.base + m.size > 0x1_0000_0000 {
                panic!("{name}: the image was linked without relocations (/FIXED) and cannot be moved");
            }
            if let Some((other, _)) = mods[..k].iter().find(|(_, o)| o.fixed && o.base < m.base + m.size && m.base < o.base + o.size) {
                panic!("{name}: linked without relocations at the addresses of {other}, which was too");
            }
            next = next.max(after(m));
        }
        for (name, img) in mods.iter_mut().filter(|(_, m)| !m.fixed) {
            let from = img.base;
            img.rebase(next).unwrap_or_else(|e| panic!("{name}: {e}"));
            if from != img.base { eprintln!("{name}: moved from 0x{from:x} to 0x{:x} ({} relocations)", img.base, img.relocs.len()); }
            next = after(img);
        }
    } else {
        // 64-bit: several images start at 0x10000000, above the runtime's data; a 32-bit exe keeps its own base (its
        // layout leaves the runtime room below and above the images: tools/build-title.sh)
        let mut next = first.unwrap_or(if mods[0].1.base >= 0x1_0000_0000 || (mods.len() > 1 && bits == 64) || mods[0].1.ps5 { 0x1000_0000 } else { mods[0].1.base });
        for (name, img) in mods.iter_mut() {
            let from = img.base;
            img.rebase(next).unwrap_or_else(|e| panic!("{name}: {e}"));
            if from != img.base { eprintln!("{name}: moved from 0x{from:x} to 0x{:x} ({} relocations)", img.base, img.relocs.len()); }
            next = after(img);
        }
    }
    let (image_low, image_high) = (mods.iter().map(|(_, m)| m.base).min().unwrap(), mods.iter().map(|(_, m)| m.base + m.size).max().unwrap());
    // the addresses handed out for imports are only compared, never read: they keep their place unless an image is there
    let import_total: u64 = mods.iter().map(|(_, m)| m.imports.len() as u64).sum();
    let import_base = if mods.iter().any(|(_, m)| IMPORT_BASE < m.base + m.size && m.base < IMPORT_BASE + 8 * import_total.max(1)) { (image_high + 0x10_FFFF) & !0xF_FFFF } else { IMPORT_BASE };
    if bits == 32 && import_base + 8 * import_total > 0x1_0000_0000 {
        panic!("no room below 4 GiB for the import addresses");
    }
    // imports from a DLL we lift resolve to its exports here; the rest go to the runtime
    // (by file name: a DLL from the system folder is named by its guest path, "c:/WINDOWS/system32/x.dll")
    let lower: Vec<String> = mods.iter().map(|(n, _)| n.rsplit('/').next().unwrap_or(n).to_ascii_lowercase()).collect();
    let mut resolved: BTreeMap<u64, u64> = BTreeMap::new(); // IAT slot -> function
    for (_, img) in &mods {
        for (slot, imp) in &img.imports {
            let (dll, name) = imp.split_once('!').unwrap();
            if let Some(k) = lower.iter().position(|n| n == dll) {
                let target = mods[k].1.exports.iter().find(|(n, o, _)| if let Some(ord) = name.strip_prefix('#') { ord.parse::<u32>().ok() == Some(*o) } else { n == name }).map(|e| e.2);
                match target {
                    Some(t) => { resolved.insert(*slot, t); }
                    None => eprintln!("warning: {imp} is not exported by {}", mods[k].0),
                }
            }
        }
    }
    // one view of every image, for decoding
    let mut img = mods[0].1.clone();
    for (_, m) in mods.iter().skip(1) {
        img.sections.extend(m.sections.iter().cloned());
        img.raw.extend(m.raw.iter().cloned());
        img.text.extend(m.text.iter().cloned());
        img.pdata.extend(m.pdata.iter());
        img.unwind.extend(m.unwind.iter());
        img.imports.extend(m.imports.iter().cloned());
        img.exports.extend(m.exports.iter().cloned());
        img.ranges.extend(m.ranges.iter().cloned());
        img.relocs.extend(m.relocs.iter().copied());   // (discover32 takes relocated pointers as candidates)
        img.seeds.extend(m.seeds.iter().copied());     // (a machine's programs: where a jump table of theirs ends)
    }
    img.seeds.sort();
    img.seeds.dedup();
    // function entries: the entry point, the exception table, then every call target, to a fixpoint
    let mut entries: BTreeSet<u64> = img.pdata.keys().copied().collect();
    let handlers: Vec<u64> = img.pdata.keys().filter_map(|&e| unwind_handler(&img, e)).collect();
    entries.extend(handlers);
    let funclets: Vec<u64> = img.pdata.keys().flat_map(|&e| msvc_funclets(&img, e)).collect();
    entries.extend(funclets);
    for (_, m) in &mods {
        entries.insert(m.entry);
        entries.extend(m.exports.iter().map(|e| e.2).filter(|&a| img.is_code(a)));
        entries.extend(m.seeds.iter().copied());
    }
    let mut funcs: BTreeMap<u64, Func> = BTreeMap::new();
    let mut found32: Option<Found> = None;
    if bits == 32 {
        // no exception table: hard entries (they cut functions) and soft ones (they do not), see discover32
        // --entry <hex>: a function start the lifter is told about (a soft entry it did not find, or left out)
        let forced: BTreeSet<u64> = args.iter().enumerate().filter(|(_, a)| *a == "--entry").map(|(i, _)| u64::from_str_radix(args[i + 1].trim_start_matches("0x"), 16).expect("--entry hex")).collect();
        let mut found = discover32(&img, &entries, &forced);
        entries = found.hard.union(&found.soft).copied().collect();
        funcs = std::mem::take(&mut found.funcs);
        found32 = Some(found);
    } else {
        // code addresses stored in data (function pointer tables, vtables). A static Linux program's switches are
        // tables of absolute addresses in data too: their slots, once a jump through them is found, are no
        // function's entry, and the functions are found again without them (else every case would be a function
        // of its own and the loop around the switch a recursion)
        let roots = entries.clone();
        let mut slots: BTreeSet<u64> = BTreeSet::new();
        for _pass in 0..3 {
            entries = roots.clone();
            for (va, d, _) in &img.sections {
                if img.is_code(*va) {
                    continue;
                }
                for (k, w) in d.chunks_exact(8).enumerate() {
                    let t = u64::from_le_bytes(w.try_into().unwrap());
                    if img.is_code(t) && !slots.contains(&(va + 8 * k as u64)) && !img.imports.iter().any(|(s, _)| *s == va + 8 * k as u64) {
                        entries.insert(t);
                    }
                }
            }
            loop {
                let before = entries.len();
                funcs.clear();
                for &e in &entries.clone() {
                    let (f, calls, _) = decode_func(&img, e, &entries);
                    entries.extend(calls.into_iter().filter(|t| img.is_code(*t)));
                    funcs.insert(e, f);
                }
                if entries.len() == before {
                    break;
                }
            }
            if !img.elf { break; }
            let found: BTreeSet<u64> = funcs.values().flat_map(|f| f.data.iter().flat_map(|&(a, b)| (a..b).step_by(8))).collect();
            if found.is_subset(&slots) { break; }
            slots.extend(found);
        }
    }
    // a fallthrough into another function's entry is emitted as a call to it: make sure it exists
    let cut = found32.as_ref().map(|f| &f.hard).unwrap_or(&entries);
    let mut em = Emit { trace_pc: args.iter().any(|a| a == "--trace-pc"), img: &img, unimpl: BTreeMap::new(), avx: 0, unresolved: 0, cut, counted: BTreeSet::new(), unresolved_at: BTreeSet::new(), pointer_jumps: BTreeSet::new() };
    let _ = em.img;
    let parts: usize = args.iter().position(|a| a == "--split").map(|i| args[i + 1].parse().expect("--split N")).unwrap_or(1).max(1);
    // (a 64-bit title's files stay as they were: cpu.h takes GUEST_BITS as 64 when it is not defined)
    let mut header = String::from(if bits == 32 { "#define GUEST_BITS 32\n#include \"cpu.h\"\n" } else { "#include \"cpu.h\"\n" });
    for e in &entries {
        writeln!(header, "void f_{e:x}(Cpu*c);").unwrap();
    }
    let mut bodies: Vec<String> = vec![header.clone(); parts];
    let mut total = 0;
    let mut sizes = vec![0usize; parts];
    for (e, f) in &funcs {
        total += f.insns.len();
        // a Linux title's sigsetjmp is its setjmp (no signal is ever delivered, so there is no mask to keep) and its
        // longjmp is the runtime's: it resumes in the lifted function that called setjmp (rt/linux-x64/jump.c)
        let body = match if img.machine { None } else { img.jumps.get(e) } {
            Some(2) if img.jumps.values().any(|k| *k == 1) => format!("void f_{e:x}(Cpu*__restrict c){{f_{:x}(c);}}\n", img.jumps.iter().find(|(_, k)| **k == 1).unwrap().0),
            Some(3) => format!("void f_{e:x}(Cpu*__restrict c){{rt_longjmp(c);}}\n"),
            // a machine's static-call trampoline (Linux's __SCT__*): the kernel rewrites it, so it is decoded as it is now
            _ if img.machine && img.named.iter().any(|(n, v)| n.starts_with("__SCT__") && v.0 == *e) => format!("void f_{e:x}(Cpu*__restrict c){{rt_static_call(c,0x{e:x}ull);}}\n"),
            _ => em.func(*e, f, &entries),
        };
        let k = (0..parts).min_by_key(|&k| sizes[k]).unwrap();
        sizes[k] += body.len();
        bodies[k].push_str(&body);
    }
    let mut out = std::mem::take(&mut bodies[0]);
    // dispatch table for indirect calls, sorted by guest address
    writeln!(out, "const unsigned fn_count={};", entries.len()).unwrap();
    writeln!(out, "const u64 fn_addrs[]={{{}}};", entries.iter().map(|e| format!("0x{e:x}ull")).collect::<Vec<_>>().join(",")).unwrap();
    writeln!(out, "void(*const fn_ptrs[])(Cpu*)={{{}}};", entries.iter().map(|e| format!("f_{e:x}")).collect::<Vec<_>>().join(",")).unwrap();
    if img.elf {
        // a Linux title: every return address, and the function (its index above) that can be entered there
        let index: BTreeMap<u64, usize> = entries.iter().enumerate().map(|(k, e)| (*e, k)).collect();
        let mut sites: BTreeMap<u64, usize> = BTreeMap::new();
        for (e, f) in &funcs {
            if img.jumps.contains_key(e) { continue; }
            for r in f.rets.iter().filter(|r| f.insns.contains_key(r) && f.leaders.contains(r)) { sites.entry(*r).or_insert(index[e]); }
        }
        writeln!(out, "const unsigned rt_resume_count={};", sites.len()).unwrap();
        writeln!(out, "const u64 rt_resume_at[]={{{}0}};", sites.keys().map(|a| format!("0x{a:x}ull,")).collect::<String>()).unwrap();
        writeln!(out, "const unsigned rt_resume_fn[]={{{}0}};", sites.values().map(|k| format!("{k},")).collect::<String>()).unwrap();
    }
    // the modules: each image's bytes stay in the user's file; the runtime copies them in and applies relocations
    if img.machine {
        // where the machine enters the kernel and which top page table it starts on (rt/microvm-x64/run.c)
        let named = |n: &str| img.named.get(n).map(|v| v.0).unwrap_or(0);
        let entry = if named("common_startup_64") != 0 { named("common_startup_64") } else { img.entry };
        writeln!(out, "const u64 rt_machine_entry=0x{entry:x}ull;const u64 rt_machine_pgt=0x{:x}ull;", named("early_top_pgt")).unwrap();
    }
    writeln!(out, "#include \"modules.h\"").unwrap();
    let mut defs = Vec::new();
    let mut import_slots = Vec::new();
    for (k, (name, m)) in mods.iter().enumerate() {
        let secs: Vec<String> = m.sections.iter().zip(&m.raw).map(|((va, d, vs), off)| format!("{{0x{va:x}ull,{off},{},{vs}}}", d.len())).collect();
        writeln!(out, "static const ModuleSection secs_{k}[]={{{}}};", if secs.is_empty() { "{0}".into() } else { secs.join(",") }).unwrap();
        writeln!(out, "static const u64 relocs_{k}[]={{{}0}};", m.relocs.iter().map(|r| format!("0x{r:x}ull,")).collect::<String>()).unwrap();
        let dir = |n: usize| m.dirs.get(n).copied().unwrap_or((0, 0));
        let va = |n: usize| if dir(n).0 != 0 { m.base + dir(n).0 } else { 0 };
        let first_import = import_slots.len();
        for (slot, imp) in &m.imports {
            import_slots.push((*slot, imp.clone(), resolved.get(slot).copied().unwrap_or(0)));
        }
        defs.push(format!(
            "{{\"{}\",0x{:x}ull,0x{:x}ull,0x{:x}ull,{},0x{:x}ull,0x{:x}ull,0x{:x}ull,{},0x{:x}ull,0x{:x}ull,{},secs_{k},{},relocs_{k},{},{},{}}}",
            name.replace('\\', "/"), m.base, m.orig_base, m.size, m.is_dll as u32, m.entry, va(9), va(3), dir(3).1, va(0), m.stack_reserve.max(1 << 20),
            (k >= eager) as u32, m.sections.len(), m.relocs.len(), first_import, m.imports.len()
        ));
    }
    writeln!(out, "const ModuleDef rt_modules[]={{{}}};const unsigned rt_module_count={};", defs.join(","), mods.len()).unwrap();
    writeln!(out, "const u64 import_base=0x{import_base:x}ull;const unsigned import_count={};", import_slots.len()).unwrap();
    if img.ps5 {
        // a PS5 title: what its relocations hold once the image is at its base (the file has 0 there: RELA)
        writeln!(out, "const u64 rt_reloc_values[]={{{}0}};", mods.iter().flat_map(|(_, m)| m.reloc_values.iter()).map(|v| format!("0x{v:x}ull,")).collect::<String>()).unwrap();
    }
    writeln!(out, "const u64 import_slots[]={{{}0}};", import_slots.iter().map(|(s, _, _)| format!("0x{s:x}ull,")).collect::<String>()).unwrap();
    writeln!(out, "const u64 import_targets[]={{{}0}};", import_slots.iter().map(|(_, _, t)| format!("0x{t:x}ull,")).collect::<String>()).unwrap();
    writeln!(out, "const char*const import_names[]={{{}0}};", import_slots.iter().map(|(_, n, _)| format!("\"{n}\",")).collect::<String>()).unwrap();
    // a part is rewritten only when it changed, so an unchanged part is not compiled again
    let write = |path: &str, text: &str| {
        if std::fs::read(path).map(|old| old != text.as_bytes()).unwrap_or(true) {
            std::fs::write(path, text).expect("write output");
        }
    };
    write(&args[2], &out);
    let stem = args[2].strip_suffix(".c").unwrap_or(&args[2]).to_string();
    for k in parts..64 {
        let _ = std::fs::remove_file(format!("{stem}_{k}.c"));
        let _ = std::fs::remove_file(format!("{stem}_{k}.c.o"));
    }
    for (k, b) in bodies.iter().enumerate().skip(1) {
        write(&format!("{stem}_{k}.c"), b);
    }
    // what a build needs to know about the title, for the shell: the guest's width and where its images are
    write(&format!("{stem}.env"), &format!("GUEST_BITS={bits}\nIMAGE_LOW=0x{image_low:x}\nIMAGE_HIGH=0x{image_high:x}\nFIXED_BASE={}\nOS={}\n", u32::from(mods.iter().any(|(_, m)| m.fixed)), if img.machine { "microvm" } else if img.elf { "linux" } else if img.ps5 { "ps5" } else { "windows" }));

    if let Some(found) = &found32 {
        census32(&img, &mods, found, &funcs, &em, resolved.len());
        return;
    }
    let missing: u32 = em.unimpl.values().sum();
    if img.machine {
        eprintln!("lifted {} functions ({} named by the symbols) in a machine's image, {} instructions", entries.len(), img.seeds.len(), total);
    } else if img.elf {
        eprintln!("lifted {} functions ({} named by the symbols and the unwind tables) in a Linux program, {} instructions", entries.len(), img.seeds.len(), total);
    } else {
        eprintln!("lifted {} functions ({} from the exception tables) in {} images, {} instructions, {} imports ({} between the images)", entries.len(), img.pdata.len(), mods.len(), total, img.imports.len(), resolved.len());
    }
    eprintln!("{} jump tables recovered; {} `jmp reg` left as indirect calls (tail calls through pointers, or switches not recognised)", funcs.values().map(|f| f.tables.len()).sum::<usize>(), em.unresolved);
    eprintln!("{} AVX-or-later instructions avx.rs does not handle become traps (the virtual CPU does not report AVX: code that checks CPUID takes its SSE paths)", em.avx);
    eprintln!("not lifted yet, in the other functions: {missing} instructions ({:.2} %)", 100.0 * f64::from(missing) / total.max(1) as f64);
    let mut v: Vec<_> = em.unimpl.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1));
    for (m, n) in v.iter().take(200) {
        eprintln!("  {n:6}  {m}");
    }
}

/// The census of a 32-bit title: what was found and how, and what is left.
fn census32(img: &Image, mods: &[(String, Image)], found: &Found, funcs: &BTreeMap<u64, Func>, em: &Emit, between: usize) {
    let debug = std::env::var_os("LIFTER_DEBUG").is_some();
    let total: usize = funcs.values().map(|f| f.insns.len()).sum();
    let mut cover = Cover::new(img);
    for f in funcs.values() {
        cover.add(f);
    }
    let unique = cover.map.iter().filter(|&&b| b == 1).count();
    eprintln!("32-bit title: {} images, {} imports ({} between the images)", mods.len(), img.imports.len(), between);
    let inside: usize = funcs.values().map(|f| f.alts.len()).sum();
    eprintln!("lifted {} functions: {} hard (entry points, exports, call targets), {} soft (code addresses in data and immediates; {} of them by their first bytes in code nothing referred to)",
        found.hard.len() + found.soft.len(), found.hard.len(), found.soft.len(), found.by_pattern);
    eprintln!("  {inside} of the soft entries are inside another function: one more way into its body, {} bodies in all", funcs.len());
    eprintln!("  soft candidates left out: {} inside an instruction of known code, {} that do not decode as code", found.inside, found.not_code);
    eprintln!("{total} instructions lifted, {unique} distinct (the code of a soft entry may also be part of another function)");
    let tables: BTreeSet<u64> = funcs.values().flat_map(|f| f.tables.keys().copied()).collect();
    eprintln!("{} jump tables recovered; {} indirect jumps left unresolved (`jmp reg`, or a table with no bound); {} jumps through a pointer in memory (imports, virtual methods, function pointers: resolved when they run)",
        tables.len(), em.unresolved_at.len(), em.pointer_jumps.len());
    if debug {
        for a in &em.unresolved_at {
            eprintln!("  unresolved: 0x{a:x}");
        }
    }
    // each image, then each of its executable sections: instructions, tables, padding between functions, and the rest
    for (name, m) in mods {
        let within = |a: &u64| *a >= m.base && *a < m.base + m.size;
        eprintln!("{name} (0x{:x}..0x{:x}{}): {} hard and {} soft entries, {} distinct instructions, {} jump tables", m.base, m.base + m.size, if m.fixed { ", not relocatable" } else { "" },
            found.hard.iter().filter(|a| within(a)).count(), found.soft.iter().filter(|a| within(a)).count(),
            (m.base..m.base + m.size).filter(|a| cover.get(*a) == 1).count(), tables.iter().filter(|a| within(a)).count());
        for (k, &(a, e)) in m.text.iter().enumerate() {
            let end = m.sections.iter().find(|s| s.0 == a).map(|s| a + (s.1.len() as u64).min(e - a)).unwrap_or(e);
            let (mut code, mut table, mut pad, mut rest) = (0u64, 0u64, 0u64, 0u64);
            let mut p = a;
            let mut gap_start = None;
            while p < end {
                match cover.get(p) {
                    0 => {
                        let n = img.bytes(p).and_then(|b| padding(img.bits, b, p)).filter(|n| (p + 1..p + n).all(|x| cover.get(x) == 0));
                        match n {
                            Some(n) => { pad += n; p += n; }
                            None => { rest += 1; if gap_start.is_none() { gap_start = Some(p); } p += 1; continue; }
                        }
                    }
                    3 => { table += 1; p += 1; }
                    _ => { code += 1; p += 1; }
                }
                if let Some(g) = gap_start.take() {
                    if debug { eprintln!("  not reached: 0x{g:x}..0x{p:x}"); }
                }
            }
            if let Some(g) = gap_start.filter(|_| debug) {
                eprintln!("  not reached: 0x{g:x}..0x{end:x}");
            }
            let size = (end - a).max(1) as f64;
            eprintln!("  executable section {k} (0x{a:x}..0x{end:x}): {:.2} % instructions, {:.2} % jump tables, {:.2} % padding, {:.2} % not reached ({rest} bytes)",
                100.0 * code as f64 / size, 100.0 * table as f64 / size, 100.0 * pad as f64 / size, 100.0 * rest as f64 / size);
        }
    }
    eprintln!("{} AVX-or-later instructions avx.rs does not handle become traps (the virtual CPU does not report AVX)", em.avx);
    let missing: u32 = em.unimpl.values().sum();
    eprintln!("not lifted, each stops the title if reached: {missing} instructions ({:.2} % of the distinct ones)", 100.0 * f64::from(missing) / unique.max(1) as f64);
    let mut v: Vec<_> = em.unimpl.iter().collect();
    v.sort_by(|a, b| b.1.cmp(a.1));
    for (m, n) in v.iter().take(200) {
        eprintln!("  {n:6}  {m}");
    }
}

fn sti(r: Register) -> u32 {
    r as u32 - Register::ST0 as u32
}
fn is_st(i: &Instruction, n: u32) -> bool {
    i.op_kind(n) == OpKind::Register && (Register::ST0 as u32..=Register::ST7 as u32).contains(&(i.op_register(n) as u32))
}
/// An x87 memory operand as a register's value (rt/cpu.h fpx: a double, or for a Linux title 64 bits of precision).
fn fmem(i: &Instruction) -> String {
    use iced_x86::MemorySize as S;
    match i.memory_size() {
        S::Float32 => format!("(fpx)MF32({})", ea(i)),
        S::Float64 => format!("(fpx)MF64({})", ea(i)),
        S::Float80 => format!("f80_load(c,{})", ea(i)),
        S::Int16 => format!("(fpx)(s16)M16({})", ea(i)),
        S::Int32 => format!("(fpx)(s32)M32({})", ea(i)),
        S::Int64 => format!("(fpx)(s64)M64({})", ea(i)),
        _ => format!("(fpx)MF64({})", ea(i)),
    }
}

/// x87 add / sub / mul / div in all their forms. `prec` (32-bit code): the result is rounded to the precision
/// the control word selects.
fn farith(i: &Instruction, prec: bool) -> String {
    let m = i.mnemonic();
    let (dst, src) = if i.op_count() == 1 && i.op_kind(0) == OpKind::Memory {
        ("ST(0)".to_string(), fmem(i))
    } else if i.op_count() == 2 {
        (format!("ST({})", sti(i.op_register(0))), format!("ST({})", sti(i.op_register(1))))
    } else {
        ("ST(1)".to_string(), "ST(0)".to_string())
    };
    let name = format!("{m:?}").to_lowercase();
    let base = name.trim_start_matches('f').trim_start_matches('i').trim_end_matches('p');
    let e = match base { "add" => format!("{dst}+{src}"), "sub" => format!("{dst}-{src}"), "subr" => format!("{src}-{dst}"), "mul" => format!("{dst}*{src}"), "div" => format!("{dst}/{src}"), _ => format!("{src}/{dst}") };
    let pop = if name.ends_with('p') { "fpop(c);" } else { "" };
    if prec {
        format!("{{fpx r=fprec(c,{e});{dst}=r;{pop}}}")
    } else {
        format!("{{fpx r=FXR(c,{e});{dst}=r;{pop}}}") // (FXR: a Linux title's precision control; nothing otherwise)
    }
}

/// Instructions beyond the first slice: atomics, string operations, bit operations, x87, CPUID and more SSE.
fn extra(i: &Instruction, elf: bool, machine: bool) -> Option<String> {
    use Mnemonic as M;
    let m = i.mnemonic();
    let mem0 = i.op_count() > 0 && i.op_kind(0) == OpKind::Memory;
    let bits = if i.op_count() > 0 && matches!(i.op_kind(0), OpKind::Register | OpKind::Memory) { opsize(i, 0) * 8 } else { 64 };
    let xd = || i.op0_register().number();
    // ---- string operations (rep: count in RCX; the direction flag is assumed clear) ----
    let string = |kind: &str, size: usize| -> String {
        let (n, u) = (size, size * 8);
        // the direction flag lives in the function's flags: clear unless this function set it (`std`; the calling
        // conventions keep it clear across calls), so the compiler drops the backward case everywhere else
        let d = format!("(f.df?-{n}ull:{n}ull)");
        let one = match kind {
            "stos" => format!("M{u}(R[RDI])=(u{u})R[0];R[RDI]+={d};"),
            "movs" => format!("M{u}(R[RDI])=M{u}(R[RSI]);R[RDI]+={d};R[RSI]+={d};"),
            "lods" => format!("{}R[RSI]+={d};", wreg(reg_of(size), &format!("M{u}(R[RSI])"))),
            "cmps" => format!("(void)sub{u}(&f,M{u}(R[RSI]),M{u}(R[RDI]),0);R[RDI]+={d};R[RSI]+={d};"),
            _ => format!("(void)sub{u}(&f,(u{u})R[0],M{u}(R[RDI]),0);R[RDI]+={d};"), // scas
        };
        if i.has_rep_prefix() && (kind == "stos" || kind == "movs" || kind == "lods") {
            let slow = format!("while(R[RCX]){{{one}R[RCX]--;}}");
            if machine {
                return slow; // element by element, through the MMU: a fault leaves exact counts
            }
            if kind == "stos" && size == 1 {
                return format!("if(!f.df){{__builtin_memset((void*)GP(R[RDI]),(u8)R[0],R[RCX]);R[RDI]+=R[RCX];R[RCX]=0;}}else{{{slow}}}");
            }
            if kind == "movs" && size == 1 {
                return format!("{{u64 n=R[RCX];if(!f.df&&R[RDI]-R[RSI]>=n){{__builtin_memcpy((void*)GP(R[RDI]),(void*)GP(R[RSI]),n);R[RDI]+=n;R[RSI]+=n;R[RCX]=0;}}else{{{slow}}}}}");
            }
            return slow;
        }
        if i.has_repe_prefix() {
            return format!("while(R[RCX]){{{one}R[RCX]--;if(!f.zf)break;}}");
        }
        if i.has_repne_prefix() {
            return format!("while(R[RCX]){{{one}R[RCX]--;if(f.zf)break;}}");
        }
        one
    };
    let s = match m {
        M::Stosb => string("stos", 1), M::Stosw => string("stos", 2), M::Stosd => string("stos", 4), M::Stosq => string("stos", 8),
        M::Movsb => string("movs", 1), M::Movsw => string("movs", 2), M::Movsq => string("movs", 8),
        M::Lodsb => string("lods", 1), M::Lodsw => string("lods", 2), M::Lodsd => string("lods", 4), M::Lodsq => string("lods", 8),
        M::Cmpsb => string("cmps", 1), M::Cmpsw => string("cmps", 2), M::Cmpsq => string("cmps", 8),
        M::Scasb => string("scas", 1), M::Scasw => string("scas", 2), M::Scasd => string("scas", 4), M::Scasq => string("scas", 8),
        M::Movsd if i.op_kind(0) == OpKind::MemoryESRDI => string("movs", 4),
        M::Cmpsd if i.op_kind(0) == OpKind::MemorySegRSI => string("cmps", 4),
        M::Cld => "f.df=0;".into(),
        M::Std => "f.df=1;".into(),
        // ---- atomics ----
        M::Xchg if mem0 || i.op_kind(1) == OpKind::Memory => {
            let (mn, rn) = if mem0 { (0, 1) } else { (1, 0) };
            let _ = mn;
            format!("{{u{bits} t=__atomic_exchange_n(ATOM{bits}({}),({}),5);{}}}", ea(i), rreg(i.op_register(rn)), wreg(i.op_register(rn), "t"))
        }
        M::Xadd if mem0 => format!("{{u{bits} b={};u{bits} o=__atomic_fetch_add(ATOM{bits}({}),b,5);(void)add{bits}(&f,o,b,0);{}}}", rreg(i.op_register(1)), ea(i), wreg(i.op_register(1), "o")),
        M::Xadd => format!("{{u{bits} a={},b={};{}{}}}", rd(i, 0), rd(i, 1), wr(i, 0, &format!("add{bits}(&f,a,b,0)")), wreg(i.op_register(1), "a")),
        M::Cmpxchg => {
            let acc = match bits { 8 => Register::AL, 16 => Register::AX, 32 => Register::EAX, _ => Register::RAX };
            if mem0 {
                format!("{{u{bits} e={},o=e;u8 ok=__atomic_compare_exchange_n(ATOM{bits}({}),&o,({}),0,5,5);(void)sub{bits}(&f,e,o,0);if(!ok){{{}}}}}", rreg(acc), ea(i), rreg(i.op_register(1)), wreg(acc, "o"))
            } else {
                format!("{{u{bits} e={},o={};(void)sub{bits}(&f,e,o,0);if(f.zf){{{}}}else{{{}}}}}", rreg(acc), rd(i, 0), wr(i, 0, &rd(i, 1)), wreg(acc, "o"))
            }
        }
        M::Add | M::Sub | M::And | M::Or | M::Xor | M::Inc | M::Dec if i.has_lock_prefix() && mem0 => {
            let b = if matches!(m, M::Inc | M::Dec) { "1".to_string() } else { rd(i, 1) };
            let (op, fl) = match m {
                M::Add | M::Inc => ("add", format!("add{bits}(&f,o,b,0)")),
                M::Sub | M::Dec => ("sub", format!("sub{bits}(&f,o,b,0)")),
                M::And => ("and", format!("logic{bits}(&f,o&b)")),
                M::Or => ("or", format!("logic{bits}(&f,o|b)")),
                _ => ("xor", format!("logic{bits}(&f,o^b)")),
            };
            let keep_cf = if matches!(m, M::Inc | M::Dec) { "u8 cf=f.cf;" } else { "" };
            let restore = if matches!(m, M::Inc | M::Dec) { "f.cf=cf;" } else { "" };
            format!("{{u{bits} b=(u{bits}){b};u{bits} o=__atomic_fetch_{op}(ATOM{bits}({}),b,5);{keep_cf}(void){fl};{restore}}}", ea(i))
        }
        M::Mfence | M::Lfence | M::Sfence => "__atomic_thread_fence(5);".into(),
        M::Prefetcht0 | M::Prefetcht1 | M::Prefetcht2 | M::Prefetchnta | M::Prefetchw | M::Wait => String::new(),
        // ---- bit operations ----
        M::Bt | M::Bts | M::Btr | M::Btc => {
            let mask = bits - 1;
            let (addr, bit) = if mem0 && i.op_kind(1) == OpKind::Register {
                // a register bit offset reaches past the operand
                (format!("({}+(u64)((s64){}>>{})*{})", ea(i), rd(i, 1).replace(&format!("(u{bits})"), ""), (bits as f64).log2() as u32, bits / 8), format!("({}&{mask})", rd(i, 1)))
            } else {
                (if mem0 { ea(i) } else { String::new() }, format!("({}&{mask})", rd(i, 1)))
            };
            let new = match m { M::Bts => "v|(1ull<<n)", M::Btr => "v&~(1ull<<n)", M::Btc => "v^(1ull<<n)", _ => "" };
            if mem0 {
                let at = format!("M{bits}({addr})");
                if new.is_empty() {
                    format!("{{u64 n={bit};f.cf=({at}>>n)&1;}}")
                } else if i.has_lock_prefix() {
                    let op = match m { M::Bts => "or", M::Btr => "and", _ => "xor" };
                    let arg = match m { M::Btr => format!("(u{bits})~(1ull<<n)"), _ => format!("(u{bits})(1ull<<n)") };
                    format!("{{u64 n={bit};u{bits} o=__atomic_fetch_{op}(ATOM{bits}({addr}),{arg},5);f.cf=(o>>n)&1;}}")
                } else {
                    format!("{{u64 n={bit};u{bits} v={at};f.cf=(v>>n)&1;{at}=(u{bits})({new});}}")
                }
            } else {
                let set = if new.is_empty() { String::new() } else { wr(i, 0, &format!("(u{bits})({new})")) };
                format!("{{u64 n={bit};u64 v={};f.cf=(v>>n)&1;{set}}}", rd(i, 0))
            }
        }
        M::Bsf | M::Tzcnt => wr(i, 0, &format!("bsf{bits}(&f,{},{})", rd(i, 0), rd(i, 1))),
        M::Bsr | M::Lzcnt => wr(i, 0, &format!("bsr{bits}(&f,{},{})", rd(i, 0), rd(i, 1))),
        M::Popcnt => wr(i, 0, &format!("popcnt(&f,{})", rd(i, 1))),
        M::Bswap if bits == 64 => wr(i, 0, &format!("__builtin_bswap64({})", rd(i, 0))),
        M::Bswap => wr(i, 0, &format!("__builtin_bswap32({})", rd(i, 0))),
        M::Shld | M::Shrd => {
            let n = if is_imm(i.op_kind(2)) { format!("{}", i.immediate(2) & (if bits == 64 { 63 } else { 31 })) } else { format!("(R[1]&{})", if bits == 64 { 63 } else { 31 }) };
            let e = if m == M::Shld {
                format!("(n?(u{bits})((a<<n)|((u64)b>>({bits}-n))):a)")
            } else {
                format!("(n?(u{bits})((a>>n)|((u64)b<<({bits}-n))):a)")
            };
            let cf = if m == M::Shld { format!("(a>>({bits}-n))&1") } else { "(a>>(n-1))&1".into() };
            format!("{{u{bits} a={},b={};u32 n={n};if(n){{f.cf={cf};u{bits} r={e};{}(void)logic{bits}(&f,r);f.cf=(n?({cf}):0);}}}}", rd(i, 0), rd(i, 1), wr(i, 0, "r"))
        }
        M::Sahf => "f.sf=(R[0]>>15)&1;f.zf=(R[0]>>14)&1;f.af=(R[0]>>12)&1;f.pf=(R[0]>>10)&1;f.cf=(R[0]>>8)&1;".into(),
        M::Lahf => "R[0]=(R[0]&~0xFF00ull)|((u64)(f.sf<<7|f.zf<<6|f.af<<4|f.pf<<2|2|f.cf)<<8);".into(),
        M::Clc => "f.cf=0;".into(),
        M::Stc => "f.cf=1;".into(),
        M::Cmc => "f.cf=!f.cf;".into(),
        M::Leave => "R[RSP]=R[RBP];R[RBP]=POP64();".into(),
        M::Cwd => "R[2]=(R[2]&~0xFFFFull)|(u16)((s16)R[0]>>15);".into(),
        M::Cbw => "R[0]=(R[0]&~0xFFFFull)|(u16)(s16)(s8)R[0];".into(),
        M::Mul if bits <= 16 => {
            if bits == 8 { format!("{{u16 p=(u16)(u8)R[0]*{};R[0]=(R[0]&~0xFFFFull)|p;f.cf=f.of=(p>>8)!=0;}}", rd(i, 0)) }
            else { format!("{{u32 p=(u32)(u16)R[0]*{};R[0]=(R[0]&~0xFFFFull)|(u16)p;R[2]=(R[2]&~0xFFFFull)|(p>>16);f.cf=f.of=(p>>16)!=0;}}", rd(i, 0)) }
        }
        M::Imul if i.op_count() == 1 && bits == 32 => format!("{{s64 p=(s64)(s32)R[0]*(s32){};R[0]=(u32)p;R[2]=(u32)(p>>32);f.cf=f.of=p!=(s32)p;}}", rd(i, 0)),
        M::Div if bits == 8 => format!("{{u16 n=(u16)R[0];u8 d={};if(!d)rt_trap(0x{:x});R[0]=(R[0]&~0xFFFFull)|(u16)((n%d)<<8|(u8)(n/d));}}", rd(i, 0), i.ip()),
        M::Div if bits == 16 => format!("{{u32 n=((u32)(u16)R[2]<<16)|(u16)R[0];u16 d={};if(!d)rt_trap(0x{:x});R[0]=(R[0]&~0xFFFFull)|(u16)(n/d);R[2]=(R[2]&~0xFFFFull)|(u16)(n%d);}}", rd(i, 0), i.ip()),
        // ---- the virtual CPU ----
        M::Cpuid => "rt_cpuid(c);".into(),
        M::Rdtsc => "{u64 t=rt_rdtsc();R[0]=(u32)t;R[2]=t>>32;}".into(),
        M::Rdtscp => "{u64 t=rt_rdtsc();R[0]=(u32)t;R[2]=t>>32;R[1]=0;}".into(),
        M::Xgetbv => "R[0]=3;R[2]=0;".into(),
        M::Rdrand | M::Rdseed => format!("{}f.cf=1;f.zf=f.sf=f.of=f.pf=f.af=0;", wr(i, 0, "rt_random64()")),
        M::Int => format!("rt_trap(0x{:x}ull);", i.ip()),
        M::Stmxcsr => format!("M32({})=c->mxcsr;", ea(i)),
        M::Ldmxcsr => format!("c->mxcsr=M32({});", ea(i)),
        // ---- x87 ----
        M::Fld if is_st(i, 0) => format!("{{fpx v=ST({});fpush(c,v);}}", sti(i.op_register(0))),
        M::Fld | M::Fild => format!("fpush(c,{});", fmem(i)),
        M::Fld1 => "fpush(c,1.0);".into(),
        M::Fldz => "fpush(c,0.0);".into(),
        M::Fldl2e => "fpush(c,FXC(1.44269504088896340735992468100189214));".into(),
        M::Fldln2 => "fpush(c,FXC(0.693147180559945309417232121458176568));".into(),
        M::Fldl2t => "fpush(c,FXC(3.32192809488736234787031942948939018));".into(),
        M::Fldlg2 => "fpush(c,FXC(0.301029995663981195213738894724493027));".into(),
        M::Fldpi => "fpush(c,FXC(3.14159265358979323846264338327950288));".into(),
        M::Fst | M::Fstp => {
            use iced_x86::MemorySize as S;
            let store = if is_st(i, 0) {
                format!("ST({})=ST(0);", sti(i.op_register(0)))
            } else {
                match i.memory_size() {
                    S::Float32 => format!("MF32({})=(float)ST(0);", ea(i)),
                    S::Float80 => format!("f80_store(c,{},ST(0));", ea(i)),
                    _ => format!("MF64({})=(double)ST(0);", ea(i)),
                }
            };
            format!("{store}{}", if m == M::Fstp { "fpop(c);" } else { "" })
        }
        M::Fist | M::Fistp | M::Fisttp => {
            let b = i.memory_size().size() * 8;
            let v = if m == M::Fisttp { format!("(s64)FX_TRUNC(ST(0))") } else { format!("fist(c,ST(0),{b})") };
            format!("M{b}({})=(u{b}){v};{}", ea(i), if m == M::Fist { "" } else { "fpop(c);" })
        }
        M::Fxch => format!("{{fpx t=ST(0);ST(0)=ST({n});ST({n})=t;}}", n = if i.op_count() > 0 { sti(i.op_register(i.op_count() - 1)) } else { 1 }), // `fxch st(i)` is (st0, sti)
        M::Fadd | M::Faddp | M::Fiadd | M::Fsub | M::Fsubp | M::Fisub | M::Fsubr | M::Fsubrp | M::Fisubr | M::Fmul | M::Fmulp | M::Fimul | M::Fdiv | M::Fdivp | M::Fidiv | M::Fdivr | M::Fdivrp | M::Fidivr => farith(i, false),
        M::Fcom | M::Fcomp | M::Fcompp | M::Fucom | M::Fucomp | M::Fucompp | M::Ficom | M::Ficomp | M::Ftst => {
            let src = if m == M::Ftst { "(fpx)0.0".to_string() } else if i.op_count() > 0 && i.op_kind(0) == OpKind::Memory { fmem(i) } else if i.op_count() > 0 && is_st(i, i.op_count() - 1) { format!("ST({})", sti(i.op_register(i.op_count() - 1))) } else { "ST(1)".into() };
            let pops = match m { M::Fcompp | M::Fucompp => "fpop(c);fpop(c);", M::Fcomp | M::Fucomp | M::Ficomp => "fpop(c);", _ => "" };
            format!("fcom_sw(c,ST(0),{src});{pops}")
        }
        M::Fcomi | M::Fcomip | M::Fucomi | M::Fucomip => format!("fcmpx(&f,ST(0),ST({}));{}", sti(i.op_register(i.op_count() - 1)), if matches!(m, M::Fcomip | M::Fucomip) { "fpop(c);" } else { "" }),
        M::Fnstsw | M::Fstsw => if i.op_kind(0) == OpKind::Register { "R[0]=(R[0]&~0xFFFFull)|fstsw(c);".into() } else { format!("M16({})=fstsw(c);", ea(i)) },
        M::Fnstcw | M::Fstcw => format!("M16({})=c->fcw;", ea(i)),
        M::Fldcw => format!("c->fcw=M16({});", ea(i)),
        M::Fninit | M::Finit => "c->ftop=0;c->fcw=0x37F;c->fsw=0;".into(),
        M::Fnclex | M::Fclex => "c->fsw&=0x7F00;".into(),
        M::Fchs => "ST(0)=-ST(0);".into(),
        M::Fabs => "ST(0)=FX_ABS(ST(0));".into(),
        M::Fsqrt => "ST(0)=FXR(c,FX_SQRT(ST(0)));".into(),
        M::Frndint => "ST(0)=fround(c,ST(0));".into(),
        M::Fscale => "ST(0)=FXR(c,FX_SCALB(ST(0),(int)FX_TRUNC(ST(1))));".into(),
        M::F2xm1 => "ST(0)=expm1(ST(0)*0.6931471805599453);".into(),
        M::Fyl2x => "{double r=(double)ST(1)*log2((double)ST(0));fpop(c);ST(0)=r;}".into(),
        M::Fyl2xp1 => "{double r=(double)ST(1)*log1p((double)ST(0))*1.4426950408889634;fpop(c);ST(0)=r;}".into(),
        M::Fxam => "fxam(c);".into(),
        M::Fprem | M::Fprem1 if elf => format!("fprem(c,{});", u8::from(m == M::Fprem1)), // (exact, as a C library's fmodl needs)
        M::Fprem | M::Fprem1 => "{double q=ST(0)/ST(1);ST(0)=ST(0)-ST(1)*(m_is_prem1?__builtin_rint(q):__builtin_trunc(q));c->fsw&=~0x400;}".replace("m_is_prem1", if m == M::Fprem1 { "1" } else { "0" }),
        M::Ffree => String::new(),
        // ---- more SSE ----
        M::Pinsrq | M::Pinsrd | M::Pinsrb => {
            let (f, n) = match m { M::Pinsrq => ("q", 1), M::Pinsrd => ("d", 3), _ => ("b", 15) };
            let src = if i.op_kind(1) == OpKind::Memory { format!("M{}({})", match m { M::Pinsrq => 64, M::Pinsrd => 32, _ => 8 }, ea(i)) } else { rreg(i.op_register(1)) };
            format!("X[{}].{f}[{}]={src};", xd(), i.immediate(2) & n)
        }
        M::Pextrq | M::Pextrd | M::Pextrb => {
            let (f, n, b) = match m { M::Pextrq => ("q", 1, 64), M::Pextrd => ("d", 3, 32), _ => ("b", 15, 8) };
            let v = format!("X[{}].{f}[{}]", i.op_register(1).number(), i.immediate(2) & n);
            if mem0 { format!("M{b}({})={v};", ea(i)) } else { wreg(i.op_register(0), &v) }
        }
        M::Minsd | M::Maxsd | M::Minss | M::Maxss => {
            let (t, fld) = if matches!(m, M::Minsd | M::Maxsd) { ("f64", "pd") } else { ("f32", "ps") };
            let src = if i.op_kind(1) == OpKind::Register { format!("X[{}].{fld}[0]", i.op_register(1).number()) } else { format!("M{}({})", if t == "f64" { "F64" } else { "F32" }, ea(i)) };
            let op = if matches!(m, M::Minsd | M::Minss) { "<" } else { ">" };
            format!("{{typeof(X[0].{fld}[0]) a=X[{d}].{fld}[0],b={src};X[{d}].{fld}[0]=a{op}b?a:b;}}", d = xd())
        }
        M::Minps | M::Maxps | M::Minpd | M::Maxpd => format!("X[{d}]={}(X[{d}],{});", format!("{m:?}").to_lowercase(), xsrc(i), d = xd()),
        M::Movddup => format!("{{u64 v={};X[{d}].q[0]=v;X[{d}].q[1]=v;}}", if i.op_kind(1) == OpKind::Memory { format!("M64({})", ea(i)) } else { format!("X[{}].q[0]", i.op_register(1).number()) }, d = xd()),
        M::Movhps | M::Movhpd => if mem0 { format!("M64({})=X[{}].q[1];", ea(i), i.op_register(1).number()) } else { format!("X[{}].q[1]=M64({});", xd(), ea(i)) },
        M::Movlps | M::Movlpd => if mem0 { format!("M64({})=X[{}].q[0];", ea(i), i.op_register(1).number()) } else { format!("X[{}].q[0]=M64({});", xd(), ea(i)) },
        M::Movhlps => format!("X[{}].q[0]=X[{}].q[1];", xd(), i.op_register(1).number()),
        M::Shufpd => format!("X[{d}]=shufpd(X[{d}],{},{});", xsrc(i), i.immediate(2), d = xd()),
        M::Pmulhw | M::Pmulhuw | M::Pmaddwd | M::Pmaddubsw | M::Pminsw | M::Pmaxsw | M::Pminub | M::Pmaxub | M::Pminsb | M::Pmaxsb | M::Pminuw | M::Pmaxuw | M::Pminsd | M::Pmaxsd | M::Pminud | M::Pmaxud
        | M::Paddsb | M::Psubsb | M::Paddsw | M::Psubsw | M::Paddusb | M::Psubusb | M::Paddusw | M::Psubusw | M::Pavgb | M::Pavgw | M::Pmulld | M::Psadbw
        | M::Packsswb | M::Packssdw | M::Packusdw | M::Pshufb | M::Pcmpeqq | M::Pcmpgtq => format!("X[{d}]={}(X[{d}],{});", format!("{m:?}").to_lowercase(), xsrc(i), d = xd()),
        M::Pabsb => format!("X[{}]=pabs_b({});", xd(), xsrc(i)),
        M::Pabsw => format!("X[{}]=pabs_w({});", xd(), xsrc(i)),
        M::Pabsd => format!("X[{}]=pabs_d({});", xd(), xsrc(i)),
        M::Palignr => format!("X[{d}]=palignr(X[{d}],{},{});", xsrc(i), i.immediate(2), d = xd()),
        M::Pblendw => format!("X[{d}]=pblendw(X[{d}],{},{});", xsrc(i), i.immediate(2), d = xd()),
        M::Blendps => format!("X[{d}]=blendps(X[{d}],{},{});", xsrc(i), i.immediate(2), d = xd()),
        M::Pblendvb => format!("X[{d}]=pblendvb(X[{d}],{},X[0]);", xsrc(i), d = xd()),
        M::Pmovsxbw | M::Pmovsxbd | M::Pmovsxbq | M::Pmovsxwd | M::Pmovsxwq | M::Pmovsxdq | M::Pmovzxbw | M::Pmovzxbd | M::Pmovzxbq | M::Pmovzxwd | M::Pmovzxwq | M::Pmovzxdq => {
            let src = if i.op_kind(1) == OpKind::Memory { format!("vlow64(M64({}))", ea(i)) } else { format!("X[{}]", i.op_register(1).number()) };
            format!("X[{}]={}({src});", xd(), format!("{m:?}").to_lowercase())
        }
        M::Movmskps => wreg(i.op_register(0), &format!("movmskps(X[{}])", i.op_register(1).number())),
        M::Movmskpd => wreg(i.op_register(0), &format!("movmskpd(X[{}])", i.op_register(1).number())),
        M::Ptest => format!("ptest(&f,X[{}],{});", xd(), xsrc(i)),
        M::Pcmpistri | M::Pcmpestri | M::Pcmpistrm | M::Pcmpestrm => {
            let imm = i.immediate(2) as u8;
            let wide = format!("{:?}", i.code()).contains("64");
            let (la, lb) = if matches!(m, M::Pcmpestri | M::Pcmpestrm) {
                if wide { ("(s64)R[0]".to_string(), "(s64)R[2]".to_string()) } else { ("(s64)(s32)R[0]".to_string(), "(s64)(s32)R[2]".to_string()) }
            } else { ("-1".to_string(), "-1".to_string()) };
            let abs = |x: &str| if x == "-1" { x.to_string() } else { format!("({x}<0?-{x}:{x})") };
            let r = format!("pcmpstr(&f,X[{}],{},{},{},{imm})", xd(), xsrc(i), abs(&la), abs(&lb));
            if matches!(m, M::Pcmpistri | M::Pcmpestri) { format!("{{u32 r={r};R[1]=pcmpstri(r,{imm});}}") } else { format!("{{u32 r={r};X[0]=pcmpstrm(r,{imm});}}") }
        }
        M::Rdsspq | M::Rdsspd => wr(i, 0, "0"), // no shadow stack: reads as 0, as when CET is off
        M::Movntdq | M::Movntps | M::Movntpd | M::Movntdqa => format!("{}={};", xmm_op(i, 0), xmm_op(i, 1)),
        M::Movnti => wr(i, 0, &rd(i, 1)),
        M::Cvttsd2si | M::Cvttss2si | M::Cvtsd2si | M::Cvtss2si => {
            let src = if i.op_kind(1) == OpKind::Register {
                format!("(double)X[{}].{}[0]", i.op_register(1).number(), if matches!(m, M::Cvttsd2si | M::Cvtsd2si) { "pd" } else { "ps" })
            } else if matches!(m, M::Cvttsd2si | M::Cvtsd2si) { format!("MF64({})", ea(i)) } else { format!("(double)MF32({})", ea(i)) };
            let v = if matches!(m, M::Cvtsd2si | M::Cvtss2si) { format!("__builtin_rint({src})") } else { src };
            let b = i.op_register(0).size() * 8;
            wreg(i.op_register(0), &format!("cvt_f2i{b}({v})"))
        }
        M::Cvtdq2ps => format!("{{V s={};for(int k=0;k<4;k++)X[{d}].ps[k]=(float)s.sd[k];}}", xsrc(i), d = xd()),
        M::Cvttpd2dq | M::Cvtpd2dq => format!("{{V s={};X[{d}].sd[0]=cvt_f2i32({r}s.pd[0]{e});X[{d}].sd[1]=cvt_f2i32({r}s.pd[1]{e});X[{d}].q[1]=0;}}", xsrc(i), d = xd(), r = if m == M::Cvtpd2dq { "__builtin_rint(" } else { "" }, e = if m == M::Cvtpd2dq { ")" } else { "" }),
        M::Cvttps2dq | M::Cvtps2dq => format!("{{V s={};for(int k=0;k<4;k++)X[{d}].sd[k]=cvt_f2i32({}s.ps[k]{});}}", xsrc(i), if m == M::Cvtps2dq { "__builtin_rintf(" } else { "" }, if m == M::Cvtps2dq { ")" } else { "" }, d = xd()),
        M::Cvtdq2pd => format!("{{V s={};X[{d}].pd[0]=(double)s.sd[0];X[{d}].pd[1]=(double)s.sd[1];}}", if i.op_kind(1) == OpKind::Memory { format!("vlow64(M64({}))", ea(i)) } else { format!("X[{}]", i.op_register(1).number()) }, d = xd()),
        M::Cvtps2pd => format!("{{V s={};X[{d}].pd[0]=(double)s.ps[0];X[{d}].pd[1]=(double)s.ps[1];}}", if i.op_kind(1) == OpKind::Memory { format!("vlow64(M64({}))", ea(i)) } else { format!("X[{}]", i.op_register(1).number()) }, d = xd()),
        M::Cvtpd2ps => format!("{{V s={};V r=vzero();r.ps[0]=(float)s.pd[0];r.ps[1]=(float)s.pd[1];X[{d}]=r;}}", xsrc(i), d = xd()),
        M::Sqrtps => format!("{{V s={};for(int k=0;k<4;k++)X[{d}].ps[k]=__builtin_sqrtf(s.ps[k]);}}", xsrc(i), d = xd()),
        M::Sqrtpd => format!("{{V s={};for(int k=0;k<2;k++)X[{d}].pd[k]=__builtin_sqrt(s.pd[k]);}}", xsrc(i), d = xd()),
        M::Rcpps => format!("{{V s={};for(int k=0;k<4;k++)X[{d}].ps[k]=1.0f/s.ps[k];}}", xsrc(i), d = xd()),
        M::Rsqrtps => format!("{{V s={};for(int k=0;k<4;k++)X[{d}].ps[k]=1.0f/__builtin_sqrtf(s.ps[k]);}}", xsrc(i), d = xd()),
        M::Cmpps | M::Cmppd => {
            let (fld, n, lane) = if m == M::Cmpps { ("ps", 4, "d") } else { ("pd", 2, "q") };
            format!("{{V s={};for(int k=0;k<{n};k++)X[{d}].{lane}[k]=fpred({},X[{d}].{fld}[k],s.{fld}[k])?(typeof(X[0].{lane}[0]))-1:0;}}", xsrc(i), i.immediate(2), d = xd())
        }
        _ => return None,
    };
    Some(s)
}

fn reg_of(size: usize) -> Register {
    match size { 1 => Register::AL, 2 => Register::AX, 4 => Register::EAX, _ => Register::RAX }
}
/// The second operand of a packed SSE instruction: an xmm register or 16 bytes of memory.
fn xsrc(i: &Instruction) -> String {
    xmm_op(i, 1)
}
fn xmm_op(i: &Instruction, n: u32) -> String {
    match i.op_kind(n) {
        OpKind::Register => format!("X[{}]", i.op_register(n).number()),
        _ => format!("MV({})", ea(i)),
    }
}

fn uleb(d: &[u8], at: &mut usize) -> Option<u64> {
    let (mut v, mut sh) = (0u64, 0);
    loop {
        let b = *d.get(*at)?;
        *at += 1;
        v |= u64::from(b & 0x7F) << sh;
        if b & 0x80 == 0 { return Some(v); }
        sh += 7;
        if sh > 63 { return None; }
    }
}
fn encoded(d: &[u8], at: &mut usize, enc: u8) -> Option<u64> {
    let take = |at: &mut usize, n: usize| -> Option<u64> { let b = d.get(*at..*at + n)?; *at += n; let mut x = [0u8; 8]; x[..n].copy_from_slice(b); Some(u64::from_le_bytes(x)) };
    match enc & 0x0F {
        0x00 => take(at, 8),
        0x01 => uleb(d, at),
        0x02 => take(at, 2),
        0x03 => take(at, 4),
        0x04 => take(at, 8),
        0x0A => take(at, 2).map(|v| v as i16 as i64 as u64),
        0x0B => take(at, 4).map(|v| v as i32 as i64 as u64),
        0x0C => take(at, 8),
        _ => None,
    }
}

/// Where exceptions can resume in the function at `entry`, from its language-specific handler data.
fn landing_pads(img: &Image, entry: u64) -> Vec<u64> {
    let (Some(&info), Some(&end)) = (img.unwind.get(&entry), img.pdata.get(&entry)) else { return vec![] };
    let Some(b) = img.bytes(info) else { return vec![] };
    if b.len() < 4 || b[0] >> 3 & 3 == 0 || b[0] >> 3 & 4 != 0 { return vec![]; }
    let count = b[2] as u64;
    let h = info + 4 + 2 * ((count + 1) & !1);
    let ib = img.base_of(entry);
    let Some(handler) = img.u32(h).map(|r| ib + u64::from(r)) else { return vec![] };
    let data = h + 4;
    // MSVC C++ (__CxxFrameHandler3, also behind the /GS check): a catch block is a funclet that returns where
    // the function resumes (`lea rax,[continuation]` before its `ret`); the FuncInfo lists the funclets
    if ["__CxxFrameHandler3", "__GSHandlerCheck_EH"].iter().any(|n| is_import_thunk(img, handler, n)) {
        return msvc_continuations(img, ib, img.u32(data).map(|r| ib + u64::from(r)).unwrap_or(0), entry, end);
    }
    // FH4 (__CxxFrameHandler4, the default of current MSVC for x64): the compressed FuncInfo4 names each catch's
    // continuations itself; the funclets' own `lea rax,[continuation]` are read as for FH3 too
    if handler_is(img, handler, "__CxxFrameHandler4") {
        let Some(fi) = img.u32(data).map(|r| ib + u64::from(r)) else { return vec![] };
        let Some(f) = fh4(img, ib, fi, entry) else { return vec![] };
        let mut out: Vec<u64> = f.continuations.into_iter().filter(|&x| x >= entry && x < end).collect();
        for x in funclet_continuations(img, &f.handlers, entry, end) { if !out.contains(&x) { out.push(x); } }
        return out;
    }
    // __C_specific_handler (through its import thunk): a scope table; jump targets are the __except blocks
    if is_import_thunk(img, handler, "__C_specific_handler") {
        let n = img.u32(data).unwrap_or(0).min(4096) as u64;
        return (0..n).filter_map(|k| img.u32(data + 4 + 16 * k + 12)).filter(|&j| j != 0).map(|j| ib + u64::from(j)).collect();
    }
    gcc_lsda(img, data, entry, end).unwrap_or_default()
}
/// FH3's FuncInfo (magic, max state, unwind map, try blocks {low, high, catch high, catches, handlers},
/// ...): each handler's funclet, decoded up to its `ret`, gives the continuations it can return.
/// The functions an MSVC throw site's ThrowInfo names (`lea rdx,[ThrowInfo]` before the call to
/// _CxxThrowException): the thrown type's destructor and each catchable type's copy constructor, as image-
/// relative addresses vcruntime calls. ThrowInfo: attributes, destructor, forward compat, catchable type array
/// (count, types); CatchableType: properties, type, displacement (12), size, copy function.
fn throw_info_functions(img: &Image, insns: &BTreeMap<u64, Instruction>, call: &Instruction) -> Vec<u64> {
    let mut out = Vec::new();
    let Some(lea) = insns.range(..call.ip()).rev().take(8).map(|(_, i)| i).find(|i| i.mnemonic() == Mnemonic::Lea && i.op0_register() == Register::RDX && i.is_ip_rel_memory_operand()) else { return out };
    let ti = lea.ip_rel_memory_address();
    let ib = img.base_of(call.ip());
    let mut add = |r: Option<u32>| if let Some(r) = r.filter(|&r| r != 0) { let a = ib + u64::from(r); if img.is_code(a) { out.push(a); } };
    add(img.u32(ti + 4));
    if let Some(cta) = img.u32(ti + 12).filter(|&r| r != 0).map(|r| ib + u64::from(r)) {
        for k in 0..img.u32(cta).unwrap_or(0).min(64) as u64 {
            if let Some(ct) = img.u32(cta + 4 + 4 * k).map(|r| ib + u64::from(r)) { add(img.u32(ct + 24)); }
        }
    }
    out
}
/// The catch funclets FH3's FuncInfo lists for the function at `entry` (vcruntime calls them by address)
fn msvc_funclets(img: &Image, entry: u64) -> Vec<u64> {
    let mut out = Vec::new();
    let (Some(&info), Some(h)) = (img.unwind.get(&entry), unwind_handler(img, entry)) else { return out };
    let Some(b) = img.bytes(info) else { return out };
    let ib = img.base_of(entry);
    let data = info + 4 + 2 * ((b[2] as u64 + 1) & !1) + 4;
    let Some(fi) = img.u32(data).map(|r| ib + u64::from(r)) else { return out };
    if handler_is(img, h, "__CxxFrameHandler4") {
        if let Some(f) = fh4(img, ib, fi, entry) { out.extend(f.actions.into_iter().chain(f.handlers).filter(|&a| img.is_code(a))); }
        return out;
    }
    if !["__CxxFrameHandler3", "__GSHandlerCheck_EH"].iter().any(|n| is_import_thunk(img, h, n)) { return out; }
    if !(0x19930520..=0x19930522).contains(&img.u32(fi).unwrap_or(0)) { return out; }
    // the unwind map's cleanup actions (destructors run while unwinding): {to state, action}
    let (n_state, umap) = (img.u32(fi + 4).unwrap_or(0).min(4096) as u64, ib + u64::from(img.u32(fi + 8).unwrap_or(0)));
    for k in 0..n_state { if let Some(r) = img.u32(umap + 8 * k + 4) { if r != 0 { let a = ib + u64::from(r); if img.is_code(a) { out.push(a); } } } }
    let (n_try, tries) = (img.u32(fi + 12).unwrap_or(0).min(1024) as u64, ib + u64::from(img.u32(fi + 16).unwrap_or(0)));
    for t in 0..n_try {
        let e = tries + 20 * t;
        let (n_catch, handlers) = (img.u32(e + 12).unwrap_or(0).min(64) as u64, ib + u64::from(img.u32(e + 16).unwrap_or(0)));
        for k in 0..n_catch { if let Some(r) = img.u32(handlers + 20 * k + 12) { let a = ib + u64::from(r); if img.is_code(a) { out.push(a); } } }
    }
    out
}
/// MSVC FH4's FuncInfo4 (vcruntime's ehdata4.h), for the function at `entry`: the unwind map's actions (destructor
/// funclets), each catch's funclet and the continuations it lists. Numbers are compressed (the low bits of the first
/// byte say how many bytes: xxxxxxx0 one, xxxxxx01 two, xxxxx011 three, xxxx0111 four, 1111 a 32-bit value after
/// it); displacements of tables and funclets are 32-bit image-relative addresses.
struct Fh4 { actions: Vec<u64>, handlers: Vec<u64>, continuations: Vec<u64> }
fn fh4_unsigned(img: &Image, at: &mut u64) -> Option<u64> {
    let b = img.bytes(*at)?;
    let (len, shift) = match b.first()? & 0x0f { x if x & 1 == 0 => (1, 1), x if x & 3 == 1 => (2, 2), x if x & 7 == 3 => (3, 3), 7 => (4, 4), _ => (5, 0) };
    if b.len() < len { return None; }
    let v = if len == 5 { u64::from(u32::from_le_bytes([b[1], b[2], b[3], b[4]])) } else { (0..len).fold(0u64, |v, k| v | u64::from(b[k]) << (8 * k)) >> shift };
    *at += len as u64;
    Some(v)
}
fn fh4_int(img: &Image, at: &mut u64) -> Option<u32> { let v = img.u32(*at)?; *at += 4; Some(v) }
fn fh4(img: &Image, ib: u64, fi: u64, entry: u64) -> Option<Fh4> {
    let mut at = fi;
    let head = *img.bytes(at)?.first()?; at += 1;
    let (is_catch, separated, bbt, unwind, tries) = (head & 1 != 0, head & 2 != 0, head & 4 != 0, head & 8 != 0, head & 16 != 0);
    let _ = (is_catch, separated);
    if bbt { fh4_unsigned(img, &mut at)?; }
    let umap = if unwind { Some(ib + u64::from(fh4_int(img, &mut at)?)) } else { None };
    let tmap = if tries { Some(ib + u64::from(fh4_int(img, &mut at)?)) } else { None };
    let mut f = Fh4 { actions: vec![], handlers: vec![], continuations: vec![] };
    if let Some(mut u) = umap {
        // {next offset << 2 | type (0 none, 1 destructor of an object, 2 of an object through a pointer, 3 a funclet)}, action, object
        for _ in 0..fh4_unsigned(img, &mut u)?.min(4096) {
            let ty = fh4_unsigned(img, &mut u)? & 3;
            if ty != 0 { f.actions.push(ib + u64::from(fh4_int(img, &mut u)?)); }
            if ty == 1 || ty == 2 { fh4_unsigned(img, &mut u)?; }
        }
    }
    if let Some(mut t) = tmap {
        // {try low, try high, catch high, handler map}; the handler map: {header, adjectives, type, catch object, funclet, continuations}
        for _ in 0..fh4_unsigned(img, &mut t)?.min(1024) {
            for _ in 0..3 { fh4_unsigned(img, &mut t)?; }
            let mut h = ib + u64::from(fh4_int(img, &mut t)?);
            for _ in 0..fh4_unsigned(img, &mut h)?.min(64) {
                let hh = *img.bytes(h)?.first()?; h += 1;
                if hh & 1 != 0 { fh4_unsigned(img, &mut h)?; }       // adjectives
                if hh & 2 != 0 { fh4_int(img, &mut h)?; }            // type descriptor
                if hh & 4 != 0 { fh4_unsigned(img, &mut h)?; }       // catch object's frame offset
                f.handlers.push(ib + u64::from(fh4_int(img, &mut h)?));
                for _ in 0..(hh >> 4 & 3).min(2) {                   // continuations: image-relative, or from the function's start
                    let c = if hh & 8 != 0 { ib + u64::from(fh4_int(img, &mut h)?) } else { entry + fh4_unsigned(img, &mut h)? };
                    f.continuations.push(c);
                }
            }
        }
    }
    Some(f)
}
/// Where catch funclets return to: the `lea rax,[continuation]` before each one's `ret`, inside [entry, end)
fn funclet_continuations(img: &Image, funclets: &[u64], entry: u64, end: u64) -> Vec<u64> {
    let mut out = Vec::new();
    for &start in funclets {
        let mut ip = start;
        for _ in 0..4096 {
            let Some(b) = img.bytes(ip) else { break };
            let mut d = Decoder::with_ip(64, &b[..b.len().min(16)], ip, DecoderOptions::NONE);
            let i = d.decode();
            if i.is_invalid() { break; }
            if i.mnemonic() == Mnemonic::Lea && i.op0_register() == Register::RAX && i.is_ip_rel_memory_operand() {
                let x = i.ip_rel_memory_address();
                if x >= entry && x < end && !out.contains(&x) { out.push(x); }
            }
            if matches!(i.mnemonic(), Mnemonic::Ret | Mnemonic::Int3) { break; }
            ip = i.next_ip();
        }
    }
    out
}
fn msvc_continuations(img: &Image, ib: u64, info: u64, entry: u64, end: u64) -> Vec<u64> {
    let mut out = Vec::new();
    let Some(magic) = img.u32(info) else { return out };
    if !(0x19930520..=0x19930522).contains(&magic) { return out; }
    let (n_try, tries) = (img.u32(info + 12).unwrap_or(0).min(1024) as u64, ib + u64::from(img.u32(info + 16).unwrap_or(0)));
    for t in 0..n_try {
        let e = tries + 20 * t;
        let (n_catch, handlers) = (img.u32(e + 12).unwrap_or(0).min(64) as u64, ib + u64::from(img.u32(e + 16).unwrap_or(0)));
        for k in 0..n_catch {
            let Some(rva) = img.u32(handlers + 20 * k + 12) else { continue };
            let mut ip = ib + u64::from(rva);
            for _ in 0..4096 {
                let Some(b) = img.bytes(ip) else { break };
                let mut d = Decoder::with_ip(64, &b[..b.len().min(16)], ip, DecoderOptions::NONE);
                let i = d.decode();
                if i.is_invalid() { break; }
                if i.mnemonic() == Mnemonic::Lea && i.op0_register() == Register::RAX && i.is_ip_rel_memory_operand() {
                    let x = i.ip_rel_memory_address();
                    if x >= entry && x < end && !out.contains(&x) { out.push(x); }
                }
                if matches!(i.mnemonic(), Mnemonic::Ret | Mnemonic::Int3) { break; }
                ip = i.next_ip();
            }
        }
    }
    out
}
/// The language handler an image's unwind info names for the function at `entry` (a personality routine, or
/// an import thunk to one): the exception dispatcher calls it, so it must be a lifted entry.
fn unwind_handler(img: &Image, entry: u64) -> Option<u64> {
    let info = *img.unwind.get(&entry)?;
    let b = img.bytes(info)?;
    if b.len() < 4 || b[0] >> 3 & 3 == 0 || b[0] >> 3 & 4 != 0 { return None; }
    let h = info + 4 + 2 * ((b[2] as u64 + 1) & !1);
    img.u32(h).map(|r| img.base_of(entry) + u64::from(r)).filter(|&a| img.is_code(a))
}
/// A frame handler that is `name` or reaches it: the import (thunk or export), or a local function that calls or jumps
/// to it within its first instructions (MSVC's __GSHandlerCheck_EH4, linked into the image, checks the frame's
/// security cookie and then calls __CxxFrameHandler4)
fn handler_is(img: &Image, at: u64, name: &str) -> bool {
    if is_import_thunk(img, at, name) { return true; }
    let mut ip = at;
    for _ in 0..64 {
        let Some(b) = img.bytes(ip) else { return false };
        let mut d = Decoder::with_ip(64, &b[..b.len().min(16)], ip, DecoderOptions::NONE);
        let i = d.decode();
        if i.is_invalid() { return false; }
        if matches!(i.mnemonic(), Mnemonic::Call | Mnemonic::Jmp) && is_near(&i) && is_import_thunk(img, i.near_branch_target(), name) { return true; }
        if matches!(i.mnemonic(), Mnemonic::Ret | Mnemonic::Int3) { return false; }
        ip = i.next_ip();
    }
    false
}
/// `at` is the function `name`: a thunk to its import, or (inside the DLL that has it, as vcruntime has
/// __C_specific_handler) its export
fn is_import_thunk(img: &Image, at: u64, name: &str) -> bool {
    if img.exports.iter().any(|(n, _, a)| *a == at && n == name) { return true; }
    let Some(b) = img.bytes(at) else { return false };
    let mut d = Decoder::with_ip(64, &b[..b.len().min(16)], at, DecoderOptions::NONE);
    let i = d.decode();
    i.mnemonic() == Mnemonic::Jmp && i.is_ip_rel_memory_operand() && img.imports.iter().any(|(slot, n)| *slot == i.ip_rel_memory_address() && n.ends_with(&format!("!{name}")))
}
/// 32-bit: is `at` an import slot of a setjmp, or a thunk (`jmp [slot]`) to one? A call to it returns twice.
fn is_setjmp_slot(img: &Image, slot: u64) -> bool {
    img.imports.iter().any(|(s, n)| *s == slot && ["!_setjmp3", "!_setjmp", "!setjmp"].iter().any(|x| n.ends_with(x)))
}
fn is_setjmp_thunk(img: &Image, at: u64) -> bool {
    let Some(b) = img.bytes(at) else { return false };
    let mut d = Decoder::with_ip(32, &b[..b.len().min(16)], at, DecoderOptions::NONE);
    let i = d.decode();
    i.mnemonic() == Mnemonic::Jmp && i.op0_kind() == OpKind::Memory && i.memory_base() == Register::None && i.memory_index() == Register::None && is_setjmp_slot(img, i.memory_displacement64())
}
/// GCC / LLVM (`.gcc_except_table`, here inline after the personality's RVA): the call-site table's landing pads.
fn gcc_lsda(img: &Image, at: u64, start: u64, end: u64) -> Option<Vec<u64>> {
    let d = img.bytes(at)?;
    let mut p = 0usize;
    let lp_enc = *d.get(p)?;
    p += 1;
    let lpstart = if lp_enc == 0xFF { start } else { encoded(d, &mut p, lp_enc)? };
    let tt_enc = *d.get(p)?;
    p += 1;
    if tt_enc != 0xFF { uleb(d, &mut p)?; }
    let cs_enc = *d.get(p)?;
    p += 1;
    if !matches!(cs_enc, 0x01 | 0x03 | 0x0B | 0x09) { return None; }
    let len = uleb(d, &mut p)? as usize;
    let table_end = p.checked_add(len)?;
    if len > 1 << 20 { return None; }
    let mut pads = Vec::new();
    while p < table_end {
        let s = encoded(d, &mut p, cs_enc)?;
        let l = encoded(d, &mut p, cs_enc)?;
        let lp = encoded(d, &mut p, cs_enc)?;
        uleb(d, &mut p)?;
        if s.checked_add(l)? > end.checked_sub(start)? + 0x100000 { return None; }
        if lp != 0 {
            let pad = lpstart.wrapping_add(lp);
            if !img.is_code(pad) { return None; }
            pads.push(pad);
        }
    }
    if p != table_end { return None; }
    Some(pads)
}
