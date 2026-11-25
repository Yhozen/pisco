// SPDX-License-Identifier: GPL-2.0-or-later
// The image format (docs/images.md): an OCI image layout (oci-layout, index.json, blobs/sha256/<digest>) holding one
// manifest whose config and layers have series-web's own media types. File layers are plain tar files written
// here so that the same files always give the same bytes: entries sorted, every time 0, owner 0, fixed modes.
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

pub const MANIFEST: &str = "application/vnd.oci.image.manifest.v1+json";
pub const INDEX: &str = "application/vnd.oci.image.index.v1+json";
pub const ARTIFACT: &str = "application/vnd.series-web.title.v1";
pub const CONFIG: &str = "application/vnd.series-web.title.config.v1+json";
pub const LAYER: &str = "application/vnd.series-web.layer.v1.tar";
pub const MODULE: &str = "application/vnd.series-web.module.v1.wasm";
pub const ROLE: &str = "org.series-web.layer.role";
pub const TITLE: &str = "org.opencontainers.image.title";
pub const REF: &str = "org.opencontainers.image.ref.name";
pub const LOCAL: &str = "org.series-web.local-only";

pub fn sha256(data: &[u8]) -> String { hex(&Sha256::digest(data)) }
pub fn hex(b: &[u8]) -> String { b.iter().map(|x| format!("{x:02x}")).collect() }

// ---- tar ----
pub enum Entry { Dir, File(Vec<u8>) }
/// A layer's files: paths relative to C:\ with '/' separators, as the title's file system shows them (case kept).
#[derive(Default)]
pub struct Tree { pub entries: BTreeMap<String, Entry> }
impl Tree {
    pub fn dir(&mut self, path: &str) {
        let mut p = String::new();
        for part in path.split('/').filter(|s| !s.is_empty()) {
            if !p.is_empty() { p.push('/'); }
            p.push_str(part);
            self.entries.entry(p.clone()).or_insert(Entry::Dir);
        }
    }
    pub fn file(&mut self, path: &str, data: Vec<u8>) {
        if let Some((d, _)) = path.rsplit_once('/') { self.dir(d); }
        self.entries.insert(path.to_string(), Entry::File(data));
    }
    /// The tar: ustar headers (a pax header first when a name does not fit), directories "name/" mode 0755, files
    /// mode 0644, mtime, uid and gid 0, two zero blocks at the end.
    pub fn tar(&self) -> Vec<u8> {
        let mut out = Vec::new();
        for (name, e) in &self.entries {
            let (full, size, kind, mode) = match e { Entry::Dir => (format!("{name}/"), 0, b'5', 0o755), Entry::File(d) => (name.clone(), d.len(), b'0', 0o644) };
            let (prefix, short) = split_name(&full);
            if short.is_none() {
                let rec = pax_record("path", &full);
                header(&mut out, "PaxHeaders/entry", "", rec.len(), b'x', 0o644);
                out.extend_from_slice(rec.as_bytes()); pad(&mut out);
            }
            let (p, n) = match short { Some(n) => (prefix, n), None => ("", "long-name") };
            header(&mut out, n, p, size, kind, mode);
            if let Entry::File(d) = e { out.extend_from_slice(d); pad(&mut out); }
        }
        out.extend_from_slice(&[0u8; 1024]);
        out
    }
}
fn pad(out: &mut Vec<u8>) { while out.len() % 512 != 0 { out.push(0); } }
// ustar: a name of up to 100 bytes, or a prefix of up to 155 and the rest after a '/'
fn split_name(full: &str) -> (&str, Option<&str>) {
    if full.len() <= 100 { return ("", Some(full)); }
    let trimmed = full.trim_end_matches('/');
    for (i, _) in trimmed.match_indices('/') {
        let (p, n) = (&full[..i], &full[i + 1..]);
        if p.len() <= 155 && n.len() <= 100 && !n.is_empty() { return (p, Some(n)); }
    }
    ("", None)
}
fn pax_record(key: &str, value: &str) -> String {
    let body = format!(" {key}={value}\n");
    let mut n = body.len() + 1;
    while format!("{n}{body}").len() != n { n += 1; }
    format!("{n}{body}")
}
fn header(out: &mut Vec<u8>, name: &str, prefix: &str, size: usize, kind: u8, mode: u32) {
    let mut h = [0u8; 512];
    let put = |h: &mut [u8; 512], at: usize, s: &[u8]| h[at..at + s.len()].copy_from_slice(s);
    put(&mut h, 0, &name.as_bytes()[..name.len().min(100)]);
    put(&mut h, 100, format!("{mode:07o}\0").as_bytes());
    put(&mut h, 108, b"0000000\0");
    put(&mut h, 116, b"0000000\0");
    put(&mut h, 124, format!("{size:011o}\0").as_bytes());
    put(&mut h, 136, b"00000000000\0");
    h[156] = kind;
    put(&mut h, 257, b"ustar\x0000");
    put(&mut h, 345, prefix.as_bytes());
    h[148..156].copy_from_slice(b"        ");
    let sum: u32 = h.iter().map(|&b| b as u32).sum();
    put(&mut h, 148, format!("{sum:06o}\0 ").as_bytes());
    out.extend_from_slice(&h);
}
/// The entries of a tar (name, offset of the data, size, is a directory), for `sw inspect`
pub fn tar_list(data: &[u8]) -> Vec<(String, usize, usize, bool)> {
    let mut out = Vec::new(); let mut at = 0; let mut long: Option<String> = None;
    while at + 512 <= data.len() {
        let h = &data[at..at + 512];
        if h.iter().all(|&b| b == 0) { break; }
        let field = |a: usize, n: usize| { let s = &h[a..a + n]; String::from_utf8_lossy(&s[..s.iter().position(|&b| b == 0).unwrap_or(n)]).into_owned() };
        let size = usize::from_str_radix(field(124, 12).trim(), 8).unwrap_or(0);
        let kind = h[156];
        let mut name = field(0, 100); let prefix = field(345, 155);
        if !prefix.is_empty() { name = format!("{prefix}/{name}"); }
        let body = at + 512;
        if kind == b'x' {
            let rec = String::from_utf8_lossy(&data[body..body + size]).into_owned();
            long = rec.lines().find_map(|l| l.split_once(' ').and_then(|(_, kv)| kv.strip_prefix("path=")).map(|s| s.to_string()));
        } else {
            if let Some(l) = long.take() { name = l; }
            out.push((name.trim_end_matches('/').to_string(), body, size, kind == b'5'));
        }
        at = body + size.div_ceil(512) * 512;
    }
    out
}

