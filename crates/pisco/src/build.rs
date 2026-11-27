// SPDX-License-Identifier: GPL-2.0-or-later
// pisco build: a recipe and the user's own installer -> a title image (docs/images.md).
//   1. the installer, checked by its sha256, is extracted (innoextract) into the cache, once per installer
//   2. the title's images are lifted and the lifted C compiled, once per lift key: the digest of the images' bytes,
//      the lifter (its version and the hash of its sources), rt/cpu.h and rt/modules.h, the lifter's options,
//      clang's version and the flags
//   3. the runtime's sources (the base's core and the recipe's libraries) are compiled, once per source and flags
//   4. the module is linked (once per set of objects), the file layers written as tars, the config and the manifest
// The cache is out/pisco-cache (--cache): what is in it only saves time, an image never depends on it.
use crate::census::Census;
use crate::oci::{self, Layout, Tree, LAYER, LOCAL, MODULE, ROLE, TITLE};
use crate::recipe::{self, Base, RegData, Recipe, R};
use serde_json::json;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::Instant;

pub struct Opts { pub recipe: PathBuf, pub source: Option<PathBuf>, pub out: Option<PathBuf>, pub cache: Option<PathBuf>, pub compare: Option<PathBuf>,
    pub with: Vec<(String, PathBuf)> }

pub fn say(s: &str) { eprintln!("pisco: {s}"); }
fn io<T>(r: std::io::Result<T>, what: &Path) -> R<T> { r.map_err(|e| format!("{}: {e}", what.display())) }
const TARGET: &[&str] = &["--target=wasm32-wasip1-threads", "-matomics", "-mbulk-memory", "-msimd128", "-mnontrapping-fptoint", "-msign-ext"];

