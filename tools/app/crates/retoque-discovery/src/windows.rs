use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

use crate::io::{MAX_TEXT_BYTES, read_bounded_bytes, read_bounded_string};
use crate::steam::{normalize_path_buf, normalize_windows_path};
use crate::targets::{detect_emulator_name, find_dlss_anywhere_with_targets, find_targets, find_targets_with_hint};
use crate::{Game, Runtime, Store};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EpicManifestItem {
    pub app_name: String,
    pub display_name: String,
    pub install_location: PathBuf,
    pub launch_executable: Option<PathBuf>,
    pub is_incomplete: bool,
    pub is_dlc: bool,
}

/// Pure parser for an Epic Games Launcher `.item` JSON manifest.
pub fn parse_epic_manifest_item(text: &str) -> Result<EpicManifestItem, String> {
    if text.len() > MAX_TEXT_BYTES {
        return Err(format!(
            "Epic .item JSON size ({}) exceeds limit ({MAX_TEXT_BYTES})",
            text.len()
        ));
    }
    let val: Value = serde_json::from_str(text).map_err(|e| format!("invalid Epic .item JSON: {e}"))?;
    let Some(obj) = val.as_object() else {
        return Err("Epic .item JSON is not an object".to_string());
    };

    let app_name = obj
        .get("AppName")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| "missing 'AppName' in Epic .item manifest".to_string())?
        .to_string();

    let install_str = obj
        .get("InstallLocation")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .ok_or_else(|| format!("missing 'InstallLocation' in Epic .item for {app_name}"))?;
    let install_location = PathBuf::from(normalize_windows_path(install_str));

    let display_name = obj
        .get("DisplayName")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| {
            install_location
                .file_name()
                .and_then(|s| s.to_str())
                .unwrap_or(&app_name)
                .to_string()
        });

    let launch_executable = obj
        .get("LaunchExecutable")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(PathBuf::from);

    let is_incomplete = obj
        .get("bIsIncompleteInstall")
        .and_then(Value::as_bool)
        .unwrap_or(false);

    let main_game = obj
        .get("MainGameAppName")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty());
    let is_dlc = main_game.is_some_and(|m| m != app_name);

    Ok(EpicManifestItem {
        app_name,
        display_name,
        install_location,
        launch_executable,
        is_incomplete,
        is_dlc,
    })
}

/// Pure parser for the Windows Xbox app `.GamingRoot` file (`RGBX` magic + 4-byte header + UTF-16LE path).
pub fn parse_gaming_root(bytes: &[u8]) -> Option<String> {
    if bytes.len() < 10 || &bytes[..4] != b"RGBX" {
        return None;
    }
    let utf16_bytes = &bytes[8..];
    let (chunks, _) = utf16_bytes.as_chunks::<2>();
    let u16_iter = chunks.iter().copied().map(u16::from_le_bytes).take_while(|&u| u != 0);
    let decoded = String::from_utf16(&u16_iter.collect::<Vec<u16>>()).ok()?;
    let trimmed = decoded.trim_matches(['/', '\\', ' ', '\t', '\r', '\n']);
    if trimmed.is_empty() {
        None
    } else {
        Some(trimmed.to_string())
    }
}

/// Discover Epic Games from a directory containing `*.item` JSON manifests.
pub fn discover_epic_from_dir(manifests_dir: &Path) -> (Vec<Game>, Vec<String>) {
    discover_epic_with_roots(manifests_dir, &[])
}

