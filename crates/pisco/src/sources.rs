// SPDX-License-Identifier: GPL-2.0-or-later
// Recipe sources (docs/spec/distribution.md#sources): what a name given to `pisco install` leads to.
//   tag                     the official source: this repository's recipes/
//   <folder>                a recipe's folder
//   owner/repo/tag          recipes/tag/ of github.com/owner/repo (as skills.sh names a skill)
//   @ns/tag                 recipes/tag/ of the git repository `pisco source add @ns <url>` named (as shadcn's registries)
//   <git url>#tag           recipes/tag/ of any git repository
// A git source is fetched (one commit, shallow) into $PISCO_HOME/sources/, and the commit is what the app records.
// A recipe from a source other than the official one is a third party's: no one here has read it, so it may not
// run a script of its own (built_by) and its inputs are plain file names (docs/spec/distribution.md#third-party-recipes).
use crate::recipe::{self, Pin, Recipe, R};
use crate::store;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::process::Command;

pub struct Resolved { pub dir: PathBuf, pub source: String, pub third_party: bool }

fn sources_file() -> PathBuf { store::home().join("sources.toml") }
/// The namespaces `pisco source add` named: `@ns` -> a git URL
pub fn namespaces() -> R<BTreeMap<String, String>> {
    let f = sources_file();
    let Ok(text) = std::fs::read_to_string(&f) else { return Ok(BTreeMap::new()) };
    let t: toml::Table = toml::from_str(&text).map_err(|e| format!("{}: {e}", f.display()))?;
    let mut out = BTreeMap::new();
    for (k, v) in t.get("sources").and_then(|v| v.as_table()).into_iter().flatten() {
        out.insert(k.clone(), v.as_str().ok_or(format!("{}: sources.{k}: a git URL", f.display()))?.to_string());
    }
    Ok(out)
}
fn save_namespaces(n: &BTreeMap<String, String>) -> R<()> {
    let mut s = String::from("# the recipe sources `pisco source add` named (docs/spec/distribution.md#sources)\n[sources]\n");
    for (k, v) in n { s.push_str(&format!("{} = {}\n", toml_str(k), toml_str(v))); }
    let f = sources_file();
    std::fs::create_dir_all(f.parent().unwrap()).map_err(|e| e.to_string())?;
    std::fs::write(&f, s).map_err(|e| format!("{}: {e}", f.display()))
}
fn toml_str(s: &str) -> String { format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\"")) }
fn check_ns(ns: &str) -> R<()> {
    let ok = ns.strip_prefix('@').is_some_and(|n| store::check_name(n).is_ok());
    if ok { Ok(()) } else { Err(format!("{ns}: a namespace is @ and lower-case letters, digits and '-'")) }
}
pub fn add(ns: &str, url: &str) -> R<()> {
    check_ns(ns)?;
    let mut n = namespaces()?;
    fetch(url, None)?;   // it must be a git repository with recipes/
    n.insert(ns.to_string(), url.to_string());
    save_namespaces(&n)
}
pub fn remove(ns: &str) -> R<()> {
    let mut n = namespaces()?;
    if n.remove(ns).is_none() { return Err(format!("{ns}: not a source (pisco source list)")); }
    save_namespaces(&n)
}

/// GitHub, or what stands for it in a test (PISCO_GITHUB)
fn github() -> String { std::env::var("PISCO_GITHUB").unwrap_or("https://github.com".into()) }
/// One commit of a git repository (`rev`, or its default branch), in the store's sources/; returns the folder and the commit
fn fetch(url: &str, rev: Option<&str>) -> R<(PathBuf, String)> {
    let key: String = url.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '-' { c } else { '_' }).collect();
    let dir = store::home().join("sources").join(key);
    let git = |args: &[&str]| -> R<String> {
        let o = Command::new("git").arg("-C").arg(&dir).args(args).output().map_err(|e| format!("git: {e}"))?;
        if !o.status.success() { return Err(format!("git {}: {}", args.join(" "), String::from_utf8_lossy(&o.stderr).trim())); }
        Ok(String::from_utf8_lossy(&o.stdout).trim().to_string())
    };
    if !dir.join(".git").is_dir() {
        std::fs::create_dir_all(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
        git(&["init", "-q"])?;
        git(&["remote", "add", "origin", url])?;
    }
    git(&["fetch", "-q", "--depth", "1", "origin", rev.unwrap_or("HEAD")]).map_err(|e| format!("{url}: {e}"))?;
    git(&["checkout", "-q", "--detach", "--force", "FETCH_HEAD"])?;
    let commit = git(&["rev-parse", "HEAD"])?;
    if !dir.join("recipes").is_dir() { return Err(format!("{url}: has no recipes/ folder: not a recipe source")); }
    Ok((dir, commit))
}

