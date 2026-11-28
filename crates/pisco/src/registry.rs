// SPDX-License-Identifier: GPL-2.0-or-later
// A registry (docs/spec/distribution.md#registries): pisco push and pisco pull, over OCI's distribution API
// (https://github.com/opencontainers/distribution-spec), with curl. A reference is <host>/<repository>[:<tag> |
// @sha256:<digest>]; ghcr.io for now. localhost and 127.0.0.1 are spoken to in plain http (as docker does), the rest
// in https.
// Credentials: PISCO_REGISTRY_USER and PISCO_REGISTRY_TOKEN; for ghcr.io, GITHUB_TOKEN or `gh auth token`. They go to
// curl in a file of headers, never on its command line.
use crate::oci;
use crate::recipe::R;
use crate::store;
use serde_json::Value;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct Ref { pub host: String, pub repo: String, pub tag: String }
pub fn parse_ref(s: &str) -> R<Ref> {
    let (host, rest) = s.split_once('/').filter(|(h, _)| h.contains('.') || h.contains(':') || *h == "localhost").ok_or(format!("{s}: a reference is <registry host>/<repository>[:<tag>] (ghcr.io/<owner>/<name>:<tag>)"))?;
    let (repo, tag) = if let Some((r, d)) = rest.split_once('@') { (r, d.to_string()) }
        else if let Some((r, t)) = rest.rsplit_once(':').filter(|(_, t)| !t.contains('/')) { (r, t.to_string()) } else { (rest, "latest".to_string()) };
    let ok = !repo.is_empty() && repo.split('/').all(|p| !p.is_empty() && p.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"._-".contains(&b)));
    if !ok { return Err(format!("{s}: the repository is lower-case letters, digits, '.', '_', '-' and '/'")); }
    Ok(Ref { host: host.to_string(), repo: repo.to_string(), tag })
}