/// Discover Epic Games from `manifests_dir` plus any unclaimed folders under `epic_roots`
/// (such as `<drive>:\Epic Games\*`).
pub fn discover_epic_with_roots(manifests_dir: &Path, epic_roots: &[PathBuf]) -> (Vec<Game>, Vec<String>) {
    let mut skipped = Vec::new();
    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    let mut claimed_dirs: HashSet<String> = HashSet::new();

    if manifests_dir.exists() {
        match fs::read_dir(manifests_dir) {
            Ok(read_dir) => {
                let mut item_paths: Vec<PathBuf> = read_dir
                    .flatten()
                    .map(|e| e.path())
                    .filter(|p| {
                        p.extension()
                            .and_then(|ext| ext.to_str())
                            .is_some_and(|ext| ext.eq_ignore_ascii_case("item"))
                    })
                    .collect();
                item_paths.sort();

                for path in item_paths {
                    let text = match read_bounded_string(&path, MAX_TEXT_BYTES) {
                        Ok(t) => t,
                        Err(e) => {
                            skipped.push(format!("epic ({}): {e}", path.display()));
                            continue;
                        }
                    };
                    let item = match parse_epic_manifest_item(&text) {
                        Ok(i) => i,
                        Err(e) => {
                            skipped.push(format!("epic ({}): {e}", path.display()));
                            continue;
                        }
                    };
                    if item.is_incomplete || item.is_dlc {
                        continue;
                    }
                    claimed_dirs.insert(
                        normalize_path_buf(&item.install_location)
                            .to_string_lossy()
                            .to_ascii_lowercase(),
                    );
                    if !item.install_location.is_dir() {
                        skipped.push(format!(
                            "epic ({}): install directory not found: {}",
                            item.display_name,
                            item.install_location.display()
                        ));
                        continue;
                    }

                    let game_targets =
                        find_targets_with_hint(&item.install_location, item.launch_executable.as_deref());
                    let dlss_anywhere = find_dlss_anywhere_with_targets(&item.install_location, &game_targets);
                    let id = format!("epic:{}", item.app_name);
                    games.entry(id.clone()).or_insert(Game {
                        id,
                        name: item.display_name,
                        store: Store::Epic,
                        install_dir: item.install_location,
                        runtime: Runtime::Windows,
                        targets: game_targets,
                        dlss_anywhere,
                        registered: true,
                    });
                }
            }
            Err(e) => {
                skipped.push(format!("epic ({}): {e}", manifests_dir.display()));
            }
        }
    }

    for epic_root in epic_roots {
        let Ok(read_dir) = fs::read_dir(epic_root) else {
            continue;
        };
        let mut subdirs: Vec<PathBuf> = read_dir
            .flatten()
            .filter(|e| e.file_type().is_ok_and(|ft| ft.is_dir()))
            .map(|e| e.path())
            .collect();
        subdirs.sort();

        for folder in subdirs {
            let install_dir = normalize_path_buf(&folder);
            let key = install_dir.to_string_lossy().to_ascii_lowercase();
            if claimed_dirs.contains(&key) {
                continue;
            }
            let Some(folder_name) = install_dir
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .filter(|s| !s.is_empty() && !s.starts_with('.'))
            else {
                continue;
            };

            let game_targets = find_targets(&install_dir);
            if game_targets.is_empty() {
                continue;
            }

            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let name = detect_emulator_name(&game_targets)
                .map(str::to_string)
                .unwrap_or(folder_name);
            let id = format!("epic-folder:{}", install_dir.display());
            claimed_dirs.insert(key);
            games.entry(id.clone()).or_insert(Game {
                id,
                name,
                store: Store::Epic,
                install_dir,
                runtime: Runtime::Windows,
                targets: game_targets,
                dlss_anywhere,
                registered: false,
            });
        }
    }

    (games.into_values().collect(), skipped)
}

