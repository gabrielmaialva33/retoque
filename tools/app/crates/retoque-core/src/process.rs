//! Running-game checks never depend on a truncated process name.
use std::path::{Path, PathBuf};

/// Processes mapping game files. A Windows sharing violation with no visible module is represented by
/// pid 0 and a description of the held file; protected processes do not expose their identity.
pub fn running_processes(game_dir: &Path) -> Vec<(u32, String)> {
    let Ok(game) = game_dir.canonicalize() else {
        return Vec::new();
    };
    running_for(&game, &[])
}

/// `game` is already canonical. A plan supplies every observed path, including read-only dependencies;
/// source files outside the game are not game locks and retain their normal I/O errors.
pub(crate) fn running_for(game: &Path, planned: &[PathBuf]) -> Vec<(u32, String)> {
    let mut hits = platform(game);
    #[cfg(windows)]
    for file in probe_paths(game, planned) {
        if is_held(&file) {
            hits.push((
                0,
                format!(
                    "A game file is open: {}. The game, its launcher or an antivirus may be using it.",
                    file.strip_prefix(game).unwrap_or(&file).display()
                ),
            ));
        }
    }
    #[cfg(not(windows))]
    let _ = planned;
    hits.sort();
    hits.dedup();
    hits
}

/// Only immediate state entries and named files are enumerated. Never recurse into game content,
/// support folders or backup trees; nested files participate only if the plan actually observes them.
#[cfg(any(windows, test))]
fn probe_paths(game: &Path, planned: &[PathBuf]) -> Vec<PathBuf> {
    use std::{collections::BTreeSet, fs, path::Component};
    // Windows attribute queries do not require opening a held file for metadata. An exclusive game
    // handle must not make the selector silently drop the very file the fallback needs to probe.
    fn directory(path: &Path) -> Option<bool> {
        #[cfg(windows)]
        {
            use std::os::windows::ffi::OsStrExt;
            use windows_sys::Win32::Storage::FileSystem::{
                FILE_ATTRIBUTE_DEVICE, FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT, GetFileAttributesW,
                INVALID_FILE_ATTRIBUTES,
            };
            let path: Vec<_> = path.as_os_str().encode_wide().chain(Some(0)).collect();
            let attributes = unsafe { GetFileAttributesW(path.as_ptr()) };
            if attributes == INVALID_FILE_ATTRIBUTES
                || attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE) != 0
            {
                return None;
            }
            Some(attributes & FILE_ATTRIBUTE_DIRECTORY != 0)
        }
        #[cfg(not(windows))]
        {
            let meta = fs::symlink_metadata(path).ok()?;
            if meta.file_type().is_symlink() || (!meta.is_file() && !meta.is_dir()) {
                return None;
            }
            Some(meta.is_dir())
        }
    }
    fn regular_inside(game: &Path, path: &Path) -> bool {
        let Ok(relative) = path.strip_prefix(game) else {
            return false;
        };
        if relative.as_os_str().is_empty() || !relative.components().all(|c| matches!(c, Component::Normal(_))) {
            return false;
        }
        let mut parent = path.parent();
        while let Some(dir) = parent {
            if directory(dir) != Some(true) {
                return false;
            }
            if dir == game {
                break;
            }
            parent = dir.parent();
        }
        directory(path) == Some(false)
    }
    let mut paths: BTreeSet<PathBuf> = planned.iter().cloned().collect();
    if directory(game) == Some(true)
        && let Ok(entries) = fs::read_dir(game)
    {
        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
            if crate::PROXY_NAMES.contains(&name.as_str())
                || [
                    "optiscaler.dll",
                    "optiscaler.ini",
                    "nvngx.dll_dlssnr.dll",
                    "dlssg_sm86.ini",
                    "dinput8.dll",
                ]
                .contains(&name.as_str())
            {
                paths.insert(entry.path());
            }
        }
    }
    let state = game.join(crate::STATE_DIR);
    if directory(&state) == Some(true)
        && let Ok(entries) = fs::read_dir(state)
    {
        paths.extend(entries.flatten().map(|e| e.path()));
    }
    paths.into_iter().filter(|p| regular_inside(game, p)).collect()
}

