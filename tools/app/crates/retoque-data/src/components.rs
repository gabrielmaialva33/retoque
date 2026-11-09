//! Content-addressed copies of local and officially downloaded sm86 releases.
//! Capability-relative I/O rejects links and keeps a swapped path from redirecting catalogue writes/removals.

use cap_fs_ext::{DirExt, FollowSymlinks, MetadataExt, OpenOptionsFollowExt, OpenOptionsSyncExt};
use cap_std::fs::{Dir, OpenOptions};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, HashSet},
    fs,
    io::{self, Read, Write},
    path::{Path, PathBuf},
};

const META: &str = "component.json";
const TREES: [&str; 5] = ["backend", "kernels", "runtime", "xefg", "sr"];
pub(crate) const REQUIRED: [&str; 6] = [
    "version.dll",
    "dlssg_sm86.ini",
    "dlssg_sm86/backend/sm86_backend.dll",
    "dlssg_sm86/kernels/fg_kernels.dll",
    "dlssg_sm86/runtime/nvngx_dlssg.dll",
    "dlssg_sm86/xefg/xefg_kernels.dll",
];
const MAX_FILES: usize = 4096;
const MAX_BYTES: u64 = 4 * 1024 * 1024 * 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum ComponentKind {
    DlssgSm86,
    ReShade,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Component {
    pub id: String,
    pub kind: ComponentKind,
    pub version: Option<String>,
    pub label: String,
    pub root: PathBuf,
    pub files: Vec<String>,
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
struct FileRecord {
    sha256: String,
    size: u64,
}
#[derive(Serialize, Deserialize)]
struct Manifest {
    schema: u32,
    kind: ComponentKind,
    label: String,
    version: Option<String>,
    files: BTreeMap<String, FileRecord>,
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}
fn valid_id(id: &str) -> bool {
    id.len() == 64 && id.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}
fn safe_name(name: &str) -> bool {
    let stem = name.split('.').next().unwrap_or_default().to_ascii_uppercase();
    !name.is_empty()
        && name != "."
        && name != ".."
        && !name.ends_with(['.', ' '])
        && name.len() <= 200
        && name
            .bytes()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'-'))
        && !matches!(
            stem.as_str(),
            "CON"
                | "PRN"
                | "AUX"
                | "NUL"
                | "COM1"
                | "COM2"
                | "COM3"
                | "COM4"
                | "COM5"
                | "COM6"
                | "COM7"
                | "COM8"
                | "COM9"
                | "LPT1"
                | "LPT2"
                | "LPT3"
                | "LPT4"
                | "LPT5"
                | "LPT6"
                | "LPT7"
                | "LPT8"
                | "LPT9"
        )
}
pub fn allowed_path(path: &str) -> bool {
    if matches!(path, "version.dll" | "dlssg_sm86.ini") {
        return true;
    }
    let parts: Vec<_> = path.split('/').collect();
    parts.len() >= 3
        && parts.len() <= 16
        && parts[0] == "dlssg_sm86"
        && TREES.contains(&parts[1])
        && parts.iter().all(|p| safe_name(p))
        && !parts.last().unwrap().to_ascii_lowercase().starts_with("nvngx_dlssnr")
}

