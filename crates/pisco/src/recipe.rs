// SPDX-License-Identifier: GPL-2.0-or-later
// Recipes (recipes/<title>/title.toml) and the bases they start FROM (bases/<name>/<version>/base.toml).
// docs/images.md has the reference of both.
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use toml::{Table, Value};

pub type R<T> = Result<T, String>;

#[derive(Clone, Debug, PartialEq)]
pub enum RegData { Dword(u32), Sz(String) }
#[derive(Clone, Debug)]
pub struct RegValue { pub key: String, pub name: String, pub data: RegData }

pub struct Source { pub file: String, pub sha256: Option<String>, pub extract: String, pub app: Option<String>, pub map: BTreeMap<String, String>, pub cabs: Vec<Cab>,
    pub root: Option<String>, pub adds: Vec<Add>,
    /// [source.installshield] groups: where each file group of an InstallShield 5 / 6 setup inside the installer
    /// goes, below the title's folder ("" the folder itself, "-" not installed); InstallShield's own script says
    /// it, and is not read
    pub installshield: Option<BTreeMap<String, String>> }
/// [[source.add]]: further files of the user's that the title needs and its download does not carry (Microsoft's
/// C++ runtime DLLs beside a title built with /MD), taken from a folder named on the command line
/// (`pisco build --with NAME=<folder>`) or by the environment variable NAME, put in a folder of C:
pub struct Add { pub from: String, pub files: Vec<String>, pub to: String }
/// [[source.cab]]: files taken out of a cabinet among the installer's files (cabextract), put in a folder of C:
pub struct Cab { pub cab: String, pub files: Vec<String>, pub to: String }
pub struct Recipe {
    pub dir: PathBuf, pub name: String, pub from: String, pub libraries: Vec<String>, pub source: Source,
    pub exe: String, pub display: Option<String>, pub args: Vec<String>,
    /// a machine's title (base/microvm-x64): [run] initrd (its initramfs, on the drive) and cmdline (the kernel's)
    pub initrd: Option<String>, pub cmdline: Option<String>,
    /// a machine's title: [lifter] programs, lifted with the kernel at their own addresses (lifter --program)
    pub programs: Vec<String>,
    pub cpu: Option<Vec<String>>, pub dll: BTreeMap<String, String>, pub registry: Vec<RegValue>,
    pub split: usize, pub entries: Vec<u64>, pub base_address: Option<u64>, pub opt: String,
    /// [lifter] load: DLLs the title loads itself (LoadLibrary) from its folder or below it, lifted with it
    pub load: Vec<String>,
    /// [files]: small text files a setup step would write (settings), by their place on C:
    pub files: BTreeMap<String, String>,
    pub heap_fill: Option<u8>, // [quirks] heap_fill: the byte a heap block the title did not ask to be zeroed holds
    pub shaders: Option<Shaders>,
}
/// [shaders]: the HLSL a title compiles at run time (D3DCompile), compiled when the image is built: the image is run
/// once in Node for `frames` frames (with the `session`, if one is given), the sources it logged are compiled with
/// dxc and translated, and the results become a layer of the image (docs/images.md)
pub struct Shaders { pub frames: u64, pub session: Option<String> }
pub struct Library { pub description: String, pub sources: Vec<String> }
pub struct Base {
    pub dir: PathBuf, pub name: String, pub version: i64, pub architecture: String, pub os: String,
    pub cpu: Vec<String>, pub sources: Vec<String>, pub opt: String, pub registry: Vec<RegValue>,
    pub files_dir: PathBuf, pub dirs: Vec<String>, pub fonts: BTreeMap<String, String>, pub system32: String,
    pub libraries: BTreeMap<String, Library>,
}
impl Base { pub fn reference(&self) -> String { format!("{}:{}", self.name, self.version) } }

