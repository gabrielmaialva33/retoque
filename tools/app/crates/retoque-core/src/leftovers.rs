//! Cleaning a folder whose game the store uninstalled but whose Retoque files it left behind (Steam removes only the
//! files it installed). Only files Retoque, OptiScaler, sm86, ReShade or their caches put there are removed; anything
//! else (a save, a store stamp, a game data file the store skipped) stays, and so does every folder still holding
//! one. Nothing is followed through a symbolic link or junction. A folder with an executable is never a leftover.
//!
//! `inspect` describes what would go; `remove` re-reads the folder and removes exactly that description, refusing if
//! the folder changed in between (a file grew, a game came back), so what the person reviewed is what happens.

use std::fs;
use std::io;
use std::path::{Path, PathBuf};

use crate::{Error, PROXY_NAMES};

const MAX_DEPTH: usize = 10;
const MAX_ENTRIES: usize = 20_000;

/// Folders whose whole content is Retoque's, OptiScaler's or sm86's (support files, caches, receipts, logs).
const OWNED_DIRS: [&str; 5] = ["optiscaler", "dlssg_sm86", ".retoque", "lsfg-cache", "d3d12_optiscaler"];
/// Files Retoque or the tools it installs put beside a game.
const OWNED_FILES: [&str; 11] = [
    "optiscaler.ini",
    "optiscaler.dll",
    "optiscaler.log",
    "optiscaler.previous.log",
    "nvngx.dll_dlssnr.dll",
    "nvngx_dlssnr.dll",
    "dlssg_sm86.ini",
    "reshade64.dll",
    "reshade.ini",
    "reshade.log",
    "reshadepreset.ini",
];

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LeftoverFile {
    /// Relative to the leftover folder, with forward slashes.
    pub path: String,
    pub size: u64,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Leftover {
    pub dir: PathBuf,
    /// Retoque's files, which `remove` deletes.
    pub remove: Vec<LeftoverFile>,
    /// Everything else, which stays (relative paths).
    pub keep: Vec<String>,
}

impl Leftover {
    pub fn remove_bytes(&self) -> u64 {
        self.remove.iter().map(|f| f.size).sum()
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Removed {
    pub files: usize,
    pub bytes: u64,
    /// The folder itself went too: nothing else was in it.
    pub folder_removed: bool,
}

/// Whether a file at this relative path is one Retoque or a tool it installs put there.
fn owned(relative: &str) -> bool {
    let lower = relative.to_ascii_lowercase();
    let mut parts: Vec<&str> = lower.split('/').collect();
    let Some(name) = parts.pop() else { return false };
    if parts.iter().any(|dir| OWNED_DIRS.contains(dir)) {
        return true;
    }
    if OWNED_FILES.contains(&name) || PROXY_NAMES.iter().any(|p| p.eq_ignore_ascii_case(name)) {
        return true;
    }
    // optiscaler_2.log, optiscaler_2.previous.log
    if let Some(rest) = name.strip_prefix("optiscaler_")
        && let Some(number) = rest.strip_suffix(".previous.log").or_else(|| rest.strip_suffix(".log"))
        && !number.is_empty()
        && number.bytes().all(|b| b.is_ascii_digit())
    {
        return true;
    }
    name.ends_with(".dll")
        && (name.starts_with("amd_fidelityfx_") || name.starts_with("libxess") || name.starts_with("nvngx_dlss"))
}

fn io_error(path: &Path, source: io::Error) -> Error {
    Error::Io {
        path: path.to_owned(),
        source,
    }
}

/// What a cleanup of `dir` would remove and keep. Refuses a folder with an executable (a game is there), a symbolic
/// link at the top, or a tree too deep or large to be what an uninstall leaves.
pub fn inspect(dir: &Path) -> Result<Leftover, Error> {
    let top = fs::symlink_metadata(dir).map_err(|e| io_error(dir, e))?;
    if !top.is_dir() {
        return Err(Error::Other(format!("{} is not a folder", dir.display())));
    }
    let mut result = Leftover {
        dir: dir.to_owned(),
        ..Default::default()
    };
    let mut entries = 0usize;
    let mut stack = vec![(dir.to_path_buf(), String::new(), 0usize)];
    while let Some((current, prefix, depth)) = stack.pop() {
        let read = fs::read_dir(&current).map_err(|e| io_error(&current, e))?;
        for entry in read {
            let entry = entry.map_err(|e| io_error(&current, e))?;
            entries += 1;
            if entries > MAX_ENTRIES {
                return Err(Error::Other(format!(
                    "{} holds too many files to be a leftover",
                    dir.display()
                )));
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            let relative = if prefix.is_empty() {
                name.clone()
            } else {
                format!("{prefix}/{name}")
            };
            let kind = entry.file_type().map_err(|e| io_error(&entry.path(), e))?;
            if kind.is_symlink() {
                // Never followed and never removed: it may point anywhere.
                result.keep.push(relative);
                continue;
            }
            if kind.is_dir() {
                if depth + 1 > MAX_DEPTH {
                    return Err(Error::Other(format!("{} is too deep to be a leftover", dir.display())));
                }
                stack.push((entry.path(), relative, depth + 1));
                continue;
            }
            if name.to_ascii_lowercase().ends_with(".exe") {
                return Err(Error::Other(format!(
                    "{} still has a program ({relative}): the game is installed",
                    dir.display()
                )));
            }
            let size = entry.metadata().map_err(|e| io_error(&entry.path(), e))?.len();
            if owned(&relative) {
                result.remove.push(LeftoverFile { path: relative, size });
            } else {
                result.keep.push(relative);
            }
        }
    }
    result.remove.sort_by(|a, b| a.path.cmp(&b.path));
    result.keep.sort();
    Ok(result)
}

/// Remove what `reviewed` describes, after checking the folder still matches it. Folders left empty go too, up to
/// and including the leftover folder; a folder still holding a kept file stays.
pub fn remove(reviewed: &Leftover) -> Result<Removed, Error> {
    let now = inspect(&reviewed.dir)?;
    if now.remove != reviewed.remove {
        return Err(Error::StalePlan(format!(
            "{} changed after the review: review it again",
            reviewed.dir.display()
        )));
    }
    let mut removed = Removed::default();
    let mut dirs: Vec<PathBuf> = Vec::new();
    for file in &reviewed.remove {
        let path = reviewed.dir.join(&file.path);
        fs::remove_file(&path).map_err(|e| io_error(&path, e))?;
        removed.files += 1;
        removed.bytes += file.size;
        let mut parent = path.parent();
        while let Some(p) = parent {
            if p == reviewed.dir || !p.starts_with(&reviewed.dir) {
                break;
            }
            if !dirs.iter().any(|d| d == p) {
                dirs.push(p.to_owned());
            }
            parent = p.parent();
        }
    }
    // Deepest first; a folder that still holds anything is simply not empty and stays.
    dirs.sort_by_key(|d| std::cmp::Reverse(d.components().count()));
    for dir in dirs {
        let _ = fs::remove_dir(&dir);
    }
    removed.folder_removed = fs::remove_dir(&reviewed.dir).is_ok();
    Ok(removed)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn write(root: &Path, relative: &str, bytes: &[u8]) {
        let path = root.join(relative);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, bytes).unwrap();
    }

    #[test]
    fn rafaels_leftovers_lose_only_retoques_files_and_keep_saves_and_game_data() {
        // The five folders found on an RTX 3060 PC on 2026-11-10, reduced to their kinds of files.
        let temp = tempfile::tempdir().unwrap();
        let subnautica = temp.path().join("Subnautica2");
        for f in [
            "Subnautica2/Binaries/Win64/dxgi.dll",
            "Subnautica2/Binaries/Win64/version.dll",
            "Subnautica2/Binaries/Win64/OptiScaler.ini",
            "Subnautica2/Binaries/Win64/OptiScaler.log",
            "Subnautica2/Binaries/Win64/nvngx.dll_dlssnr.dll",
            "Subnautica2/Binaries/Win64/nvngx_dlssnr.dll",
            "Subnautica2/Binaries/Win64/dlssg_sm86.ini",
            "Subnautica2/Binaries/Win64/dlssg_sm86/sr/sr_kernels.dll",
            "Subnautica2/Binaries/Win64/dlssg_sm86/backend/sm86_backend.dll",
        ] {
            write(&subnautica, f, b"0123456789");
        }
        write(&subnautica, "Subnautica2/Saved/SaveGames/steam_autocloud.vdf", b"save");
        let found = inspect(&subnautica).unwrap();
        assert_eq!(found.remove.len(), 9);
        assert_eq!(found.remove_bytes(), 90);
        assert_eq!(found.keep, ["Subnautica2/Saved/SaveGames/steam_autocloud.vdf"]);
        let done = remove(&found).unwrap();
        assert_eq!((done.files, done.bytes, done.folder_removed), (9, 90, false));
        assert!(
            subnautica
                .join("Subnautica2/Saved/SaveGames/steam_autocloud.vdf")
                .is_file()
        );
        assert!(!subnautica.join("Subnautica2/Binaries").exists(), "emptied folders go");

        let expedition = temp.path().join("Expedition 33");
        for f in ["dxgi.dll", "OptiScaler.ini", "nvngx.dll_dlssnr.dll", "nvngx_dlssnr.dll"] {
            write(&expedition, &format!("Sandfall/Binaries/Win64/{f}"), b"x");
        }
        let found = inspect(&expedition).unwrap();
        assert!(found.keep.is_empty());
        assert!(remove(&found).unwrap().folder_removed);
        assert!(!expedition.exists(), "nothing else was there: the folder goes");

        let generation = temp.path().join("GenerationZero");
        for f in [
            "winmm.dll",
            "OptiScaler.ini",
            "OptiScaler/streamline/sl.dlss_g.dll",
            "OptiScaler/amd_fidelityfx_loader_dx12.dll",
            "dlssg_sm86/logs/backend_1.jsonl",
            "lsfg-cache/abc/hlsl/0.hlsl",
            "OptiScaler_2.previous.log",
        ] {
            write(&generation, f, b"x");
        }
        write(&generation, "dropzone/text/master_eng.stringlookup", b"game");
        let found = inspect(&generation).unwrap();
        assert_eq!(found.keep, ["dropzone/text/master_eng.stringlookup"]);
        assert_eq!(found.remove.len(), 7);
    }

    #[test]
    fn a_game_a_change_after_review_and_links_are_refused_or_kept() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("Game");
        write(&game, "bin/OptiScaler.ini", b"x");
        write(&game, "bin/Game.exe", b"MZ");
        assert!(inspect(&game).is_err(), "an executable means the game is installed");

        let left = temp.path().join("Left");
        write(&left, "bin/OptiScaler.ini", b"x");
        let reviewed = inspect(&left).unwrap();
        write(&left, "bin/OptiScaler.ini", b"grown since the review");
        assert!(matches!(remove(&reviewed), Err(Error::StalePlan(_))));
        assert!(
            left.join("bin/OptiScaler.ini").is_file(),
            "nothing removed on a stale review"
        );

        #[cfg(unix)]
        {
            let outside = temp.path().join("outside");
            write(&outside, "dxgi.dll", b"keep me");
            let linked = temp.path().join("Linked");
            write(&linked, "OptiScaler.ini", b"x");
            std::os::unix::fs::symlink(&outside, linked.join("OptiScaler")).unwrap();
            let found = inspect(&linked).unwrap();
            assert_eq!(found.keep, ["OptiScaler"], "a link is kept, never followed");
            remove(&found).unwrap();
            assert!(outside.join("dxgi.dll").is_file());
        }
        assert!(inspect(&temp.path().join("missing")).is_err());
    }
}