struct Client { base: String, host: String, repo: String, token: Option<String>, tmp: PathBuf }
struct Resp { status: u32, headers: BTreeMap<String, String>, body: Vec<u8> }
fn b64(data: &[u8]) -> String {
    const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 { s.push(if i <= c.len() { T[(n >> (18 - 6 * i) & 63) as usize] as char } else { '=' }); }
    }
    s
}
fn credentials(host: &str) -> Option<(String, String)> {
    if let Ok(t) = std::env::var("PISCO_REGISTRY_TOKEN").map(|t| t.trim().to_string()).and_then(|t| if t.is_empty() { Err(std::env::VarError::NotPresent) } else { Ok(t) }) { return Some((std::env::var("PISCO_REGISTRY_USER").unwrap_or("pisco".into()), t)); }
    if host == "ghcr.io" {
        let t = std::env::var("GITHUB_TOKEN").ok().filter(|t| !t.trim().is_empty()).or_else(|| Command::new("gh").args(["auth", "token"]).output().ok().filter(|o| o.status.success()).map(|o| String::from_utf8_lossy(&o.stdout).trim().to_string()))?;
        return (!t.is_empty()).then(|| ("pisco".into(), t));
    }
    None
}
impl Client {
    fn new(r: &Ref) -> R<Client> {
        let plain = r.host.starts_with("localhost") || r.host.starts_with("127.0.0.1");
        let tmp = store::home().join("tmp").join(format!("registry.{}", std::process::id()));
        std::fs::create_dir_all(&tmp).map_err(|e| format!("{}: {e}", tmp.display()))?;
        Ok(Client { base: format!("{}://{}", if plain { "http" } else { "https" }, r.host), host: r.host.clone(), repo: r.repo.clone(), token: None, tmp })
    }
    /// One request with curl: the headers (and the token) from a file only this user can read
    fn raw(&self, method: &str, url: &str, headers: &[String], body: Option<&Path>, basic: Option<&(String, String)>) -> R<Resp> {
        use std::os::unix::fs::OpenOptionsExt;
        let hf = self.tmp.join("headers"); let out = self.tmp.join("body"); let got = self.tmp.join("got");
        let mut h = headers.to_vec();
        if let Some((u, p)) = basic { h.push(format!("Authorization: Basic {}", b64(format!("{u}:{p}").as_bytes()))); }
        else if let Some(t) = &self.token { h.push(format!("Authorization: Bearer {t}")); }
        let _ = std::fs::remove_file(&hf);
        let mut f = std::fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&hf).map_err(|e| e.to_string())?;
        std::io::Write::write_all(&mut f, (h.join("\n") + "\n").as_bytes()).map_err(|e| e.to_string())?;
        drop(f);
        let mut c = Command::new("curl");
        if method == "HEAD" { c.arg("-I"); } else { c.args(["-X", method]); }
        c.args(["-sS", "-H"]).arg(format!("@{}", hf.display())).arg("-o").arg(&out).arg("-D").arg(&got).args(["-w", "%{http_code}"]);
        if method == "GET" { c.arg("-L"); }   // a registry may send a blob's GET elsewhere (ghcr.io does); curl does not take the token there
        if let Some(b) = body { c.arg("--data-binary").arg(format!("@{}", b.display())); }
        let o = c.arg(url).output().map_err(|e| format!("curl: {e}"))?;
        let _ = std::fs::remove_file(&hf);
        if !o.status.success() { return Err(format!("{method} {url}: {}", String::from_utf8_lossy(&o.stderr).trim())); }
        let status = String::from_utf8_lossy(&o.stdout).trim().parse().unwrap_or(0);
        let text = std::fs::read_to_string(&got).unwrap_or_default();
        // the last response's headers (after a redirect)
        let last = text.rsplit("\r\n\r\n").find(|b| b.starts_with("HTTP/")).unwrap_or(&text);
        let headers = last.lines().filter_map(|l| l.split_once(':')).map(|(k, v)| (k.trim().to_ascii_lowercase(), v.trim().to_string())).collect();
        let body = std::fs::read(&out).unwrap_or_default();
        let _ = std::fs::remove_file(&out);
        Ok(Resp { status, headers, body })
    }
    /// A request, with the bearer-token dance when the registry asks for it (WWW-Authenticate: Bearer realm=...)
    fn req(&mut self, method: &str, path: &str, headers: &[String], body: Option<&Path>, push: bool) -> R<Resp> {
        let url = if path.starts_with("http") { path.to_string() } else { format!("{}{path}", self.base) };
        let r = self.raw(method, &url, headers, body, None)?;
        if r.status != 401 { return Ok(r); }
        let www = r.headers.get("www-authenticate").cloned().unwrap_or_default();
        let field = |k: &str| www.split(&format!("{k}=\"")).nth(1).and_then(|s| s.split('"').next()).map(String::from);
        let realm = field("realm").ok_or(format!("{}: asks for credentials, and not with a bearer token", self.host))?;
        let scope = format!("repository:{}:{}", self.repo, if push { "pull,push" } else { "pull" });
        let mut turl = format!("{realm}?scope={scope}");
        if let Some(s) = field("service") { turl.push_str(&format!("&service={s}")); }
        let creds = credentials(&self.host);
        let mut t = self.raw("GET", &turl, &[], None, creds.as_ref())?;
        // a public repository gives anyone a token to pull, whatever the credentials allow
        if t.status != 200 && !push && creds.is_some() { t = self.raw("GET", &turl, &[], None, None)?; }
        if t.status != 200 { return Err(format!("{}: no token for {scope} ({}){}", self.host, t.status, if creds.is_none() { ": no credentials (PISCO_REGISTRY_TOKEN, or GITHUB_TOKEN / gh auth for ghcr.io)" } else { "" })); }
        let v: Value = serde_json::from_slice(&t.body).map_err(|e| format!("{}: token: {e}", self.host))?;
        self.token = v["token"].as_str().or(v["access_token"].as_str()).map(String::from);
        let r = self.raw(method, &url, headers, body, None)?;
        if r.status == 401 || r.status == 403 { return Err(format!("{method} {url}: {} (the credentials do not allow {scope})", r.status)); }
        Ok(r)
    }
}
impl Drop for Client { fn drop(&mut self) { let _ = std::fs::remove_dir_all(&self.tmp); } }