fn load(path: &Path) -> R<Table> {
    let text = std::fs::read_to_string(path).map_err(|e| format!("{}: {e}", path.display()))?;
    text.parse::<Table>().map_err(|e| format!("{}: {e}", path.display()))
}
fn s(t: &Table, k: &str) -> Option<String> { t.get(k).and_then(|v| v.as_str()).map(|s| s.to_string()) }
fn strs(t: &Table, k: &str) -> R<Option<Vec<String>>> {
    match t.get(k) {
        None => Ok(None),
        Some(Value::Array(a)) => a.iter().map(|v| v.as_str().map(|s| s.to_string()).ok_or(format!("{k}: a list of strings"))).collect::<R<Vec<_>>>().map(Some),
        Some(_) => Err(format!("{k}: a list of strings")),
    }
}
fn table<'a>(t: &'a Table, k: &str) -> R<Option<&'a Table>> {
    match t.get(k) { None => Ok(None), Some(Value::Table(x)) => Ok(Some(x)), Some(_) => Err(format!("[{k}] must be a table")) }
}
fn int(v: &Value, what: &str) -> R<u64> {
    match v { Value::Integer(i) if *i >= 0 => Ok(*i as u64), Value::String(x) => u64::from_str_radix(x.trim_start_matches("0x"), 16).map_err(|_| format!("{what}: {x} is not hex")), _ => Err(format!("{what}: a number")) }
}

/// A registry table: 'HKLM\Key' = { Name = 1, Other = "text" }, roots written short or long
pub fn registry(t: Option<&Table>) -> R<Vec<RegValue>> {
    let mut out = Vec::new();
    for (key, values) in t.into_iter().flatten() {
        let (root, rest) = key.split_once('\\').unwrap_or((key.as_str(), ""));
        let root = match root.to_ascii_uppercase().as_str() {
            "HKLM" | "HKEY_LOCAL_MACHINE" => "HKEY_LOCAL_MACHINE", "HKCU" | "HKEY_CURRENT_USER" => "HKEY_CURRENT_USER",
            "HKCR" | "HKEY_CLASSES_ROOT" => "HKEY_CLASSES_ROOT", "HKU" | "HKEY_USERS" => "HKEY_USERS",
            r => return Err(format!("registry: {r} is not a root key (HKLM, HKCU, HKCR, HKU)")),
        };
        let full = if rest.is_empty() { root.to_string() } else { format!("{root}\\{}", rest.trim_matches('\\')) };
        let Value::Table(vals) = values else { return Err(format!("registry: '{key}' = {{ Name = value, ... }}")) };
        for (name, v) in vals {
            let data = match v {
                Value::Integer(i) if (0..=u32::MAX as i64).contains(i) => RegData::Dword(*i as u32),
                Value::String(x) => RegData::Sz(x.clone()),
                _ => return Err(format!("registry: {key}\\{name}: a number (REG_DWORD, 0 to 4294967295) or a string (REG_SZ)")),
            };
            if full.contains('\t') || name.contains('\t') || matches!(&data, RegData::Sz(x) if x.contains('\n') || x.contains('\t')) { return Err(format!("registry: {key}\\{name}: no tabs or line breaks")); }
            out.push(RegValue { key: full.clone(), name: name.clone(), data });
        }
    }
    Ok(out)
}
/// Values of `over` replace those of `base` with the same key and name (without regard to case); sorted
pub fn merge_registry(base: &[RegValue], over: &[RegValue]) -> Vec<RegValue> {
    let mut m: BTreeMap<(String, String), RegValue> = BTreeMap::new();
    for v in base.iter().chain(over) { m.insert((v.key.to_ascii_lowercase(), v.name.to_ascii_lowercase()), v.clone()); }
    m.into_values().collect()
}

