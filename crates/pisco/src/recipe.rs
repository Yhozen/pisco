// SPDX-License-Identifier: GPL-2.0-or-later
// Recipes (recipes/<app>/recipe.toml, docs/spec/recipe.md) and the bases they start FROM (bases/<name>/<version>/base.toml).
// docs/images.md has the reference of both.
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use toml::{Table, Value};

pub type R<T> = Result<T, String>;

#[derive(Clone, Debug, PartialEq)]
pub enum RegData { Dword(u32), Sz(String) }
#[derive(Clone, Debug)]
pub struct RegValue { pub key: String, pub name: String, pub data: RegData }

/// What pins an input (docs/spec/recipe.md#inputs)
#[derive(Clone, Debug)]
pub enum Pin { Sha256(Vec<String>), BuiltBy(String), Unpinned(String) }
/// [[input]]: a file or folder the user supplies
#[derive(Clone, Debug)]
pub struct Input { pub id: String, pub folder: bool, pub file: String, pub files: Option<Vec<String>>, pub pin: Pin,
    /// where a builder may fetch it (a pinned file input only: docs/spec/recipe.md#inputs)
    pub url: Option<String>,
    /// for a person: where it comes from
    pub where_: Option<String> }
/// The recipe's steps as pisco runs them: the first ([[extract]] innosetup, zip, wise, file or folder) with what
/// follows it (installshield, cab, copy)
pub struct Source { pub extract: String, pub app: Option<String>, pub map: BTreeMap<String, String>, pub cabs: Vec<Cab>,
    pub root: Option<String>, pub adds: Vec<Add>,
    /// [source.installshield] groups: where each file group of an InstallShield 5 / 6 setup inside the installer
    /// goes, below the title's folder ("" the folder itself, "-" not installed); InstallShield's own script says
    /// it, and is not read
    pub installshield: Option<BTreeMap<String, String>> }
