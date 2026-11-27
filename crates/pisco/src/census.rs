// SPDX-License-Identifier: GPL-2.0-or-later
// A title's imports against what answers them: the DLLs it ships (lifted with it), the base's core, the base's
// libraries (those the recipe names, and those it would need). Used by `pisco build` (a title that imports from a
// library its recipe leaves out is refused) and `pisco census`.
use crate::recipe::{self, Base, R};
use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;

pub struct Census { provider: BTreeMap<String, Vec<String>>, selected: Vec<String>, shipped: Vec<String> }
pub struct Report { pub summary: String, pub needs: Vec<String>, pub used: BTreeSet<String>, pub lines: Vec<String> }

impl Census {
    /// `selected`: the recipe's libraries; `shipped`: DLLs beside the exe that are lifted with it (lower case)
    pub fn new(root: &Path, base: &Base, selected: &[String], shipped: &[String]) -> R<Census> {
        let mut provider: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for f in recipe::implemented(root, &base.sources)? { provider.entry(f).or_default().push("base".into()); }
        for (n, l) in &base.libraries { for f in recipe::implemented(root, &l.sources)? { provider.entry(f).or_default().push(n.clone()); } }
        Ok(Census { provider, selected: selected.to_vec(), shipped: shipped.to_vec() })
    }
    /// The DLLs a part ("base" or a library) answers for
    pub fn dlls_of(&self, part: &str) -> Vec<String> {
        let s: BTreeSet<String> = self.provider.iter().filter(|(_, p)| p.iter().any(|x| x == part)).map(|(f, _)| f.split('!').next().unwrap().to_string()).collect();
        s.into_iter().collect()
    }
    pub fn check(&self, imports: &[String]) -> Report {
        let mut by: BTreeMap<String, usize> = BTreeMap::new();
        let (mut needs, mut used, mut lines, mut missing) = (BTreeSet::new(), BTreeSet::new(), Vec::new(), 0);
        for imp in imports {
            let (dll, name) = imp.split_once('!').unwrap_or((imp, ""));
            let dll = dll.to_ascii_lowercase();
            let full = format!("{dll}!{name}");
            if self.shipped.contains(&dll) { *by.entry("lifted with the app".into()).or_default() += 1; lines.push(format!("lifted   {imp}")); continue; }
            // as the runtime's find_import (rt/process.c) looks for it: API sets and the C runtimes by the DLL behind
            // them, and a DLL's decorated stdcall name (fmod.dll's `_FSOUND_Init@12`) without its decoration
            let mut names = vec![full.clone()];
            if let Some(at) = name.rfind('@') { if name.starts_with('_') && name[at + 1..].starts_with(|c: char| c.is_ascii_digit()) { names.push(format!("{dll}!{}", &name[1..at])); } }
            if dll.starts_with("api-ms-win-crt-") || dll == "ucrtbase.dll" || dll.starts_with("vcruntime140") { names.push(format!("ucrtbase.dll!{name}")); names.push(format!("msvcrt.dll!{name}")); }
            if dll.starts_with("api-ms-win-core-") || dll == "kernelbase.dll" || dll == "ntdll.dll" { names.push(format!("kernel32.dll!{name}")); }
            if dll == "wsock32.dll" {   // its functions are ws2_32's; three ordinals are numbered differently (rt/process.c)
                let n = match &name[..] { "#10" => "#11", "#11" => "#12", "#12" => "#10", x => x };
                names.push(format!("ws2_32.dll!{n}"));
            }
            if dll == "psapi.dll" { names.push(format!("kernel32.dll!K32{name}")); }
            match names.iter().find_map(|n| self.provider.get(n)) {
                Some(p) if p.iter().any(|x| x == "base") => { *by.entry("base".into()).or_default() += 1; lines.push(format!("base     {imp}")); }
                Some(p) => match p.iter().find(|x| self.selected.contains(x)) {
                    Some(l) => { used.insert(l.clone()); *by.entry(l.clone()).or_default() += 1; lines.push(format!("{l:8} {imp}")); }
                    None => { needs.insert(p[0].clone()); lines.push(format!("needs    {imp} (library {})", p[0])); }
                },
                None => {
                    missing += 1;
                    // a DLL a library answers for, without this function: the library still has to be chosen
                    let owner = self.provider.keys().find(|f| f.split('!').next() == Some(dll.as_str())).map(|f| self.provider[f][0].clone());
                    if let Some(o) = &owner { if o != "base" { if self.selected.contains(o) { used.insert(o.clone()); } else { needs.insert(o.clone()); } } }
                    lines.push(format!("missing  {imp} ({}: a stub that returns 0, and SW_STRICT=1 stops if it is called)", owner.map(|o| format!("{o} answers for {dll}, not for this function")).unwrap_or(format!("nothing answers for {dll}"))));
                }
            }
        }
        let mut parts: Vec<String> = by.iter().map(|(k, n)| format!("{n} {k}")).collect();
        if missing > 0 { parts.push(format!("{missing} not implemented")); }
        if !needs.is_empty() { parts.push(format!("libraries needed and not chosen: {}", needs.iter().cloned().collect::<Vec<_>>().join(", "))); }
        Report { summary: format!("{} imports: {}", imports.len(), parts.join(", ")), needs: needs.into_iter().collect(), used, lines }
    }
}