/// 'C:\Program Files\X' -> "Program Files/X"; a Linux title's '/usr/bin/x' -> "usr/bin/x" (its root is the drive)
pub fn drive_path(p: &str) -> R<String> {
    if p.starts_with('/') { return Ok(p.split('/').filter(|s| !s.is_empty() && *s != ".").collect::<Vec<_>>().join("/")); }
    let rest = p.strip_prefix("C:\\").or_else(|| p.strip_prefix("c:\\")).ok_or(format!("{p}: a path on C:\\ (or, for a Linux title, from /) is expected"))?;
    Ok(rest.split('\\').filter(|s| !s.is_empty()).collect::<Vec<_>>().join("/"))
}

pub fn load_recipe(dir: &Path) -> R<Recipe> {
    let path = if dir.is_dir() { dir.join("title.toml") } else { dir.to_path_buf() };
    let t = load(&path)?;
    let dir = path.parent().unwrap().to_path_buf();
    let known = ["name", "from", "libraries", "source", "run", "cpu", "dll", "registry", "lifter", "build", "quirks", "shaders", "files"];
    if let Some(k) = t.keys().find(|k| !known.contains(&k.as_str())) { return Err(format!("{}: {k} is not a recipe key ({})", path.display(), known.join(", "))); }
    let src = table(&t, "source")?.ok_or(format!("{}: no [source]", path.display()))?;
    let run = table(&t, "run")?.ok_or(format!("{}: no [run]", path.display()))?;
    let lifter = table(&t, "lifter")?.cloned().unwrap_or_default();
    let build = table(&t, "build")?.cloned().unwrap_or_default();
    let mut map = BTreeMap::new();
    for (k, v) in table(src, "map")?.into_iter().flatten() { map.insert(k.clone(), v.as_str().ok_or("source.map: strings")?.to_string()); }
    let mut cabs = Vec::new();
    match src.get("cab") {
        None => {}
        Some(Value::Array(a)) => for c in a {
            let c = c.as_table().ok_or("[[source.cab]]: tables")?;
            cabs.push(Cab { cab: s(c, "cab").ok_or("[[source.cab]] cab = 'C:\\...\\x.cab'")?, files: strs(c, "files")?.filter(|f| !f.is_empty()).ok_or("[[source.cab]] files = [\"name.dll\"]")?,
                to: s(c, "to").ok_or("[[source.cab]] to = 'C:\\WINDOWS\\system32'")? });
        },
        Some(_) => return Err("[[source.cab]]: an array of tables".into()),
    }
    let mut adds = Vec::new();
    match src.get("add") {
        None => {}
        Some(Value::Array(a)) => for c in a {
            let c = c.as_table().ok_or("[[source.add]]: tables")?;
            adds.push(Add { from: s(c, "from").ok_or("[[source.add]] from = \"NAME\" (pisco build --with NAME=<folder>, or the environment variable NAME)")?,
                files: strs(c, "files")?.filter(|f| !f.is_empty()).ok_or("[[source.add]] files = [\"name.dll\"]")?,
                to: s(c, "to").ok_or("[[source.add]] to = 'C:\\...'")? });
        },
        Some(_) => return Err("[[source.add]]: an array of tables".into()),
    }
    let installshield = match table(src, "installshield")? {
        None => None,
        Some(is) => {
            let mut g = BTreeMap::new();
            for (k, v) in table(is, "groups")?.ok_or("[source.installshield] groups = { \"group\" = 'folder', ... }")? { g.insert(k.clone(), v.as_str().ok_or("[source.installshield] groups: strings")?.to_string()); }
            Some(g)
        }
    };
    let mut dll = BTreeMap::new();
    for (k, v) in table(&t, "dll")?.into_iter().flatten() {
        let how = v.as_str().ok_or("[dll]: \"name.dll\" = \"builtin\"")?;
        if how != "builtin" { return Err(format!("[dll] {k} = {how}: only \"builtin\" is known (the runtime answers for the DLL)")); }
        dll.insert(k.to_ascii_lowercase(), how.to_string());
    }
    Ok(Recipe {
        name: s(&t, "name").ok_or("recipe: no name")?,
        from: s(&t, "from").ok_or("recipe: no from (a base, e.g. \"base/windows-x86:1\")")?,
        libraries: strs(&t, "libraries")?.unwrap_or_default(),
        source: Source { file: s(src, "file").ok_or("[source] file")?, sha256: s(src, "sha256").map(|x| x.to_ascii_lowercase()), extract: s(src, "extract").unwrap_or("file".into()), app: s(src, "app"), map, cabs,
            root: s(src, "root").map(|r| r.trim_matches('/').to_string()), adds, installshield },
        exe: s(run, "exe").ok_or("[run] exe")?,
        display: s(run, "display"),
        args: strs(run, "args")?.unwrap_or_default(),
        initrd: s(run, "initrd"), cmdline: s(run, "cmdline"),
        programs: strs(&lifter, "programs")?.unwrap_or_default(),
        cpu: match table(&t, "cpu")? { Some(c) => strs(c, "features")?, None => None },
        dll,
        registry: registry(table(&t, "registry")?)?,
        files: match table(&t, "files")? { None => BTreeMap::new(), Some(f) => f.iter().map(|(k, v)| v.as_str().map(|x| (k.clone(), x.to_string())).ok_or(format!("[files] {k}: a string"))).collect::<R<_>>()? },
        split: lifter.get("split").map(|v| v.as_integer().filter(|n| *n >= 1).ok_or("lifter.split: a number from 1")).transpose()?.unwrap_or(8) as usize,
        entries: match lifter.get("entries") { Some(Value::Array(a)) => a.iter().map(|v| int(v, "lifter.entries")).collect::<R<_>>()?, _ => vec![] },
        load: strs(&lifter, "load")?.unwrap_or_default(),
        base_address: lifter.get("base").map(|v| int(v, "lifter.base")).transpose()?,
        opt: s(&build, "opt").unwrap_or("-O2".into()),
        heap_fill: match table(&t, "quirks")?.and_then(|q| q.get("heap_fill")) {
            None => None,
            Some(v) => Some(v.as_integer().filter(|n| (0..=255).contains(n)).ok_or("quirks.heap_fill: a byte, 0 to 255")? as u8),
        },
        shaders: match table(&t, "shaders")? {
            None => None,
            Some(sh) => Some(Shaders { frames: sh.get("frames").and_then(|v| v.as_integer()).filter(|n| *n > 0).ok_or("[shaders] frames = <presents to run for>")? as u64, session: s(sh, "session") }),
        },
        dir,
    })
}

