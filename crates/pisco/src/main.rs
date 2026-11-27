// SPDX-License-Identifier: GPL-2.0-or-later
// pisco: builds, runs and inspects title images (docs/images.md).
//   pisco build <recipe dir> [--input <id>=<path>]... [--source <path>] [--out <image dir>] [--cache <dir>] [--compare <installed folder>]
//   pisco recipe <recipe dir> [--json | --keys]: the recipe as pisco reads it (JSON, for tools/recipe-gen.py), or the keys schema 1 knows
//   pisco run <image> [--session <session.json>] [--saves <dir>] [--chrome] [-- app arguments]
//   pisco census <image | title.exe> [--recipe <recipe dir> | --from base/windows-x86:1 --libraries a,b] [--dll-dir <system32>] [--all]
//   pisco inspect <image> [--files]
//   pisco sources <base | recipe dir> [--libraries a,b | --all-libraries]: the runtime files a title of that base links
// The repository is found from this program's own place (target/release/pisco), or SW_ROOT.
mod build;
mod census;
mod installers;
mod oci;
mod recipe;

use recipe::R;
use serde_json::json;
use std::path::{Path, PathBuf};
use std::process::Command;

fn root() -> PathBuf {
    if let Ok(r) = std::env::var("SW_ROOT") { return r.into(); }
    let is_root = |p: &Path| p.join("bases").is_dir() && p.join("rt").is_dir() && p.join("crates/pisco").is_dir();
    let starts = [std::env::current_exe().ok(), std::env::current_dir().ok()];
    for s in starts.into_iter().flatten() { for a in s.ancestors() { if is_root(a) { return a.to_path_buf(); } } }
    PathBuf::from(".")
}
fn usage() -> ! {
    eprintln!("usage:\n  pisco build <recipe dir> [--input <id>=<path>]... [--source <path>] [--out <image dir>] [--cache <dir>] [--compare <installed folder>]\n  pisco recipe <recipe dir> [--json | --keys]\n  pisco run <image> [--session <session.json>] [--saves <dir>] [--chrome] [-- app arguments]\n  pisco census <image | title.exe> [--recipe <recipe dir> | --from <base> --libraries a,b] [--all]\n  pisco inspect <image> [--files]\n  pisco sources <base | recipe dir> [--libraries a,b | --all-libraries]");
    std::process::exit(2);
}
struct Args { pos: Vec<String>, opts: Vec<(String, Option<String>)>, rest: Vec<String> }
fn parse(args: &[String], with_value: &[&str]) -> Args {
    let (mut pos, mut opts, mut rest) = (Vec::new(), Vec::new(), Vec::new());
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        if a == "--" { rest = args[i + 1..].to_vec(); break; }
        if let Some(name) = a.strip_prefix("--") {
            if with_value.contains(&name) { opts.push((name.to_string(), args.get(i + 1).cloned())); i += 1; } else { opts.push((name.to_string(), None)); }
        } else { pos.push(a.clone()); }
        i += 1;
    }
    Args { pos, opts, rest }
}
impl Args {
    fn get(&self, k: &str) -> Option<PathBuf> { self.opts.iter().find(|(n, _)| n == k).and_then(|(_, v)| v.clone()).map(PathBuf::from) }
    fn flag(&self, k: &str) -> bool { self.opts.iter().any(|(n, _)| n == k) }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let Some(cmd) = args.first() else { usage() };
    let root = root();
    let r = match cmd.as_str() {
        "build" => { let a = parse(&args[1..], &["source", "out", "cache", "compare", "with", "input"]); let Some(p) = a.pos.first() else { usage() };
            build::build(&root, &build::Opts { recipe: p.into(), source: a.get("source"), out: a.get("out"), cache: a.get("cache"), compare: a.get("compare"),
                with: a.opts.iter().filter(|(n, _)| n == "with" || n == "input").filter_map(|(_, v)| v.as_ref()?.split_once('=').map(|(k, p)| (k.to_string(), PathBuf::from(p)))).collect() }) }
        "run" => run(&root, &parse(&args[1..], &["session", "saves"])),
        "recipe" => recipe_cmd(&parse(&args[1..], &[])),
        "census" => census(&root, &parse(&args[1..], &["from", "libraries", "recipe", "dll-dir"])),
        "inspect" => inspect(&parse(&args[1..], &[])),
        "sources" => sources(&root, &parse(&args[1..], &["libraries"])),
        "-h" | "--help" | "help" => usage(),
        _ => { eprintln!("pisco: unknown command {cmd}"); usage() }
    };
    if let Err(e) = r { eprintln!("pisco: {e}"); std::process::exit(1); }
}