/// What a name leads to (see the top of this file); `rev`: the commit, branch or tag of a git source
pub fn resolve(root: &Path, what: &str, rev: Option<&str>) -> R<Resolved> {
    let p = Path::new(what);
    if p.join("recipe.toml").is_file() || (what.ends_with(".toml") && p.is_file()) {
        let abs = std::fs::canonicalize(p).map_err(|e| format!("{what}: {e}"))?;
        return Ok(Resolved { dir: p.to_path_buf(), source: format!("dir:{}", abs.display()), third_party: false });
    }
    let git = |url: &str, app: &str| -> R<Resolved> {
        store::check_name(app)?;
        let (dir, commit) = fetch(url, rev)?;
        let d = dir.join("recipes").join(app);
        if !d.join("recipe.toml").is_file() { return Err(format!("{url}: no recipes/{app}/recipe.toml (at {commit})")); }
        Ok(Resolved { dir: d, source: format!("git:{url}#{app}@{commit}"), third_party: true })
    };
    if let Some((url, app)) = what.rsplit_once('#') { return git(url, app); }
    if let Some(rest) = what.strip_prefix('@') {
        let (ns, app) = rest.split_once('/').ok_or(format!("{what}: @<namespace>/<app>"))?;
        let ns = format!("@{ns}");
        let url = namespaces()?.get(&ns).cloned().ok_or(format!("{ns}: no such source (pisco source add {ns} <git url>)"))?;
        return git(&url, app);
    }
    let parts: Vec<&str> = what.split('/').collect();
    if parts.len() == 3 && !what.contains(':') && parts.iter().all(|s| !s.is_empty() && *s != "." && *s != "..") {
        return git(&format!("{}/{}/{}", github(), parts[0], parts[1]), parts[2]);
    }
    if store::check_name(what).is_ok() {
        if rev.is_some() { return Err(format!("{what}: an official recipe is this pisco's own: --rev is for a git source")); }
        let d = root.join("recipes").join(what);
        if d.join("recipe.toml").is_file() { return Ok(Resolved { dir: d, source: "official".into(), third_party: false }); }
        return Err(format!("{what}: no official recipe of that name (pisco search), and not a folder"));
    }
    Err(format!("{what}: not a recipe's folder or a name (an app, owner/repo/app, @ns/app or <git url>#app: docs/spec/distribution.md#names)"))
}

/// What a third party's recipe may not do (docs/spec/distribution.md#third-party-recipes)
pub fn check_third_party(r: &Recipe, folder: &str) -> R<()> {
    if r.name != folder { return Err(format!("recipes/{folder}: its name is \"{}\": a source's recipe is in the folder of its name", r.name)); }
    let plain = |f: &str| !f.is_empty() && !f.contains('/') && !f.contains('\\') && f != "." && f != "..";
    for i in &r.inputs {
        if let Pin::BuiltBy(b) = &i.pin { return Err(format!("{}: input {}: built_by = \"{b}\": a third party's recipe may not run a script; its inputs are the user's files, known by sha256", r.name, i.id)); }
        if !plain(&i.file) { return Err(format!("{}: input {}: file = \"{}\": a third party's recipe names its inputs by a plain file name", r.name, i.id, i.file)); }
    }
    if let Some(s) = r.shaders.as_ref().and_then(|w| w.session.as_ref()) {
        if s.starts_with('/') || s.split(['/', '\\']).any(|c| c == "..") { return Err(format!("{}: warmup.session = \"{s}\": a path inside the recipe's folder", r.name)); }
    }
    Ok(())
}

/// The recipes of the official source and of every namespace, as (name, display name, base, how to install it)
pub fn search(root: &Path, words: &[String]) -> R<Vec<(String, String, String, String)>> {
    let mut found = Vec::new();
    let mut scan = |dir: &Path, how: &dyn Fn(&str) -> String| {
        let Ok(rd) = std::fs::read_dir(dir) else { return };
        let mut names: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        for n in names {
            let Ok(r) = recipe::load_recipe(&dir.join(&n)) else { continue };
            let text = format!("{} {}", r.name, r.display_name.clone().unwrap_or_default()).to_lowercase();
            if words.iter().all(|w| text.contains(&w.to_lowercase())) { found.push((r.name.clone(), r.display_name.clone().unwrap_or_default(), r.from.clone(), how(&r.name))); }
        }
    };
    scan(&root.join("recipes"), &|n| n.to_string());
    for (ns, url) in namespaces()? {
        match fetch(&url, None) { Ok((dir, _)) => scan(&dir.join("recipes"), &|n| format!("{ns}/{n}")), Err(e) => eprintln!("pisco: {ns}: {e}") }
    }
    Ok(found)
}