pub(crate) struct HeldDir {
    pub(crate) root: PathBuf,
    pub(crate) dir: Dir,
    #[cfg(windows)]
    _ancestors: Vec<Dir>,
}
impl HeldDir {
    pub(crate) fn open(path: &Path) -> io::Result<Self> {
        if fs::symlink_metadata(path)?.file_type().is_symlink() {
            return Err(invalid("component root is a link"));
        }
        let root = fs::canonicalize(path)?;
        #[cfg(not(windows))]
        let dir = Dir::open_ambient_dir(&root, cap_std::ambient_authority())?;
        #[cfg(windows)]
        let (dir, ancestors) = {
            let mut ancestors: Vec<Dir> = Vec::new();
            let mut volume = PathBuf::new();
            for component in root.components() {
                match component {
                    std::path::Component::Prefix(_) => volume.push(component),
                    std::path::Component::RootDir => {
                        volume.push(component);
                        ancestors.push(Dir::open_ambient_dir(&volume, cap_std::ambient_authority())?);
                    }
                    std::path::Component::Normal(name) => {
                        let parent = ancestors
                            .last()
                            .ok_or_else(|| invalid("component root is not absolute"))?;
                        ancestors.push(parent.open_dir_nofollow(name)?);
                    }
                    _ => return Err(invalid("component root is not canonical")),
                }
            }
            let dir = ancestors
                .pop()
                .ok_or_else(|| invalid("component root is not absolute"))?;
            (dir, ancestors)
        };
        Ok(Self {
            root,
            dir,
            #[cfg(windows)]
            _ancestors: ancestors,
        })
    }
    pub(crate) fn current(&self) -> io::Result<()> {
        let current = Dir::open_ambient_dir(&self.root, cap_std::ambient_authority())?.dir_metadata()?;
        let held = self.dir.dir_metadata()?;
        if current.dev() != held.dev() || current.ino() != held.ino() {
            return Err(invalid("component directory was replaced; reopen it"));
        }
        Ok(())
    }
}
pub(crate) fn open_regular(dir: &Dir, path: &str) -> io::Result<cap_std::fs::File> {
    let mut parts = path.split('/').peekable();
    let mut parent = dir.try_clone()?;
    while let Some(part) = parts.next() {
        if !safe_name(part) {
            return Err(invalid("unsafe component path"));
        }
        if parts.peek().is_some() {
            parent = parent.open_dir_nofollow(part)?;
        } else {
            let file = parent.open_with(
                part,
                OpenOptions::new().read(true).follow(FollowSymlinks::No).nonblock(true),
            )?;
            if !file.metadata()?.is_file() {
                return Err(invalid("component member is not a regular file"));
            }
            return Ok(file);
        }
    }
    Err(invalid("empty component path"))
}
fn walk(dir: &Dir, prefix: &str, paths: &mut Vec<String>, names: &mut HashSet<String>) -> io::Result<()> {
    if prefix.split('/').count() > 15 {
        return Err(invalid("component tree is too deep"));
    }
    for entry in dir.entries()? {
        let entry = entry?;
        let name = entry.file_name();
        let name = name
            .to_str()
            .filter(|n| safe_name(n))
            .ok_or_else(|| invalid("unsafe component filename"))?;
        let relative = format!("{prefix}/{name}");
        if !names.insert(relative.to_ascii_lowercase()) {
            return Err(invalid("component paths collide on Windows"));
        }
        let kind = entry.file_type()?;
        if kind.is_dir() {
            walk(&dir.open_dir_nofollow(name)?, &relative, paths, names)?;
        } else if kind.is_file() && allowed_path(&relative) {
            paths.push(relative);
        } else {
            return Err(invalid(
                "component tree contains a link, special file or forbidden model",
            ));
        }
        if paths.len() > MAX_FILES || names.len() > MAX_FILES {
            return Err(invalid("component has too many files"));
        }
    }
    Ok(())
}
fn members(dir: &Dir) -> io::Result<Vec<String>> {
    let mut paths = vec!["version.dll".into(), "dlssg_sm86.ini".into()];
    let mut names = HashSet::new();
    let tree = dir.open_dir_nofollow("dlssg_sm86")?;
    for name in TREES {
        // SR is an optional runtime feature; old local FG-only imports remain supported.
        if name == "sr" && !tree.try_exists(name)? {
            continue;
        }
        walk(
            &tree.open_dir_nofollow(name)?,
            &format!("dlssg_sm86/{name}"),
            &mut paths,
            &mut names,
        )?;
    }
    paths.sort();
    if REQUIRED.iter().any(|name| !paths.iter().any(|p| p == name)) {
        return Err(invalid("incomplete sm86 external-runtime release"));
    }
    Ok(paths)
}
fn manifest_id(files: &BTreeMap<String, FileRecord>) -> String {
    let mut hash = Sha256::new();
    for (path, file) in files {
        hash.update((path.len() as u64).to_le_bytes());
        hash.update(path.as_bytes());
        hash.update(file.sha256.as_bytes());
    }
    format!("{:x}", hash.finalize())
}
fn scan(
    dir: &Dir,
    paths: &[String],
    destination: Option<&Dir>,
) -> io::Result<(BTreeMap<String, FileRecord>, Option<String>)> {
    let mut files = BTreeMap::new();
    let mut version = None;
    let mut total = 0u64;
    for path in paths {
        let mut input = open_regular(dir, path)?;
        if input.metadata()?.len() == 0 {
            return Err(invalid("component member is empty"));
        }
        let mut output = if let Some(out) = destination {
            if let Some(parent) = Path::new(path).parent()
                && !parent.as_os_str().is_empty()
            {
                out.create_dir_all(parent)?;
            }
            Some(out.open_with(path, OpenOptions::new().write(true).create_new(true))?)
        } else {
            None
        };
        let mut digest = Sha256::new();
        let mut bytes = [0; 65536];
        let mut size = 0u64;
        let mut tail = Vec::new();
        loop {
            let n = input.read(&mut bytes)?;
            if n == 0 {
                break;
            }
            total = total
                .checked_add(n as u64)
                .ok_or_else(|| invalid("component size overflow"))?;
            if total > MAX_BYTES {
                return Err(invalid("component exceeds size limit"));
            }
            if size == 0 && path.ends_with(".dll") && !bytes[..n].starts_with(b"MZ") {
                return Err(invalid("component DLL has no DOS executable header"));
            }
            size += n as u64;
            digest.update(&bytes[..n]);
            if let Some(out) = &mut output {
                out.write_all(&bytes[..n])?;
            }
            if path == "version.dll" {
                tail.extend_from_slice(&bytes[..n]);
                if tail.windows(8).any(|s| s == [0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0]) {
                    version = crate::models::pe_file_version(&tail);
                }
                if tail.len() > 15 {
                    tail.drain(..tail.len() - 15);
                }
            }
        }
        if size == 0 {
            return Err(invalid("component member became empty"));
        }
        if let Some(out) = output {
            out.sync_all()?;
        }
        files.insert(
            path.clone(),
            FileRecord {
                sha256: format!("{:x}", digest.finalize()),
                size,
            },
        );
    }
    Ok((files, version))
}
fn label(value: Option<&str>) -> io::Result<String> {
    let value = value.unwrap_or("DLSS-G for sm86").trim();
    if value.is_empty() || value.len() > 200 || value.chars().any(char::is_control) {
        return Err(invalid("invalid component label"));
    }
    Ok(value.into())
}
fn clean_version(value: Option<&str>) -> io::Result<Option<String>> {
    let Some(v) = value.map(str::trim).filter(|v| !v.is_empty()) else {
        return Ok(None);
    };
    if v.len() > 64 || v.chars().any(char::is_control) {
        return Err(invalid("invalid component version"));
    }
    Ok(Some(v.into()))
}