/// "base/windows-x86:1" -> <root>/bases/windows-x86/1/base.toml
pub fn load_base(root: &Path, from: &str) -> R<Base> {
    let (name, version) = from.split_once(':').ok_or(format!("from = \"{from}\": <base>:<version>"))?;
    let short = name.strip_prefix("base/").ok_or(format!("from = \"{from}\": bases are named base/<name>"))?;
    let dir = root.join("bases").join(short).join(version);
    let t = load(&dir.join("base.toml"))?;
    let rt = table(&t, "runtime")?.ok_or("base: no [runtime]")?;
    let files = table(&t, "files")?.cloned().unwrap_or_default();
    let exclude = strs(rt, "exclude")?.unwrap_or_default();
    let mut libraries = BTreeMap::new();
    for (n, l) in table(&t, "libraries")?.into_iter().flatten() {
        let l = l.as_table().ok_or("[libraries.<name>] tables")?;
        let mut srcs = Vec::new();
        for g in strs(l, "sources")?.unwrap_or_default() { srcs.extend(glob(root, &g)?); }
        libraries.insert(n.clone(), Library { description: s(l, "description").unwrap_or_default(), sources: srcs });
    }
    let mut sources = Vec::new();
    for g in strs(rt, "sources")?.unwrap_or_default() { sources.extend(glob(root, &g)?); }
    sources.retain(|f| !exclude.contains(f) && !libraries.values().any(|l| l.sources.contains(f)));
    sources.sort(); sources.dedup();
    let mut fonts = BTreeMap::new();
    for (k, v) in table(&files, "fonts")?.into_iter().flatten() { fonts.insert(k.clone(), v.as_str().ok_or("files.fonts: strings")?.to_string()); }
    let b = Base {
        name: s(&t, "name").ok_or("base: no name")?, version: t.get("version").and_then(|v| v.as_integer()).ok_or("base: no version")?,
        architecture: s(&t, "architecture").unwrap_or("386".into()), os: s(&t, "os").unwrap_or("windows".into()),
        cpu: table(&t, "cpu")?.map(|c| strs(c, "features")).transpose()?.flatten().unwrap_or_default(),
        sources, opt: s(rt, "opt").unwrap_or("-O2".into()), registry: registry(table(&t, "registry")?)?,
        files_dir: dir.join(s(&files, "dir").unwrap_or("files".into())), dirs: strs(&files, "dirs")?.unwrap_or_default(), fonts,
        system32: s(&files, "system32").unwrap_or("WINDOWS\\system32".into()), libraries, dir,
    };
    if b.reference() != from { return Err(format!("{}: is {}, not {from}", b.dir.display(), b.reference())); }
    Ok(b)
}

