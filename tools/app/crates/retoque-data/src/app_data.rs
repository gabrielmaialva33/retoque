//! The app's own data folder: what it takes on disk, and the staging left behind by an operation that never
//! finished (a crash or power loss during an import, download or unpack).
//!
//! Every staging folder or file the app creates has one of a few reserved names (`.import-…`, `.download-…`,
//! `.unpack-…`, `.prepare-…`, `*.json.tmp`) and is published by a rename or removed when the operation ends, so
//! one that is still there a day later belongs to nobody. Cleanup looks only for those names, only in the data
//! folder and the folders directly inside it, never follows a link, and keeps anything younger than `max_age` so a
//! running operation (in this app or a second copy of it) is never touched. Catalogs, models, components,
//! settings and logs have other names and are never candidates.
use std::{
    fs, io,
    path::Path,
    time::{Duration, SystemTime},
};

const STAGING_PREFIXES: &[&str] = &[".import-", ".download-", ".unpack-", ".prepare-"];
const STAGING_SUFFIX: &str = ".json.tmp";

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Cleaned {
    pub removed: usize,
    pub bytes: u64,
}

fn is_staging(name: &str) -> bool {
    STAGING_PREFIXES.iter().any(|p| name.starts_with(p)) || name.ends_with(STAGING_SUFFIX)
}

/// Removes staging older than `max_age` from `data` and the folders directly inside it.
pub fn clean_stale(data: &Path, now: SystemTime, max_age: Duration) -> io::Result<Cleaned> {
    let mut cleaned = Cleaned::default();
    let Ok(meta) = fs::symlink_metadata(data) else {
        return Ok(cleaned);
    };
    if !meta.is_dir() {
        return Ok(cleaned);
    }
    let mut folders = vec![data.to_path_buf()];
    for entry in fs::read_dir(data)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() && !is_staging(&entry.file_name().to_string_lossy()) {
            folders.push(entry.path());
        }
    }
    for folder in folders {
        let Ok(entries) = fs::read_dir(&folder) else { continue };
        for entry in entries.flatten() {
            let name = entry.file_name();
            if !is_staging(&name.to_string_lossy()) {
                continue;
            }
            let path = entry.path();
            // symlink_metadata: a link named like staging is left alone, and never followed.
            let Ok(meta) = fs::symlink_metadata(&path) else {
                continue;
            };
            let file_type = meta.file_type();
            if file_type.is_symlink() || !(file_type.is_dir() || file_type.is_file()) {
                continue;
            }
            let age = meta
                .modified()
                .ok()
                .and_then(|m| now.duration_since(m).ok())
                .unwrap_or_default();
            if age < max_age {
                continue;
            }
            let bytes = size(&path);
            let result = if file_type.is_dir() {
                fs::remove_dir_all(&path)
            } else {
                fs::remove_file(&path)
            };
            if result.is_ok() {
                cleaned.removed += 1;
                cleaned.bytes += bytes;
            }
        }
    }
    Ok(cleaned)
}

/// Bytes under `path`, without following links (a link counts as itself).
pub fn size(path: &Path) -> u64 {
    let Ok(meta) = fs::symlink_metadata(path) else { return 0 };
    if !meta.is_dir() {
        return meta.len();
    }
    fs::read_dir(path)
        .map(|entries| entries.flatten().map(|e| size(&e.path())).sum())
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn only_old_staging_goes_and_everything_else_stays() {
        let temp = tempfile::tempdir().unwrap();
        let data = temp.path();
        let models = data.join("models");
        fs::create_dir_all(models.join("abc123")).unwrap();
        fs::write(models.join("abc123/nvngx_dlssnr.dll"), b"model").unwrap();
        // Left behind by operations that died: an import folder with a file in it, a download, a store temp file.
        let import = models.join(".import-4242-1-0");
        fs::create_dir(&import).unwrap();
        fs::write(import.join("nvngx_dlssnr.dll"), b"half a model").unwrap();
        fs::create_dir_all(data.join("dlss/.download-99-0")).unwrap();
        fs::write(data.join("file-facts.json.tmp"), b"{}").unwrap();
        fs::write(data.join("file-facts.json"), b"{}").unwrap();
        fs::write(data.join("app.ron"), b"()").unwrap();
        let day = Duration::from_secs(86_400);

        // Just made: it may belong to an operation still running, so nothing goes.
        let fresh = clean_stale(data, SystemTime::now(), day).unwrap();
        assert_eq!(fresh, Cleaned::default());
        assert!(import.is_dir());

        // Two days on, the three staging items go; catalog, stores and settings stay.
        let later = clean_stale(data, SystemTime::now() + 2 * day, day).unwrap();
        assert_eq!(later.removed, 3);
        assert_eq!(later.bytes, 12 + 2);
        assert!(!import.exists());
        assert!(!data.join("dlss/.download-99-0").exists());
        assert!(!data.join("file-facts.json.tmp").exists());
        assert!(
            models.join("abc123/nvngx_dlssnr.dll").is_file(),
            "a catalog entry is never staging"
        );
        assert!(data.join("file-facts.json").is_file());
        assert!(data.join("app.ron").is_file());
    }

    #[cfg(unix)]
    #[test]
    fn a_link_named_like_staging_is_never_followed() {
        let temp = tempfile::tempdir().unwrap();
        let outside = temp.path().join("outside");
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("keep.txt"), b"user data").unwrap();
        let data = temp.path().join("data");
        fs::create_dir(&data).unwrap();
        std::os::unix::fs::symlink(&outside, data.join(".import-1-1")).unwrap();
        let later = SystemTime::now() + Duration::from_secs(10 * 86_400);
        clean_stale(&data, later, Duration::from_secs(86_400)).unwrap();
        assert!(outside.join("keep.txt").is_file());
        assert!(fs::symlink_metadata(data.join(".import-1-1")).is_ok());
    }

    #[test]
    fn a_missing_folder_is_nothing_to_clean() {
        let temp = tempfile::tempdir().unwrap();
        let cleaned = clean_stale(&temp.path().join("absent"), SystemTime::now(), Duration::ZERO).unwrap();
        assert_eq!(cleaned, Cleaned::default());
    }
}