/// Read-only validation/hash for CLI dry-run; no catalogue is opened or created. Extra release documentation,
/// archives and alternative proxy loaders are intentionally not deployment members.
pub fn inspect_dir(source: &Path, name: &str) -> io::Result<Component> {
    let held = HeldDir::open(source)?;
    let paths = members(&held.dir)?;
    let (files, version) = scan(&held.dir, &paths, None)?;
    held.current()?;
    Ok(Component {
        id: manifest_id(&files),
        kind: ComponentKind::DlssgSm86,
        version,
        label: label(Some(name))?,
        root: held.root,
        files: paths,
    })
}

pub struct ComponentCatalog {
    held: HeldDir,
}
struct Staging<'a> {
    parent: &'a Dir,
    name: String,
    dir: Option<Dir>,
}
impl Drop for Staging<'_> {
    fn drop(&mut self) {
        drop(self.dir.take());
        let _ = self.parent.remove_dir_all(&self.name);
    }
}
impl ComponentCatalog {
    pub fn open(root: Option<&Path>) -> io::Result<Self> {
        let root = match root {
            Some(p) => p.to_owned(),
            None => {
                let base = std::env::var_os(if cfg!(windows) { "LOCALAPPDATA" } else { "HOME" })
                    .filter(|p| !p.is_empty())
                    .ok_or_else(|| invalid("component data directory unavailable"))?;
                PathBuf::from(base).join(if cfg!(windows) {
                    "Retoque/components"
                } else {
                    ".local/share/retoque/components"
                })
            }
        };
        fs::create_dir_all(&root)?;
        Ok(Self {
            held: HeldDir::open(&root)?,
        })
    }
    pub fn list(&self) -> io::Result<Vec<Component>> {
        self.held.current()?;
        let mut components = Vec::new();
        for entry in self.held.dir.entries()? {
            let name = entry?.file_name();
            if let Some(id) = name.to_str().filter(|s| valid_id(s))
                && let Ok(entry) = self.entry(id)
            {
                components.push(entry);
            }
        }
        components.sort_by(|a, b| a.id.cmp(&b.id));
        Ok(components)
    }
    fn entry(&self, id: &str) -> io::Result<Component> {
        if !valid_id(id) {
            return Err(invalid("invalid component id"));
        }
        let dir = self.held.dir.open_dir_nofollow(id)?;
        let mut bytes = Vec::new();
        open_regular(&dir, META)?.take(2_097_153).read_to_end(&mut bytes)?;
        if bytes.len() > 2_097_152 {
            return Err(invalid("component manifest too large"));
        }
        let manifest: Manifest = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
        if manifest.schema != 1 || manifest.files.len() > MAX_FILES || manifest.files.keys().any(|p| !allowed_path(p)) {
            return Err(invalid("invalid component manifest"));
        }
        let paths = members(&dir)?;
        if paths != manifest.files.keys().cloned().collect::<Vec<_>>() {
            return Err(invalid("component members changed"));
        }
        let (files, pe_version) = scan(&dir, &paths, None)?;
        let manifest_version = clean_version(manifest.version.as_deref())?;
        if files != manifest.files
            || manifest_id(&files) != id
            || (pe_version.is_some() && pe_version != manifest_version)
        {
            return Err(invalid("component integrity check failed"));
        }
        let version = pe_version.or(manifest_version);
        self.held.current()?;
        Ok(Component {
            id: id.into(),
            kind: manifest.kind,
            version,
            label: label(Some(&manifest.label))?,
            root: self.held.root.join(id),
            files: paths,
        })
    }
    pub fn import_dir(&self, source: &Path, name: Option<&str>) -> io::Result<Component> {
        self.import_dir_with_version(source, name, None)
    }
    pub fn import_dir_with_version(
        &self,
        source: &Path,
        name: Option<&str>,
        release_version: Option<&str>,
    ) -> io::Result<Component> {
        self.held.current()?;
        let source = HeldDir::open(source)?;
        let paths = members(&source.dir)?;
        let label = label(name)?;
        let fallback_version = clean_version(release_version)?;
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let mut stage = None;
        for _ in 0..128 {
            let serial = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let name = format!(".import-{}-{serial}", std::process::id());
            match self.held.dir.create_dir(&name) {
                Ok(()) => {
                    let dir = self.held.dir.open_dir_nofollow(&name)?;
                    stage = Some(Staging {
                        parent: &self.held.dir,
                        name,
                        dir: Some(dir),
                    });
                    break;
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
        let mut stage = stage.ok_or_else(|| invalid("component staging directory unavailable"))?;
        let dir = stage
            .dir
            .as_ref()
            .ok_or_else(|| invalid("component staging directory unavailable"))?;
        let (files, pe_version) = scan(&source.dir, &paths, Some(dir))?;
        source.current()?;
        let version = pe_version.or_else(|| fallback_version.clone());
        let id = manifest_id(&files);
        let manifest = Manifest {
            schema: 1,
            kind: ComponentKind::DlssgSm86,
            label,
            version,
            files,
        };
        let mut metadata = dir.open_with(META, OpenOptions::new().write(true).create_new(true))?;
        metadata.write_all(&serde_json::to_vec_pretty(&manifest).map_err(io::Error::other)?)?;
        metadata.sync_all()?;
        drop(metadata);
        // The parent stays pinned while Windows handles to the staged directory close for rename.
        drop(stage.dir.take());
        let with_fallback = |mut c: Component| {
            if c.version.is_none() {
                c.version = fallback_version.clone();
            }
            c
        };
        if self.held.dir.try_exists(&id)? {
            return self.entry(&id).map(with_fallback);
        }
        match self.held.dir.rename(&stage.name, &self.held.dir, &id) {
            Ok(()) => self.entry(&id).map(with_fallback),
            Err(_) if self.held.dir.try_exists(&id)? => self.entry(&id).map(with_fallback),
            Err(e) => Err(e),
        }
    }
    pub fn remove(&self, id: &str) -> io::Result<()> {
        if !valid_id(id) {
            return Err(invalid("invalid component id"));
        }
        self.held.current()?;
        match self.held.dir.symlink_metadata(id) {
            Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(e) => Err(e),
            Ok(m) if !m.file_type().is_dir() => Err(invalid("component entry is not a directory")),
            Ok(_) => self.held.dir.remove_dir_all(id),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture(root: &Path) {
        for path in REQUIRED {
            let path = root.join(path);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, b"MZ synthetic user release").unwrap();
        }
        fs::create_dir_all(root.join("alternatives")).unwrap();
        fs::write(root.join("alternatives/dxgi.dll"), b"excluded proxy").unwrap();
        fs::write(root.join("README.md"), b"excluded documentation").unwrap();
    }
    #[test]
    fn inspect_import_deduplicate_verify_and_remove() {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let inspected = inspect_dir(&source, "Synthetic").unwrap();
        assert_eq!(inspected.files.len(), 6);
        assert!(!temp.path().join("catalog").exists());
        let catalog = ComponentCatalog::open(Some(&temp.path().join("catalog"))).unwrap();
        let first = catalog.import_dir(&source, Some("Synthetic")).unwrap();
        assert_eq!(first.id, inspected.id);
        assert_eq!(first.files, inspected.files);
        assert_eq!(
            catalog.import_dir(&source, Some("Ignored second label")).unwrap(),
            first
        );
        assert_eq!(catalog.list().unwrap(), vec![first.clone()]);
        assert!(!first.root.join("alternatives").exists());
        fs::write(first.root.join("version.dll"), b"changed").unwrap();
        assert!(catalog.list().unwrap().is_empty());
        assert!(catalog.import_dir(&source, None).is_err());
        assert!(catalog.remove("../source").is_err());
        catalog.remove(&first.id).unwrap();
        catalog.remove(&first.id).unwrap();
        assert!(source.join("version.dll").is_file());
    }
    #[test]
    fn missing_required_unsafe_and_case_colliding_members_are_refused() {
        let temp = tempfile::tempdir().unwrap();
        fixture(temp.path());
        fs::remove_file(temp.path().join(REQUIRED[5])).unwrap();
        assert!(inspect_dir(temp.path(), "Synthetic").is_err());
        fixture(temp.path());
        fs::write(temp.path().join("dlssg_sm86/backend/NUL.dll"), b"bad").unwrap();
        assert!(inspect_dir(temp.path(), "Synthetic").is_err());
        fs::remove_file(temp.path().join("dlssg_sm86/backend/NUL.dll")).unwrap();
        fs::write(
            temp.path().join("dlssg_sm86/backend/nvngx_dlssnr.dll"),
            b"never import NR models here",
        )
        .unwrap();
        assert!(inspect_dir(temp.path(), "Synthetic").is_err());
        assert!(!allowed_path("dlssg_sm86/backend/../../outside.dll"));
        assert!(!allowed_path("dlssg_sm86/backend/file:stream"));
        assert!(allowed_path("dlssg_sm86/sr/sr_kernels.dll"));
        assert!(allowed_path("dlssg_sm86/sr/nvngx_dlss.dll"));
        assert!(!allowed_path("dlssg_sm86/sr/nvngx_dlssnr.dll"));
        assert!(!allowed_path("alternatives/dxgi.dll"));
        assert!(!allowed_path("archive/0.3.5/version.dll"));
    }
    #[cfg(unix)]
    #[test]
    fn failed_import_cleans_staging_and_never_follows_a_source_link() {
        use std::os::unix::fs::symlink;
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let catalog_root = temp.path().join("catalog");
        let catalog = ComponentCatalog::open(Some(&catalog_root)).unwrap();
        let outside = temp.path().join("private");
        fs::write(&outside, b"outside sentinel").unwrap();
        fs::remove_file(source.join("version.dll")).unwrap();
        symlink(&outside, source.join("version.dll")).unwrap();
        assert!(catalog.import_dir(&source, None).is_err());
        assert_eq!(fs::read_dir(&catalog_root).unwrap().count(), 0);
        assert_eq!(fs::read(&outside).unwrap(), b"outside sentinel");
    }

    #[cfg(unix)]
    #[test]
    fn case_collision_and_fifo_are_rejected_without_blocking() {
        let temp = tempfile::tempdir().unwrap();
        fixture(temp.path());
        let duplicate = temp.path().join("dlssg_sm86/backend/SM86_BACKEND.dll");
        fs::write(&duplicate, b"MZ second").unwrap();
        assert!(inspect_dir(temp.path(), "Synthetic").is_err());
        fs::remove_file(duplicate).unwrap();
        let fifo = temp.path().join("dlssg_sm86/backend/pipe.dll");
        assert!(
            std::process::Command::new("mkfifo")
                .arg(&fifo)
                .status()
                .unwrap()
                .success()
        );
        assert!(inspect_dir(temp.path(), "Synthetic").is_err());
    }

    #[cfg(unix)]
    #[test]
    fn links_and_root_replacement_do_not_redirect_catalogue_operations() {
        use std::os::unix::fs::symlink;
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("source");
        fixture(&source);
        let outside = temp.path().join("outside");
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("sentinel"), b"private").unwrap();
        symlink(outside.join("sentinel"), source.join("dlssg_sm86/backend/linked.dll")).unwrap();
        assert!(inspect_dir(&source, "Synthetic").is_err());
        fs::remove_file(source.join("dlssg_sm86/backend/linked.dll")).unwrap();
        let root = temp.path().join("catalog");
        let catalog = ComponentCatalog::open(Some(&root)).unwrap();
        let entry = catalog.import_dir(&source, None).unwrap();
        fs::rename(&root, temp.path().join("moved")).unwrap();
        symlink(&outside, &root).unwrap();
        assert!(catalog.import_dir(&source, None).is_err());
        assert!(catalog.list().is_err());
        assert!(catalog.remove(&entry.id).is_err());
        assert_eq!(fs::read(outside.join("sentinel")).unwrap(), b"private");
    }
}