pub struct Tools { pub root: PathBuf, pub clang: PathBuf, pub clang_version: String, pub lifter: PathBuf, pub lifter_hash: String }
pub fn tools(root: &Path) -> R<Tools> {
    let wasi = std::env::var("WASI_SDK").map(PathBuf::from).unwrap_or_else(|_| home().join("tools/wasi-sdk-34.0-x86_64-linux"));
    let clang = wasi.join("bin/clang");
    let v = Command::new(&clang).arg("--version").output().map_err(|e| format!("{}: {e} (tools/setup-toolchain.sh wasi)", clang.display()))?;
    let clang_version = String::from_utf8_lossy(&v.stdout).lines().next().unwrap_or("").to_string();
    // the lifter: built from its sources first, so that the hash of the sources is the lifter's
    if std::env::var_os("SW_NO_CARGO").is_none() {
        let st = Command::new("cargo").args(["build", "-q", "--release", "-p", "pisco-lifter"]).current_dir(root).status().map_err(|e| format!("cargo: {e}"))?;
        if !st.success() { return Err("cargo build -p pisco-lifter failed".into()); }
    }
    let lifter = root.join("target/release/lifter");
    if !lifter.is_file() { return Err(format!("{}: not built (cargo build --release -p pisco-lifter)", lifter.display())); }
    let mut h = String::new();
    let mut files = vec![root.join("crates/lifter/Cargo.toml"), root.join("Cargo.lock")];
    let mut src: Vec<PathBuf> = io(std::fs::read_dir(root.join("crates/lifter/src")), &root.join("crates/lifter/src"))?.filter_map(|e| e.ok()).map(|e| e.path()).collect();
    src.sort(); files.extend(src);
    for f in files { h.push_str(&format!("{} {}\n", f.strip_prefix(root).unwrap().display(), oci::sha256(&io(std::fs::read(&f), &f)?))); }
    Ok(Tools { root: root.to_path_buf(), clang, clang_version, lifter, lifter_hash: oci::sha256(h.as_bytes()) })
}
pub fn home() -> PathBuf { PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/".into())) }

// ---- running compiles in parallel ----
fn run_all(jobs: Vec<(String, Command)>) -> R<()> {
    let n = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(2);
    let n = std::env::var("JOBS").ok().and_then(|j| j.parse().ok()).unwrap_or(n).max(1);
    let queue = std::sync::Mutex::new(jobs.into_iter().collect::<Vec<_>>());
    let errors = std::sync::Mutex::new(Vec::<String>::new());
    std::thread::scope(|s| {
        for _ in 0..n {
            s.spawn(|| loop {
                let job = queue.lock().unwrap().pop();
                let Some((what, mut cmd)) = job else { break };
                match cmd.output() {
                    Ok(o) if o.status.success() => {}
                    Ok(o) => errors.lock().unwrap().push(format!("{what}: {}", String::from_utf8_lossy(&o.stderr).lines().take(8).collect::<Vec<_>>().join("\n  "))),
                    Err(e) => errors.lock().unwrap().push(format!("{what}: {e}")),
                }
            });
        }
    });
    let e = errors.into_inner().unwrap();
    if e.is_empty() { Ok(()) } else { Err(e.join("\n")) }
}
/// A finished cache entry is renamed into place, so a build that stops leaves nothing half made
fn publish(tmp: &Path, dst: &Path) -> R<()> {
    if dst.exists() { let _ = std::fs::remove_dir_all(tmp); let _ = std::fs::remove_file(tmp); return Ok(()); }
    io(std::fs::rename(tmp, dst), dst)
}
fn tmp_name(p: &Path) -> PathBuf { let mut s = p.as_os_str().to_owned(); s.push(format!(".tmp{}", std::process::id())); PathBuf::from(s) }

// ---- 1. the source ----
pub struct Extracted { pub files: BTreeMap<String, PathBuf>, pub dirs: Vec<String>, pub app_guest: String, pub added: Vec<String> }
fn innoextract() -> PathBuf {
    if let Ok(p) = std::env::var("INNOEXTRACT") { return p.into(); }
    let p = home().join("tools/innoextract/bin/amd64/innoextract");
    if p.is_file() { p } else { "innoextract".into() }
}
fn walk(dir: &Path, rel: &str, files: &mut Vec<(String, PathBuf)>, dirs: &mut Vec<String>) -> R<()> {
    let mut es: Vec<_> = io(std::fs::read_dir(dir), dir)?.filter_map(|e| e.ok()).collect();
    es.sort_by_key(|e| e.file_name());
    for e in es {
        let name = e.file_name().to_string_lossy().into_owned();
        let r = if rel.is_empty() { name.clone() } else { format!("{rel}/{name}") };
        if e.path().is_dir() { dirs.push(r.clone()); walk(&e.path(), &r, files, dirs)?; } else { files.push((r, e.path())); }
    }
    Ok(())
}
/// extract = "zip": the archive's files (stored or deflated) into `to`; with `root`, only those under that folder of
/// the archive, without it. A name that leaves the folder (absolute, or with ..) is refused.
fn unzip(src: &Path, root: Option<&str>, to: &Path) -> R<()> {
    let z = io(std::fs::read(src), src)?;
    let u16at = |o: usize| -> R<usize> { z.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as usize).ok_or(format!("{}: not a zip archive (cut short)", src.display())) };
    let u32at = |o: usize| -> R<usize> { z.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]) as usize).ok_or(format!("{}: not a zip archive (cut short)", src.display())) };
    // the end of central directory record: in the last 64 KiB + 22 bytes
    let lo = z.len().saturating_sub(65557);
    let eocd = (lo..z.len().saturating_sub(21)).rev().find(|&o| z[o..o + 4] == [0x50, 0x4b, 5, 6]).ok_or(format!("{}: not a zip archive", src.display()))?;
    let (count, mut o) = (u16at(eocd + 10)?, u32at(eocd + 16)?);
    let mut n = 0;
    for _ in 0..count {
        if u32at(o)? != 0x02014b50 { return Err(format!("{}: a broken central directory", src.display())); }
        let (flags, method, csize, size) = (u16at(o + 8)?, u16at(o + 10)?, u32at(o + 20)?, u32at(o + 24)?);
        let (nlen, xlen, clen, local) = (u16at(o + 28)?, u16at(o + 30)?, u16at(o + 32)?, u32at(o + 42)?);
        let name = String::from_utf8_lossy(z.get(o + 46..o + 46 + nlen).ok_or("zip: cut short")?).replace('\\', "/");
        o += 46 + nlen + xlen + clen;
        if flags & 1 != 0 { return Err(format!("{}: {name} is encrypted", src.display())); }
        let rel = match root { Some(r) => match name.strip_prefix(r).and_then(|x| x.strip_prefix('/')) { Some(x) => x.to_string(), None => continue }, None => name.clone() };
        if rel.is_empty() { continue; }
        if rel.starts_with('/') || rel.split('/').any(|p| p == ".." || p.contains(':')) { return Err(format!("{}: {name} leaves the folder", src.display())); }
        let dst = to.join(rel.trim_end_matches('/'));
        if name.ends_with('/') { io(std::fs::create_dir_all(&dst), &dst)?; continue; }
        let data_at = local + 30 + u16at(local + 26)? + u16at(local + 28)?;
        let raw = z.get(data_at..data_at + csize).ok_or(format!("{}: {name} cut short", src.display()))?;
        let data = match method {
            0 => raw.to_vec(),
            8 => miniz_oxide::inflate::decompress_to_vec(raw).map_err(|e| format!("{}: {name}: {e:?}", src.display()))?,
            m => return Err(format!("{}: {name}: compression method {m} (only stored and deflate are read)", src.display())),
        };
        if data.len() != size { return Err(format!("{}: {name}: {} bytes, the archive says {size}", src.display(), data.len())); }
        if let Some(p) = dst.parent() { io(std::fs::create_dir_all(p), p)?; }
        io(std::fs::write(&dst, data), &dst)?;
        n += 1;
    }
    if n == 0 { return Err(format!("{}: no files{}", src.display(), root.map(|r| format!(" under {r}/")).unwrap_or_default())); }
    Ok(())
}
/// extract = "wise": a Wise installer (or a zip around one: its one .exe), its files to the title's folder; with
/// [source.installshield], the InstallShield setup among them is read instead and its file groups go where the
/// recipe says
fn unwise(r: &Recipe, src: &Path, tmp: &Path) -> R<()> {
    let mut exe = io(std::fs::read(src), src)?;
    if exe.starts_with(b"PK\x03\x04") {
        let z = tmp.join("zip");
        unzip(src, None, &z)?;
        let (mut fs, mut ds) = (Vec::new(), Vec::new()); walk(&z, "", &mut fs, &mut ds)?;
        let exes: Vec<_> = fs.iter().filter(|(n, _)| n.to_ascii_lowercase().ends_with(".exe")).collect();
        if exes.len() != 1 { return Err(format!("{}: extract = \"wise\" wants a zip with one .exe in it ({} found)", src.display(), exes.len())); }
        exe = io(std::fs::read(&exes[0].1), &exes[0].1)?;
        io(std::fs::remove_dir_all(&z), &z)?;
    }
    let files = crate::installers::wise(&exe).map_err(|e| format!("{}: {e}", src.display()))?;
    drop(exe);
    let app = tmp.join("app");
    let write = |rel: &str, data: &[u8]| -> R<()> {
        let dst = app.join(rel);
        if let Some(p) = dst.parent() { io(std::fs::create_dir_all(p), p)?; }
        io(std::fs::write(&dst, data), &dst)
    };
    let Some(groups) = &r.source.installshield else {
        for (rel, data) in &files { write(rel, data)?; }
        return Ok(());
    };
    let find = |n: &str| files.iter().find(|(rel, _)| rel.eq_ignore_ascii_case(n)).map(|(_, d)| d);
    let hdr = find("data1.hdr").ok_or(format!("{}: [source.installshield], but the installer has no data1.hdr", src.display()))?;
    let vol = |n: u16| -> R<Vec<u8>> { find(&format!("data{n}.cab")).cloned().ok_or(format!("the installer has no data{n}.cab")) };
    let loose = |n: &str, size: usize| -> R<Vec<u8>> {
        // by name: the size the file table gives such a file is not always the size of the file the setup finds
        let m: Vec<_> = files.iter().filter(|(rel, _)| rel.rsplit('/').next().unwrap().eq_ignore_ascii_case(n)).collect();
        let exact: Vec<_> = m.iter().filter(|(_, d)| d.len() == size).collect();
        match (m.as_slice(), exact.as_slice()) {
            (_, [(_, d)]) | ([(_, d)], _) => Ok(d.clone()),
            ([], _) => Err(format!("{n}: a file the setup copies from beside itself, and the installer has none of that name")),
            _ => Err(format!("{n}: the installer has {} files of that name", m.len())),
        }
    };
    let is = crate::installers::installshield(hdr, &vol, &loose, &|g| groups.get(g).map(|t| t != "-").unwrap_or(true)).map_err(|e| format!("{}: InstallShield: {e}", src.display()))?;
    let mut unknown = std::collections::BTreeSet::new();
    for f in &is {
        let Some(to) = groups.get(&f.group) else { unknown.insert(f.group.clone()); continue };
        if to == "-" { continue; }
        let mut rel = to.replace('\\', "/");
        for part in [f.dir.as_str(), f.name.as_str()] { if part.is_empty() { continue; } if !rel.is_empty() { rel.push('/'); } rel.push_str(part); }
        let rel = rel.replace('\\', "/");
        if rel.split('/').any(|p| p.is_empty() || p == ".." || p == "." || p.contains(':')) { return Err(format!("InstallShield: {}: a path that leaves the title's folder", f.name)); }
        write(&rel, &f.data)?;
    }
    if !unknown.is_empty() { return Err(format!("{}: InstallShield file groups the recipe does not place: {} (say where each goes in [source.installshield] groups, \"-\" for none)", src.display(), unknown.into_iter().map(|g| format!("\"{g}\"")).collect::<Vec<_>>().join(", "))); }
    Ok(())
}
/// [[source.add]]: each file from its folder (--with NAME=<folder>, or $NAME), found without regard to case;
/// returns (where it goes on C:, its path here) and the material of the cache key (names and sha256s)
fn added(r: &Recipe, with: &[(String, PathBuf)]) -> R<(Vec<(String, PathBuf)>, String)> {
    let (mut out, mut key) = (Vec::new(), String::new());
    for a in &r.source.adds {
        let dir = with.iter().find(|(n, _)| *n == a.from).map(|x| x.1.clone()).or_else(|| std::env::var_os(&a.from).map(PathBuf::from))
            .ok_or(format!("[[source.add]] from = \"{0}\": say where its files are: pisco build --with {0}=<folder> (or the environment variable {0})", a.from))?;
        let to = recipe::drive_path(&a.to)?;
        let list: Vec<_> = io(std::fs::read_dir(&dir), &dir)?.filter_map(|e| e.ok()).collect();
        for f in &a.files {
            let p = list.iter().find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(f)).map(|e| e.path()).ok_or(format!("[[source.add]] {f}: not in {} ({})", dir.display(), a.from))?;
            key.push_str(&format!("{to}/{f} {}\n", oci::sha256(&io(std::fs::read(&p), &p)?)));
            out.push((format!("{to}/{f}"), p));
        }
    }
    Ok((out, key))
}
fn extract(r: &Recipe, src: &Path, sha: &str, cache: &Path, with: &[(String, PathBuf)]) -> R<Extracted> {
    let exe_guest = recipe::drive_path(&r.exe)?;
    let app_guest = match &r.source.app { Some(a) => recipe::drive_path(a)?, None => exe_guest.rsplit_once('/').map(|x| x.0.to_string()).unwrap_or_default() };
    // added files go into the extraction (the lifter takes the DLLs it finds beside the exe): its key says which
    let (adds, add_key) = added(r, with)?;
    let suffix = if adds.is_empty() { String::new() } else { format!("-add-{}", &oci::sha256(add_key.as_bytes())[..16]) };
    let dir = cache.join("extract").join(format!("{sha}-{}{suffix}", r.source.extract));
    let t = Instant::now();
    if dir.is_dir() { say(&format!("extract: cached ({})", dir.display())); } else {
        let tmp = tmp_name(&dir); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        match r.source.extract.as_str() {
            "innosetup" => {
                let ie = innoextract();
                let o = Command::new(&ie).args(["--extract", "--silent", "--output-dir"]).arg(&tmp).arg(src).output()
                    .map_err(|e| format!("{}: {e} (tools/setup-toolchain.sh innoextract)", ie.display()))?;
                if !o.status.success() { return Err(format!("innoextract failed: {}", String::from_utf8_lossy(&o.stderr))); }
            }
            "file" => {
                let name = exe_guest.rsplit('/').next().unwrap();
                io(std::fs::create_dir_all(tmp.join("app")), &tmp)?;
                io(std::fs::copy(src, tmp.join("app").join(name)), src)?;
            }
            "folder" => { // the folder's files go to the exe's folder ({app}), as they are
                if !src.is_dir() { return Err(format!("{}: extract = \"folder\" wants a folder", src.display())); }
                let (mut fs, mut ds) = (Vec::new(), Vec::new()); walk(src, "", &mut fs, &mut ds)?;
                for d in ds { io(std::fs::create_dir_all(tmp.join("app").join(&d)), &tmp)?; }
                io(std::fs::create_dir_all(tmp.join("app")), &tmp)?;
                for (rel, p) in fs { io(std::fs::copy(&p, tmp.join("app").join(&rel)), &p)?; }
            }
            "zip" => unzip(src, r.source.root.as_deref(), &tmp.join("app"))?,
            "wise" => unwise(r, src, &tmp)?,
            x => return Err(format!("[source] extract = \"{x}\": known are \"innosetup\", \"file\", \"folder\", \"zip\" and \"wise\"")),
        }
        for (guest, host) in &adds {
            let rel = if guest.eq_ignore_ascii_case(&app_guest) { "" } else { match guest.get(..app_guest.len() + 1) { Some(p) if p.eq_ignore_ascii_case(&format!("{app_guest}/")) => &guest[app_guest.len() + 1..], _ => return Err(format!("[[source.add]] {guest}: files are added in the title's folder ({app_guest}) or under it")) } };
            let dst = tmp.join("app").join(rel);
            if let Some(p) = dst.parent() { io(std::fs::create_dir_all(p), p)?; }
            io(std::fs::copy(host, &dst), host)?;
        }
        publish(&tmp, &dir)?;
        say(&format!("extract: {} with {} ({:.1} s)", src.display(), r.source.extract, t.elapsed().as_secs_f64()));
    }
    // innoextract's folders are the installer's constants ({app} -> app): each goes where the recipe says
    let (mut files, mut dirs) = (BTreeMap::new(), Vec::new());
    for e in io(std::fs::read_dir(&dir), &dir)?.filter_map(|e| e.ok()) {
        let top = e.file_name().to_string_lossy().into_owned();
        let guest = if top == "app" { app_guest.clone() } else {
            match r.source.map.get(&top) { Some(p) => recipe::drive_path(p)?, None => return Err(format!("the installer has files in {{{top}}}: say where they go in [source] map = {{ {top} = 'C:\\...' }}")) }
        };
        let (mut fs, mut ds) = (Vec::new(), Vec::new());
        walk(&e.path(), "", &mut fs, &mut ds)?;
        let join = |rel: &str| if guest.is_empty() { rel.to_string() } else if rel.is_empty() { guest.clone() } else { format!("{guest}/{rel}") };
        dirs.push(join("")); for d in ds { dirs.push(join(&d)); }
        for (rel, host) in fs { files.insert(join(&rel), host); }
    }
    for c in &r.source.cabs { uncab(c, &mut files, &mut dirs, &dir)?; }
    Ok(Extracted { files, dirs, app_guest, added: add_key.lines().map(String::from).collect() })
}
fn cabextract() -> PathBuf {
    if let Ok(p) = std::env::var("CABEXTRACT") { return p.into(); }
    let p = home().join("tools/cabextract/bin/cabextract");
    if p.is_file() { p } else { "cabextract".into() }
}
/// [[source.cab]]: files taken out of a cabinet the installer carries (cabextract), once per cabinet and list,
/// into a folder of their own beside the extraction's
fn uncab(c: &recipe::Cab, files: &mut BTreeMap<String, PathBuf>, dirs: &mut Vec<String>, extracted: &Path) -> R<()> {
    let cab_guest = recipe::drive_path(&c.cab)?;
    let to = recipe::drive_path(&c.to)?;
    let cab = files.iter().find(|(g, _)| g.eq_ignore_ascii_case(&cab_guest)).map(|x| x.1.clone()).ok_or(format!("[[source.cab]] {}: not among the installer's files", c.cab))?;
    let key = oci::sha256(format!("{cab_guest}\n{}\n", c.files.join("\n")).as_bytes());
    let mut name = extracted.as_os_str().to_owned(); name.push(format!("-cab-{}", &key[..16]));
    let dir = PathBuf::from(name);
    if !dir.is_dir() {
        let tmp = tmp_name(&dir); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        let ce = cabextract();
        for f in &c.files {
            let o = Command::new(&ce).args(["-q", "-F", f, "-d"]).arg(&tmp).arg(&cab).output()
                .map_err(|e| format!("{}: {e}: [[source.cab]] needs cabextract on this machine (tools/setup-toolchain.sh cabextract, or CABEXTRACT=<path>)", ce.display()))?;
            if !o.status.success() { return Err(format!("cabextract {f} from {}: {}", c.cab, String::from_utf8_lossy(&o.stderr).trim())); }
        }
        publish(&tmp, &dir)?;
        say(&format!("extract: {} from {}", c.files.join(" "), c.cab));
    }
    for f in &c.files {
        let host = io(std::fs::read_dir(&dir), &dir)?.filter_map(|e| e.ok()).find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(f)).map(|e| e.path())
            .ok_or(format!("[[source.cab]] {f}: not in {}", c.cab))?;
        let n = host.file_name().unwrap().to_string_lossy().into_owned();
        files.insert(if to.is_empty() { n } else { format!("{to}/{n}") }, host);
    }
    if !dirs.contains(&to) { dirs.push(to); }
    Ok(())
}
/// The extracted files of {app} against another copy of the same installation (a Wine prefix's)
fn compare(ex: &Extracted, other: &Path) -> R<()> {
    let (mut theirs, mut d) = (Vec::new(), Vec::new());
    walk(other, "", &mut theirs, &mut d)?;
    let ours: BTreeMap<String, &PathBuf> = ex.files.iter().filter_map(|(g, h)| g.strip_prefix(&format!("{}/", ex.app_guest)).map(|r| (r.to_string(), h))).collect();
    let (mut same, mut lines) = (0, Vec::new());
    for (rel, h) in &ours {
        match theirs.iter().find(|(r, _)| r == rel) {
            None => lines.push(format!("only in the installer: {rel}")),
            Some((_, p)) => { if io(std::fs::read(h), h)? == io(std::fs::read(p), p)? { same += 1 } else { lines.push(format!("differs: {rel} ({} bytes here, {} there)", h.metadata().map(|m| m.len()).unwrap_or(0), p.metadata().map(|m| m.len()).unwrap_or(0))) } }
        }
    }
    for (rel, _) in &theirs { if !ours.contains_key(rel) { lines.push(format!("only in {}: {rel}", other.display())); } }
    say(&format!("compare: {same} files equal to {}'s{}", other.display(), if lines.is_empty() { String::new() } else { format!("; {}", lines.len()) }));
    for l in lines { say(&format!("compare:   {l}")); }
    Ok(())
}