/// The image in Node (web/run-node.mjs) or in headless Chrome (tools/run-chrome.mjs): both read it with web/image.mjs
fn run(root: &Path, a: &Args) -> R<()> {
    let Some(image) = a.pos.first() else { usage() };
    let image = PathBuf::from(image);
    oci::open(&image)?;
    let script = if a.flag("chrome") { "tools/run-chrome.mjs" } else { "web/run-node.mjs" };
    let mut cmd = Command::new("node");
    cmd.arg(root.join(script)).arg(&image).args(&a.rest);
    if let Some(s) = a.get("session") { cmd.env("SW_SESSION", s); }
    if let Some(s) = a.get("saves") { cmd.env("SW_SAVES", s); }
    let st = cmd.status().map_err(|e| format!("node: {e}"))?;
    std::process::exit(st.code().unwrap_or(1));
}

/// `pisco recipe <dir> --json`: the recipe as pisco reads it, the one reader of recipes (tools/recipe-gen.py and
/// tools/build-title.sh take this); `--keys`: what schema 1 knows, for tests/run.sh spec
fn recipe_cmd(a: &Args) -> R<()> {
    if a.flag("keys") {
        for k in recipe::KEYS { println!("key {k}"); }
        for k in recipe::HINTS { println!("hint {k}"); }
        for k in recipe::EXTRACT_KINDS { println!("extract-kind {k}"); }
        for k in recipe::CPU_FEATURES { println!("cpu-feature {k}"); }
        for k in recipe::DLL_CHOICES { println!("dll-choice {k}"); }
        for k in recipe::QUIRKS { println!("quirk {k}"); }
        return Ok(());
    }
    let Some(dir) = a.pos.first() else { usage() };
    let r = recipe::load_recipe(Path::new(dir))?;
    let pin = |p: &recipe::Pin| match p { recipe::Pin::Sha256(l) => json!({ "sha256": l }), recipe::Pin::BuiltBy(b) => json!({ "built_by": b }), recipe::Pin::Unpinned(w) => json!({ "unpinned": w }) };
    let j = json!({
        "schema": 1, "name": r.name, "from": r.from, "platform": r.platform, "libraries": r.libraries,
        "about": { "display_name": r.display_name, "version": r.version, "revision": r.revision },
        "inputs": r.inputs.iter().map(|i| json!({ "id": i.id, "kind": if i.folder { "folder" } else { "file" }, "file": i.file, "files": i.files, "pin": pin(&i.pin) })).collect::<Vec<_>>(),
        "run": { "exe": r.exe, "display": r.display, "args": r.args, "initrd": r.initrd, "cmdline": r.cmdline },
        "cpu": { "features": r.cpu }, "dll": r.dll, "files": r.files, "quirks": { "heap_fill": r.heap_fill },
        "registry": r.registry.iter().map(|v| match &v.data {
            recipe::RegData::Dword(n) => json!({ "key": v.key, "name": v.name, "type": "dword", "value": n }),
            recipe::RegData::Sz(s) => json!({ "key": v.key, "name": v.name, "type": "sz", "value": s }) }).collect::<Vec<_>>(),
        "code": { "entries": r.entries.iter().map(|e| format!("{e:x}")).collect::<Vec<_>>(), "base": r.base_address.map(|b| format!("{b:x}")), "load": r.load },
        "warmup": r.shaders.as_ref().map(|w| json!({ "frames": w.frames, "session": w.session })),
        "hints": { "pisco": { "split": r.split, "opt": r.opt, "programs": r.programs, "modules": r.modules } },
    });
    println!("{}", serde_json::to_string_pretty(&j).map_err(|e| e.to_string())?);
    Ok(())
}