/// Discover Xbox App games under `<drive>\XboxGames\<Game>\Content` (and any valid root named in
/// `<drive>\.GamingRoot`). Never accepts roots inside `Users` / `AppData` or child folders without
/// a `Content` subdirectory.
pub fn discover_xbox_from_drives(drives: &[PathBuf]) -> Vec<Game> {
    let mut xbox_roots: Vec<PathBuf> = Vec::new();
    let mut seen_roots: HashSet<String> = HashSet::new();

    for drive in drives {
        let mut candidates = vec![drive.join("XboxGames")];
        let gaming_root_file = drive.join(".GamingRoot");
        if gaming_root_file.is_file()
            && let Ok(bytes) = read_bounded_bytes(&gaming_root_file, 4096)
            && let Some(rel) = parse_gaming_root(&bytes)
            && !is_ignored_xbox_root_rel(&rel)
        {
            candidates.push(drive.join(rel));
        }

        for cand in candidates {
            if !cand.is_dir() || is_ignored_xbox_root_path(&cand) {
                continue;
            }
            let norm = normalize_path_buf(&cand);
            let key = norm.to_string_lossy().to_ascii_lowercase();
            if seen_roots.insert(key) {
                xbox_roots.push(norm);
            }
        }
    }

    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    for root in xbox_roots {
        let Ok(read_dir) = fs::read_dir(&root) else {
            continue;
        };
        let is_named_xboxgames = root
            .file_name()
            .is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case("xboxgames"));

        let mut subdirs: Vec<PathBuf> = read_dir
            .flatten()
            .filter(|e| e.file_type().is_ok_and(|ft| ft.is_dir()))
            .map(|e| e.path())
            .collect();
        subdirs.sort();

        for game_dir in subdirs {
            let Some(game_name) = game_dir
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .filter(|s| !s.is_empty() && !s.starts_with('.'))
            else {
                continue;
            };
            let content_dir = game_dir.join("Content");
            if !content_dir.is_dir() {
                continue;
            }
            if !is_named_xboxgames
                && !content_dir.join("MicrosoftGame.config").is_file()
                && !game_dir.join("MicrosoftGame.config").is_file()
                && !content_dir.join("appxmanifest.xml").is_file()
            {
                continue;
            }
            let install_dir = normalize_path_buf(&content_dir);

            let game_targets = find_targets(&install_dir);
            if game_targets.is_empty() {
                continue;
            }
            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let id = format!("xbox:{}", install_dir.display());
            games.entry(id.clone()).or_insert(Game {
                id,
                name: game_name,
                store: Store::Xbox,
                install_dir,
                runtime: Runtime::Windows,
                targets: game_targets,
                dlss_anywhere,
                registered: true,
            });
        }
    }

    games.into_values().collect()
}

fn is_ignored_xbox_root_rel(rel: &str) -> bool {
    rel.split(['/', '\\']).any(|seg| {
        matches!(
            seg.trim().to_ascii_lowercase().as_str(),
            "users"
                | "appdata"
                | "programdata"
                | "windows"
                | "program files"
                | "program files (x86)"
                | "temp"
                | "local"
                | "roaming"
                | "locallow"
        )
    })
}

fn is_ignored_xbox_root_path(path: &Path) -> bool {
    is_ignored_xbox_root_rel(&path.to_string_lossy())
}

/// Find extra Steam library folders at `<drive>\Games\SteamLibrary` and `<drive>\SteamLibrary`.
pub fn extra_steam_libraries_from_drives(drives: &[PathBuf]) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for drive in drives {
        for rel in ["Games/SteamLibrary", "SteamLibrary"] {
            let cand = drive.join(rel);
            if cand.is_dir() {
                let norm = normalize_path_buf(&cand);
                if !out.contains(&norm) {
                    out.push(norm);
                }
            }
        }
    }
    out
}

/// Discover standalone emulator folders at the root of fixed drives (e.g. `F:\PCSX2`).
pub fn discover_emulators_from_drives(drives: &[PathBuf]) -> Vec<Game> {
    const EMU_DIRS: &[&str] = &["PCSX2", "RPCS3", "Ryujinx", "yuzu", "xemu", "Dolphin"];
    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    for drive in drives {
        for &dir_name in EMU_DIRS {
            let cand = drive.join(dir_name);
            if !cand.is_dir() {
                continue;
            }
            let install_dir = normalize_path_buf(&cand);
            let game_targets = find_targets(&install_dir);
            let Some(emu_name) = detect_emulator_name(&game_targets) else {
                continue;
            };
            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let id = format!("folder:{}", install_dir.display());
            games.entry(id.clone()).or_insert(Game {
                id,
                name: emu_name.to_string(),
                store: Store::Manual,
                install_dir,
                runtime: Runtime::Windows,
                targets: game_targets,
                dlss_anywhere,
                registered: true,
            });
        }
    }
    games.into_values().collect()
}

#[cfg(windows)]
pub fn fixed_drives() -> Vec<PathBuf> {
    // DRIVE_FIXED == 3 in Win32 GetDriveTypeW.
    unsafe extern "system" {
        fn GetDriveTypeW(lpRootPathName: *const u16) -> u32;
    }
    const DRIVE_FIXED: u32 = 3;

    let mut drives = Vec::new();
    for letter in b'A'..=b'Z' {
        let root_str = format!("{}:\\", letter as char);
        let wide: Vec<u16> = root_str.encode_utf16().chain(std::iter::once(0)).collect();
        let dtype = unsafe { GetDriveTypeW(wide.as_ptr()) };
        if dtype == DRIVE_FIXED {
            drives.push(PathBuf::from(root_str));
        }
    }
    drives
}