/// [[extract]] copy: the files of a folder input that the app needs and its download does not carry (Microsoft's
/// C++ runtime DLLs beside an app built with /MD), from the folder named on the command line (`pisco build --input
/// <id>=<folder>`, or --with) or by the environment variable <ID>, put in a folder of C:
pub struct Add { pub from: String, pub files: Vec<String>, pub to: String, pub pin: Pin }
/// [[extract]] cab: files taken out of a cabinet among the installer's files (cabextract), put in a folder of C:
pub struct Cab { pub cab: String, pub files: Vec<String>, pub to: String }
pub struct Recipe {
    pub dir: PathBuf, pub name: String, pub from: String, pub platform: String, pub libraries: Vec<String>, pub source: Source,
    pub display_name: Option<String>, pub version: Option<String>, pub revision: Option<i64>,
    pub inputs: Vec<Input>, pub primary: Input,
    pub exe: String, pub display: Option<String>, pub args: Vec<String>,
    /// a machine's app (base/microvm-x64): [run] initrd (its initramfs, on the drive) and cmdline (the kernel's); its
    /// kernel is `exe`
    pub initrd: Option<String>, pub cmdline: Option<String>,
    /// a machine's app: [hints.pisco] programs, lifted with the kernel at their own addresses (lifter --program)
    pub programs: Vec<String>,
    /// a machine's app: [hints.pisco] modules, programs each in a wasm module of its own (a side module the host loads)
    pub modules: Vec<String>,
    pub cpu: Option<Vec<String>>, pub dll: BTreeMap<String, String>, pub registry: Vec<RegValue>,
    pub split: usize, pub entries: Vec<u64>, pub base_address: Option<u64>, pub opt: String,
    /// [code] load: DLLs the app loads itself (LoadLibrary) from its folder or below it, lifted with it
    pub load: Vec<String>,
    /// [files]: small text files a setup step would write (settings), by their place on C:
    pub files: BTreeMap<String, String>,
    pub heap_fill: Option<u8>, // [quirks] heap_fill: the byte a heap block the app did not ask to be zeroed holds
    pub shaders: Option<Shaders>,
}
/// [warmup]: the HLSL an app compiles at run time (D3DCompile), compiled when the image is built: the image is run
/// once in Node for `frames` frames (with the `session`, if one is given), the sources it logged are compiled with
/// dxc and translated, and the results become a layer of the image (docs/images.md)
pub struct Shaders { pub frames: u64, pub session: Option<String> }
pub struct Library { pub description: String, pub sources: Vec<String>, pub provides: Vec<String> }
pub struct Base {
    pub dir: PathBuf, pub name: String, pub version: i64, pub architecture: String, pub os: String,
    pub platform: String, pub stability: String, pub provides: Vec<String>,
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

/// 'C:\Program Files\X' -> "Program Files/X"; a Linux app's '/usr/bin/x' -> "usr/bin/x" (its root is the drive)
pub fn drive_path(p: &str) -> R<String> {
    if p.starts_with('/') { return Ok(p.split('/').filter(|s| !s.is_empty() && *s != ".").collect::<Vec<_>>().join("/")); }
    let rest = p.strip_prefix("C:\\").or_else(|| p.strip_prefix("c:\\")).ok_or(format!("{p}: a path on C:\\ (or, for a Linux app, from /) is expected"))?;
    Ok(rest.split('\\').filter(|s| !s.is_empty()).collect::<Vec<_>>().join("/"))
}

/// Every key a recipe of schema 1 may have, "table.key" (docs/spec/recipe.md has the same list: tests/run.sh spec)
pub const KEYS: &[&str] = &["schema", "name", "from", "libraries",
    "about.display_name", "about.version", "about.revision", "about.notes",
    "input.id", "input.kind", "input.file", "input.files", "input.sha256", "input.built_by", "input.unpinned", "input.url", "input.where",
    "extract.kind", "extract.input", "extract.app", "extract.map", "extract.root", "extract.groups", "extract.from", "extract.files", "extract.to",
    "run.exe", "run.display", "run.args", "run.kernel", "run.initrd", "run.cmdline",
    "cpu.features", "dll", "registry", "files", "quirks",
    "code.entries", "code.base", "code.load", "warmup.frames", "warmup.session", "hints"];
/// pisco's own options, [hints.pisco] (docs/spec/recipe.md)
pub const HINTS: &[&str] = &["split", "opt", "programs", "modules"];
pub const EXTRACT_KINDS: &[&str] = &["innosetup", "zip", "wise", "installshield", "file", "folder", "cab", "copy"];
pub const DLL_CHOICES: &[&str] = &["native", "builtin"];
pub const QUIRKS: &[&str] = &["heap_fill"];
/// The CPU features a recipe may name (tools/recipe-gen.py has their bits)
pub const CPU_FEATURES: &[&str] = &["fpu", "tsc", "cx8", "cmov", "mmx", "fxsr", "sse", "sse2", "sse3", "ssse3", "sse4.1", "sse4.2", "popcnt", "cx16", "pclmul"];

/// A table's keys, each one of `known` (keys of KEYS under `prefix`); the message lists them
fn only(t: &Table, prefix: &str, what: &str) -> R<()> {
    let known: Vec<&str> = KEYS.iter().filter_map(|k| if prefix.is_empty() { Some(k.split('.').next().unwrap()) } else { k.strip_prefix(&format!("{prefix}.")) }).collect();
    for k in t.keys() {
        if !known.contains(&k.as_str()) {
            let mut list = known.clone(); list.dedup();
            return Err(format!("{what}: {k} is not a key of {} (schema 1 knows {})", if prefix.is_empty() { "a recipe".to_string() } else { format!("[{prefix}]") }, list.join(", ")));
        }
    }
    Ok(())
}
fn slug(x: &str) -> bool { !x.is_empty() && x.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-') }
fn sha_list(v: &Value, what: &str) -> R<Vec<String>> {
    let list = match v { Value::String(x) => vec![x.clone()], Value::Array(a) => a.iter().map(|x| x.as_str().map(String::from).ok_or(format!("{what}: strings"))).collect::<R<_>>()?, _ => return Err(format!("{what}: a sha256, or a list of them")) };
    let list: Vec<String> = list.into_iter().map(|x| x.trim_start_matches("sha256:").to_ascii_lowercase()).collect();
    if list.is_empty() || list.iter().any(|x| x.len() != 64 || !x.bytes().all(|b| b.is_ascii_hexdigit())) { return Err(format!("{what}: 64 hex digits")); }
    Ok(list)
}
/// The platform a base imitates, from its name (the [run] keys depend on it)
pub fn platform_of(from: &str) -> &str {
    from.strip_prefix("base/").and_then(|x| x.split(':').next()).unwrap_or("")
}

pub fn load_recipe(dir: &Path) -> R<Recipe> {
    let path = if dir.is_dir() { dir.join("recipe.toml") } else { dir.to_path_buf() };
    if !path.is_file() && path.with_file_name("title.toml").is_file() { return Err(format!("{}: a recipe is recipe.toml now (git mv title.toml recipe.toml)", path.with_file_name("title.toml").display())); }
    let t = load(&path)?;
    let at = path.display().to_string();
    let dir = path.parent().unwrap().to_path_buf();
    match t.get("schema") {
        Some(Value::Integer(1)) => {}
        Some(v) => return Err(format!("{at}: schema = {}: this pisco reads recipes of schema 1 (docs/spec/recipe.md)", match v { Value::Integer(i) => i.to_string(), other => format!("a {}", other.type_str()) })),
        None => return Err(format!("{at}: no schema: a recipe starts with `schema = 1` (docs/spec/recipe.md)")),
    }
    only(&t, "", &at)?;
    let name = s(&t, "name").ok_or(format!("{at}: no name"))?;
    if !slug(&name) { return Err(format!("{at}: name = \"{name}\": lower-case letters, digits and -")); }
    let from = s(&t, "from").ok_or(format!("{at}: no from (a base, e.g. \"base/windows-x86:1\")"))?;
    let platform = platform_of(&from).to_string();
    for k in ["about", "run", "cpu", "code", "warmup"] { if let Some(x) = table(&t, k)? { only(x, k, &at)?; } }
    let about = table(&t, "about")?.cloned().unwrap_or_default();
    if let Some(v) = about.get("revision") { if !matches!(v, Value::Integer(n) if *n >= 1) { return Err(format!("{at}: about.revision: an integer from 1")); } }
    for k in ["display_name", "version", "notes"] { if about.get(k).is_some_and(|v| !v.is_str()) { return Err(format!("{at}: about.{k}: a string")); } }

    // [[input]]
    let mut inputs: Vec<Input> = Vec::new();
    for v in t.get("input").map(|v| v.as_array().cloned().ok_or(format!("{at}: [[input]]: an array of tables"))).transpose()?.unwrap_or_default() {
        let i = v.as_table().ok_or(format!("{at}: [[input]]: tables"))?;
        only(i, "input", &at)?;
        let id = s(i, "id").ok_or(format!("{at}: an [[input]] without id"))?;
        if !slug(&id) { return Err(format!("{at}: input id \"{id}\": lower-case letters, digits and -")); }
        if inputs.iter().any(|x| x.id == id) { return Err(format!("{at}: two inputs are \"{id}\"")); }
        let kind = s(i, "kind").unwrap_or("file".into());
        if kind != "file" && kind != "folder" { return Err(format!("{at}: input {id}: kind = \"{kind}\": \"file\" or \"folder\"")); }
        let files = strs(i, "files")?;
        if files.is_some() && kind != "folder" { return Err(format!("{at}: input {id}: files are for a folder input (kind = \"folder\")")); }
        let pins = ["sha256", "built_by", "unpinned"].iter().filter(|k| i.contains_key(**k)).count();
        if pins != 1 { return Err(format!("{at}: input {id}: say exactly one of sha256 (pinned), built_by (made here by a script) or unpinned (and why)")); }
        let pin = if let Some(v) = i.get("sha256") { Pin::Sha256(sha_list(v, &format!("{at}: input {id}: sha256"))?) }
            else if let Some(b) = s(i, "built_by") { Pin::BuiltBy(b) } else { Pin::Unpinned(s(i, "unpinned").ok_or(format!("{at}: input {id}: unpinned = \"why\""))?) };
        let url = s(i, "url");
        if let Some(u) = &url {
            if !matches!(pin, Pin::Sha256(_)) || kind != "file" { return Err(format!("{at}: input {id}: url is for a file input pinned by its sha256")); }
            if !(u.starts_with("https://") || u.starts_with("http://")) { return Err(format!("{at}: input {id}: url = \"{u}\": an https:// (or http://) URL")); }
        }
        inputs.push(Input { id: id.clone(), folder: kind == "folder", file: s(i, "file").ok_or(format!("{at}: input {id}: no file"))?, files, pin, url, where_: s(i, "where") });
    }
    let input = |id: &Option<String>, step: &str| -> R<Input> {
        let id = id.as_ref().ok_or(format!("{at}: [[extract]] {step}: no input"))?;
        inputs.iter().find(|x| &x.id == id).cloned().ok_or(format!("{at}: [[extract]] {step}: no input \"{id}\""))
    };

    // [[extract]]: the first step unpacks the app; installshield follows wise; copy and cab steps follow
    let steps = t.get("extract").map(|v| v.as_array().cloned().ok_or(format!("{at}: [[extract]]: an array of tables"))).transpose()?.unwrap_or_default();
    if steps.is_empty() { return Err(format!("{at}: no [[extract]] step")); }
    let (mut source, mut primary): (Option<Source>, Option<Input>) = (None, None);
    for (n, v) in steps.iter().enumerate() {
        let e = v.as_table().ok_or(format!("{at}: [[extract]]: tables"))?;
        only(e, "extract", &at)?;
        let kind = s(e, "kind").ok_or(format!("{at}: an [[extract]] step without kind"))?;
        if !EXTRACT_KINDS.contains(&kind.as_str()) { return Err(format!("{at}: [[extract]] kind = \"{kind}\": one of {}", EXTRACT_KINDS.join(", "))); }
        let allowed: &[&str] = match kind.as_str() {
            "innosetup" => &["kind", "input", "app", "map"], "zip" => &["kind", "input", "app", "root"], "wise" | "folder" => &["kind", "input", "app"], "file" => &["kind", "input"],
            "installshield" => &["kind", "groups"], "cab" => &["kind", "from", "files", "to"], _ => &["kind", "input", "to"],
        };
        if let Some(k) = e.keys().find(|k| !allowed.contains(&k.as_str())) { return Err(format!("{at}: [[extract]] {kind}: {k} is not one of its keys ({})", allowed.join(", "))); }
        let first = matches!(kind.as_str(), "innosetup" | "zip" | "wise" | "file" | "folder");
        if first != (n == 0) { return Err(format!("{at}: [[extract]] {kind}: {}", if first { "only the first step unpacks the app (there is one such step)" } else { "the first step unpacks the app: innosetup, zip, wise, file or folder" })); }
        match kind.as_str() {
            _ if first => {
                let i = input(&s(e, "input"), &kind)?;
                if (kind == "folder") != i.folder { return Err(format!("{at}: [[extract]] {kind} reads input {}, a {}", i.id, if i.folder { "folder" } else { "file" })); }
                let mut map = BTreeMap::new();
                for (k, v) in table(e, "map")?.into_iter().flatten() { map.insert(k.clone(), v.as_str().ok_or(format!("{at}: [[extract]] map: strings"))?.to_string()); }
                source = Some(Source { extract: kind.clone(), app: s(e, "app"), map, cabs: vec![],
                    root: s(e, "root").map(|r| r.trim_matches('/').to_string()), adds: vec![], installshield: None });
                primary = Some(i);
            }
            "installshield" => {
                let src = source.as_mut().unwrap();
                if n != 1 || src.extract != "wise" { return Err(format!("{at}: [[extract]] installshield: pisco reads it straight after the wise step that unpacks it")); }
                let mut g = BTreeMap::new();
                for (k, v) in table(e, "groups")?.ok_or(format!("{at}: [[extract]] installshield: groups = {{ \"group\" = 'folder', ... }}"))? { g.insert(k.clone(), v.as_str().ok_or(format!("{at}: [[extract]] installshield groups: strings"))?.to_string()); }
                src.installshield = Some(g);
            }
            "cab" => source.as_mut().unwrap().cabs.push(Cab { cab: s(e, "from").ok_or(format!("{at}: [[extract]] cab: from = 'C:\\...\\x.cab'"))?,
                files: strs(e, "files")?.filter(|f| !f.is_empty()).ok_or(format!("{at}: [[extract]] cab: files = [\"name.dll\"]"))?, to: s(e, "to").ok_or(format!("{at}: [[extract]] cab: to = 'C:\\...'"))? }),
            _ => { // copy
                let i = input(&s(e, "input"), "copy")?;
                let files = i.files.clone().filter(|f| !f.is_empty()).ok_or(format!("{at}: [[extract]] copy reads input {}: a folder input with its files named", i.id))?;
                source.as_mut().unwrap().adds.push(Add { from: i.id.clone(), files, to: s(e, "to").ok_or(format!("{at}: [[extract]] copy: to = 'C:\\...'"))?, pin: i.pin.clone() });
            }
        }
    }
    let source = source.unwrap(); let primary = primary.unwrap();
    for i in &inputs { if i.id != primary.id && !source.adds.iter().any(|a| a.from == i.id) { return Err(format!("{at}: input {} is read by no step", i.id)); } }

    // [run], by platform
    let run = table(&t, "run")?.ok_or(format!("{at}: no [run]"))?;
    let run_keys: &[&str] = match platform.as_str() { "microvm-x64" => &["kernel", "initrd", "cmdline"], "linux-x64" | "ps5" => &["exe", "args"], _ => &["exe", "display", "args"] };
    if let Some(k) = run.keys().find(|k| !run_keys.contains(&k.as_str())) { return Err(format!("{at}: [run] {k}: not a key of {platform}'s [run] ({}; docs/spec/platforms/)", run_keys.join(", "))); }
    let exe = if platform == "microvm-x64" { s(run, "kernel").ok_or(format!("{at}: [run] kernel"))? } else { s(run, "exe").ok_or(format!("{at}: [run] exe"))? };

    let mut dll = BTreeMap::new();
    for (k, v) in table(&t, "dll")?.into_iter().flatten() {
        let how = v.as_str().ok_or(format!("{at}: [dll]: \"name.dll\" = \"builtin\""))?;
        if !DLL_CHOICES.contains(&how) { return Err(format!("{at}: [dll] {k} = \"{how}\": one of {}", DLL_CHOICES.join(", "))); }
        if how == "builtin" { dll.insert(k.to_ascii_lowercase(), how.to_string()); }   // native: what a shipped DLL gets anyway
    }
    let cpu = match table(&t, "cpu")? { Some(c) => strs(c, "features")?, None => None };
    if let Some(f) = cpu.iter().flatten().find(|f| !CPU_FEATURES.contains(&f.as_str())) { return Err(format!("{at}: cpu.features: {f} is not one of {}", CPU_FEATURES.join(" "))); }
    let quirks = table(&t, "quirks")?.cloned().unwrap_or_default();
    if let Some(k) = quirks.keys().find(|k| !QUIRKS.contains(&k.as_str())) { return Err(format!("{at}: [quirks] {k}: not a quirk ({})", QUIRKS.join(", "))); }
    let code = table(&t, "code")?.cloned().unwrap_or_default();
    let hints = table(&t, "hints")?.cloned().unwrap_or_default();
    for (k, v) in &hints { if !v.is_table() { return Err(format!("{at}: hints.{k}: a table, [hints.{k}]")); } }
    let ours = table(&hints, "pisco")?.cloned().unwrap_or_default();
    if let Some(k) = ours.keys().find(|k| !HINTS.contains(&k.as_str())) { return Err(format!("{at}: [hints.pisco] {k}: not one of pisco's ({})", HINTS.join(", "))); }
    let micro = |k: &str| -> R<Vec<String>> { let v = strs(&ours, k)?.unwrap_or_default(); if !v.is_empty() && platform != "microvm-x64" { return Err(format!("{at}: [hints.pisco] {k}: for a machine's app (base/microvm-x64)")); } Ok(v) };
    let mut files = BTreeMap::new();
    for (k, v) in table(&t, "files")?.into_iter().flatten() { files.insert(k.clone(), v.as_str().ok_or(format!("{at}: [files] {k}: a string"))?.to_string()); }
    Ok(Recipe {
        display_name: s(&about, "display_name"), version: s(&about, "version"), revision: about.get("revision").and_then(|v| v.as_integer()),
        libraries: strs(&t, "libraries")?.unwrap_or_default(),
        display: s(run, "display"),
        args: strs(run, "args")?.unwrap_or_default(),
        initrd: s(run, "initrd"), cmdline: s(run, "cmdline"),
        programs: micro("programs")?, modules: micro("modules")?,
        cpu, dll, files,
        registry: registry(table(&t, "registry")?)?,
        split: ours.get("split").map(|v| v.as_integer().filter(|n| *n >= 1).ok_or(format!("{at}: hints.pisco.split: a number from 1"))).transpose()?.unwrap_or(8) as usize,
        entries: match code.get("entries") { Some(Value::Array(a)) => a.iter().map(|v| int(v, "code.entries")).collect::<R<_>>()?, Some(_) => return Err(format!("{at}: code.entries: a list")), None => vec![] },
        load: strs(&code, "load")?.unwrap_or_default(),
        base_address: code.get("base").map(|v| int(v, "code.base")).transpose()?,
        opt: s(&ours, "opt").unwrap_or("-O2".into()),
        heap_fill: match quirks.get("heap_fill") {
            None => None,
            Some(v) => Some(v.as_integer().filter(|n| (0..=255).contains(n)).ok_or(format!("{at}: quirks.heap_fill: a byte, 0 to 255"))? as u8),
        },
        shaders: match table(&t, "warmup")? {
            None => None,
            Some(sh) => Some(Shaders { frames: sh.get("frames").and_then(|v| v.as_integer()).filter(|n| *n > 0).ok_or(format!("{at}: [warmup] frames = <frames to run for>"))? as u64, session: s(sh, "session") }),
        },
        primary, inputs, source, exe, platform, name, from, dir,
    })
}

/// "base/windows-x86:1" -> <root>/bases/windows-x86/1/base.toml
/// Every key a base may have (docs/spec/base.md has the same list: tests/run.sh spec)
pub const BASE_KEYS: &[&str] = &["name", "version", "platform", "stability", "architecture", "os", "provides", "cpu.features", "registry",
    "files.dir", "files.dirs", "files.fonts", "files.system32", "libraries.<name>.description", "libraries.<name>.provides", "impl"];
pub fn load_base(root: &Path, from: &str) -> R<Base> {
    let (name, version) = from.split_once(':').ok_or(format!("from = \"{from}\": <base>:<version>"))?;
    let short = name.strip_prefix("base/").ok_or(format!("from = \"{from}\": bases are named base/<name>"))?;
    let dir = root.join("bases").join(short).join(version);
    let at = dir.join("base.toml").display().to_string();
    let t = load(&dir.join("base.toml"))?;
    // the contract's keys, then [impl] (pisco's own)
    let top: Vec<&str> = BASE_KEYS.iter().map(|k| k.split('.').next().unwrap()).collect();
    if let Some(k) = t.keys().find(|k| !top.contains(&k.as_str())) { return Err(format!("{at}: {k} is not a key of a base ({})", { let mut v = top.clone(); v.dedup(); v.join(", ") })); }
    let sub = |tab: &str, keys: &[&str]| -> R<()> { if let Some(x) = table(&t, tab)? { if let Some(k) = x.keys().find(|k| !keys.contains(&k.as_str())) { return Err(format!("{at}: [{tab}] {k}: not one of its keys ({})", keys.join(", "))); } } Ok(()) };
    sub("cpu", &["features"])?; sub("files", &["dir", "dirs", "fonts", "system32"])?; sub("impl", &["sources", "opt", "exclude", "libraries"])?;
    let stability = s(&t, "stability").ok_or(format!("{at}: no stability (stable or experimental)"))?;
    if stability != "stable" && stability != "experimental" { return Err(format!("{at}: stability = \"{stability}\": stable or experimental")); }
    let rt = table(&t, "impl")?.ok_or(format!("{at}: no [impl]"))?;
    let files = table(&t, "files")?.cloned().unwrap_or_default();
    let exclude = strs(rt, "exclude")?.unwrap_or_default();
    let impl_libs = table(rt, "libraries")?.cloned().unwrap_or_default();
    let mut libraries = BTreeMap::new();
    for (n, l) in table(&t, "libraries")?.into_iter().flatten() {
        let l = l.as_table().ok_or(format!("{at}: [libraries.<name>] tables"))?;
        if let Some(k) = l.keys().find(|k| !["description", "provides"].contains(&k.as_str())) { return Err(format!("{at}: [libraries.{n}] {k}: not one of its keys (description, provides; its sources are [impl.libraries.{n}])")); }
        let imp = impl_libs.get(n).and_then(|v| v.as_table()).ok_or(format!("{at}: library {n} has no [impl.libraries.{n}]"))?;
        let mut srcs = Vec::new();
        for g in strs(imp, "sources")?.unwrap_or_default() { srcs.extend(glob(root, &g)?); }
        libraries.insert(n.clone(), Library { description: s(l, "description").unwrap_or_default(), sources: srcs, provides: strs(l, "provides")?.unwrap_or_default() });
    }
    if let Some(n) = impl_libs.keys().find(|n| !libraries.contains_key(*n)) { return Err(format!("{at}: [impl.libraries.{n}], but no [libraries.{n}]")); }
    let mut sources = Vec::new();
    for g in strs(rt, "sources")?.unwrap_or_default() { sources.extend(glob(root, &g)?); }
    sources.retain(|f| !exclude.contains(f) && !libraries.values().any(|l| l.sources.contains(f)));
    sources.sort(); sources.dedup();
    let mut fonts = BTreeMap::new();
    for (k, v) in table(&files, "fonts")?.into_iter().flatten() { fonts.insert(k.clone(), v.as_str().ok_or("files.fonts: strings")?.to_string()); }
    let b = Base {
        name: s(&t, "name").ok_or("base: no name")?, version: t.get("version").and_then(|v| v.as_integer()).ok_or("base: no version")?,
        platform: s(&t, "platform").ok_or(format!("{at}: no platform"))?, stability, provides: strs(&t, "provides")?.unwrap_or_default(),
        architecture: s(&t, "architecture").ok_or(format!("{at}: no architecture"))?, os: s(&t, "os").ok_or(format!("{at}: no os"))?,
        cpu: table(&t, "cpu")?.map(|c| strs(c, "features")).transpose()?.flatten().unwrap_or_default(),
        sources, opt: s(rt, "opt").unwrap_or("-O2".into()), registry: registry(table(&t, "registry")?)?,
        files_dir: dir.join(s(&files, "dir").unwrap_or("files".into())), dirs: strs(&files, "dirs")?.unwrap_or_default(), fonts,
        system32: s(&files, "system32").unwrap_or("WINDOWS\\system32".into()), libraries, dir,
    };
    if b.reference() != from { return Err(format!("{}: is {}, not {from}", b.dir.display(), b.reference())); }
    if b.platform != short { return Err(format!("{at}: platform = \"{}\", but the base is {short}", b.platform)); }
    Ok(b)
}
/// What the base promises against what its sources answer for: a line for each DLL `provides` lists and the sources do
/// not answer, or the reverse (tests/layout.sh)
pub fn check_provides(root: &Path, b: &Base) -> R<Vec<String>> {
    let dlls = |files: &[String]| -> R<Vec<String>> { let mut v: Vec<String> = implemented(root, files)?.iter().map(|f| f.split('!').next().unwrap().to_string()).collect(); v.sort(); v.dedup(); Ok(v) };
    let mut out = Vec::new();
    let mut cmp = |what: &str, said: &[String], have: Vec<String>| {
        for d in &have { if !said.contains(d) { out.push(format!("{} {what}: its sources answer for {d}, which provides does not list", b.reference())); } }
        for d in said { if !have.contains(d) { out.push(format!("{} {what}: provides lists {d}, which its sources do not answer for", b.reference())); } }
    };
    cmp("(the base)", &b.provides, dlls(&b.sources)?);
    for (n, l) in &b.libraries { cmp(&format!("library {n}"), &l.provides, dlls(&l.sources)?); }
    Ok(out)
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
