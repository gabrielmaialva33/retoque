//! User-supplied model files only. Imports are content-addressed and published after both the copy and metadata
//! are complete, so an interrupted import never appears in the catalog.

use cap_fs_ext::{DirExt, FollowSymlinks, MetadataExt, OpenOptionsFollowExt, OpenOptionsSyncExt};
use cap_std::fs::{Dir, OpenOptions};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const MODEL: &str = "nvngx_dlssnr.dll";
const METADATA: &str = "metadata.json";
const SIGNATURE: [u8; 8] = [0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0];

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Model {
    pub compatibility: crate::model_compat::ModelCompatibility,
    pub sha256: String,
    pub version: Option<String>,
    pub size: u64,
    pub label: String,
    pub path: PathBuf,
}

#[derive(Serialize, Deserialize)]
struct Metadata {
    schema: u32,
    label: String,
    original_file_name: String,
    imported_unix_seconds: u64,
    version: Option<String>,
    size: u64,
}

pub struct Catalog {
    root: PathBuf,
    dir: Dir,
    // Windows path-backed directory operations must retain every ancestor without FILE_SHARE_DELETE.
    #[cfg(windows)]
    _ancestors: Vec<Dir>,
}

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn valid_hash(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn regular_file(dir: &Dir, name: impl AsRef<Path>) -> io::Result<cap_std::fs::File> {
    let file = dir.open_with(
        name,
        OpenOptions::new().read(true).follow(FollowSymlinks::No).nonblock(true),
    )?;
    if !file.metadata()?.is_file() {
        return Err(invalid("catalog entry is not a regular file"));
    }
    Ok(file)
}

// Every lookup, publication and cleanup stays relative to the retained root handle, even if any ancestor
// is renamed or replaced. Only the initial open uses ambient authority; the displayed path is not used for I/O.
struct Stage<'a> {
    root: &'a Dir,
    name: String,
    dir: Option<Dir>,
}

impl<'a> Stage<'a> {
    fn new(root: &'a Dir) -> io::Result<Self> {
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        for _ in 0..128 {
            let serial = NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let time = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(io::Error::other)?
                .as_nanos();
            let name = format!(".import-{}-{time}-{serial}", std::process::id());
            match root.create_dir(&name) {
                Ok(()) => {
                    return Ok(Self {
                        root,
                        dir: Some(root.open_dir_nofollow(&name)?),
                        name,
                    });
                }
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(e),
            }
        }
        Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            "model staging name is occupied",
        ))
    }
}

impl Drop for Stage<'_> {
    fn drop(&mut self) {
        drop(self.dir.take());
        let _ = self.root.remove_dir_all(&self.name);
    }
}