#[cfg(target_os = "linux")]
fn platform(game: &Path) -> Vec<(u32, String)> {
    use std::os::unix::ffi::OsStrExt;
    let mut hits = Vec::new();
    let mut needle = b" ".to_vec();
    // Linux maps escapes newlines as \012, but preserves literal backslashes (the encoding is ambiguous).
    for b in game.as_os_str().as_bytes() {
        match b {
            b'\n' => needle.extend(b"\\012"),
            _ => needle.push(*b),
        }
    }
    needle.push(b'/');
    let Ok(entries) = std::fs::read_dir("/proc") else {
        return hits;
    };
    for entry in entries.flatten() {
        let Some(pid) = entry.file_name().to_str().and_then(|s| s.parse::<u32>().ok()) else {
            continue;
        };
        if pid == std::process::id() {
            continue;
        }
        let Ok(maps) = std::fs::read(entry.path().join("maps")) else {
            continue;
        };
        if let Some(pos) = maps.windows(needle.len()).position(|s| s == needle) {
            let rest = &maps[pos + needle.len()..];
            let end = rest.iter().position(|b| *b == b'\n').unwrap_or(rest.len());
            let name = std::fs::read_to_string(entry.path().join("comm")).unwrap_or_else(|_| "?".into());
            hits.push((
                pid,
                format!("{} maps {}", name.trim(), String::from_utf8_lossy(&rest[..end])),
            ));
        }
    }
    hits
}

#[cfg(windows)]
fn is_held(path: &Path) -> bool {
    use std::os::windows::fs::OpenOptionsExt;
    match std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .share_mode(0)
        .open(path)
    {
        Ok(_) => false,
        Err(e) => matches!(e.raw_os_error(), Some(32 | 33)),
    }
}
#[cfg(windows)]
fn platform(game: &Path) -> Vec<(u32, String)> {
    use windows_sys::Win32::{
        Foundation::{CloseHandle, INVALID_HANDLE_VALUE},
        System::Diagnostics::ToolHelp::*,
    };
    fn wide(s: &[u16]) -> String {
        String::from_utf16_lossy(&s[..s.iter().position(|v| *v == 0).unwrap_or(s.len())])
    }
    fn comparable(s: &str) -> String {
        s.trim_start_matches("\\\\?\\").replace('/', "\\").to_lowercase()
    }
    let prefix = format!("{}\\", comparable(&game.to_string_lossy()).trim_end_matches('\\'));
    let mut hits = Vec::new();
    // Toolhelp structures require dwSize; snapshot handles are closed along every path.
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE {
            return hits;
        }
        let mut proc: PROCESSENTRY32W = std::mem::zeroed();
        proc.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut more = Process32FirstW(snap, &mut proc);
        while more != 0 {
            if proc.th32ProcessID != std::process::id() {
                let modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.th32ProcessID);
                if modules != INVALID_HANDLE_VALUE {
                    let mut module: MODULEENTRY32W = std::mem::zeroed();
                    module.dwSize = std::mem::size_of::<MODULEENTRY32W>() as u32;
                    let mut next = Module32FirstW(modules, &mut module);
                    while next != 0 {
                        let path = wide(&module.szExePath);
                        if comparable(&path).starts_with(&prefix) {
                            hits.push((
                                proc.th32ProcessID,
                                format!("{} has {} loaded", wide(&proc.szExeFile), wide(&module.szModule)),
                            ));
                            break;
                        }
                        next = Module32NextW(modules, &mut module);
                    }
                    CloseHandle(modules);
                }
            }
            more = Process32NextW(snap, &mut proc);
        }
        CloseHandle(snap);
    }
    hits
}
#[cfg(not(any(target_os = "linux", windows)))]
fn platform(_: &Path) -> Vec<(u32, String)> {
    Vec::new()
}

