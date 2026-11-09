//! Finding installed games and the folder that holds each one's executable
//! (docs/superpowers/specs/2026-11-05-retoque-app.md). Parsers are pure functions over file contents; `discover`
//! is the only function that reads the disk. Nothing is launched and no store API is called.

use std::collections::HashSet;
use std::fs;
use std::path::{Path, PathBuf};

pub mod art;
pub mod heroic;
pub mod io;
pub mod lutris;
pub mod steam;
pub mod targets;
pub mod windows;

pub use art::{Artwork, steam_artwork};
pub use heroic::{
    HeroicInstalledGame, parse_heroic_game_config, parse_heroic_gog_installed, parse_heroic_installed_json,
    parse_heroic_legendary_installed, parse_heroic_nile_installed,
};
pub use lutris::{
    LutrisDbGame, LutrisYamlConfig, parse_lutris_db, parse_lutris_yaml, read_lutris_db_file, try_parse_lutris_yaml,
};
pub use steam::{
    SteamAppManifest, VdfValue, is_proton_or_wine_tool, is_steam_tool_or_runtime, normalize_path_buf,
    normalize_windows_path, parse_appmanifest_acf, parse_compat_tool_mapping_vdf, parse_library_folders_vdf, parse_vdf,
};
pub use targets::{
    DLSS_DLLS, PROXY_DLLS, detect_emulator_name, emulator_name_for_exe, find_dlss_anywhere,
    find_dlss_anywhere_with_targets, has_elf_executable, is_ignored_exe, resolve_hint_path,
};
pub use windows::{
    EpicManifestItem, discover_emulators_from_drives, discover_epic_from_dir, discover_epic_with_roots,
    discover_xbox_from_drives, extra_steam_libraries_from_drives, parse_epic_manifest_item, parse_gaming_root,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Store {
    Steam,
    Heroic,
    Lutris,
    Epic,
    Gog,
    Xbox,
    Manual,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Runtime {
    /// The game runs as a Windows program on Windows.
    Windows,
    /// Steam Play. `prefix` is `compatdata/<app_id>/pfx` when it exists.
    Proton {
        app_id: u32,
        prefix: Option<PathBuf>,
    },
    /// Heroic or Lutris with a Wine prefix.
    Wine {
        prefix: Option<PathBuf>,
    },
    /// A native Linux build: Retoque does not apply.
    NativeLinux,
    Unknown,
}

/// A folder that holds a game executable, ranked by `score` (higher first).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TargetDir {
    pub path: PathBuf,
    pub executables: Vec<String>,
    pub score: i32,
    /// DLSS DLLs present (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`, `sl.interposer.dll`).
    pub dlss: Vec<String>,
    /// `.retoque/receipt.txt` exists here.
    pub has_receipt: bool,
    /// A proxy name here holds something (OptiScaler or not); the core decides what.
    pub proxies_present: Vec<String>,
    /// Third-party loaders present here (`"dlssg_for_sm86"`, `"dlssg_for_sm86-legacy"`, `"reshade"`, `"special_k"`).
    pub loaders: Vec<String>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Game {
    /// Stable across runs: `steam:<appid>`, `heroic:<runner>:<app_name>`, `lutris:<slug>`, `manual:<path>`, …
    pub id: String,
    pub name: String,
    pub store: Store,
    pub install_dir: PathBuf,
    pub runtime: Runtime,
    pub targets: Vec<TargetDir>,
    /// DLSS / Streamline DLLs found anywhere under `install_dir` (e.g. Unreal `Engine/Plugins/...`), excluding mod/loader folders.
    pub dlss_anywhere: Vec<String>,
    /// `true` when backed by a store manifest/database/registry entry (or manual config); `false` for orphan `steamapps/common/*` folders.
    pub registered: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Config {
    /// Folders the user added by hand; each is a game.
    pub manual: Vec<PathBuf>,
    /// Folders whose immediate child folders are games.
    pub libraries: Vec<PathBuf>,
    /// Override the home folder (tests).
    pub home: Option<PathBuf>,
}

/// Every game found, sorted by name. A store that cannot be read is skipped and named in the second value.
pub fn discover(config: &Config) -> (Vec<Game>, Vec<String>) {
    let home = config.home.clone().or_else(default_home_dir);
    let mut games = Vec::new();
    let mut skipped = Vec::new();

    #[cfg(windows)]
    let extra_steam_roots = if config.home.is_none() {
        windows::windows_steam_roots()
    } else {
        Vec::new()
    };
    #[cfg(not(windows))]
    let extra_steam_roots: Vec<PathBuf> = Vec::new();

    let windows_steam_runtime = cfg!(windows) && config.home.is_none();
    let (steam_games, steam_skipped) =
        steam::discover_steam(home.as_deref(), &extra_steam_roots, windows_steam_runtime);
    games.extend(steam_games);
    skipped.extend(steam_skipped);

    let (heroic_games, heroic_skipped) = heroic::discover_heroic(home.as_deref());
    games.extend(heroic_games);
    skipped.extend(heroic_skipped);

    let (lutris_games, lutris_skipped) = lutris::discover_lutris(home.as_deref());
    games.extend(lutris_games);
    skipped.extend(lutris_skipped);

    #[cfg(windows)]
    if config.home.is_none() {
        let (epic_games, epic_skipped) = windows::discover_windows_epic();
        games.extend(epic_games);
        skipped.extend(epic_skipped);

        let (gog_games, gog_skipped) = windows::discover_windows_gog();
        games.extend(gog_games);
        skipped.extend(gog_skipped);

        let drives = windows::fixed_drives();
        games.extend(windows::discover_xbox_from_drives(&drives));
        games.extend(windows::discover_emulators_from_drives(&drives));
    }

    let mut seen_install_dirs: HashSet<PathBuf> = games.iter().map(|g| g.install_dir.clone()).collect();

    for lib_dir in &config.libraries {
        let norm_lib = normalize_path_buf(lib_dir);
        if !norm_lib.is_dir() {
            skipped.push(format!("library ({}): directory not found", norm_lib.display()));
            continue;
        }
        let read_dir = match fs::read_dir(&norm_lib) {
            Ok(rd) => rd,
            Err(e) => {
                skipped.push(format!("library ({}): {e}", norm_lib.display()));
                continue;
            }
        };

        let mut children: Vec<PathBuf> = read_dir
            .flatten()
            .filter(|e| e.file_type().is_ok_and(|ft| ft.is_dir()))
            .map(|e| e.path())
            .collect();
        children.sort();

        for child in children {
            let child_norm = normalize_path_buf(&child);
            let Some(folder_name) = child_norm
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .filter(|s| !s.is_empty() && !s.starts_with('.'))
            else {
                continue;
            };
            if !seen_install_dirs.insert(child_norm.clone()) {
                continue;
            }

            let game_targets = targets(&child_norm);
            if game_targets.is_empty() {
                continue;
            }
            let dlss_anywhere = targets::find_dlss_anywhere_with_targets(&child_norm, &game_targets);
            let runtime = if cfg!(windows) && config.home.is_none() {
                Runtime::Windows
            } else {
                Runtime::Unknown
            };
            let name = detect_emulator_name(&game_targets)
                .map(str::to_string)
                .unwrap_or(folder_name);

            games.push(Game {
                id: format!("folder:{}", child_norm.display()),
                name,
                store: Store::Manual,
                install_dir: child_norm,
                runtime,
                targets: game_targets,
                dlss_anywhere,
                registered: true,
            });
        }
    }

    for manual_dir in &config.manual {
        let norm_dir = normalize_path_buf(manual_dir);
        if !norm_dir.is_dir() {
            skipped.push(format!("manual ({}): directory not found", norm_dir.display()));
            continue;
        }
        if !seen_install_dirs.insert(norm_dir.clone()) {
            continue;
        }
        let game_targets = targets(&norm_dir);
        let dlss_anywhere = targets::find_dlss_anywhere_with_targets(&norm_dir, &game_targets);
        let runtime = if cfg!(windows) && config.home.is_none() {
            Runtime::Windows
        } else if game_targets.is_empty() && targets::has_elf_executable(&norm_dir) {
            Runtime::NativeLinux
        } else {
            Runtime::Unknown
        };
        let name = detect_emulator_name(&game_targets)
            .map(str::to_string)
            .unwrap_or_else(|| {
                norm_dir
                    .file_name()
                    .map(|s| s.to_string_lossy().into_owned())
                    .filter(|s| !s.is_empty())
                    .unwrap_or_else(|| "Manual".to_string())
            });

        games.push(Game {
            id: format!("manual:{}", norm_dir.display()),
            name,
            store: Store::Manual,
            install_dir: norm_dir,
            runtime,
            targets: game_targets,
            dlss_anywhere,
            registered: true,
        });
    }

    games.sort_by(|a, b| {
        a.name
            .to_lowercase()
            .cmp(&b.name.to_lowercase())
            .then_with(|| a.name.cmp(&b.name))
            .then_with(|| a.store.cmp(&b.store))
            .then_with(|| a.id.cmp(&b.id))
    });

    (games, skipped)
}

/// The candidate executable folders under an install folder, best first.
pub fn targets(install_dir: &Path) -> Vec<TargetDir> {
    targets::find_targets(install_dir)
}

fn default_home_dir() -> Option<PathBuf> {
    std::env::var_os("HOME")
        .or_else(|| std::env::var_os("USERPROFILE"))
        .map(PathBuf::from)
        .filter(|p| !p.as_os_str().is_empty())
}