// ---- 2. lifting ----
pub struct Lifted { pub dir: PathBuf, pub bits: u32, pub low: u64, pub high: u64, pub linux: bool, pub os: String, pub imports: Vec<String>, pub key: String, pub objects: Vec<PathBuf> }
fn cflags(t: &Tools, cache: &Path, opt: &str, bits: u32, os: &str) -> Vec<String> {
    let linux = os == "linux";
    let mut f: Vec<String> = TARGET.iter().map(|s| s.to_string()).collect();
    f.extend(["-mllvm", "-wasm-enable-sjlj", opt, "-fno-strict-aliasing", "-Wno-everything"].map(String::from));
    f.push(format!("-I{}", t.root.join("rt").display()));
    f.push(format!("-DGUEST_BITS={bits}"));
    if linux { f.push("-DGUEST_LINUX=1".into()); }
    if os == "microvm" { f.push("-DGUEST_MACHINE=1".into()); f.push("-mtail-call".into()); }   // a machine's image: memory through its MMU (rt/cpu.h)   // the x87 at its full precision (rt/cpu.h fpx), in the lifted code and the runtime alike
    // no path of this machine in what is built: two checkouts give the same bytes
    f.push(format!("-ffile-prefix-map={}=/pisco-cache", cache.display()));
    f.push(format!("-ffile-prefix-map={}=/pisco", t.root.display()));
    f
}
/// What the lifter puts in the import table of the lifted C ("dll!name")
pub fn lifted_imports(title_c: &Path) -> R<Vec<String>> {
    let text = io(std::fs::read_to_string(title_c), title_c)?;
    let line = text.lines().find(|l| l.starts_with("const char*const import_names[]={")).ok_or("title.c has no import_names")?;
    let mut v: Vec<String> = line.split('"').skip(1).step_by(2).map(|s| s.to_string()).collect();
    v.sort(); v.dedup();
    Ok(v)
}
/// A path below `dir` ("main/gamex86.dll"), each part found without regard to case
fn find_ci(dir: &Path, rel: &str) -> Option<PathBuf> {
    let mut p = dir.to_path_buf();
    for part in rel.split('/').filter(|x| !x.is_empty()) {
        if part == ".." { return None; }
        let e = std::fs::read_dir(&p).ok()?.filter_map(|e| e.ok()).find(|e| e.file_name().to_string_lossy().eq_ignore_ascii_case(part))?;
        p = e.path();
    }
    Some(p)
}
fn lift(t: &Tools, r: &Recipe, exe: &Path, ex: &Extracted, cache: &Path, programs: &[PathBuf]) -> R<Lifted> {
    let dir = exe.parent().unwrap();
    // the images the lifter reads: the exe and every DLL beside it that is not builtin, and the DLLs the image puts in
    // C:\WINDOWS\system32 (a [[source.cab]]'s), which it may import too: the lifter looks in their folders
    let mut images: Vec<(String, PathBuf)> = io(std::fs::read_dir(dir), dir)?.filter_map(|e| e.ok()).map(|e| (e.file_name().to_string_lossy().to_ascii_lowercase(), e.path()))
        .filter(|(n, p)| p == exe || (n.ends_with(".dll") && !r.dll.contains_key(n))).collect();
    let mut sys_dirs: Vec<PathBuf> = Vec::new();
    for (g, h) in &ex.files {
        let Some((folder, name)) = g.rsplit_once('/') else { continue };
        if folder.eq_ignore_ascii_case("WINDOWS/system32") && name.to_ascii_lowercase().ends_with(".dll") {
            images.push((format!("c:/windows/system32/{}", name.to_ascii_lowercase()), h.clone()));
            if let Some(p) = h.parent() { if !sys_dirs.iter().any(|d| d == p) { sys_dirs.push(p.to_path_buf()); } }
        }
    }
    // [lifter] load: DLLs the title loads itself, by their path below the exe's folder
    let mut loads: Vec<String> = Vec::new();
    for l in &r.load {
        let rel = l.replace('\\', "/");
        let host = find_ci(dir, &rel).ok_or(format!("[lifter] load: {l}: not in the title's folder"))?;
        loads.extend(["--load".into(), format!("{}={rel}", host.display())]);
        images.push((rel.to_ascii_lowercase(), host));
    }
    for (k, p) in programs.iter().enumerate() { images.push((format!("program {k}"), p.clone())); }   // (a machine's: lifted into its module)
    images.sort(); images.dedup_by(|a, b| a.0 == b.0);
    let mut key = format!("lifter {}\n", t.lifter_hash);
    for (n, p) in &images { key.push_str(&format!("image {n} {}\n", oci::sha256(&io(std::fs::read(p), p)?))); }
    let mut args: Vec<String> = vec!["--split".into(), r.split.to_string()];
    for e in &r.entries { args.extend(["--entry".into(), format!("{e:x}")]); }
    let mut program_args: Vec<String> = Vec::new();   // not in the key as paths (they differ between machines): their bytes are
    for p in programs { program_args.extend(["--program".into(), p.display().to_string()]); }
    if let Some(b) = r.base_address { args.extend(["--base".into(), format!("{b:x}")]); }
    for d in r.dll.keys() { args.extend(["--builtin".into(), d.clone()]); }
    if std::env::var_os("PISCO_TRACE_PC").is_some() { args.push("--trace-pc".into()); }   // each block notes its address (debugging a trap)
    let mut dll_dirs: Vec<String> = Vec::new();   // not in the key: where the files are differs between machines
    for d in &sys_dirs { dll_dirs.extend(["--dll-dir".into(), format!("{}=c:/WINDOWS/system32", d.display())]); }
    key.push_str(&format!("options {}\n", args.join(" ")));
    for l in &r.load { key.push_str(&format!("load {l}\n")); }
    for h in ["rt/cpu.h", "rt/modules.h"] { key.push_str(&format!("{h} {}\n", oci::sha256(&io(std::fs::read(t.root.join(h)), &t.root.join(h))?))); }
    key.push_str(&format!("clang {}\nopt {}\n", t.clang_version, r.opt));
    let key = oci::sha256(key.as_bytes());
    let out = cache.join("lift").join(&key);
    let time = Instant::now();
    if out.join("ok").is_file() {
        say(&format!("lift: cached, lift key {}: not lifted or compiled again", &key[..16]));
    } else {
        let tmp = tmp_name(&out); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        let o = Command::new(&t.lifter).arg(exe).arg("title.c").args(&args).args(&dll_dirs).args(&loads).args(&program_args).current_dir(&tmp).output().map_err(|e| format!("lifter: {e}"))?;
        io(std::fs::write(tmp.join("lifter.log"), [&o.stdout[..], &o.stderr[..]].concat()), &tmp)?;
        if !o.status.success() { return Err(format!("the lifter failed: see {}", tmp.join("lifter.log").display())); }
        let lifted_at = time.elapsed().as_secs_f64();
        let (bits, _, _, os) = env_of(&tmp)?;
        let flags = cflags(t, cache, &r.opt, bits, &os);
        let mut jobs = Vec::new();
        for c in c_files(&tmp)? {
            let mut cmd = Command::new(&t.clang);
            cmd.args(&flags).arg("-ffreestanding").arg("-c").arg(&c).arg("-o").arg(format!("{c}.o")).current_dir(&tmp);
            jobs.push((c, cmd));
        }
        let n = jobs.len();
        run_all(jobs)?;
        io(std::fs::write(tmp.join("ok"), b""), &tmp)?;
        publish(&tmp, &out)?;
        say(&format!("lift: lifted in {lifted_at:.1} s, {n} files of C compiled in {:.1} s (lift key {})", time.elapsed().as_secs_f64() - lifted_at, &key[..16]));
    }
    let (bits, low, high, os) = env_of(&out)?;
    let objects = c_files(&out)?.into_iter().map(|c| out.join(format!("{c}.o"))).collect();
    Ok(Lifted { imports: lifted_imports(&out.join("title.c"))?, dir: out, bits, low, high, linux: os == "linux", os, key, objects })
}
fn c_files(dir: &Path) -> R<Vec<String>> {
    let mut v: Vec<String> = io(std::fs::read_dir(dir), dir)?.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| n.starts_with("title") && n.ends_with(".c")).collect();
    v.sort(); Ok(v)
}
fn env_of(dir: &Path) -> R<(u32, u64, u64, String)> {
    let text = io(std::fs::read_to_string(dir.join("title.env")), &dir.join("title.env"))?;
    let get = |k: &str| text.lines().find_map(|l| l.strip_prefix(&format!("{k}="))).map(|v| v.to_string()).unwrap_or_default();
    let hexv = |v: String| u64::from_str_radix(v.trim_start_matches("0x"), 16).unwrap_or(0);
    Ok((get("GUEST_BITS").parse().unwrap_or(64), hexv(get("IMAGE_LOW")), hexv(get("IMAGE_HIGH")), get("OS")))
}