/// An exclusive open checks existing destinations without changing their contents. Access-denied errors
/// are distinct from a sharing violation: dry runs may show them, but apply must stop before its first write.
#[cfg(windows)]
pub(crate) fn denied_writes(game: &Path, paths: &[&str]) -> Vec<String> {
    use std::os::windows::fs::OpenOptionsExt;
    let mut denied = Vec::new();
    for rel in paths {
        let path = game.join(rel);
        if !path.is_file() {
            continue;
        }
        if let Err(e) = std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .share_mode(0)
            .open(&path)
            && e.kind() == std::io::ErrorKind::PermissionDenied
            && !matches!(e.raw_os_error(), Some(32 | 33))
        {
            denied.push((*rel).to_owned());
        }
    }
    denied.sort();
    denied.dedup();
    denied
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        fs,
        time::{Duration, Instant},
    };

    #[test]
    fn large_unrelated_tree_is_not_enumerated_for_windows_fallback() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path();
        for dir in 0..100 {
            let dir = game.join(format!("content/{dir}"));
            fs::create_dir_all(&dir).unwrap();
            for file in 0..100 {
                fs::write(dir.join(format!("{file}.pak")), []).unwrap();
            }
        }
        for name in [
            "DXGI.DLL",
            "OptiScaler.ini",
            ".retoque/receipt.txt",
            ".retoque/pending-receipt.txt",
            ".retoque/components.txt",
            "OptiScaler/support.dll",
            ".retoque/backup/old/original.dll",
        ] {
            let path = game.join(name);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, "fixture").unwrap();
        }
        let planned = vec![
            game.join("OptiScaler/support.dll"),
            game.join(".retoque/backup/old/original.dll"),
        ];
        let start = Instant::now();
        let paths = probe_paths(game, &planned);
        assert!(
            start.elapsed() < Duration::from_secs(2),
            "bounded selection must finish independently of large content trees"
        );
        assert_eq!(
            paths.len(),
            7,
            "only planned files, proxy/INI and direct state files belong in fallback"
        );
        assert!(paths.contains(&game.join("DXGI.DLL")));
        assert!(paths.contains(&planned[0]) && paths.contains(&planned[1]));
        assert!(!paths.iter().any(|p| p.starts_with(game.join("content"))));
    }

    #[cfg(unix)]
    #[test]
    fn fallback_does_not_follow_links_or_probe_outside_game() {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        let outside = temp.path().join("outside");
        fs::create_dir(&game).unwrap();
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("held.dll"), "fixture").unwrap();
        std::os::unix::fs::symlink(&outside, game.join("linked")).unwrap();
        std::os::unix::fs::symlink(&outside, game.join(".retoque")).unwrap();
        std::os::unix::fs::symlink(outside.join("held.dll"), game.join("dxgi.dll")).unwrap();
        let paths = probe_paths(
            &game,
            &[
                game.join("linked/held.dll"),
                outside.join("held.dll"),
                game.join("../outside/held.dll"),
            ],
        );
        assert!(paths.is_empty(), "{paths:?}");
    }

    #[cfg(windows)]
    #[test]
    fn planned_nested_file_lock_refuses_without_probing_unrelated_content() {
        use std::os::windows::fs::OpenOptionsExt;
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().canonicalize().unwrap();
        let planned = game.join("OptiScaler/locked.dll");
        fs::create_dir_all(planned.parent().unwrap()).unwrap();
        fs::write(&planned, "fixture").unwrap();
        let unrelated = game.join("content/locked.pak");
        fs::create_dir_all(unrelated.parent().unwrap()).unwrap();
        fs::write(&unrelated, "fixture").unwrap();
        let _held = fs::OpenOptions::new().read(true).share_mode(0).open(&planned).unwrap();
        let _other = fs::OpenOptions::new()
            .read(true)
            .share_mode(0)
            .open(&unrelated)
            .unwrap();
        let hits = running_for(&game, &[planned]);
        assert!(
            hits.iter()
                .any(|(pid, name)| *pid == 0 && name.contains("locked.dll") && name.contains("antivirus"))
        );
        assert!(!hits.iter().any(|(_, name)| name.contains("locked.pak")));
    }
    #[cfg(windows)]
    #[test]
    fn ini_save_plan_and_apply_refuse_a_write_sharing_conflict() {
        use std::os::windows::fs::OpenOptionsExt;
        use windows_sys::Win32::Storage::FileSystem::FILE_SHARE_READ;
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        fs::write(&ini, "[LSFG]\nEnabled=false\n").unwrap();
        let edits = [("LSFG".into(), "Enabled".into(), "true".into())];
        let reviewed = crate::plan_settings(temp.path(), &edits).unwrap();
        let _held = fs::OpenOptions::new()
            .read(true)
            .share_mode(FILE_SHARE_READ)
            .open(&ini)
            .unwrap();
        let plan = crate::plan_settings(temp.path(), &edits).unwrap();
        assert!(plan.refusals.iter().any(|r| matches!(r, crate::Refusal::GameRunning {processes} if processes.iter().any(|(pid,name)|*pid==0 && name.contains("OptiScaler.ini")))));
        assert!(crate::apply(&reviewed, &mut |_| panic!("locked INI changed")).is_err());
        assert_eq!(fs::read(&ini).unwrap(), b"[LSFG]\nEnabled=false\n");
    }
}
