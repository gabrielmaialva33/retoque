//! Folders a store uninstalled but Retoque's files kept alive: no executable left anywhere, and Retoque's own marker
//! files still there. Only flags candidates; `retoque_core::leftovers` decides file by file what to remove.

use std::fs;
use std::path::Path;

/// Deeper or larger trees are not treated as leftovers: an uninstalled game keeps a handful of files.
const MAX_DEPTH: usize = 10;
const MAX_ENTRIES: usize = 20_000;

/// Files only Retoque/OptiScaler or sm86 put in a game folder.
const MARKERS: [&str; 4] = ["optiscaler.ini", "nvngx.dll_dlssnr.dll", "dlssg_sm86.ini", ".retoque"];

/// True for a folder with no executable at any depth and at least one Retoque marker. Symbolic links and junctions
/// are never followed; a tree too deep or too large to read fully is not a leftover.
pub fn looks_like_leftover(dir: &Path) -> bool {
    let mut marker = false;
    let mut entries = 0usize;
    let mut stack = vec![(dir.to_path_buf(), 0usize)];
    while let Some((current, depth)) = stack.pop() {
        let Ok(read) = fs::read_dir(&current) else { return false };
        for entry in read.flatten() {
            entries += 1;
            if entries > MAX_ENTRIES {
                return false;
            }
            let Ok(kind) = entry.file_type() else { return false };
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            marker |= MARKERS.contains(&name.as_str());
            if kind.is_symlink() {
                continue;
            }
            if kind.is_dir() {
                if depth + 1 > MAX_DEPTH {
                    return false;
                }
                stack.push((entry.path(), depth + 1));
            } else if name.ends_with(".exe") {
                return false;
            }
        }
    }
    marker
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn retoque_files_without_a_game_are_a_leftover_and_a_game_is_not() {
        let temp = tempfile::tempdir().unwrap();
        let left = temp.path().join("Grounded/Maine/Binaries/Win64");
        fs::create_dir_all(&left).unwrap();
        fs::write(left.join("OptiScaler.ini"), "[DlssNr]").unwrap();
        fs::write(left.join("dxgi.dll"), "MZ").unwrap();
        assert!(looks_like_leftover(&temp.path().join("Grounded")));

        fs::write(left.join("Maine-Win64-Shipping.exe"), "MZ").unwrap();
        assert!(
            !looks_like_leftover(&temp.path().join("Grounded")),
            "a game is still there"
        );

        let plain = temp.path().join("Other");
        fs::create_dir_all(&plain).unwrap();
        fs::write(plain.join("readme.txt"), "x").unwrap();
        assert!(!looks_like_leftover(&plain), "no Retoque marker");
        assert!(!looks_like_leftover(&temp.path().join("missing")));
    }
}