// ---- 3. the runtime ----
fn rt_deps(root: &Path) -> R<String> {
    // everything under rt/ that is not a compilation unit of its own: headers, generated tables, included sources, fonts
    let (mut fs, mut ds) = (Vec::new(), Vec::new());
    walk(&root.join("rt"), "rt", &mut fs, &mut ds)?;
    let mut h = String::new();
    for (rel, p) in fs {
        let unit = rel.ends_with(".c") && rel.matches('/').count() <= 2; // a file of rt/ or of one runtime folder: a unit a base can list
        if !unit { h.push_str(&format!("{rel} {}\n", oci::sha256(&io(std::fs::read(&p), &p)?))); }
    }
    Ok(oci::sha256(h.as_bytes()))
}
fn compile_cached(t: &Tools, cache: &Path, kind: &str, items: Vec<(String, PathBuf, String)>, flags: &[String]) -> R<(Vec<PathBuf>, usize)> {
    // (what, source, key material) -> cache/<kind>/<key>.o
    io(std::fs::create_dir_all(cache.join(kind)), cache)?;
    let (mut objs, mut jobs) = (Vec::new(), Vec::new());
    for (what, src, material) in items {
        let key = oci::sha256(format!("{material}\nclang {}\nflags {}\n", t.clang_version, flags.join(" ")).as_bytes());
        let o = cache.join(kind).join(format!("{key}.o"));
        if !o.is_file() {
            let tmp = tmp_name(&o);
            let mut cmd = Command::new(&t.clang);
            cmd.args(flags).arg("-c").arg(&src).arg("-o").arg(&tmp);
            jobs.push((what, cmd, tmp, o.clone()));
        }
        objs.push(o);
    }
    let n = jobs.len();
    let mut renames = Vec::new();
    run_all(jobs.into_iter().map(|(w, c, tmp, o)| { renames.push((tmp, o)); (w, c) }).collect())?;
    for (tmp, o) in renames { io(std::fs::rename(&tmp, &o), &o)?; }
    Ok((objs, n))
}