/// `pisco push <image | name> <ref>`: an image whose every layer may be shared (none is org.pisco.local-only)
pub fn push(image: &Path, to: &str) -> R<String> {
    let img = oci::open(image)?;
    let name = img.config["name"].as_str().unwrap_or("?").to_string();
    for l in img.manifest["layers"].as_array().into_iter().flatten() {
        if l["annotations"][oci::LOCAL] == "true" {
            return Err(format!("{name}: its {} layer is made from the user's files (org.pisco.local-only): never pushed (docs/spec/distribution.md#what-may-be-published)", l["annotations"][oci::ROLE].as_str().unwrap_or("?")));
        }
    }
    let r = parse_ref(to)?;
    if r.tag.starts_with("sha256:") { return Err(format!("{to}: push to a tag; the digest is the manifest's")); }
    let mut c = Client::new(&r)?;
    let mut digests: Vec<String> = vec![img.manifest["config"]["digest"].as_str().unwrap_or("").to_string()];
    digests.extend(img.manifest["layers"].as_array().into_iter().flatten().filter_map(|l| l["digest"].as_str().map(String::from)));
    let (mut sent, mut had) = (0, 0);
    for d in &digests {
        let blob = img.dir.join("blobs/sha256").join(d.trim_start_matches("sha256:"));
        let h = c.req("HEAD", &format!("/v2/{}/blobs/{d}", r.repo), &[], None, true)?;
        if h.status == 200 { had += 1; continue; }
        let up = c.req("POST", &format!("/v2/{}/blobs/uploads/", r.repo), &["Content-Length: 0".into()], None, true)?;
        if up.status != 202 { return Err(format!("{to}: starting an upload: {} {}", up.status, String::from_utf8_lossy(&up.body))); }
        let loc = up.headers.get("location").ok_or(format!("{to}: an upload without a Location"))?;
        let loc = if loc.starts_with("http") { loc.clone() } else { format!("{}{loc}", c.base) };
        let put = format!("{loc}{}digest={d}", if loc.contains('?') { "&" } else { "?" });
        let p = c.req("PUT", &put, &["Content-Type: application/octet-stream".into()], Some(&blob), true)?;
        if p.status != 201 { return Err(format!("{to}: blob {d}: {} {}", p.status, String::from_utf8_lossy(&p.body))); }
        sent += 1;
    }
    let mf = img.dir.join("blobs/sha256").join(img.manifest_digest.trim_start_matches("sha256:"));
    let m = c.req("PUT", &format!("/v2/{}/manifests/{}", r.repo, r.tag), &[format!("Content-Type: {}", oci::MANIFEST)], Some(&mf), true)?;
    if m.status != 201 { return Err(format!("{to}: the manifest: {} {}", m.status, String::from_utf8_lossy(&m.body))); }
    crate::build::say(&format!("{name}: {sent} blobs sent, {had} the registry had"));
    Ok(img.manifest_digest)
}

/// `pisco pull <ref> [--as <name>]`: the image into a fresh folder of the store's (every blob checked by its digest);
/// returns the folder, the app's name and the manifest's digest
pub fn pull(from: &str) -> R<(PathBuf, String, String)> {
    let r = parse_ref(from)?;
    let mut c = Client::new(&r)?;
    let m = c.req("GET", &format!("/v2/{}/manifests/{}", r.repo, r.tag), &[format!("Accept: {}", oci::MANIFEST)], None, false)?;
    if m.status != 200 { return Err(format!("{from}: {} {}", m.status, String::from_utf8_lossy(&m.body).chars().take(200).collect::<String>())); }
    let digest = format!("sha256:{}", oci::sha256(&m.body));
    if r.tag.starts_with("sha256:") && r.tag != digest { return Err(format!("{from}: the registry's manifest is {digest}")); }
    let manifest: Value = serde_json::from_slice(&m.body).map_err(|e| format!("{from}: the manifest: {e}"))?;
    if manifest["artifactType"] != oci::ARTIFACT { return Err(format!("{from}: not a pisco app (artifactType {})", manifest["artifactType"])); }
    let name = manifest["annotations"][oci::TITLE].as_str().ok_or(format!("{from}: the manifest does not name its app"))?.to_string();
    let dir = store::staging(&name)?;
    let lay = oci::Layout::create(&dir)?;
    let mut descs = vec![manifest["config"].clone()];
    descs.extend(manifest["layers"].as_array().cloned().unwrap_or_default());
    for d in descs {
        let dg = d["digest"].as_str().ok_or(format!("{from}: a descriptor without a digest"))?;
        if d["annotations"][oci::LOCAL] == "true" { return Err(format!("{from}: has a local-only layer ({dg}), which no registry should have")); }
        let b = c.req("GET", &format!("/v2/{}/blobs/{dg}", r.repo), &[], None, false)?;
        if b.status != 200 { return Err(format!("{from}: blob {dg}: {}", b.status)); }
        let got = lay.put("", &b.body, &[])?;
        if format!("sha256:{}", got.digest) != dg { return Err(format!("{from}: blob {dg} is not what its digest says")); }
    }
    let man = lay.put(oci::MANIFEST, &m.body, &[])?;
    lay.finish(&man, &name)?;
    oci::open(&dir)?;
    Ok((dir, name, digest))
}