fn census(root: &Path, a: &Args) -> R<()> {
    let Some(target) = a.pos.first() else { usage() };
    let target = PathBuf::from(target);
    let (imports, base, libraries, shipped) = if oci::is_image(&target) {
        let img = oci::open(&target)?;
        let c = &img.config;
        let strs = |v: &serde_json::Value| v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>()).unwrap_or_default();
        (strs(&c["imports"]), c["base"].as_str().unwrap_or("").to_string(), strs(&c["libraries"]), vec![])
    } else {
        // an exe: lifted (with the recipe's builtin DLLs, if one is given) into a folder of its own to read its import table
        let rec = a.get("recipe").map(|r| recipe::load_recipe(&r)).transpose()?;
        let builtin: Vec<String> = rec.as_ref().map(|r| r.dll.keys().cloned().collect()).unwrap_or_default();
        let tmp = root.join("out/pisco-census"); let _ = std::fs::remove_dir_all(&tmp); std::fs::create_dir_all(&tmp).map_err(|e| e.to_string())?;
        let t = build::tools(root)?;
        // --dll-dir <folder>: DLLs it imports from the system's folder (an image's C:\WINDOWS\system32) are lifted with it
        let dll_dir = a.get("dll-dir").map(|d| format!("{}=c:/WINDOWS/system32", d.canonicalize().unwrap_or(d.clone()).display()));
        let o = Command::new(&t.lifter).arg(target.canonicalize().map_err(|e| format!("{}: {e}", target.display()))?).arg("title.c").args(builtin.iter().flat_map(|d| ["--builtin", d.as_str()]))
            .args(dll_dir.iter().flat_map(|d| ["--dll-dir", d.as_str()])).current_dir(&tmp).output().map_err(|e| e.to_string())?;
        std::fs::write(tmp.join("lifter.log"), [&o.stdout[..], &o.stderr[..]].concat()).map_err(|e| e.to_string())?;
        if !o.status.success() { return Err(format!("the lifter failed: {}", tmp.join("lifter.log").display())); }
        let dir = target.parent().unwrap_or(Path::new("."));
        let shipped = std::fs::read_dir(dir).map(|d| d.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().to_ascii_lowercase()).filter(|n| n.ends_with(".dll") && !builtin.contains(n)).collect()).unwrap_or_default();
        let libs = if let Some(r) = &rec { r.libraries.clone() } else { a.get("libraries").map(|l| l.to_string_lossy().split(',').filter(|s| !s.is_empty()).map(String::from).collect()).unwrap_or_default() };
        let from = rec.as_ref().map(|r| r.from.clone()).or(a.get("from").map(|p| p.to_string_lossy().into_owned())).unwrap_or("base/windows-x86:1".into());
        (build::lifted_imports(&tmp.join("title.c"))?, from, libs, shipped)
    };
    let base = recipe::load_base(root, &base)?;
    let c = census::Census::new(root, &base, &libraries, &shipped)?;
    let rep = c.check(&imports);
    println!("{} FROM {}, libraries: {}", target.display(), base.reference(), if libraries.is_empty() { "none".into() } else { libraries.join(", ") });
    for (n, l) in &base.libraries { println!("  library {n:8} {} ({})", if libraries.contains(n) { "chosen    " } else if rep.needs.contains(n) { "NEEDED    " } else { "not needed" }, l.description); }
    // by DLL: how many of its imports are answered
    let mut by: std::collections::BTreeMap<String, (usize, usize)> = Default::default();
    for l in &rep.lines { let imp = l.split_whitespace().nth(1).unwrap_or(""); let e = by.entry(imp.split('!').next().unwrap_or("").to_ascii_lowercase()).or_default(); e.1 += 1; if !l.starts_with("missing") && !l.starts_with("needs") { e.0 += 1; } }
    for (d, (ok, n)) in &by { println!("  {ok:4} / {n:<4} {d}"); }
    for l in rep.lines.iter().filter(|l| a.flag("all") || l.starts_with("missing") || l.starts_with("needs")) { println!("  {l}"); }
    println!("{}", rep.summary);
    Ok(())
}