// ---- the build ----
pub fn build(root: &Path, o: &Opts) -> R<()> {
    let start = Instant::now();
    let r = recipe::load_recipe(&o.recipe)?;
    let base = recipe::load_base(root, &r.from)?;
    for l in &r.libraries { if !base.libraries.contains_key(l) { return Err(format!("library {l} is not one of {}'s ({})", base.reference(), base.libraries.keys().cloned().collect::<Vec<_>>().join(", "))); } }
    let cache = o.cache.clone().unwrap_or(root.join("out/pisco-cache"));
    io(std::fs::create_dir_all(&cache), &cache)?;
    let cache = io(cache.canonicalize(), &cache)?;
    let out = o.out.clone().unwrap_or(root.join("out/images").join(&r.name));
    if out.exists() && !oci::is_image(&out) && io(std::fs::read_dir(&out), &out)?.next().is_some() { return Err(format!("{}: not an image, and not empty: not replaced", out.display())); }
    let t = tools(root)?;
    say(&format!("{}: FROM {}{}", r.name, base.reference(), if r.libraries.is_empty() { String::new() } else { format!(", libraries {}", r.libraries.join(" ")) }));

    // 1. the source, then its files
    let src = o.source.clone().unwrap_or(r.dir.join(&r.source.file));
    // a folder (extract = "folder", a title the user built or copied) is hashed as its list of files and their hashes
    let sha = if src.is_dir() {
        let (mut fs, mut ds) = (Vec::new(), Vec::new()); walk(&src, "", &mut fs, &mut ds)?;
        let mut h = String::new(); for (rel, p) in fs { h.push_str(&format!("{rel} {}\n", oci::sha256(&io(std::fs::read(&p), &p)?))); }
        oci::sha256(h.as_bytes())
    } else { let data = io(std::fs::read(&src), &src)?; oci::sha256(&data) };
    match &r.source.sha256 {
        Some(w) if *w != sha => return Err(format!("{}: sha256 {sha}, the recipe wants {w}: not the installer the recipe is for", src.display())),
        Some(_) => say(&format!("source: {} has the recipe's sha256", src.display())),
        None => say(&format!("source: the recipe gives no sha256 for {} (it has {sha}): not checked", src.display())),
    }
    let mut ex = extract(&r, &src, &sha, &cache, &o.with)?;
    if let Some(c) = &o.compare { compare(&ex, c)?; }
    // [files]: settings a setup step would write, put in the title's files (a file the installer has is replaced)
    for (place, text) in &r.files {
        let guest = recipe::drive_path(place)?;
        let host = cache.join("files").join(oci::sha256(text.as_bytes()));
        if !host.is_file() { if let Some(p) = host.parent() { io(std::fs::create_dir_all(p), p)?; } io(std::fs::write(&host, text.replace('\n', "\r\n")), &host)?; }
        if let Some(k) = ex.files.keys().find(|k| k.eq_ignore_ascii_case(&guest)).cloned() { ex.files.remove(&k); }
        ex.files.insert(guest, host);
    }
    let exe_guest = recipe::drive_path(&r.exe)?;
    let exe = ex.files.iter().find(|(g, _)| g.eq_ignore_ascii_case(&exe_guest)).map(|x| x.1.clone()).ok_or(format!("{}: not among the installer's files", r.exe))?;

    // 2. lifted: the exe, or every program of a Linux title (each ELF among its files: execve starts them; the same
    // program under two names is lifted once). A Linux title's exe may be a script: its first process starts in
    // any program's module and runs it (rt/linux-x64/start.c)
    let is_elf = |p: &Path| std::fs::File::open(p).ok().and_then(|mut f| { let mut b = [0u8; 4]; std::io::Read::read_exact(&mut f, &mut b).ok().map(|_| b == *b"\x7fELF") }).unwrap_or(false);
    let programs: Vec<(String, PathBuf)> = if base.os == "linux" { ex.files.iter().filter(|(_, h)| is_elf(h)).map(|(g, h)| (g.clone(), h.clone())).collect() } else { vec![(exe_guest.clone(), exe.clone())] };
    if programs.is_empty() { return Err(format!("{}: no program among the title's files", r.exe)); }
    let mut lifts: Vec<Lifted> = Vec::new(); let mut lift_of: BTreeMap<String, usize> = BTreeMap::new(); let mut by_sha: BTreeMap<String, usize> = BTreeMap::new();
    for (g, h) in &programs {
        let sha = oci::sha256(&io(std::fs::read(h), h)?);
        // a machine's programs (its kernel's initramfs, [lifter] programs): lifted into the kernel's module
        let with: Vec<PathBuf> = if base.os == "microvm" { r.programs.iter().map(|p| { let g = recipe::drive_path(p)?; ex.files.iter().find(|(f, _)| f.eq_ignore_ascii_case(&g)).map(|x| x.1.clone()).ok_or(format!("[lifter] programs: {p} is not among the title's files")) }).collect::<R<_>>()? } else { vec![] };
        let k = match by_sha.get(&sha) { Some(&k) => k, None => { if programs.len() > 1 { say(&format!("program /{g}")); } lifts.push(lift(&t, &r, h, &ex, &cache, &with)?); by_sha.insert(sha, lifts.len() - 1); lifts.len() - 1 } };
        lift_of.insert(g.clone(), k);
    }
    let main_lift = lift_of.get(&exe_guest).copied().unwrap_or(0);
    let lifted = &lifts[main_lift];
    // the imports against what the base and the libraries answer for
    let shipped: Vec<String> = ex.files.keys().filter(|g| g.rsplit_once('/').map(|x| x.0).unwrap_or("").eq_ignore_ascii_case(exe_guest.rsplit_once('/').map(|x| x.0).unwrap_or("")))
        .map(|g| g.rsplit('/').next().unwrap().to_ascii_lowercase()).filter(|n| n.ends_with(".dll") && !r.dll.contains_key(n)).collect();
    let cen = Census::new(root, &base, &r.libraries, &shipped)?;
    let report = cen.check(&lifted.imports);
    say(&format!("imports: {}", report.summary));
    if !report.needs.is_empty() { return Err(format!("the title imports from DLLs of libraries the recipe does not name: add {} to `libraries`", report.needs.join(", "))); }
    for l in &r.libraries { if !report.used.contains(l) { say(&format!("imports: library {l} is not used by the title's imports (it may still be loaded at run time)")); } }

    // 3. the runtime: the base's core and the libraries
    let t3 = Instant::now();
    let mut sources = base.sources.clone();
    for l in &r.libraries { sources.extend(base.libraries[l].sources.iter().cloned()); }
    sources.sort(); sources.dedup();   // (two libraries may share a file: the decoders of FMOD 3 and FMOD Ex)
    let deps = rt_deps(root)?;
    if lifted.linux != (base.os == "linux") || (lifted.os == "microvm") != (base.os == "microvm") {
        return Err(format!("{}: {} image, but {} is for {}", r.exe, match lifted.os.as_str() { "linux" => "a Linux program's", "microvm" => "a machine's (a kernel's)", _ => "a Windows" }, base.reference(), base.os));
    }
    let rt_flags = cflags(&t, &cache, &base.opt, lifted.bits, &lifted.os);
    let items = sources.iter().map(|s| { let p = root.join(s); let c = std::fs::read(&p).map(|d| oci::sha256(&d)).unwrap_or_default(); (s.clone(), p, format!("source {s} {c}\ndeps {deps}")) }).collect();
    let (rt_objs, rt_new) = compile_cached(&t, &cache, "rt", items, &rt_flags)?;
    // the import table and the recipe's CPU (registry values are not built in: they reach the runtime from the config)
    let gen = tmp_name(&cache.join("gen-work")); io(std::fs::create_dir_all(&gen), &gen)?;
    let st = Command::new(root.join("tools/gen-imports.sh")).arg(lifted.bits.to_string()).arg(&lifted.dir).arg(gen.join("imports.gen.c")).args(sources.iter().map(|s| root.join(s))).status().map_err(|e| e.to_string())?;
    if !st.success() { return Err("tools/gen-imports.sh failed".into()); }
    let features = r.cpu.clone().unwrap_or(base.cpu.clone());
    let mut cpu_toml = format!("[cpu]\nfeatures = [{}]\n", features.iter().map(|f| format!("\"{f}\"")).collect::<Vec<_>>().join(", "));
    if let Some(b) = r.heap_fill { cpu_toml.push_str(&format!("[quirks]\nheap_fill = {b}\n")); }
    io(std::fs::write(gen.join("cpu.toml"), &cpu_toml), &gen)?;
    let o2 = Command::new("python3").args(["-I", &root.join("tools/recipe-gen.py").to_string_lossy(), "cpu.toml"]).current_dir(&gen).output().map_err(|e| e.to_string())?;
    if !o2.status.success() { return Err(format!("tools/recipe-gen.py: {}", String::from_utf8_lossy(&o2.stderr))); }
    io(std::fs::write(gen.join("recipe.gen.c"), &o2.stdout), &gen)?;
    let mut gen_items = Vec::new();
    for f in ["imports.gen.c", "recipe.gen.c"] {
        let c = io(std::fs::read(gen.join(f)), &gen)?;
        // the file is compiled where it is kept: its name in the cache does not depend on this build's folder
        let kept = cache.join("gen").join(format!("{}.c", oci::sha256(&c)));
        io(std::fs::create_dir_all(cache.join("gen")), &cache)?;
        if !kept.is_file() { io(std::fs::write(&kept, &c), &kept)?; }
        gen_items.push((f.to_string(), kept, format!("generated {}", oci::sha256(&c))));
    }
    let _ = std::fs::remove_dir_all(&gen);
    let (gen_objs, _) = compile_cached(&t, &cache, "gen", gen_items, &rt_flags)?;
    say(&format!("runtime: {} sources, {rt_new} compiled, {} cached ({:.1} s)", sources.len(), sources.len() - rt_new, t3.elapsed().as_secs_f64()));

    // 4. the modules
    let link_module = |lifted: &Lifted| -> R<PathBuf> {
    let mut layout_flags = String::from("--stack-first,-z,stack-size=8388608");
    // (a Linux program that is not position-independent loads low too: 0x400000 unless its link said otherwise)
    if lifted.bits == 32 || (lifted.linux && lifted.low < 0x1000_0000) {
        if lifted.low < 0x100000 { return Err(format!("an image at 0x{:x} leaves no room for the runtime's stack below it", lifted.low)); }
        let stack = lifted.low.min(0x800000) & !0xFFFF;
        layout_flags = format!("--stack-first,-z,stack-size={stack},--global-base={}", (lifted.high + 0xFFFFF) & !0xFFFFF);
    }
    let mut link: Vec<String> = TARGET.iter().map(|s| s.to_string()).collect();
    link.push("-pthread".into());
    let objects: Vec<&PathBuf> = lifted.objects.iter().chain(&rt_objs).chain(&gen_objs).collect();
    let tail = ["-mllvm".to_string(), "-wasm-enable-sjlj".into(), "-lsetjmp".into(),
        format!("-Wl,--import-memory,--shared-memory,--initial-memory=67108864,--max-memory=4294967296,--export=wasi_thread_start,--export=rt_last_pc,{layout_flags}"), "-Wl,--export=__heap_base".into()];
    let mut key = format!("link 2 (as title.wasm)\nclang {}\nflags {} {}\n", t.clang_version, link.join(" "), tail.join(" "));
    for o in &objects { key.push_str(&format!("{}\n", oci::sha256(&io(std::fs::read(o), o)?))); }
    let key = oci::sha256(key.as_bytes());
    let wasm = cache.join("link").join(format!("{key}.wasm"));
    let t4 = Instant::now();
    if wasm.is_file() { say(&format!("link: cached ({})", &key[..16])); } else {
        io(std::fs::create_dir_all(cache.join("link")), &cache)?;
        // linked as "title.wasm" in a folder of its own: wasm-ld names the module after its output file (the name section)
        let tmp = tmp_name(&wasm); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        let o = Command::new(&t.clang).args(&link).args(&objects).arg("-o").arg(tmp.join("title.wasm")).args(&tail).output().map_err(|e| e.to_string())?;
        if !o.status.success() { return Err(format!("link failed: {}", String::from_utf8_lossy(&o.stderr).lines().take(12).collect::<Vec<_>>().join("\n"))); }
        io(std::fs::rename(tmp.join("title.wasm"), &wasm), &wasm)?;
        let _ = std::fs::remove_dir_all(&tmp);
        say(&format!("link: {} objects ({:.1} s)", objects.len(), t4.elapsed().as_secs_f64()));
    }
    Ok(wasm)
    };
    let wasms: Vec<PathBuf> = lifts.iter().map(|l| link_module(l)).collect::<R<_>>()?;
    let wasm = wasms[main_lift].clone();

    // 5. the image; with [shaders], written twice: once to run it and collect the shaders it compiles, then with them
    let write = |extra: Option<&Tree>| -> R<String> {
    let tmp_out = tmp_name(&out);
    let lay = Layout::create(&tmp_out)?;
    let mut layers = Vec::new();
    let base_tree = base_layer(root, &base, &cen)?;
    layers.push(lay.put(LAYER, &base_tree.tar(), &[(ROLE, "base"), (TITLE, &base.reference())])?);
    for l in &r.libraries {
        let mut tree = Tree::default();
        placeholders(&mut tree, &base, &cen.dlls_of(l), &format!("library {l} of {}", base.reference()));
        layers.push(lay.put(LAYER, &tree.tar(), &[(ROLE, "library"), (TITLE, &format!("{}/{l}", base.reference()))])?);
    }
    let mut game = Tree::default();
    for d in &ex.dirs { if !d.is_empty() { game.dir(d); } }
    for (g, h) in &ex.files { game.file(g, io(std::fs::read(h), h)?); }
    layers.push(lay.put(LAYER, &game.tar(), &[(ROLE, "game"), (TITLE, &r.name), (LOCAL, "true")])?);
    if let Some(t) = extra { layers.push(lay.put(LAYER, &t.tar(), &[(ROLE, "shaders"), (TITLE, &format!("{} shaders", r.name)), (LOCAL, "true")])?); }
    let module = lay.put(MODULE, &io(std::fs::read(&wasm), &wasm)?, &[(ROLE, "module"), (TITLE, "title.wasm"), (LOCAL, "true")])?;
    // a Linux title's other programs: a module each (the first module above is the one its first process starts in)
    let mut program_layers = Vec::new(); let mut digest_of: BTreeMap<usize, String> = BTreeMap::new();
    digest_of.insert(main_lift, format!("sha256:{}", module.digest));
    for (k, w) in wasms.iter().enumerate() {
        if k == main_lift { continue; }
        let name = lift_of.iter().find(|(_, &v)| v == k).map(|(g, _)| format!("/{g}")).unwrap_or_default();
        let b = lay.put(MODULE, &io(std::fs::read(w), w)?, &[(ROLE, "program"), (TITLE, &name), (LOCAL, "true")])?;
        digest_of.insert(k, format!("sha256:{}", b.digest)); program_layers.push(b);
    }
    let program_map: BTreeMap<String, String> = if base.os == "linux" { lift_of.iter().map(|(g, k)| (format!("/{g}"), digest_of[k].clone())).collect() } else { BTreeMap::new() };
    let registry = recipe::merge_registry(&base.registry, &r.registry);
    let config = json!({
        "architecture": base.architecture, "os": base.os, "base": base.reference(), "title": r.name,
        "run": { "exe": r.exe, "display": r.display, "args": r.args, "initrd": r.initrd, "cmdline": r.cmdline },
        "guest": { "bits": lifted.bits, "image": [format!("0x{:x}", lifted.low), format!("0x{:x}", lifted.high)] },
        "cpu": { "features": features },
        "dll": r.dll, "libraries": r.libraries,
        "registry": registry.iter().map(|v| match &v.data {
            RegData::Dword(n) => json!({ "key": v.key, "name": v.name, "type": "dword", "value": n }),
            RegData::Sz(s) => json!({ "key": v.key, "name": v.name, "type": "sz", "value": s }) }).collect::<Vec<_>>(),
        "imports": lifted.imports,
        "programs": if program_map.is_empty() { serde_json::Value::Null } else { json!(program_map) },
        "source": { "sha256": sha, "extract": r.source.extract, "added": ex.added },
        "build": { "lift": lifted.key, "lifter": t.lifter_hash, "clang": t.clang_version, "opt": r.opt, "runtime_opt": base.opt },
    });
    let cfg = lay.put_json(oci::CONFIG, &config, &[])?;
    let mut descs: Vec<_> = layers.iter().map(|b| b.descriptor()).collect();
    descs.push(module.descriptor());
    for b in &program_layers { descs.push(b.descriptor()); }
    let manifest = json!({ "schemaVersion": 2, "mediaType": oci::MANIFEST, "artifactType": oci::ARTIFACT, "config": cfg.descriptor(), "layers": descs,
        "annotations": { TITLE: r.name, "org.pisco.base": base.reference() } });
    let man = lay.put_json(oci::MANIFEST, &manifest, &[])?;
    lay.finish(&man, &r.name)?;
    if out.exists() { io(std::fs::remove_dir_all(&out), &out)?; }
    if let Some(p) = out.parent() { io(std::fs::create_dir_all(p), p)?; }
    io(std::fs::rename(&tmp_out, &out), &out)?;
    for (b, what) in layers.iter().zip(std::iter::repeat("layer")).chain([(&module, "module")]).chain(program_layers.iter().zip(std::iter::repeat("module"))).chain([(&cfg, "config")]) {
        say(&format!("  {what:6} {:8} sha256:{} {:>10} bytes  {}", b.annotations.get(ROLE).map(|s| s.as_str()).unwrap_or(""), b.digest, b.size, b.annotations.get(TITLE).map(|s| s.as_str()).unwrap_or("")));
    }
    Ok(man.digest.clone())
    };
    let digest = match &r.shaders {
        None => write(None)?,
        Some(sh) => {
            let first = write(None)?;
            let tree = shaders_layer(root, &r, sh, &out, &first, &ex.app_guest, &cache)?;
            write(Some(&tree))?
        }
    };
    say(&format!("image {}: manifest sha256:{} ({:.1} s)", out.display(), digest, start.elapsed().as_secs_f64()));
    println!("sha256:{digest}");
    Ok(())
}