impl Catalog {
    /// Uses the documented platform location unless an explicit root is provided. No game folder is changed.
    pub fn open(root: Option<&Path>) -> io::Result<Catalog> {
        let root = match root {
            Some(path) => path.to_owned(),
            None => {
                let variable = if cfg!(windows) { "LOCALAPPDATA" } else { "HOME" };
                let base = std::env::var_os(variable)
                    .filter(|s| !s.is_empty())
                    .ok_or_else(|| io::Error::new(io::ErrorKind::NotFound, "model data folder is unavailable"))?;
                PathBuf::from(base).join(if cfg!(windows) {
                    "Retoque/models"
                } else {
                    ".local/share/retoque/models"
                })
            }
        };
        fs::create_dir_all(&root)?;
        let root = fs::canonicalize(root)?;
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
                            .ok_or_else(|| invalid("catalog root is not absolute"))?;
                        ancestors.push(parent.open_dir_nofollow(name)?);
                    }
                    _ => return Err(invalid("catalog root is not canonical")),
                }
            }
            let dir = ancestors.pop().ok_or_else(|| invalid("catalog root is not absolute"))?;
            (dir, ancestors)
        };
        Ok(Catalog {
            root,
            dir,
            #[cfg(windows)]
            _ancestors: ancestors,
        })
    }

    fn entry(&self, hash: &str) -> io::Result<Model> {
        if !valid_hash(hash) {
            return Err(invalid("invalid model sha256"));
        }
        let dir = self.dir.open_dir_nofollow(hash)?;
        let mut bytes = Vec::new();
        regular_file(&dir, METADATA)?.take(65537).read_to_end(&mut bytes)?;
        if bytes.len() > 65536 {
            return Err(invalid("model metadata is too large"));
        }
        let metadata: Metadata = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
        let path = self.root.join(hash).join(MODEL);
        let size = regular_file(&dir, MODEL)?.metadata()?.len();
        if metadata.schema != 1 || metadata.size != size {
            return Err(invalid("model metadata does not match the stored file"));
        }
        self.ensure_path_current()?;
        let compatibility = crate::model_compat::inspect(&mut regular_file(&dir, MODEL)?).unwrap_or_default();
        Ok(Model {
            compatibility,
            sha256: hash.to_owned(),
            version: metadata.version,
            size,
            label: metadata.label,
            path,
        })
    }

    pub fn list(&self) -> io::Result<Vec<Model>> {
        self.ensure_path_current()?;
        let mut models = Vec::new();
        for entry in self.dir.entries()? {
            let entry = entry?;
            let name = entry.file_name();
            if let Some(hash) = name.to_str().filter(|s| valid_hash(s)) {
                // One damaged entry (a half-deleted folder, edited metadata) must not hide every other model.
                if let Ok(model) = self.entry(hash) {
                    models.push(model);
                }
            }
        }
        models.sort_by(|a, b| a.sha256.cmp(&b.sha256));
        Ok(models)
    }

    /// Copies and hashes the same bytes. A repeated import preserves the first label and import time.
    pub fn import(&self, file: &Path, label: Option<&str>) -> io::Result<Model> {
        self.ensure_path_current()?;
        // Open the source without following a last-component link and without blocking on a swapped FIFO.
        let parent = file
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));
        let source_dir = Dir::open_ambient_dir(parent, cap_std::ambient_authority())?;
        let name = file.file_name().ok_or_else(|| invalid("model has no file name"))?;
        let mut input = regular_file(&source_dir, name)?;
        let mut stage = Stage::new(&self.dir)?;
        let mut copy = stage
            .dir
            .as_ref()
            .unwrap()
            .open_with(MODEL, OpenOptions::new().write(true).create_new(true))?;
        let mut hash = Sha256::new();
        let mut buffer = [0; 65536];
        let mut tail = Vec::new();
        let mut version = None;
        let mut size = 0u64;
        loop {
            let count = input.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            copy.write_all(&buffer[..count])?;
            hash.update(&buffer[..count]);
            size += count as u64;
            tail.extend_from_slice(&buffer[..count]);
            if tail.windows(8).any(|s| s == SIGNATURE) {
                version = pe_file_version(&tail);
            }
            if tail.len() > 15 {
                tail.drain(..tail.len() - 15);
            }
        }
        copy.sync_all()?;
        drop(copy);
        let hash = format!("{:x}", hash.finalize());
        if self.dir.try_exists(&hash)? {
            return self.verified_entry(&hash);
        }
        let original_file_name = file
            .file_name()
            .ok_or_else(|| invalid("model has no file name"))?
            .to_string_lossy()
            .into_owned();
        let metadata = Metadata {
            schema: 1,
            label: label.unwrap_or(&original_file_name).to_owned(),
            original_file_name,
            imported_unix_seconds: SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_err(io::Error::other)?
                .as_secs(),
            version,
            size,
        };
        let bytes = serde_json::to_vec_pretty(&metadata).map_err(io::Error::other)?;
        if bytes.len() > 65536 {
            return Err(invalid("model metadata is too large"));
        }
        let mut output = stage
            .dir
            .as_ref()
            .unwrap()
            .open_with(METADATA, OpenOptions::new().write(true).create_new(true))?;
        output.write_all(&bytes)?;
        output.sync_all()?;
        drop(output);
        // Windows directory handles intentionally deny rename until they are closed. The parent stays pinned.
        drop(stage.dir.take());
        match self.dir.rename(&stage.name, &self.dir, &hash) {
            Ok(()) => self.entry(&hash),
            Err(_) if self.dir.try_exists(&hash)? => self.verified_entry(&hash),
            Err(error) => Err(error),
        }
    }

    fn verified_entry(&self, hash: &str) -> io::Result<Model> {
        let model = self.entry(hash)?;
        let dir = self.dir.open_dir_nofollow(hash)?;
        let mut file = regular_file(&dir, MODEL)?;
        let mut digest = Sha256::new();
        let mut buffer = [0; 65536];
        loop {
            let count = file.read(&mut buffer)?;
            if count == 0 {
                break;
            }
            digest.update(&buffer[..count]);
        }
        if format!("{:x}", digest.finalize()) != hash {
            return Err(invalid("stored model hash mismatch"));
        }
        Ok(model)
    }

    // Reject a stale locator before handing a model path to callers. This check is not the I/O security
    // boundary: all catalog reads/writes/removals still use handles if a rename races after this check.
    fn ensure_path_current(&self) -> io::Result<()> {
        let current = Dir::open_ambient_dir(&self.root, cap_std::ambient_authority())?.dir_metadata()?;
        let held = self.dir.dir_metadata()?;
        if current.dev() != held.dev() || current.ino() != held.ino() {
            return Err(invalid("model catalog directory was replaced; reopen the catalog"));
        }
        Ok(())
    }

    /// Removes only a validated hash directory. Missing entries are already removed.
    pub fn remove(&self, sha256: &str) -> io::Result<()> {
        if !valid_hash(sha256) {
            return Err(invalid("invalid model sha256"));
        }
        match self.dir.symlink_metadata(sha256) {
            Err(error) if error.kind() == io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(error),
            Ok(metadata) if !metadata.file_type().is_dir() => Err(invalid("model entry is not a directory")),
            Ok(_) => self.dir.remove_dir_all(sha256),
        }
    }

    /// Catalog entries have absolute paths; Wine uses its standard root mapping on drive Z:.
    pub fn model_file_value(model: &Model, wine: bool) -> String {
        if wine {
            retoque_core::wine_path(&model.path)
        } else {
            model.path.to_string_lossy().into_owned()
        }
    }
}