// ---- the layout ----
pub struct Blob { pub media: String, pub digest: String, pub size: usize, pub annotations: BTreeMap<String, String> }
impl Blob {
    pub fn descriptor(&self) -> Value {
        let mut d = json!({ "mediaType": self.media, "digest": format!("sha256:{}", self.digest), "size": self.size });
        if !self.annotations.is_empty() { d["annotations"] = json!(self.annotations); }
        d
    }
}
pub struct Layout { pub dir: PathBuf }
impl Layout {
    /// A new layout in `dir` (what was there goes)
    pub fn create(dir: &Path) -> Result<Layout, String> {
        let _ = std::fs::remove_dir_all(dir);
        std::fs::create_dir_all(dir.join("blobs/sha256")).map_err(|e| format!("{}: {e}", dir.display()))?;
        std::fs::write(dir.join("oci-layout"), b"{\"imageLayoutVersion\":\"1.0.0\"}").map_err(|e| e.to_string())?;
        Ok(Layout { dir: dir.to_path_buf() })
    }
    pub fn put(&self, media: &str, data: &[u8], annotations: &[(&str, &str)]) -> Result<Blob, String> {
        let digest = sha256(data);
        let p = self.dir.join("blobs/sha256").join(&digest);
        if !p.exists() { std::fs::write(&p, data).map_err(|e| format!("{}: {e}", p.display()))?; }
        Ok(Blob { media: media.into(), digest, size: data.len(), annotations: annotations.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect() })
    }
    /// The canonical JSON of a document: keys sorted (serde_json's map), no spaces, so equal documents are equal bytes
    pub fn put_json(&self, media: &str, v: &Value, annotations: &[(&str, &str)]) -> Result<Blob, String> {
        self.put(media, serde_json::to_string(v).unwrap().as_bytes(), annotations)
    }
    pub fn finish(&self, manifest: &Blob, name: &str) -> Result<(), String> {
        let mut m = manifest.descriptor();
        m["annotations"] = json!({ REF: name });
        let index = json!({ "schemaVersion": 2, "mediaType": INDEX, "manifests": [m] });
        std::fs::write(self.dir.join("index.json"), serde_json::to_string(&index).unwrap()).map_err(|e| e.to_string())
    }
}

// ---- reading an image ----
pub struct Image { pub dir: PathBuf, pub manifest_digest: String, pub manifest: Value, pub config: Value }
pub fn blob(dir: &Path, digest: &str) -> Result<Vec<u8>, String> {
    let hexd = digest.strip_prefix("sha256:").ok_or_else(|| format!("{digest}: not a sha256 digest"))?;
    let data = std::fs::read(dir.join("blobs/sha256").join(hexd)).map_err(|e| format!("{}: blob {digest}: {e}", dir.display()))?;
    if sha256(&data) != hexd { return Err(format!("{}: blob {digest} does not have that digest", dir.display())); }
    Ok(data)
}
pub fn is_image(p: &Path) -> bool { p.join("oci-layout").is_file() && p.join("index.json").is_file() }
pub fn open(dir: &Path) -> Result<Image, String> {
    let index: Value = serde_json::from_slice(&std::fs::read(dir.join("index.json")).map_err(|e| format!("{}: {e}", dir.display()))?).map_err(|e| e.to_string())?;
    let m = index["manifests"].as_array().and_then(|a| a.first()).ok_or("index.json has no manifest")?;
    let digest = m["digest"].as_str().ok_or("manifest without a digest")?.to_string();
    let manifest: Value = serde_json::from_slice(&blob(dir, &digest)?).map_err(|e| e.to_string())?;
    if manifest["artifactType"] != ARTIFACT { return Err(format!("{}: not a series-web title image (artifactType {})", dir.display(), manifest["artifactType"])); }
    let config: Value = serde_json::from_slice(&blob(dir, manifest["config"]["digest"].as_str().ok_or("no config")?)?).map_err(|e| e.to_string())?;
    Ok(Image { dir: dir.to_path_buf(), manifest_digest: digest, manifest, config })
}