/// [shaders]: the image run once in Node (SW_HLSL_CAPTURE: D3DCompile logs each source it has no shader for and
/// answers with a placeholder; SW_EXIT_FRAMES: the run ends after that many presents), the logged sources compiled
/// by tools/hlsl-prep.mjs (dxc, then tools/shader-prep.mjs: dxil2spv and naga) into <cache>/shaders/<key>, once per
/// image and session; returned as a layer: the files under the title's folder, `shaders/`
fn shaders_layer(root: &Path, r: &Recipe, sh: &recipe::Shaders, image: &Path, image_digest: &str, app_guest: &str, cache: &Path) -> R<Tree> {
    let session = sh.session.as_ref().map(|s| r.dir.join(s));
    let mut key = format!("image {image_digest}\nframes {}\n", sh.frames);
    if let Some(s) = &session { key.push_str(&format!("session {}\n", oci::sha256(&io(std::fs::read(s), s)?))); }
    for f in ["tools/hlsl-prep.mjs", "tools/shader-prep.mjs", "target/dxil2spv"] { let p = root.join(f); if let Ok(d) = std::fs::read(&p) { key.push_str(&format!("{f} {}\n", oci::sha256(&d))); } }
    let key = oci::sha256(key.as_bytes());
    let dir = cache.join("shaders").join(&key[..32]);
    let t = Instant::now();
    if dir.is_dir() { say(&format!("shaders: cached ({})", &key[..16])); } else {
        let tmp = tmp_name(&dir); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        let exe = std::env::current_exe().map_err(|e| e.to_string())?;
        let mut cmd = Command::new(exe);
        cmd.arg("run").arg(image).env("SW_HLSL_CAPTURE", "1").env("SW_EXIT_FRAMES", sh.frames.to_string()).env("SW_PACE", "off").env_remove("SW_STRICT");
        if let Some(s) = &session { cmd.arg("--session").arg(s); }
        let o = cmd.output().map_err(|e| format!("pisco run: {e}"))?;
        let log = tmp.join("capture.log");
        io(std::fs::write(&log, [&o.stdout[..], &o.stderr[..]].concat()), &log)?;
        let sources = String::from_utf8_lossy(&std::fs::read(&log).unwrap_or_default()).matches("d3dcompile-source ").count();
        if !o.status.success() && sources == 0 { return Err(format!("[shaders]: the run ended with {} and compiled nothing: see {}", o.status, log.display())); }
        let title = tmp.join("title");
        let o = Command::new("node").arg(root.join("tools/hlsl-prep.mjs")).arg(&log).arg("--title").arg(&title).output().map_err(|e| format!("node: {e}"))?;
        io(std::fs::write(tmp.join("prep.log"), [&o.stdout[..], &o.stderr[..]].concat()), &tmp)?;
        if !o.status.success() { return Err(format!("[shaders]: tools/hlsl-prep.mjs failed: see {}", tmp.join("prep.log").display())); }
        publish(&tmp, &dir)?;
        say(&format!("shaders: {sources} sources compiled and translated ({:.1} s)", t.elapsed().as_secs_f64()));
    }
    let (mut fs, mut ds) = (Vec::new(), Vec::new());
    let src = dir.join("title");
    if src.is_dir() { walk(&src, "", &mut fs, &mut ds)?; }
    let mut tree = Tree::default();
    let join = |rel: &str| if app_guest.is_empty() { rel.to_string() } else { format!("{app_guest}/{rel}") };
    for d in ds { tree.dir(&join(&d)); }
    for (rel, h) in fs { tree.file(&join(&rel), io(std::fs::read(&h), &h)?); }
    Ok(tree)
}