/// Last VS_FIXEDFILEINFO signature wins, matching the model inspection tool's convention.
pub fn pe_file_version(bytes: &[u8]) -> Option<String> {
    let start = bytes.windows(SIGNATURE.len()).rposition(|window| window == SIGNATURE)?;
    let ms = u32::from_le_bytes(bytes.get(start + 8..start + 12)?.try_into().ok()?);
    let ls = u32::from_le_bytes(bytes.get(start + 12..start + 16)?.try_into().ok()?);
    Some(format!("{}.{}.{}.{}", ms >> 16, ms & 0xffff, ls >> 16, ls & 0xffff))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(windows)]
    #[test]
    fn catalog_pins_root_and_ancestors_until_closed() {
        let temp = tempfile::tempdir().unwrap();
        let parent = temp.path().join("parent");
        let root = parent.join("catalog");
        let catalog = Catalog::open(Some(&root)).unwrap();
        assert!(fs::rename(&root, parent.join("moved")).is_err());
        assert!(fs::rename(&parent, temp.path().join("moved-parent")).is_err());
        drop(catalog);
        fs::rename(&parent, temp.path().join("moved-parent")).unwrap();
    }

    #[cfg(unix)]
    #[test]
    fn review_replaced_root_or_ancestor_never_reads_writes_or_removes_outside() {
        for replace_ancestor in [false, true] {
            let temp = tempfile::tempdir().unwrap();
            let parent = temp.path().join("parent");
            let root = parent.join("catalog");
            let catalog = Catalog::open(Some(&root)).unwrap();
            let source = temp.path().join("input.dll");
            fs::write(&source, b"owned-model").unwrap();
            let original = catalog.import(&source, Some("owned")).unwrap();
            let outside = temp.path().join("outside");
            let outside_root = if replace_ancestor {
                outside.join("catalog")
            } else {
                outside.clone()
            };
            fs::create_dir_all(outside_root.join(&original.sha256)).unwrap();
            fs::write(outside_root.join(&original.sha256).join("unrelated.txt"), b"sentinel").unwrap();
            fs::write(outside_root.join(&original.sha256).join(MODEL), b"owned-model").unwrap();
            let mut metadata: Metadata =
                serde_json::from_slice(&fs::read(root.join(&original.sha256).join(METADATA)).unwrap()).unwrap();
            metadata.label = "outside".into();
            fs::write(
                outside_root.join(&original.sha256).join(METADATA),
                serde_json::to_vec(&metadata).unwrap(),
            )
            .unwrap();
            let moved = if replace_ancestor { &parent } else { &root };
            fs::rename(moved, temp.path().join("held")).unwrap();
            std::os::unix::fs::symlink(&outside, moved).unwrap();
            // A handle anchored to the original catalog may continue there, or reject the moved path.
            // It must never enumerate, publish or delete through the replacement.
            if let Ok(entries) = catalog.list() {
                assert!(entries.iter().all(|entry| entry.label == "owned"));
            }
            fs::write(&source, b"new-model").unwrap();
            let _ = catalog.import(&source, None);
            let _ = catalog.remove(&original.sha256);
            assert_eq!(
                fs::read(outside_root.join(&original.sha256).join("unrelated.txt")).unwrap(),
                b"sentinel"
            );
            assert_eq!(fs::read_dir(outside_root).unwrap().count(), 1);
        }
    }

    fn version(ms: u32, ls: u32) -> Vec<u8> {
        [SIGNATURE.as_slice(), &ms.to_le_bytes(), &ls.to_le_bytes()].concat()
    }

    #[test]
    fn pe_last_match_and_truncation() {
        let mut bytes = version(1 << 16, 2);
        bytes.extend(version((310 << 16) | 9, 16 << 16));
        assert_eq!(pe_file_version(&bytes).as_deref(), Some("310.9.16.0"));
        bytes.extend(SIGNATURE);
        assert_eq!(pe_file_version(&bytes), None);
        assert_eq!(pe_file_version(b"not a PE"), None);
        let mut wrong = version(0, 0);
        wrong[6] = 2;
        assert_eq!(pe_file_version(&wrong), None);
    }

    #[test]
    fn import_deduplicates_persists_and_removes() {
        let temp = tempfile::tempdir().unwrap();
        let file = temp.path().join("user-model.dll");
        fs::write(&file, b"abc").unwrap();
        let root = temp.path().join("catalog");
        let catalog = Catalog::open(Some(&root)).unwrap();
        let model = catalog.import(&file, Some("One\nline\tlabel")).unwrap();
        assert_eq!(
            model.sha256,
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
        assert_eq!(model.size, 3);
        assert!(model.path.is_absolute());
        assert_eq!(fs::read(&model.path).unwrap(), b"abc");
        assert_eq!(catalog.import(&file, Some("changed")).unwrap(), model);
        assert_eq!(Catalog::open(Some(&root)).unwrap().list().unwrap(), vec![model.clone()]);
        let meta: Metadata =
            serde_json::from_slice(&fs::read(root.join(&model.sha256).join(METADATA)).unwrap()).unwrap();
        assert_eq!(meta.original_file_name, "user-model.dll");
        assert!(meta.imported_unix_seconds > 0);
        assert_eq!(Catalog::model_file_value(&model, false), model.path.to_string_lossy());
        assert_eq!(
            Catalog::model_file_value(&model, true),
            retoque_core::wine_path(&model.path)
        );
        assert!(catalog.remove("../escape").is_err());
        fs::write(&model.path, b"bad").unwrap();
        assert!(catalog.import(&file, None).is_err());
        catalog.remove(&model.sha256).unwrap();
        catalog.remove(&model.sha256).unwrap();
        assert!(catalog.list().unwrap().is_empty());
        assert_eq!(fs::read(file).unwrap(), b"abc");
    }

    #[cfg(unix)]
    #[test]
    fn catalog_rejects_linked_metadata_and_model_files() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("catalog");
        let source = temp.path().join("input.dll");
        fs::write(&source, b"owned-model").unwrap();
        let catalog = Catalog::open(Some(&root)).unwrap();
        let model = catalog.import(&source, None).unwrap();
        for name in [MODEL, METADATA] {
            let path = root.join(&model.sha256).join(name);
            let bytes = fs::read(&path).unwrap();
            let outside = temp.path().join("outside");
            fs::write(&outside, &bytes).unwrap();
            fs::remove_file(&path).unwrap();
            std::os::unix::fs::symlink(&outside, &path).unwrap();
            assert!(catalog.list().unwrap().is_empty());
            assert!(catalog.import(&source, None).is_err());
            assert_eq!(fs::read(&outside).unwrap(), bytes);
            fs::remove_file(&path).unwrap();
            fs::write(path, bytes).unwrap();
        }
    }

    #[test]
    fn streamed_version_crosses_buffer_boundaries() {
        let temp = tempfile::tempdir().unwrap();
        let catalog = Catalog::open(Some(&temp.path().join("catalog"))).unwrap();
        for offset in 65521..65537 {
            let mut bytes = vec![0; offset];
            bytes.extend(version((310 << 16) | 9, 16 << 16));
            let source = temp.path().join("input.dll");
            fs::write(&source, bytes).unwrap();
            assert_eq!(
                catalog.import(&source, None).unwrap().version.as_deref(),
                Some("310.9.16.0")
            );
        }
    }

    #[cfg(unix)]
    #[test]
    fn refuses_linked_hash_directory() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("catalog");
        let catalog = Catalog::open(Some(&root)).unwrap();
        let hash = "a".repeat(64);
        std::os::unix::fs::symlink(temp.path(), root.join(&hash)).unwrap();
        // A linked entry is never listed (and so never offered to a game); the rest of the catalog still is.
        assert!(catalog.list().unwrap().iter().all(|m| m.sha256 != hash));
        assert!(catalog.remove(&hash).is_err());
        assert!(temp.path().exists());
    }
}