/// "rt/windows-x86/d3d9*.c" -> the files it names, sorted (a `*` matches within one file name)
pub fn glob(root: &Path, pattern: &str) -> R<Vec<String>> {
    let (dir, pat) = pattern.rsplit_once('/').unwrap_or((".", pattern));
    let mut out = Vec::new();
    for e in std::fs::read_dir(root.join(dir)).map_err(|e| format!("{dir}: {e}"))? {
        let n = e.map_err(|e| e.to_string())?.file_name().to_string_lossy().into_owned();
        if wild(pat.as_bytes(), n.as_bytes()) && root.join(dir).join(&n).is_file() { out.push(format!("{dir}/{n}")); }
    }
    out.sort();
    if out.is_empty() { return Err(format!("{pattern}: no such files")); }
    Ok(out)
}
fn wild(p: &[u8], s: &[u8]) -> bool {
    match p.split_first() {
        None => s.is_empty(),
        Some((b'*', rest)) => (0..=s.len()).any(|i| wild(rest, &s[i..])),
        Some((c, rest)) => s.first() == Some(c) && wild(rest, &s[1..]),
    }
}

/// Every IMP(dll, name) in the files: "dll.dll!name"
pub fn implemented(root: &Path, files: &[String]) -> R<Vec<String>> {
    let mut out = Vec::new();
    for f in files {
        let text = std::fs::read_to_string(root.join(f)).map_err(|e| format!("{f}: {e}"))?;
        let mut rest = text.as_str();
        while let Some(i) = rest.find("IMP(") {
            rest = &rest[i + 4..];
            let end = rest.find(')').unwrap_or(0);
            if let Some((dll, name)) = rest[..end].split_once(',') {
                let (dll, name) = (dll.trim(), name.trim());
                let ok = |s: &str, upper: bool| !s.is_empty() && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_' || (upper && b.is_ascii_uppercase()));
                if ok(dll, false) && ok(name, true) { out.push(format!("{dll}.dll!{name}")); }
            }
        }
        // ALIAS(dll, "imported name", fn, bytes): an import by ordinal or by a C++ name, answered by IMP(dll, fn)
        let mut rest = text.as_str();
        while let Some(i) = rest.find("ALIAS(") {
            rest = &rest[i + 6..];
            let (Some(q1), Some(comma)) = (rest.find('"'), rest.find(',')) else { continue };
            if comma > q1 { continue; }
            let dll = rest[..comma].trim();
            let Some(q2) = rest[q1 + 1..].find('"') else { continue };
            let name = &rest[q1 + 1..q1 + 1 + q2];
            if !dll.is_empty() && dll.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_') && !name.is_empty() { out.push(format!("{dll}.dll!{name}")); }
        }
    }
    out.sort(); out.dedup();
    Ok(out)
}