/// The runtime's source files for a base (and libraries), one per line, from the repository's root: the only list
/// of them is the bases' (tools/build-title.sh asks here)
fn sources(root: &Path, a: &Args) -> R<()> {
    let Some(what) = a.pos.first() else { usage() };
    let (base, libs) = if what.starts_with("base/") {
        let base = recipe::load_base(root, what)?;
        let libs: Vec<String> = if a.flag("all-libraries") { base.libraries.keys().cloned().collect() }
            else { a.get("libraries").map(|l| l.to_string_lossy().split(',').filter(|s| !s.is_empty()).map(String::from).collect()).unwrap_or_default() };
        (base, libs)
    } else {
        let r = recipe::load_recipe(Path::new(what))?;
        let base = recipe::load_base(root, &r.from)?;
        (base, r.libraries)
    };
    let mut files = base.sources.clone();
    for l in &libs { files.extend(base.libraries.get(l).ok_or(format!("{}: no library {l} (it has {})", base.reference(), base.libraries.keys().cloned().collect::<Vec<_>>().join(", ")))?.sources.iter().cloned()); }
    files.sort(); files.dedup();
    for f in files { println!("{f}"); }
    Ok(())
}

fn inspect(a: &Args) -> R<()> {
    let Some(image) = a.pos.first() else { usage() };
    let img = oci::open(Path::new(image))?;
    let c = &img.config;
    println!("{image}: {} (manifest sha256:{})", img.manifest["annotations"][oci::TITLE].as_str().unwrap_or("?"), img.manifest_digest.trim_start_matches("sha256:"));
    println!("  FROM {}, libraries {}", c["base"].as_str().unwrap_or("?"), c["libraries"]);
    println!("  run {} ({}), {}-bit, CPU {}", c["run"]["exe"].as_str().unwrap_or("?"), c["run"]["display"].as_str().unwrap_or("no display"), c["guest"]["bits"], c["cpu"]["features"]);
    println!("  dll {}, {} registry values, {} imports", c["dll"], c["registry"].as_array().map(|a| a.len()).unwrap_or(0), c["imports"].as_array().map(|a| a.len()).unwrap_or(0));
    for v in c["registry"].as_array().into_iter().flatten() { println!("    {}\\{} = {}", v["key"].as_str().unwrap_or(""), v["name"].as_str().unwrap_or(""), v["value"]); }
    println!("  config  {} {:>10} bytes", img.manifest["config"]["digest"].as_str().unwrap_or(""), img.manifest["config"]["size"]);
    for l in img.manifest["layers"].as_array().into_iter().flatten() {
        let d = l["digest"].as_str().unwrap_or("");
        let role = l["annotations"][oci::ROLE].as_str().unwrap_or("");
        let mut extra = String::new();
        if l["mediaType"] == oci::LAYER {
            let data = oci::blob(&img.dir, d)?;
            let list = oci::tar_list(&data);
            extra = format!(", {} files", list.iter().filter(|e| !e.3).count());
            if a.flag("files") { for (n, _, size, dir) in &list { if !dir { extra.push_str(&format!("\n      {size:>10}  {n}")); } } }
        }
        println!("  {role:7} {d} {:>10} bytes  {}{extra}", l["size"], l["annotations"][oci::TITLE].as_str().unwrap_or(""));
    }
    Ok(())
}
