//! Static recompiler: an x86-64 PE image -> C (one function per guest function), to be compiled
//! to WebAssembly. Also the census: what an image uses that the lifter does not handle yet.

use iced_x86::{ConditionCode, Decoder, DecoderOptions, FlowControl, Formatter, Instruction, IntelFormatter, Mnemonic, OpKind, Register};
use object::read::pe::{ImageNtHeaders, PeFile64};
use object::{LittleEndian as LE, Object, ObjectSection};
use std::collections::{BTreeMap, BTreeSet};
use std::fmt::Write as _;

/// Guest addresses handed out for imports (the IAT slots hold these).
const IMPORT_BASE: u64 = 0x0F80_0000;

#[derive(Clone)]
struct Image {
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
    relocs: Vec<u64>,                   // addresses of 64-bit absolute pointers
    size: u64,                          // SizeOfImage
    is_dll: bool,
    exports: Vec<(String, u32, u64)>,   // name ("" if by ordinal only), ordinal, VA (forwarders left out)
    /// in a merged view of several images: each image's (base, end)
    ranges: Vec<(u64, u64)>,
}

impl Image {
    fn load(data: &[u8]) -> Result<Image, String> {
        let pe = PeFile64::parse(data).map_err(|e| e.to_string())?;
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
                    match e >> 12 {
                        0 => {}
                        10 => relocs.push(base + page + u64::from(e & 0xFFF)),
                        t => return Err(format!("relocation type {t} not supported")),
                    }
                }
                at += len;
            }
        }
        let size = u64::from(object::read::pe::ImageOptionalHeader::size_of_image(opt));
        let characteristics = pe.nt_headers().file_header().characteristics.get(LE);
        let mut img = Image { base, entry, sections, text, pdata: BTreeMap::new(), unwind: BTreeMap::new(), imports: Vec::new(), relocs, raw, orig_base: base, dirs, stack_reserve,
            fixed: characteristics & 1 != 0, size, is_dll: characteristics & 0x2000 != 0, exports: Vec::new(), ranges: vec![(base, base + size)] };
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
        // exception table: 12-byte RUNTIME_FUNCTION entries
        if let Some(dir) = pe.data_directory(3) {
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
                while let Some(t) = thunks.next::<object::pe::ImageNtHeaders64>().map_err(|e| e.to_string())? {
                    let name = match table.import::<object::pe::ImageNtHeaders64>(t).map_err(|e| e.to_string())? {
                        object::read::pe::Import::Name(_, n) => String::from_utf8_lossy(n).into_owned(),
                        object::read::pe::Import::Ordinal(o) => format!("#{o}"),
                    };
                    img.imports.push((slot, format!("{dll}!{name}")));
                    slot += 8;
                }
            }
        }
        Ok(img)
    }
    /// Moves the image to `to`: every address it holds and every address we keep.
    fn rebase(&mut self, to: u64) -> Result<(), String> {
        let d = to.wrapping_sub(self.base);
        if d == 0 { return Ok(()); }
        if self.fixed { return Err("the image was linked without relocations (/FIXED) and cannot be moved".into()); }
        for &r in &self.relocs.clone() {
            let (sa, data, _) = self.sections.iter_mut().find(|(a, d, _)| r >= *a && r + 8 <= *a + d.len() as u64).ok_or(format!("relocation at 0x{r:x} outside the file data"))?;
            let o = (r - *sa) as usize;
            let v = u64::from_le_bytes(data[o..o + 8].try_into().unwrap()).wrapping_add(d);
            data[o..o + 8].copy_from_slice(&v.to_le_bytes());
        }
        let mv = |a: &mut u64| *a = a.wrapping_add(d);
        mv(&mut self.base); mv(&mut self.entry);
        self.sections.iter_mut().for_each(|s| mv(&mut s.0));
        self.text.iter_mut().for_each(|t| { mv(&mut t.0); mv(&mut t.1) });
        self.pdata = self.pdata.iter().map(|(b, e)| (b.wrapping_add(d), e.wrapping_add(d))).collect();
        self.unwind = self.unwind.iter().map(|(b, u)| (b.wrapping_add(d), u.wrapping_add(d))).collect();
        self.imports.iter_mut().for_each(|i| mv(&mut i.0));
        self.relocs.iter_mut().for_each(mv);
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
}

