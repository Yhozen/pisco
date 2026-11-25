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

pub struct Opts { pub recipe: PathBuf, pub source: Option<PathBuf>, pub out: Option<PathBuf>, pub cache: Option<PathBuf>, pub compare: Option<PathBuf> }

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
pub struct Extracted { pub files: BTreeMap<String, PathBuf>, pub dirs: Vec<String>, pub app_guest: String }
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
fn extract(r: &Recipe, src: &Path, sha: &str, cache: &Path) -> R<Extracted> {
    let exe_guest = recipe::drive_path(&r.exe)?;
    let app_guest = match &r.source.app { Some(a) => recipe::drive_path(a)?, None => exe_guest.rsplit_once('/').map(|x| x.0.to_string()).unwrap_or_default() };
    let dir = cache.join("extract").join(format!("{sha}-{}", r.source.extract));
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
            x => return Err(format!("[source] extract = \"{x}\": known are \"innosetup\", \"file\" and \"folder\"")),
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
    Ok(Extracted { files, dirs, app_guest })
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
pub struct Lifted { pub dir: PathBuf, pub bits: u32, pub low: u64, pub high: u64, pub imports: Vec<String>, pub key: String, pub objects: Vec<PathBuf> }
fn cflags(t: &Tools, cache: &Path, opt: &str, bits: u32) -> Vec<String> {
    let mut f: Vec<String> = TARGET.iter().map(|s| s.to_string()).collect();
    f.extend(["-mllvm", "-wasm-enable-sjlj", opt, "-fno-strict-aliasing", "-Wno-everything"].map(String::from));
    f.push(format!("-I{}", t.root.join("rt").display()));
    f.push(format!("-DGUEST_BITS={bits}"));
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
fn lift(t: &Tools, r: &Recipe, exe: &Path, ex: &Extracted, cache: &Path) -> R<Lifted> {
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
    images.sort();
    let mut key = format!("lifter {}\n", t.lifter_hash);
    for (n, p) in &images { key.push_str(&format!("image {n} {}\n", oci::sha256(&io(std::fs::read(p), p)?))); }
    let mut args: Vec<String> = vec!["--split".into(), r.split.to_string()];
    for e in &r.entries { args.extend(["--entry".into(), format!("{e:x}")]); }
    if let Some(b) = r.base_address { args.extend(["--base".into(), format!("{b:x}")]); }
    for d in r.dll.keys() { args.extend(["--builtin".into(), d.clone()]); }
    if std::env::var_os("PISCO_TRACE_PC").is_some() { args.push("--trace-pc".into()); }   // each block notes its address (debugging a trap)
    let mut dll_dirs: Vec<String> = Vec::new();   // not in the key: where the files are differs between machines
    for d in &sys_dirs { dll_dirs.extend(["--dll-dir".into(), format!("{}=c:/WINDOWS/system32", d.display())]); }
    key.push_str(&format!("options {}\n", args.join(" ")));
    for h in ["rt/cpu.h", "rt/modules.h"] { key.push_str(&format!("{h} {}\n", oci::sha256(&io(std::fs::read(t.root.join(h)), &t.root.join(h))?))); }
    key.push_str(&format!("clang {}\nopt {}\n", t.clang_version, r.opt));
    let key = oci::sha256(key.as_bytes());
    let out = cache.join("lift").join(&key);
    let time = Instant::now();
    if out.join("ok").is_file() {
        say(&format!("lift: cached, lift key {}: not lifted or compiled again", &key[..16]));
    } else {
        let tmp = tmp_name(&out); let _ = std::fs::remove_dir_all(&tmp); io(std::fs::create_dir_all(&tmp), &tmp)?;
        let o = Command::new(&t.lifter).arg(exe).arg("title.c").args(&args).args(&dll_dirs).current_dir(&tmp).output().map_err(|e| format!("lifter: {e}"))?;
        io(std::fs::write(tmp.join("lifter.log"), [&o.stdout[..], &o.stderr[..]].concat()), &tmp)?;
        if !o.status.success() { return Err(format!("the lifter failed: see {}", tmp.join("lifter.log").display())); }
        let lifted_at = time.elapsed().as_secs_f64();
        let bits = env_of(&tmp)?.0;
        let flags = cflags(t, cache, &r.opt, bits);
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
    let (bits, low, high) = env_of(&out)?;
    let objects = c_files(&out)?.into_iter().map(|c| out.join(format!("{c}.o"))).collect();
    Ok(Lifted { imports: lifted_imports(&out.join("title.c"))?, dir: out, bits, low, high, key, objects })
}
fn c_files(dir: &Path) -> R<Vec<String>> {
    let mut v: Vec<String> = io(std::fs::read_dir(dir), dir)?.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| n.starts_with("title") && n.ends_with(".c")).collect();
    v.sort(); Ok(v)
}
fn env_of(dir: &Path) -> R<(u32, u64, u64)> {
    let text = io(std::fs::read_to_string(dir.join("title.env")), &dir.join("title.env"))?;
    let get = |k: &str| text.lines().find_map(|l| l.strip_prefix(&format!("{k}="))).map(|v| v.to_string()).unwrap_or_default();
    let hexv = |v: String| u64::from_str_radix(v.trim_start_matches("0x"), 16).unwrap_or(0);
    Ok((get("GUEST_BITS").parse().unwrap_or(64), hexv(get("IMAGE_LOW")), hexv(get("IMAGE_HIGH"))))
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
    let ex = extract(&r, &src, &sha, &cache)?;
    if let Some(c) = &o.compare { compare(&ex, c)?; }
    let exe_guest = recipe::drive_path(&r.exe)?;
    let exe = ex.files.iter().find(|(g, _)| g.eq_ignore_ascii_case(&exe_guest)).map(|x| x.1.clone()).ok_or(format!("{}: not among the installer's files", r.exe))?;

    // 2. lifted
    let lifted = lift(&t, &r, &exe, &ex, &cache)?;
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
    let rt_flags = cflags(&t, &cache, &base.opt, lifted.bits);
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

    // 4. the module
    let mut layout_flags = String::from("--stack-first,-z,stack-size=8388608");
    if lifted.bits == 32 {
        if lifted.low < 0x100000 { return Err(format!("a 32-bit image at 0x{:x} leaves no room for the runtime's stack below it", lifted.low)); }
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

    // 5. the image
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
    let module = lay.put(MODULE, &io(std::fs::read(&wasm), &wasm)?, &[(ROLE, "module"), (TITLE, "title.wasm"), (LOCAL, "true")])?;
    let registry = recipe::merge_registry(&base.registry, &r.registry);
    let config = json!({
        "architecture": base.architecture, "os": base.os, "base": base.reference(), "title": r.name,
        "run": { "exe": r.exe, "display": r.display, "args": r.args },
        "guest": { "bits": lifted.bits, "image": [format!("0x{:x}", lifted.low), format!("0x{:x}", lifted.high)] },
        "cpu": { "features": features },
        "dll": r.dll, "libraries": r.libraries,
        "registry": registry.iter().map(|v| match &v.data {
            RegData::Dword(n) => json!({ "key": v.key, "name": v.name, "type": "dword", "value": n }),
            RegData::Sz(s) => json!({ "key": v.key, "name": v.name, "type": "sz", "value": s }) }).collect::<Vec<_>>(),
        "imports": lifted.imports,
        "source": { "sha256": sha, "extract": r.source.extract },
        "build": { "lift": lifted.key, "lifter": t.lifter_hash, "clang": t.clang_version, "opt": r.opt, "runtime_opt": base.opt },
    });
    let cfg = lay.put_json(oci::CONFIG, &config, &[])?;
    let mut descs: Vec<_> = layers.iter().map(|b| b.descriptor()).collect();
    descs.push(module.descriptor());
    let manifest = json!({ "schemaVersion": 2, "mediaType": oci::MANIFEST, "artifactType": oci::ARTIFACT, "config": cfg.descriptor(), "layers": descs,
        "annotations": { TITLE: r.name, "org.pisco.base": base.reference() } });
    let man = lay.put_json(oci::MANIFEST, &manifest, &[])?;
    lay.finish(&man, &r.name)?;
    if out.exists() { io(std::fs::remove_dir_all(&out), &out)?; }
    if let Some(p) = out.parent() { io(std::fs::create_dir_all(p), p)?; }
    io(std::fs::rename(&tmp_out, &out), &out)?;
    for (b, what) in layers.iter().zip(std::iter::repeat("layer")).chain([(&module, "module"), (&cfg, "config")]) {
        say(&format!("  {what:6} {:8} sha256:{} {:>10} bytes  {}", b.annotations.get(ROLE).map(|s| s.as_str()).unwrap_or(""), b.digest, b.size, b.annotations.get(TITLE).map(|s| s.as_str()).unwrap_or("")));
    }
    say(&format!("image {}: manifest sha256:{} ({:.1} s)", out.display(), man.digest, start.elapsed().as_secs_f64()));
    println!("sha256:{}", man.digest);
    Ok(())
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
