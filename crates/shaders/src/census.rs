// SPDX-License-Identifier: GPL-2.0-only
//! A census of the shader containers in an app's files (see the shader-census tool).
use crate::xbox;
use std::collections::BTreeMap;
use std::path::Path;

#[derive(Default)]
pub struct Census {
    pub files: u64,
    pub bytes: u64,
    pub containers: u64,
    pub console: u64,
    pub with_dxil: u64,
    pub unparsed: u64,
    pub stages: BTreeMap<String, u64>,
    pub per_file: BTreeMap<String, u64>,
    pub stats: xbox::InstStats,
    pub programs: u64,
}

pub fn scan(c: &mut Census, root: &Path, p: &Path) {
    if p.is_dir() {
        let Ok(rd) = std::fs::read_dir(p) else { return };
        let mut es: Vec<_> = rd.filter_map(|e| e.ok()).map(|e| e.path()).collect();
        es.sort();
        for e in es {
            scan(c, root, &e);
        }
        return;
    }
    let Ok(meta) = p.metadata() else { return };
    if meta.len() < 32 || meta.len() > 4 << 30 {
        return;
    }
    let Ok(data) = std::fs::read(p) else { return };
    c.files += 1;
    c.bytes += data.len() as u64;
    let rel = p.strip_prefix(root).unwrap_or(p).display().to_string();
    let mut at = 0;
    while let Some(k) = memchr4(&data[at..], b"DXBC") {
        let o = at + k;
        at = o + 4;
        // DXBC header: magic, 16-byte hash, version, total size
        let Some(size) = data.get(o + 24..o + 28).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize) else { continue };
        if size < 32 || o + size > data.len() {
            continue;
        }
        let blob = &data[o..o + size];
        c.containers += 1;
        *c.per_file.entry(rel.clone()).or_default() += 1;
        if xbox::is_xbox(blob) {
            c.console += 1;
            match xbox::parse(blob) {
                Some(sh) => {
                    for f in &sh.fssm {
                        *c.stages.entry(f.stage_label()).or_default() += 1;
                    }
                    c.programs += sh.fssm.len() as u64;
                    c.stats.add_shader(&sh);
                }
                None => c.unparsed += 1,
            }
        } else if crate::container::container_parts(blob).is_some_and(|ps| ps.iter().any(|p| p.fourcc == "DXIL")) {
            c.with_dxil += 1;
        }
        at = o + size;
    }
}

fn memchr4(h: &[u8], n: &[u8; 4]) -> Option<usize> {
    h.windows(4).position(|w| w == n)
}


#[cfg(test)]
mod tests {
    use super::*;
    use crate::xbox::tests::{fssm, sample, CODE};
    #[test]
    fn finds_a_console_shader_inside_any_file() {
        let dir = std::env::temp_dir().join(format!("shader-census-{}", std::process::id()));
        std::fs::create_dir_all(dir.join("pack")).unwrap();
        let mut file = vec![0x55u8; 1000];
        file.extend(sample(0x0006_0060, &[fssm(&CODE, &[(6, 3)])])); // a compute shader
        file.extend(vec![0xAAu8; 333]);
        std::fs::write(dir.join("pack/materials.bin"), &file).unwrap();
        let mut c = Census::default();
        scan(&mut c, &dir, &dir);
        std::fs::remove_dir_all(&dir).unwrap();
        assert_eq!((c.containers, c.console, c.unparsed, c.programs), (1, 1, 0, 1), "{:?}", c.stages);
        assert_eq!(c.per_file.keys().next().map(|s| s.as_str()), Some("pack/materials.bin"));
        assert!(!c.stats.sorted().is_empty());
    }
}