/// A `switch` compiled to `jmp reg`. Forms seen:
/// - relative: `lea B,[rip+T] ... movsxd R,[B+I*4]; add R,B; jmp R` (LLVM, GCC; B may be set far earlier);
/// - image-relative (MSVC): `lea B,[__ImageBase] ... mov R32,[B+I*4+rva]; add R,B; jmp R`.
/// The bound comes from the `cmp I,N; ja` before it; without one, entries are read while they land in the function.
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
    // the bound: `cmp I,N` then ja / jae (N+1 or N entries); I may reach the load through copies
    let mut bound = None;
    let mut names = vec![index];
    let mut mems: Vec<(Register, Register, u32, u64)> = Vec::new(); // memory the index was loaded from
    let mut seen_load = false;
    let same_mem = |i: &Instruction, m: &(Register, Register, u32, u64)| i.memory_base() == m.0 && i.memory_index() == m.1 && i.memory_index_scale() == m.2 && i.memory_displacement64() == m.3;
    // the index's copies first (a copy may come before the compare), then the compare on any of them
    for _ in 0..3 {
        let mut seen = false;
        for i in &prev {
            if i.ip() == load.ip() { seen = true; continue; }
            if !seen { continue; }
            if matches!(i.mnemonic(), Mnemonic::Mov | Mnemonic::Movzx | Mnemonic::Movsxd | Mnemonic::Movsx) && i.op_kind(0) == OpKind::Register && names.contains(&i.op0_register().full_register()) {
                if i.op_kind(1) == OpKind::Register && !names.contains(&i.op_register(1).full_register()) { names.push(i.op_register(1).full_register()); }
                else if i.op_kind(1) == OpKind::Memory { mems.push((i.memory_base(), i.memory_index(), i.memory_index_scale(), i.memory_displacement64())); }
            }
        }
    }
    for (k, i) in prev.iter().enumerate() {
        if i.ip() == load.ip() { seen_load = true; continue; }
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

fn decode_func(img: &Image, entry: u64, entries: &BTreeSet<u64>) -> (Func, BTreeSet<u64>) {
    let range_end = img.pdata.get(&entry).copied();
    let mut f = Func { insns: BTreeMap::new(), leaders: BTreeSet::new(), tables: BTreeMap::new(), pads: BTreeSet::new() };
    let mut calls = BTreeSet::new();
    let mut taken = BTreeSet::new();
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
    while let Some(start) = work.pop() {
        let mut ip = start;
        loop {
            if f.insns.contains_key(&ip) {
                break;
            }
            let Some(bytes) = img.bytes(ip) else { break };
            let mut d = Decoder::with_ip(64, &bytes[..bytes.len().min(16)], ip, DecoderOptions::NONE);
            let ins = d.decode();
            if ins.is_invalid() {
                break;
            }
            f.insns.insert(ip, ins);
            if ins.mnemonic() == Mnemonic::Lea && ins.is_ip_rel_memory_operand() && img.is_code(ins.ip_rel_memory_address()) {
                taken.insert(ins.ip_rel_memory_address());
            }
            let next = ins.next_ip();
            match ins.flow_control() {
                FlowControl::Next => {}
                FlowControl::Call => {
                    calls.insert(ins.near_branch_target());
                    f.leaders.insert(next);
                }
                FlowControl::IndirectCall => {
                    f.leaders.insert(next);
                }
                FlowControl::ConditionalBranch => {
                    let t = ins.near_branch_target();
                    f.leaders.insert(t);
                    f.leaders.insert(next);
                    work.push(t);
                }
                FlowControl::UnconditionalBranch => {
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
                    if ins.op0_kind() == OpKind::Register {
                        if let Some(ts) = jump_table(img, &f.insns, &ins, range_end.map(|e| (entry, e))) {
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
                _ => break, // ret, int3, ud2, ...
            }
            ip = next;
        }
    }
    calls.extend(taken.into_iter().filter(|t| !f.insns.contains_key(t)));
    (f, calls)
}

struct Emit<'a> {
    img: &'a Image,
    unimpl: BTreeMap<String, u32>,
    /// `jmp reg` with no table found: a tail call through a pointer, or a switch the lifter missed
    unresolved: u32,
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
fn ea(i: &Instruction) -> String {
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

impl Emit<'_> {
    fn insn(&mut self, i: &Instruction, f: &Func, entries: &BTreeSet<u64>, entry: u64) -> String {
        use Mnemonic as M;
        let m = i.mnemonic();
        let bits = if i.op_count() > 0 && matches!(i.op_kind(0), OpKind::Register | OpKind::Memory) && !i.op0_register().is_xmm() { opsize(i, 0) * 8 } else { 64 };
        let goto = |t: u64| -> String {
            if f.insns.contains_key(&t) && f.leaders.contains(&t) && (t == entry || !entries.contains(&t)) {
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
        if let Some(s) = extra(i) {
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
            M::Ucomisd | M::Comisd => format!("fcmp(&f,X[{}].pd[0],{});", i.op0_register().number(), fsrc(1, "f64")),
            M::Ucomiss | M::Comiss => format!("fcmp(&f,X[{}].ps[0],{});", i.op0_register().number(), fsrc(1, "f32")),
            M::Cmpsd if i.op_count() == 3 => format!("X[{d}].q[0]=fpred({},X[{d}].pd[0],{})?~0ull:0;", i.immediate(2), fsrc(1, "f64"), d = i.op0_register().number()),
            M::Cmpss if i.op_count() == 3 => format!("X[{d}].d[0]=fpred({},X[{d}].ps[0],{})?~0u:0;", i.immediate(2), fsrc(1, "f32"), d = i.op0_register().number()),
            _ => {
                let mut text = String::new();
                IntelFormatter::new().format(i, &mut text);
                *self.unimpl.entry(format!("{m:?}")).or_default() += 1;
                format!("rt_unimpl(0x{:x}ull); /* {text} */", i.ip())
            }
        }
    }

    fn func(&mut self, entry: u64, f: &Func, entries: &BTreeSet<u64>) -> String {
        if !f.insns.contains_key(&entry) {
            // nothing decodes there (a code-looking value in data): reached only by mistake
            return format!("void f_{entry:x}(Cpu*__restrict c){{rt_trap(0x{entry:x}ull);}}\n");
        }
        let mut s = if f.pads.is_empty() {
            format!("void f_{entry:x}(Cpu*__restrict c){{Flags f={{0}};(void)f;goto L_{entry:x};\n")
        } else {
            // the landing: an exception resumes here (rt_restore_context); the body starts at the landing pad
            let mut w = format!("static void b_{entry:x}(Cpu*__restrict c,u64 resume);\nvoid f_{entry:x}(Cpu*__restrict c){{LANDING(b_{entry:x});}}\n");
            write!(w, "static void b_{entry:x}(Cpu*__restrict c,u64 resume){{Flags f={{0}};(void)f;if(resume){{switch(resume){{").unwrap();
            for p in &f.pads {
                if f.insns.contains_key(p) {
                    write!(w, "case 0x{p:x}ull:goto L_{p:x};").unwrap();
                }
            }
            w.push_str("default:rt_call(c,resume);return;}}\n");
            write!(w, "goto L_{entry:x};\n").unwrap();
            w
        };
        let mut prev_next = 0;
        for (&ip, i) in &f.insns {
            // a gap in the decoded code: the block before it must not run into this one
            if prev_next != 0 && prev_next != ip {
                writeln!(s, "rt_trap(0x{prev_next:x}ull);").unwrap();
            }
            if f.leaders.contains(&ip) {
                writeln!(s, "L_{ip:x}:;").unwrap();
            }
            let body = self.insn(i, f, entries, entry);
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

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: lifter <image.exe> <out.c> [--census]");
        std::process::exit(2);
    }
    let exe = std::path::Path::new(&args[1]);
    let dir = exe.parent().unwrap_or(std::path::Path::new("."));
    let listing: Vec<String> = std::fs::read_dir(dir).map(|d| d.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect()).unwrap_or_default();
    let mut mods: Vec<(String, Image)> = Vec::new();
    let mut queue = vec![exe.file_name().unwrap().to_string_lossy().into_owned()];
    while let Some(name) = queue.pop() {
        if mods.iter().any(|(n, _)| n.eq_ignore_ascii_case(&name)) { continue; }
        let data = std::fs::read(dir.join(&name)).expect("read image");
        let img = Image::load(&data).unwrap_or_else(|e| panic!("{name}: {e}"));
        for (_, imp) in &img.imports {
            let dll = imp.split('!').next().unwrap();
            if let Some(file) = listing.iter().find(|f| f.eq_ignore_ascii_case(dll)) {
                queue.push(file.clone()); // a DLL the title ships: lifted with it
            }
        }
        mods.push((name, img));
    }
    // where each image goes in guest memory (see docs/memory.md): the exe at 0x10000000 (or --base), the DLLs after it
    let first = args.iter().position(|a| a == "--base").map(|i| u64::from_str_radix(args[i + 1].trim_start_matches("0x"), 16).expect("--base hex"));
    let mut next = first.unwrap_or(if mods[0].1.base >= 0x1_0000_0000 || mods.len() > 1 { 0x1000_0000 } else { mods[0].1.base });
    for (name, img) in mods.iter_mut() {
        let from = img.base;
        img.rebase(next).unwrap_or_else(|e| panic!("{name}: {e}"));
        if from != img.base { eprintln!("{name}: moved from 0x{from:x} to 0x{:x} ({} relocations)", img.base, img.relocs.len()); }
        next = (img.base + img.size + 0x10_FFFF) & !0xF_FFFF; // a 1 MiB gap between images
    }
    // imports from a DLL we lift resolve to its exports here; the rest go to the runtime
    let lower: Vec<String> = mods.iter().map(|(n, _)| n.to_ascii_lowercase()).collect();
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
        img.ranges.extend(m.ranges.iter().cloned());
    }
    // function entries: the entry point, the exception table, then every call target, to a fixpoint
    let mut entries: BTreeSet<u64> = img.pdata.keys().copied().collect();
    for (_, m) in &mods {
        entries.insert(m.entry);
        entries.extend(m.exports.iter().map(|e| e.2).filter(|&a| img.is_code(a)));
    }
    // code addresses stored in data (function pointer tables, vtables)
    for (va, d, _) in &img.sections {
        if img.is_code(*va) {
            continue;
        }
        for (k, w) in d.chunks_exact(8).enumerate() {
            let t = u64::from_le_bytes(w.try_into().unwrap());
            if img.is_code(t) && !img.imports.iter().any(|(s, _)| *s == va + 8 * k as u64) {
                entries.insert(t);
            }
        }
    }
    let mut funcs: BTreeMap<u64, Func> = BTreeMap::new();
    loop {
        let before = entries.len();
        funcs.clear();
        for &e in &entries.clone() {
            let (f, calls) = decode_func(&img, e, &entries);
            entries.extend(calls.into_iter().filter(|t| img.is_code(*t)));
            funcs.insert(e, f);
        }
        if entries.len() == before {
            break;
        }
    }
    // a fallthrough into another function's entry is emitted as a call to it: make sure it exists
    let mut em = Emit { img: &img, unimpl: BTreeMap::new(), unresolved: 0 };
    let _ = em.img;
    let parts: usize = args.iter().position(|a| a == "--split").map(|i| args[i + 1].parse().expect("--split N")).unwrap_or(1).max(1);
    let mut header = String::from("#include \"cpu.h\"\n");
    for e in &entries {
        writeln!(header, "void f_{e:x}(Cpu*c);").unwrap();
    }
    let mut bodies: Vec<String> = vec![header.clone(); parts];
    let mut total = 0;
    let mut stubbed = 0;
    let mut sizes = vec![0usize; parts];
    for (e, f) in &funcs {
        total += f.insns.len();
        // code for AVX and later (VEX / EVEX): the virtual CPU does not report it, so such functions never run
        let body = if f.insns.values().any(|i| i.encoding() != iced_x86::EncodingKind::Legacy && i.encoding() != iced_x86::EncodingKind::D3NOW) {
            stubbed += 1;
            format!("void f_{e:x}(Cpu*c){{rt_unimpl(0x{e:x}ull);}}\n")
        } else {
            em.func(*e, f, &entries)
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
    // the modules: each image's bytes stay in the user's file; the runtime copies them in and applies relocations
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
            0, m.sections.len(), m.relocs.len(), first_import, m.imports.len()
        ));
    }
    writeln!(out, "const ModuleDef rt_modules[]={{{}}};const unsigned rt_module_count={};", defs.join(","), mods.len()).unwrap();
    writeln!(out, "const u64 import_base=0x{IMPORT_BASE:x}ull;const unsigned import_count={};", import_slots.len()).unwrap();
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

    let missing: u32 = em.unimpl.values().sum();
    eprintln!("lifted {} functions ({} from the exception tables) in {} images, {} instructions, {} imports ({} between the images)", entries.len(), img.pdata.len(), mods.len(), total, img.imports.len(), resolved.len());
    eprintln!("{} jump tables recovered; {} `jmp reg` left as indirect calls (tail calls through pointers, or switches not recognised)", funcs.values().map(|f| f.tables.len()).sum::<usize>(), em.unresolved);
    eprintln!("{stubbed} functions use AVX or later and are left out (the virtual CPU does not report those)");
    eprintln!("not lifted yet, in the other functions: {missing} instructions ({:.2} %)", 100.0 * f64::from(missing) / total.max(1) as f64);
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
/// An x87 memory operand as a double.
fn fmem(i: &Instruction) -> String {
    use iced_x86::MemorySize as S;
    match i.memory_size() {
        S::Float32 => format!("(double)MF32({})", ea(i)),
        S::Float64 => format!("MF64({})", ea(i)),
        S::Float80 => format!("f80_load({})", ea(i)),
        S::Int16 => format!("(double)(s16)M16({})", ea(i)),
        S::Int32 => format!("(double)(s32)M32({})", ea(i)),
        S::Int64 => format!("(double)(s64)M64({})", ea(i)),
        _ => format!("MF64({})", ea(i)),
    }
}

/// Instructions beyond the first slice: atomics, string operations, bit operations, x87, CPUID and more SSE.
fn extra(i: &Instruction) -> Option<String> {
    use Mnemonic as M;
    let m = i.mnemonic();
    let mem0 = i.op_count() > 0 && i.op_kind(0) == OpKind::Memory;
    let bits = if i.op_count() > 0 && matches!(i.op_kind(0), OpKind::Register | OpKind::Memory) { opsize(i, 0) * 8 } else { 64 };
    let xd = || i.op0_register().number();
    // ---- string operations (rep: count in RCX; the direction flag is assumed clear) ----
    let string = |kind: &str, size: usize| -> String {
        let (n, u) = (size, size * 8);
        let one = match kind {
            "stos" => format!("M{u}(R[RDI])=(u{u})R[0];R[RDI]+={n};"),
            "movs" => format!("M{u}(R[RDI])=M{u}(R[RSI]);R[RDI]+={n};R[RSI]+={n};"),
            "lods" => format!("{}R[RSI]+={n};", wreg(reg_of(size), &format!("M{u}(R[RSI])"))),
            "cmps" => format!("(void)sub{u}(&f,M{u}(R[RSI]),M{u}(R[RDI]),0);R[RDI]+={n};R[RSI]+={n};"),
            _ => format!("(void)sub{u}(&f,(u{u})R[0],M{u}(R[RDI]),0);R[RDI]+={n};"), // scas
        };
        if i.has_rep_prefix() && (kind == "stos" || kind == "movs" || kind == "lods") {
            if kind == "stos" && size == 1 {
                return "__builtin_memset((void*)GP(R[RDI]),(u8)R[0],R[RCX]);R[RDI]+=R[RCX];R[RCX]=0;".into();
            }
            if kind == "movs" && size == 1 {
                return "{u64 n=R[RCX];if(R[RDI]-R[RSI]>=n){__builtin_memcpy((void*)GP(R[RDI]),(void*)GP(R[RSI]),n);R[RDI]+=n;R[RSI]+=n;R[RCX]=0;}else{while(R[RCX]){M8(R[RDI])=M8(R[RSI]);R[RDI]++;R[RSI]++;R[RCX]--;}}}".into();
            }
            return format!("while(R[RCX]){{{one}R[RCX]--;}}");
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
        M::Cld => String::new(),
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
        M::Fld if is_st(i, 0) => format!("{{double v=ST({});fpush(c,v);}}", sti(i.op_register(0))),
        M::Fld | M::Fild => format!("fpush(c,{});", fmem(i)),
        M::Fld1 => "fpush(c,1.0);".into(),
        M::Fldz => "fpush(c,0.0);".into(),
        M::Fldl2e => "fpush(c,1.4426950408889634);".into(),
        M::Fldln2 => "fpush(c,0.6931471805599453);".into(),
        M::Fldl2t => "fpush(c,3.321928094887362);".into(),
        M::Fldlg2 => "fpush(c,0.3010299956639812);".into(),
        M::Fldpi => "fpush(c,3.141592653589793);".into(),
        M::Fst | M::Fstp => {
            use iced_x86::MemorySize as S;
            let store = if is_st(i, 0) {
                format!("ST({})=ST(0);", sti(i.op_register(0)))
            } else {
                match i.memory_size() {
                    S::Float32 => format!("MF32({})=(float)ST(0);", ea(i)),
                    S::Float80 => format!("f80_store({},ST(0));", ea(i)),
                    _ => format!("MF64({})=ST(0);", ea(i)),
                }
            };
            format!("{store}{}", if m == M::Fstp { "fpop(c);" } else { "" })
        }
        M::Fist | M::Fistp | M::Fisttp => {
            let b = i.memory_size().size() * 8;
            let v = if m == M::Fisttp { format!("(s64)__builtin_trunc(ST(0))") } else { format!("fist(c,ST(0),{b})") };
            format!("M{b}({})=(u{b}){v};{}", ea(i), if m == M::Fist { "" } else { "fpop(c);" })
        }
        M::Fxch => format!("{{double t=ST(0);ST(0)=ST({n});ST({n})=t;}}", n = if i.op_count() > 0 { sti(i.op_register(0)) } else { 1 }),
        M::Fadd | M::Faddp | M::Fiadd | M::Fsub | M::Fsubp | M::Fisub | M::Fsubr | M::Fsubrp | M::Fisubr | M::Fmul | M::Fmulp | M::Fimul | M::Fdiv | M::Fdivp | M::Fidiv | M::Fdivr | M::Fdivrp | M::Fidivr => {
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
            format!("{{double r={e};{dst}=r;{pop}}}")
        }
        M::Fcom | M::Fcomp | M::Fcompp | M::Fucom | M::Fucomp | M::Fucompp | M::Ficom | M::Ficomp | M::Ftst => {
            let src = if m == M::Ftst { "0.0".to_string() } else if i.op_count() > 0 && i.op_kind(0) == OpKind::Memory { fmem(i) } else if i.op_count() > 0 && is_st(i, i.op_count() - 1) { format!("ST({})", sti(i.op_register(i.op_count() - 1))) } else { "ST(1)".into() };
            let pops = match m { M::Fcompp | M::Fucompp => "fpop(c);fpop(c);", M::Fcomp | M::Fucomp | M::Ficomp => "fpop(c);", _ => "" };
            format!("fcom_sw(c,ST(0),{src});{pops}")
        }
        M::Fcomi | M::Fcomip | M::Fucomi | M::Fucomip => format!("fcmp(&f,ST(0),ST({}));{}", sti(i.op_register(i.op_count() - 1)), if matches!(m, M::Fcomip | M::Fucomip) { "fpop(c);" } else { "" }),
        M::Fnstsw | M::Fstsw => if i.op_kind(0) == OpKind::Register { "R[0]=(R[0]&~0xFFFFull)|fstsw(c);".into() } else { format!("M16({})=fstsw(c);", ea(i)) },
        M::Fnstcw | M::Fstcw => format!("M16({})=c->fcw;", ea(i)),
        M::Fldcw => format!("c->fcw=M16({});", ea(i)),
        M::Fninit | M::Finit => "c->ftop=0;c->fcw=0x37F;c->fsw=0;".into(),
        M::Fnclex | M::Fclex => "c->fsw&=0x7F00;".into(),
        M::Fchs => "ST(0)=-ST(0);".into(),
        M::Fabs => "ST(0)=__builtin_fabs(ST(0));".into(),
        M::Fsqrt => "ST(0)=__builtin_sqrt(ST(0));".into(),
        M::Frndint => "ST(0)=fround(c,ST(0));".into(),
        M::Fscale => "ST(0)=ldexp(ST(0),(int)__builtin_trunc(ST(1)));".into(),
        M::F2xm1 => "ST(0)=expm1(ST(0)*0.6931471805599453);".into(),
        M::Fyl2x => "{double r=ST(1)*log2(ST(0));fpop(c);ST(0)=r;}".into(),
        M::Fyl2xp1 => "{double r=ST(1)*log1p(ST(0))*1.4426950408889634;fpop(c);ST(0)=r;}".into(),
        M::Fxam => "fxam(c);".into(),
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
    // __C_specific_handler (through its import thunk): a scope table; jump targets are the __except blocks
    if is_import_thunk(img, handler, "__C_specific_handler") {
        let n = img.u32(data).unwrap_or(0).min(4096) as u64;
        return (0..n).filter_map(|k| img.u32(data + 4 + 16 * k + 12)).filter(|&j| j != 0).map(|j| ib + u64::from(j)).collect();
    }
    gcc_lsda(img, data, entry, end).unwrap_or_default()
}
fn is_import_thunk(img: &Image, at: u64, name: &str) -> bool {
    let Some(b) = img.bytes(at) else { return false };
    let mut d = Decoder::with_ip(64, &b[..b.len().min(16)], at, DecoderOptions::NONE);
    let i = d.decode();
    i.mnemonic() == Mnemonic::Jmp && i.is_ip_rel_memory_operand() && img.imports.iter().any(|(slot, n)| *slot == i.ip_rel_memory_address() && n.ends_with(&format!("!{name}")))
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