/// The base's layer: its folder, the folders it names, its fonts, a placeholder for each DLL of the core
fn base_layer(root: &Path, base: &Base, cen: &Census) -> R<Tree> {
    let mut tree = Tree::default();
    let (mut fs, mut ds) = (Vec::new(), Vec::new());
    if base.files_dir.is_dir() { walk(&base.files_dir, "", &mut fs, &mut ds)?; }
    for d in ds { tree.dir(&d); }
    for (g, h) in fs { tree.file(&g, io(std::fs::read(&h), &h)?); }
    for d in &base.dirs { tree.dir(&recipe::drive_path(&format!("C:\\{d}"))?); }
    for (g, src) in &base.fonts { let p = root.join(src); tree.file(&recipe::drive_path(&format!("C:\\{g}"))?, io(std::fs::read(&p), &p)?); }
    placeholders(&mut tree, base, &cen.dlls_of("base"), &base.reference());
    Ok(tree)
}
fn placeholders(tree: &mut Tree, base: &Base, dlls: &[String], from: &str) {
    let dir = recipe::drive_path(&format!("C:\\{}", base.system32)).unwrap_or_default();
    for d in dlls { tree.file(&format!("{dir}/{d}"), format!("pisco: {d} is built into the runtime ({from}); there is no code in this file.\r\n").into_bytes()); }
}

