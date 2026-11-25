// SPDX-License-Identifier: GPL-2.0-only
// From XCL (https://github.com/ivall/xcl, GPL-2.0-only), crates/xcl/src/gpu/shader.rs at 6025201: the DXBC part table.
#[derive(Debug, Clone, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub struct Part {
    pub fourcc: String,
    pub offset: usize,
    pub size: usize,
}
pub fn container_parts(blob: &[u8]) -> Option<Vec<Part>> {
    if blob.len() < 32 || &blob[0..4] != b"DXBC" {
        return None;
    }
    let rd = |o: usize| blob.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize);
    let count = rd(28)?;
    if count > 64 {
        return None;
    }
    let mut parts = Vec::with_capacity(count);
    for i in 0..count {
        let off = rd(32 + i * 4)?;
        let fourcc = blob.get(off..off + 4)?;
        let size = rd(off + 4)?;
        if off + 8 + size > blob.len() {
            return None;
        }
        parts.push(Part {
            fourcc: String::from_utf8_lossy(fourcc).chars().map(|c| if c.is_ascii_graphic() { c } else { '?' }).collect(),
            offset: off + 8,
            size,
        });
    }
    Some(parts)
}
