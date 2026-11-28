// SPDX-License-Identifier: GPL-2.0-or-later
// The store (docs/images.md#the-store): the apps installed on this machine, by name.
//   $PISCO_HOME (default ~/.pisco)
//     blobs/sha256/<digest>   every app's blobs, once: apps FROM one base share its layer
//     apps/<name>/            an image layout (oci-layout, index.json) whose blobs/ is a link to ../../blobs, so a
//                             host reads an installed app as it reads any image folder
//     saves/<name>/           what the app keeps between runs (SW_SAVES), which installing it again leaves alone
// An app is installed by building its recipe into a folder of the store's and moving that folder into place, so an
// install that stops leaves the app as it was.
use crate::oci;
use crate::recipe::R;
use serde_json::Value;
use std::collections::BTreeSet;
use std::path::{Path, PathBuf};

pub fn home() -> PathBuf {
    if let Some(h) = std::env::var_os("PISCO_HOME") { return h.into(); }
    PathBuf::from(std::env::var_os("HOME").unwrap_or_else(|| ".".into())).join(".pisco")
}
fn io<T>(r: std::io::Result<T>, what: &Path) -> R<T> { r.map_err(|e| format!("{}: {e}", what.display())) }

/// An app's name as the store keeps it: what a recipe's `name` may be (docs/spec/recipe.md)
pub fn check_name(name: &str) -> R<()> {
    let ok = !name.is_empty() && name.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-') && !name.starts_with('-');
    if ok { Ok(()) } else { Err(format!("{name}: not an app's name (lower-case letters, digits and '-')")) }
}
pub fn app_dir(name: &str) -> PathBuf { home().join("apps").join(name) }
/// An image folder, or the name of an installed app
pub fn resolve_image(what: &str) -> R<PathBuf> {
    let p = PathBuf::from(what);
    if oci::is_image(&p) { return Ok(p); }
    if check_name(what).is_ok() && oci::is_image(&app_dir(what)) { return Ok(app_dir(what)); }
    Err(format!("{what}: not an image folder, and no app of that name is installed (pisco list)"))
}

/// Where a build for the store writes its image: a fresh folder inside the store (one file system, so moving it is a rename)
pub fn staging(name: &str) -> R<PathBuf> {
    check_name(name)?;
    let d = home().join("tmp").join(format!("{name}.{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    io(std::fs::create_dir_all(d.parent().unwrap()), &d)?;
    Ok(d)
}

/// Moves a built image (a folder with its own blobs) into the store as `name`; `annotations` are added to its entry
/// in index.json (where it came from). Returns the app's folder
pub fn adopt(built: &Path, name: &str, annotations: &[(&str, String)]) -> R<PathBuf> {
    check_name(name)?;
    let img = oci::open(built)?;
    let blobs = home().join("blobs/sha256");
    io(std::fs::create_dir_all(&blobs), &blobs)?;
    let own = built.join("blobs/sha256");
    for e in io(std::fs::read_dir(&own), &own)? {
        let e = io(e, &own)?;
        let dst = blobs.join(e.file_name());
        if dst.exists() { io(std::fs::remove_file(e.path()), &e.path())?; } else { io(std::fs::rename(e.path(), &dst), &dst)?; }
    }
    io(std::fs::remove_dir_all(built.join("blobs")), built)?;
    io(std::os::unix::fs::symlink("../../blobs", built.join("blobs")), built)?;
    let mut index: Value = serde_json::from_slice(&io(std::fs::read(built.join("index.json")), built)?).map_err(|e| e.to_string())?;
    let m = &mut index["manifests"][0]["annotations"];
    m[oci::REF] = Value::String(name.to_string());
    for (k, v) in annotations { m[*k] = Value::String(v.clone()); }
    io(std::fs::write(built.join("index.json"), serde_json::to_string(&index).unwrap()), built)?;
    drop(img);
    let dst = app_dir(name);
    io(std::fs::create_dir_all(dst.parent().unwrap()), &dst)?;
    // the old app goes aside first, so that the new one is in place by one rename
    let old = home().join("tmp").join(format!("{name}.old{}", std::process::id()));
    if dst.exists() { io(std::fs::rename(&dst, &old), &dst)?; }
    io(std::fs::rename(built, &dst), &dst)?;
    let _ = std::fs::remove_dir_all(&old);
    gc()?;
    oci::open(&dst)?;
    Ok(dst)
}

pub struct Installed { pub name: String, pub image: oci::Image, pub annotations: Value }
pub fn installed() -> R<Vec<Installed>> {
    let apps = home().join("apps");
    let mut out = Vec::new();
    let Ok(rd) = std::fs::read_dir(&apps) else { return Ok(out) };
    let mut names: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| check_name(n).is_ok()).collect();
    names.sort();
    for name in names {
        let dir = apps.join(&name);
        let index: Value = serde_json::from_slice(&io(std::fs::read(dir.join("index.json")), &dir)?).map_err(|e| e.to_string())?;
        let image = oci::open(&dir)?;
        out.push(Installed { name, image, annotations: index["manifests"][0]["annotations"].clone() });
    }
    Ok(out)
}
/// The blobs an image names: its manifest, config and layers
fn blobs_of(img: &oci::Image) -> Vec<String> {
    let mut v = vec![img.manifest_digest.clone()];
    if let Some(d) = img.manifest["config"]["digest"].as_str() { v.push(d.to_string()); }
    for l in img.manifest["layers"].as_array().into_iter().flatten() { if let Some(d) = l["digest"].as_str() { v.push(d.to_string()); } }
    v.into_iter().map(|d| d.trim_start_matches("sha256:").to_string()).collect()
}
/// How many bytes an app's blobs take, and how many of them no other app shares
pub fn sizes(apps: &[Installed]) -> Vec<(u64, u64)> {
    let all: Vec<Vec<String>> = apps.iter().map(|a| blobs_of(&a.image)).collect();
    let size = |d: &str| std::fs::metadata(home().join("blobs/sha256").join(d)).map(|m| m.len()).unwrap_or(0);
    all.iter().enumerate().map(|(i, mine)| {
        let others: BTreeSet<&String> = all.iter().enumerate().filter(|(j, _)| *j != i).flat_map(|(_, b)| b).collect();
        (mine.iter().map(|d| size(d)).sum(), mine.iter().filter(|d| !others.contains(d)).map(|d| size(d)).sum())
    }).collect()
}
pub fn remove(name: &str) -> R<()> {
    check_name(name)?;
    let dir = app_dir(name);
    if !dir.exists() { return Err(format!("{name}: not installed (pisco list)")); }
    io(std::fs::remove_dir_all(&dir), &dir)?;
    gc()
}
/// Removes the blobs no installed app names
fn gc() -> R<()> {
    let keep: BTreeSet<String> = installed()?.iter().flat_map(|a| blobs_of(&a.image)).collect();
    let blobs = home().join("blobs/sha256");
    let Ok(rd) = std::fs::read_dir(&blobs) else { return Ok(()) };
    for e in rd.filter_map(|e| e.ok()) {
        if !keep.contains(&*e.file_name().to_string_lossy()) { io(std::fs::remove_file(e.path()), &e.path())?; }
    }
    Ok(())
}