#[cfg(test)]
mod tests {
    use super::unzip;
    // a zip of (name, data, deflate?) built here: local headers, the central directory, its end record
    fn zip(entries: &[(&str, &[u8], bool)]) -> Vec<u8> {
        let (mut out, mut dir) = (Vec::new(), Vec::new());
        for (name, data, deflate) in entries {
            let body = if *deflate { miniz_oxide::deflate::compress_to_vec(data, 6) } else { data.to_vec() };
            let at = out.len() as u32;
            let h = |sig: u32, central: bool| { let mut v = sig.to_le_bytes().to_vec();
                if central { v.extend(20u16.to_le_bytes()); }
                v.extend(20u16.to_le_bytes()); v.extend(0u16.to_le_bytes()); v.extend((if *deflate { 8u16 } else { 0 }).to_le_bytes());
                v.extend([0u8; 8]); v.extend((body.len() as u32).to_le_bytes()); v.extend((data.len() as u32).to_le_bytes());
                v.extend((name.len() as u16).to_le_bytes()); v.extend(0u16.to_le_bytes());
                if central { v.extend([0u8; 10]); v.extend(at.to_le_bytes()); }
                v.extend(name.as_bytes()); v };
            out.extend(h(0x04034b50, false)); out.extend(&body);
            dir.extend(h(0x02014b50, true));
        }
        let (at, n) = (out.len() as u32, entries.len() as u16);
        out.extend(&dir);
        out.extend(0x06054b50u32.to_le_bytes()); out.extend([0u8; 4]); out.extend(n.to_le_bytes()); out.extend(n.to_le_bytes());
        out.extend((dir.len() as u32).to_le_bytes()); out.extend(at.to_le_bytes()); out.extend(0u16.to_le_bytes());
        out
    }
    fn tmp(name: &str) -> std::path::PathBuf { let d = std::env::temp_dir().join(format!("pisco-test-{name}-{}", std::process::id())); let _ = std::fs::remove_dir_all(&d); std::fs::create_dir_all(&d).unwrap(); d }
    #[test]
    fn zip_stored_deflated_and_root() {
        let d = tmp("zip");
        let text = b"maze maze maze maze maze maze".repeat(20);
        std::fs::write(d.join("a.zip"), zip(&[("Game/", b"", false), ("Game/Game.exe", b"MZ", false), ("Game/Assets/map.txt", &text, true), ("other.txt", b"x", false)])).unwrap();
        unzip(&d.join("a.zip"), Some("Game"), &d.join("app")).unwrap();
        assert_eq!(std::fs::read(d.join("app/Game.exe")).unwrap(), b"MZ");
        assert_eq!(std::fs::read(d.join("app/Assets/map.txt")).unwrap(), text);
        assert!(!d.join("app/other.txt").exists());
        std::fs::remove_dir_all(&d).unwrap();
    }
    #[test]
    fn zip_refuses_leaving_the_folder() {
        let d = tmp("zipslip");
        std::fs::write(d.join("b.zip"), zip(&[("../evil.txt", b"x", false)])).unwrap();
        assert!(unzip(&d.join("b.zip"), None, &d.join("app")).unwrap_err().contains("leaves the folder"));
        assert!(!d.join("evil.txt").exists());
        std::fs::remove_dir_all(&d).unwrap();
    }
}