#[cfg(windows)]
pub fn windows_steam_roots() -> Vec<PathBuf> {
    use winreg::RegKey;
    use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};

    let mut roots = Vec::new();
    if let Ok(hkcu) = RegKey::predef(HKEY_CURRENT_USER).open_subkey("Software\\Valve\\Steam")
        && let Ok(steam_path) = hkcu.get_value::<String, _>("SteamPath")
        && !steam_path.trim().is_empty()
    {
        roots.push(PathBuf::from(normalize_windows_path(&steam_path)));
    }
    if let Ok(hklm) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey("SOFTWARE\\WOW6432Node\\Valve\\Steam")
        && let Ok(install_path) = hklm.get_value::<String, _>("InstallPath")
        && !install_path.trim().is_empty()
    {
        let p = PathBuf::from(normalize_windows_path(&install_path));
        if !roots.contains(&p) {
            roots.push(p);
        }
    }
    for extra in extra_steam_libraries_from_drives(&fixed_drives()) {
        if !roots.contains(&extra) {
            roots.push(extra);
        }
    }
    roots
}

#[cfg(windows)]
pub fn discover_windows_epic() -> (Vec<Game>, Vec<String>) {
    let program_data = std::env::var_os("ProgramData")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"));
    let manifests_dir = program_data
        .join("Epic")
        .join("EpicGamesLauncher")
        .join("Data")
        .join("Manifests");
    let epic_roots: Vec<PathBuf> = fixed_drives()
        .into_iter()
        .map(|d| d.join("Epic Games"))
        .filter(|p| p.is_dir())
        .collect();
    discover_epic_with_roots(&manifests_dir, &epic_roots)
}

#[cfg(windows)]
pub fn discover_windows_gog() -> (Vec<Game>, Vec<String>) {
    use winreg::RegKey;
    use winreg::enums::HKEY_LOCAL_MACHINE;

    let hklm = RegKey::predef(HKEY_LOCAL_MACHINE);
    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    let mut skipped = Vec::new();

    for key_path in ["SOFTWARE\\WOW6432Node\\GOG.com\\Games", "SOFTWARE\\GOG.com\\Games"] {
        let Ok(games_key) = hklm.open_subkey(key_path) else {
            continue;
        };

        for subkey_name in games_key.enum_keys().flatten() {
            let Ok(subkey) = games_key.open_subkey(&subkey_name) else {
                continue;
            };

            let depends_on: String = subkey.get_value("dependsOn").unwrap_or_default();
            if !depends_on.trim().is_empty() {
                continue;
            }

            let raw_path: String = subkey
                .get_value("path")
                .or_else(|_| subkey.get_value("PATH"))
                .unwrap_or_default();
            if raw_path.trim().is_empty() {
                continue;
            }
            let install_dir = PathBuf::from(normalize_windows_path(raw_path.trim()));

            let game_id: String = subkey
                .get_value("gameID")
                .or_else(|_| subkey.get_value("gameId"))
                .unwrap_or_else(|_| subkey_name.clone());
            let game_id = game_id.trim();
            if game_id.is_empty() {
                continue;
            }

            let game_name: String = subkey
                .get_value("gameName")
                .or_else(|_| subkey.get_value("GAMENAME"))
                .unwrap_or_else(|_| {
                    install_dir
                        .file_name()
                        .and_then(|s| s.to_str())
                        .unwrap_or(game_id)
                        .to_string()
                });

            if !install_dir.is_dir() {
                skipped.push(format!(
                    "gog ({game_name}): install directory not found: {}",
                    install_dir.display()
                ));
                continue;
            }

            let exe_hint: Option<PathBuf> = subkey
                .get_value::<String, _>("exe")
                .or_else(|_| subkey.get_value::<String, _>("EXE"))
                .ok()
                .map(|s| s.trim().to_string())
                .filter(|s| !s.is_empty())
                .map(PathBuf::from);

            let game_targets = find_targets_with_hint(&install_dir, exe_hint.as_deref());
            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let id = format!("gog:{game_id}");
            games.entry(id.clone()).or_insert(Game {
                id,
                name: game_name,
                store: Store::Gog,
                install_dir,
                runtime: Runtime::Windows,
                targets: game_targets,
                dlss_anywhere,
                registered: true,
            });
        }
    }

    (games.into_values().collect(), skipped)
}
