use std::collections::{BTreeMap, HashMap};
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use crate::TargetDir;
use crate::io::{open_regular_file, read_bounded_bytes};

fn observed_retoque_dirs() -> &'static Mutex<HashMap<PathBuf, bool>> {
    static DIRS: OnceLock<Mutex<HashMap<PathBuf, bool>>> = OnceLock::new();
    DIRS.get_or_init(|| Mutex::new(HashMap::new()))
}

const MAX_WALK_DEPTH: usize = 5;
const MAX_WALK_ENTRIES: usize = 10_000;
const MAX_DLSS_DEPTH: usize = 12;
const MAX_DLSS_ENTRIES: usize = 15_000;
const MAX_DLL_PROBE_BYTES: usize = 64 * 1024 * 1024;

/// Canonical DLSS / Streamline DLL names checked in each target directory.
pub const DLSS_DLLS: &[&str] = &[
    "nvngx_dlss.dll",
    "nvngx_dlssd.dll",
    "nvngx_dlssg.dll",
    "sl.interposer.dll",
];

/// Canonical proxy DLL names that OptiScaler can impersonate.
pub const PROXY_DLLS: &[&str] = &[
    "dxgi.dll",
    "winmm.dll",
    "version.dll",
    "dbghelp.dll",
    "d3d12.dll",
    "winhttp.dll",
    "wininet.dll",
];

const LOADER_CANDIDATE_DLLS: &[&str] = &[
    "dxgi.dll",
    "winmm.dll",
    "version.dll",
    "dbghelp.dll",
    "d3d12.dll",
    "winhttp.dll",
    "wininet.dll",
    "dinput8.dll",
];

/// If `exe_name` is a known emulator binary, returns the canonical emulator display name.
pub fn emulator_name_for_exe(exe_name: &str) -> Option<&'static str> {
    let lower = exe_name.to_ascii_lowercase();
    let stem = lower.strip_suffix(".exe").unwrap_or(&lower);
    if stem == "pcsx2" || stem == "pcsx2-qt" || stem.starts_with("pcsx2-qt") {
        Some("PCSX2")
    } else if stem == "rpcs3" {
        Some("RPCS3")
    } else if stem == "ryujinx" || stem == "ryujinx.ava" {
        Some("Ryujinx")
    } else if stem == "yuzu" || stem == "yuzu-cmd" {
        Some("yuzu")
    } else if stem == "xemu" {
        Some("xemu")
    } else if stem == "dolphin" || stem == "dolphinqt2" || stem == "dolphinwx" {
        Some("Dolphin")
    } else {
        None
    }
}

/// If any target directory contains a known emulator executable, returns the emulator's canonical name.
pub fn detect_emulator_name(targets: &[TargetDir]) -> Option<&'static str> {
    for t in targets {
        for exe in &t.executables {
            if let Some(name) = emulator_name_for_exe(exe) {
                return Some(name);
            }
        }
    }
    None
}

const MARK_OPTISCALER: &[u8] = b"OptiScaler working as";
const MARK_FORWARDER: &[u8] = b"dlssnr_call_";
const MARK_SM86_ROLE: &[u8] = b"DlssgProxy_Role";
const MARK_SM86_INFO: &[u8] = b"DlssgMod_GetInfo";
const MARK_RESHADE: &[u8] = b"ReShade";
const MARK_SPECIALK_1: &[u8] = b"Special K";
const MARK_SPECIALK_2: &[u8] = b"SpecialK";

#[derive(Clone, Debug)]
struct ExeCandidate {
    name: String,
    size: u64,
}

#[derive(Clone, Debug, Default)]
struct DirInspection {
    exes: Vec<ExeCandidate>,
    dlss: Vec<String>,
    proxies: Vec<String>,
    has_receipt: bool,
    has_retoque: bool,
    loaders: Vec<String>,
}

/// Returns `true` if the `.retoque/receipt.txt` in `dir` is a standalone DLSS update receipt
/// (owning `.retoque/dlss.txt` / `nvngx_dlss*.dll` without a Retoque proxy record).
pub fn is_standalone_dlss_receipt(dir: &Path) -> bool {
    let receipt_path = dir.join(".retoque/receipt.txt");
    let Ok(bytes) = read_bounded_bytes(&receipt_path, 64 * 1024) else {
        return false;
    };
    let text = String::from_utf8_lossy(&bytes);
    let has_proxy = text.lines().any(|l| l.starts_with("proxy\t"));
    if has_proxy {
        return false;
    }
    if dir.join(".retoque/dlss.txt").is_file() {
        return true;
    }
    text.lines()
        .any(|l| l.contains(".retoque/dlss.txt") || l.to_ascii_lowercase().contains("nvngx_dlss"))
}

/// Returns `true` when `target` has a Retoque installation (either a Retoque receipt or an
/// observed OptiScaler proxy / companion DLL). Standalone DLSS root receipts do not count.
pub fn target_has_retoque(target: &TargetDir) -> bool {
    if target.has_receipt && (target.path.as_os_str().is_empty() || !is_standalone_dlss_receipt(&target.path)) {
        return true;
    }
    if target.path.as_os_str().is_empty() {
        return false;
    }
    if let Ok(map) = observed_retoque_dirs().lock()
        && let Some(&cached) = map.get(&target.path)
        && cached
    {
        return true;
    }
    if target.path.join(".retoque/receipt.txt").is_file() && !is_standalone_dlss_receipt(&target.path) {
        return true;
    }
    if target.path.join("OptiScaler.dll").is_file() || target.path.join("nvngx.dll_dlssnr.dll").is_file() {
        return true;
    }
    for proxy in &target.proxies_present {
        let p = target.path.join(proxy);
        if let Ok(bytes) = read_bounded_bytes(&p, MAX_DLL_PROBE_BYTES)
            && (contains_subslice(&bytes, MARK_OPTISCALER) || contains_subslice(&bytes, MARK_FORWARDER))
        {
            return true;
        }
    }
    false
}

/// Normalize a launcher executable hint against `install_dir` (and optional Wine `prefix`),
/// converting Windows backslash separators and drive letters when the literal Unix path does not
/// exist on disk.
pub fn resolve_hint_path(install_dir: &Path, prefix: Option<&Path>, raw_hint: &Path) -> PathBuf {
    let literal = if raw_hint.is_absolute() {
        raw_hint.to_path_buf()
    } else {
        install_dir.join(raw_hint)
    };
    if literal.exists() {
        return literal;
    }

    let raw_str = raw_hint.to_string_lossy();
    let bytes = raw_str.as_bytes();

    // Check for Windows drive letter `X:\` or `X:/`.
    if bytes.len() >= 2
        && bytes[0].is_ascii_alphabetic()
        && bytes[1] == b':'
        && (bytes.len() == 2 || bytes[2] == b'\\' || bytes[2] == b'/')
    {
        let drive = (bytes[0] as char).to_ascii_lowercase();
        let rest = &raw_str[2..];
        let segs: Vec<&str> = rest.split(['/', '\\']).filter(|s| !s.is_empty() && *s != ".").collect();

        if drive == 'z' && !cfg!(windows) {
            let mut p = PathBuf::from("/");
            for seg in segs {
                p.push(seg);
            }
            return p;
        }

        if let Some(pfx) = prefix {
            let drive_dir = pfx.join(format!("drive_{drive}"));
            let dos_dir = pfx.join("dosdevices").join(format!("{drive}:"));
            let base = if drive_dir.is_dir() || !dos_dir.is_dir() {
                drive_dir
            } else {
                dos_dir
            };
            let mut p = base;
            for seg in segs {
                p.push(seg);
            }
            return p;
        }

        if cfg!(windows) {
            let mut p = PathBuf::from(format!("{}:\\", drive.to_ascii_uppercase()));
            for seg in segs {
                p.push(seg);
            }
            return p;
        }
    }

    if raw_str.contains('\\') {
        let is_abs = raw_str.starts_with('/') || raw_str.starts_with('\\');
        let mut out = if is_abs {
            PathBuf::from("/")
        } else {
            install_dir.to_path_buf()
        };
        for seg in raw_str.split(['/', '\\']) {
            if seg.is_empty() || seg == "." {
                continue;
            }
            out.push(seg);
        }
        return out;
    }

    literal
}

/// Find candidate executable directories under `install_dir`, ranked highest score first.
pub fn find_targets(install_dir: &Path) -> Vec<TargetDir> {
    find_targets_with_hint(install_dir, None)
}

/// Find candidate executable directories under `install_dir`, with an optional hinted executable
/// path (from a launcher manifest such as Lutris YAML, Heroic JSON, or Epic `.item`).
pub fn find_targets_with_hint(install_dir: &Path, hinted_exe: Option<&Path>) -> Vec<TargetDir> {
    if !install_dir.is_dir() {
        return Vec::new();
    }

    let mut entries_budget = MAX_WALK_ENTRIES;
    let mut dirs: BTreeMap<PathBuf, DirInspection> = BTreeMap::new();

    walk_dir(install_dir, install_dir, 0, &mut entries_budget, &mut dirs);

    let resolved_hint = hinted_exe.map(|exe| resolve_hint_path(install_dir, None, exe));

    // If a store manifest named a specific executable outside the default walk or deeper inside
    // a prefix, inspect its parent directory as well if it exists inside or alongside install_dir.
    if let Some(resolved) = &resolved_hint
        && let Some(parent) = resolved.parent()
        && parent.is_dir()
        && !dirs.contains_key(parent)
    {
        let mut single_budget = 512;
        walk_dir(install_dir, parent, MAX_WALK_DEPTH, &mut single_budget, &mut dirs);
    }

    let game_folder_name = install_dir
        .file_name()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();

    let hinted_parent = resolved_hint.as_ref().and_then(|p| p.parent().map(Path::to_path_buf));
    let hinted_exe_name = resolved_hint
        .as_ref()
        .and_then(|p| p.file_name())
        .map(|s| s.to_string_lossy().to_ascii_lowercase());

    // Check whether any subdirectory has a large executable or Unreal Shipping binary so a tiny
    // root bootstrapper stub does not outrank the real binary folder.
    let has_strong_subdir = dirs.iter().any(|(path, info)| {
        path != install_dir
            && (is_unreal_win64_dir(install_dir, path)
                || info
                    .exes
                    .iter()
                    .any(|e| is_unreal_shipping_exe(&e.name) || e.size >= 10 * 1024 * 1024 || !info.dlss.is_empty()))
    });

    let mut results = Vec::with_capacity(dirs.len());
    for (dir_path, mut info) in dirs {
        if info.exes.is_empty() {
            continue;
        }

        // Order executables inside this directory so the primary game executable appears first.
        info.exes.sort_by(|a, b| {
            let sa = score_single_exe(&a.name, a.size, &game_folder_name, hinted_exe_name.as_deref());
            let sb = score_single_exe(&b.name, b.size, &game_folder_name, hinted_exe_name.as_deref());
            sb.cmp(&sa).then_with(|| a.name.cmp(&b.name))
        });

        let score = score_directory(
            install_dir,
            &dir_path,
            &info,
            &game_folder_name,
            has_strong_subdir,
            hinted_parent.as_deref(),
            hinted_exe_name.as_deref(),
        );

        let executables = info.exes.into_iter().map(|e| e.name).collect();
        results.push(TargetDir {
            path: dir_path,
            executables,
            score,
            dlss: info.dlss,
            has_receipt: info.has_receipt,
            proxies_present: info.proxies,
            loaders: info.loaders,
        });
    }

    results.sort_by(|a, b| b.score.cmp(&a.score).then_with(|| a.path.cmp(&b.path)));
    results
}

/// Bounded scan across `install_dir` (including Unreal `Engine/Plugins/Runtime/Nvidia/...` trees)
/// for DLSS / Streamline DLLs (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`,
/// `sl.interposer.dll`), skipping our own and third-party loader folders (`dlssg_sm86`,
/// `OptiScaler`, `.retoque`).
pub fn find_dlss_anywhere(install_dir: &Path) -> Vec<String> {
    find_dlss_anywhere_with_targets(install_dir, &[])
}

pub fn find_dlss_anywhere_with_targets(install_dir: &Path, targets: &[TargetDir]) -> Vec<String> {
    let mut found: Vec<String> = Vec::new();
    for t in targets {
        for d in &t.dlss {
            if !found.contains(d) {
                found.push(d.clone());
            }
        }
    }
    if has_core_dlss(&found) || !install_dir.is_dir() {
        found.sort();
        return found;
    }

    let mut budget = MAX_DLSS_ENTRIES;
    if !targets.is_empty() {
        // `find_targets` already walked the binary folders up to depth 5; only Unreal `Plugins`
        // subtrees (`Engine/Plugins`, `<Project>/Engine/Plugins`, `<Project>/Plugins`) remain.
        for rel in ["Engine/Plugins", "Plugins"] {
            if has_core_dlss(&found) || budget == 0 {
                break;
            }
            let proot = install_dir.join(rel);
            if proot.is_dir() {
                walk_dlss_anywhere(&proot, 2, true, &mut budget, &mut found);
            }
        }
        if !has_core_dlss(&found)
            && budget > 0
            && let Ok(rd) = fs::read_dir(install_dir)
        {
            for entry in rd.flatten() {
                if has_core_dlss(&found) || budget == 0 {
                    break;
                }
                if !entry.file_type().is_ok_and(|ft| ft.is_dir()) {
                    continue;
                }
                let fname = entry.file_name();
                let name_cow = fname.to_string_lossy();
                let skip = with_ascii_lower(&name_cow, |lower| {
                    lower == "engine" || lower == "plugins" || should_skip_dlss_subdir(lower)
                });
                if skip {
                    continue;
                }
                let p = entry.path();
                for rel in ["Engine/Plugins", "Plugins"] {
                    if has_core_dlss(&found) || budget == 0 {
                        break;
                    }
                    let proot = p.join(rel);
                    if proot.is_dir() {
                        walk_dlss_anywhere(&proot, 2, true, &mut budget, &mut found);
                    }
                }
            }
        }
    } else {
        walk_dlss_anywhere(install_dir, 0, false, &mut budget, &mut found);
    }

    found.sort();
    found
}

fn with_ascii_lower<R>(s: &str, f: impl FnOnce(&str) -> R) -> R {
    if !s.bytes().any(|b| b.is_ascii_uppercase()) {
        return f(s);
    }
    if s.len() <= 128 {
        let mut buf = [0u8; 128];
        buf[..s.len()].copy_from_slice(s.as_bytes());
        buf[..s.len()].make_ascii_lowercase();
        if let Ok(lower) = std::str::from_utf8(&buf[..s.len()]) {
            return f(lower);
        }
    }
    let owned = s.to_ascii_lowercase();
    f(&owned)
}

fn has_core_dlss(found: &[String]) -> bool {
    found.len() == DLSS_DLLS.len()
        || (found.iter().any(|d| d == "nvngx_dlss.dll") && found.iter().any(|d| d == "nvngx_dlssg.dll"))
}

fn walk_dlss_anywhere(dir: &Path, depth: usize, in_plugins_or_bins: bool, budget: &mut usize, found: &mut Vec<String>) {
    if depth > MAX_DLSS_DEPTH || *budget == 0 || found.len() == DLSS_DLLS.len() {
        return;
    }
    let Ok(read_dir) = fs::read_dir(dir) else {
        return;
    };

    let mut subdirs: Vec<(i32, bool, PathBuf)> = Vec::new();
    for entry in read_dir.flatten() {
        if *budget == 0 {
            break;
        }
        *budget -= 1;

        let Ok(ft) = entry.file_type() else {
            continue;
        };
        let file_name = entry.file_name();
        let name_cow = file_name.to_string_lossy();

        if ft.is_dir() {
            if depth < MAX_DLSS_DEPTH {
                with_ascii_lower(&name_cow, |lower| {
                    if should_enter_dlss_subdir(lower, depth, in_plugins_or_bins) {
                        let next_in_plugins = in_plugins_or_bins
                            || matches!(
                                lower,
                                "plugins" | "thirdparty" | "streamline" | "streamlinecore" | "dlss"
                            );
                        let prio = dlss_subdir_priority(lower);
                        subdirs.push((prio, next_in_plugins, entry.path()));
                    }
                });
            }
        } else if ft.is_file() && matches!(name_cow.len(), 14 | 15 | 17) {
            with_ascii_lower(&name_cow, |lower| {
                if let Some(&canonical) = DLSS_DLLS.iter().find(|&&d| d == lower)
                    && !found.iter().any(|d| d == canonical)
                {
                    found.push(canonical.to_string());
                }
            });
        }
    }

    subdirs.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| a.2.cmp(&b.2)));
    for (_, next_in_plugins, sub) in subdirs {
        if *budget == 0 || found.len() == DLSS_DLLS.len() {
            break;
        }
        walk_dlss_anywhere(&sub, depth + 1, next_in_plugins, budget, found);
    }
}

fn should_enter_dlss_subdir(lower_name: &str, depth: usize, in_plugins_or_bins: bool) -> bool {
    if should_skip_dlss_subdir(lower_name) {
        return false;
    }
    if lower_name.ends_with("_data") || lower_name.ends_with(".pak") || lower_name.starts_with("dw_dlc") {
        return false;
    }
    if matches!(
        lower_name,
        "content"
            | "data"
            | "archives"
            | "levels"
            | "worlds"
            | "maps"
            | "textures"
            | "meshes"
            | "characters"
            | "artbookost"
            | "erd_artbookost"
            | "devtools"
            | "streamingassets"
            | "bundles"
            | "assets"
            | "config"
            | "documentation"
            | "extras"
            | "intermediate"
            | "programs"
            | "saved"
            | "source"
            | "resources"
            | "slate"
            | "editor"
    ) {
        return false;
    }

    // At depth 0 (install_dir root) or once inside `Plugins` / `ThirdParty`, allow descending into
    // any non-excluded child directory (such as `<Project>`, `Marketplace`, `Runtime`, `NVIDIA`).
    if depth == 0 || in_plugins_or_bins {
        return true;
    }

    // At depth >= 1 outside `Plugins`, only enter engine/plugin/binary subtrees.
    matches!(
        lower_name,
        "engine"
            | "plugins"
            | "binaries"
            | "thirdparty"
            | "win64"
            | "wingdk"
            | "bin"
            | "bin64"
            | "x64"
            | "x64_dx12"
            | "game"
            | "defed"
            | "work"
            | "dlss"
            | "dlssg"
            | "streamline"
            | "streamlinecore"
            | "nvidia"
    )
}

fn should_skip_dlss_subdir(lower_name: &str) -> bool {
    if lower_name.starts_with('.') {
        return true;
    }
    matches!(
        lower_name,
        "dlssg_sm86"
            | "optiscaler"
            | "lsfg-cache"
            | "alternatives"
            | "archive"
            | "_commonredist"
            | "redist"
            | "redistributables"
            | "prereq"
            | "prerequisites"
            | "directx"
            | "vcredist"
            | "dotnet"
            | "dotnetcore"
            | "easyanticheat"
            | "battleye"
            | "crashreporter"
            | "crashreport"
            | "shadercache"
            | "shaders"
            | "paks"
            | "content"
            | "monobleedingedge"
            | "movies"
            | "video"
            | "videos"
            | "sound"
            | "sounds"
            | "audio"
            | "music"
            | "localization"
            | "locales"
            | "licenses"
            | "third_party_licenses"
            | "logs"
            | "saves"
            | "mods"
            | "isos"
            | "iso"
            | "roms"
            | "bios"
            | "memcards"
            | "savestates"
            | "sstates"
            | "cheats"
            | "cheats_ws"
            | "cheats_ni"
    )
}

fn dlss_subdir_priority(lower_name: &str) -> i32 {
    match lower_name {
        "engine" | "plugins" | "runtime" | "nvidia" | "dlss" | "dlssg" | "streamline" | "streamlinecore"
        | "binaries" | "thirdparty" | "win64" => 100,
        "bin" | "bin64" | "x64" | "x64_dx12" | "game" | "defed" => 80,
        _ => 0,
    }
}

/// Returns `true` when `install_dir` has at least one ELF executable (not a shared library)
/// within the bounded walk. Used only when no `.exe` targets were found.
pub fn has_elf_executable(install_dir: &Path) -> bool {
    if !install_dir.is_dir() {
        return false;
    }
    let mut budget = 2_000usize;
    check_elf_recursive(install_dir, install_dir, 0, &mut budget)
}

fn check_elf_recursive(root: &Path, dir: &Path, depth: usize, budget: &mut usize) -> bool {
    if depth > MAX_WALK_DEPTH || *budget == 0 {
        return false;
    }
    let Ok(read_dir) = fs::read_dir(dir) else {
        return false;
    };
    let mut subdirs = Vec::new();
    for entry in read_dir.flatten() {
        if *budget == 0 {
            break;
        }
        *budget -= 1;
        let Ok(ft) = entry.file_type() else {
            continue;
        };
        let path = entry.path();
        let name = entry.file_name();
        let name_cow = name.to_string_lossy();
        let name_str = name_cow.as_ref();
        if ft.is_dir() {
            if depth < MAX_WALK_DEPTH && !should_skip_subdir(root, &path, name_str) {
                subdirs.push(path);
            }
        } else if ft.is_file() && is_candidate_elf_name(name_str) && is_elf_file(&path) {
            return true;
        }
    }
    for sub in subdirs {
        if check_elf_recursive(root, &sub, depth + 1, budget) {
            return true;
        }
    }
    false
}

fn is_candidate_elf_name(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    if lower.ends_with(".so")
        || lower.contains(".so.")
        || lower.ends_with(".dll")
        || lower.ends_with(".exe")
        || lower.ends_with(".pak")
        || lower.ends_with(".txt")
        || lower.ends_with(".ini")
        || lower.ends_with(".json")
        || lower.ends_with(".xml")
        || lower.ends_with(".png")
        || lower.ends_with(".jpg")
        || lower.ends_with(".ogg")
        || lower.ends_with(".wav")
        || lower.ends_with(".mp4")
        || lower.ends_with(".webm")
        || lower.ends_with(".pck")
        || lower.ends_with(".assets")
        || lower.ends_with(".ress")
        || lower.ends_with(".resource")
    {
        return false;
    }
    true
}

fn is_elf_file(path: &Path) -> bool {
    let Ok((mut file, _)) = open_regular_file(path) else {
        return false;
    };
    let mut magic = [0u8; 4];
    if file.read_exact(&mut magic).is_err() {
        return false;
    }
    magic == [0x7f, b'E', b'L', b'F']
}

fn walk_dir(root: &Path, dir: &Path, depth: usize, budget: &mut usize, out: &mut BTreeMap<PathBuf, DirInspection>) {
    if depth > MAX_WALK_DEPTH || *budget == 0 {
        return;
    }
    let Ok(read_dir) = fs::read_dir(dir) else {
        return;
    };

    let mut inspection = DirInspection::default();
    let mut subdirs: Vec<(i32, PathBuf)> = Vec::new();
    let rel_dir = dir.strip_prefix(root).unwrap_or(Path::new(""));
    let in_root_engine = rel_dir
        .components()
        .next()
        .is_some_and(|c| c.as_os_str().to_string_lossy().eq_ignore_ascii_case("engine"));

    let mut has_retoque_dir = false;
    let mut has_retoque_companion = false;
    let mut has_sm86_ini = false;
    let mut sm86_dir_path: Option<PathBuf> = None;
    let mut has_reshade_file = false;
    let mut has_specialk_file = false;
    let mut candidate_loader_dlls: Vec<(String, PathBuf, u64)> = Vec::new();

    for entry in read_dir.flatten() {
        if *budget == 0 {
            break;
        }
        *budget -= 1;

        let Ok(ft) = entry.file_type() else {
            continue;
        };
        let file_name = entry.file_name();
        let name_cow = file_name.to_string_lossy();
        let name_str = name_cow.as_ref();

        if ft.is_dir() {
            if name_str == ".retoque" {
                has_retoque_dir = true;
                continue;
            }
            if name_str.starts_with('.') {
                continue;
            }
            with_ascii_lower(name_str, |lower| {
                if lower == "dlssg_sm86" {
                    sm86_dir_path = Some(entry.path());
                } else if depth < MAX_WALK_DEPTH
                    && !in_root_engine
                    && !(depth == 0 && lower == "engine")
                    && !should_skip_subdir_lower(lower)
                {
                    let priority = subdir_walk_priority(lower);
                    subdirs.push((priority, entry.path()));
                }
            });
            continue;
        }

        if !ft.is_file() {
            continue;
        }

        // Fast suffix filter: only .exe, .dll, and .ini files matter inside walk_dir.
        let b = name_str.as_bytes();
        if b.len() < 4 || b[b.len() - 4] != b'.' {
            continue;
        }
        let ext = [
            b[b.len() - 3].to_ascii_lowercase(),
            b[b.len() - 2].to_ascii_lowercase(),
            b[b.len() - 1].to_ascii_lowercase(),
        ];
        if !matches!(&ext, b"exe" | b"dll" | b"ini") {
            continue;
        }

        with_ascii_lower(name_str, |lower| {
            if lower == "dlssg_sm86.ini" {
                has_sm86_ini = true;
                return;
            }
            if matches!(lower, "optiscaler.dll" | "nvngx.dll_dlssnr.dll") {
                has_retoque_companion = true;
                return;
            }
            if matches!(lower, "reshade64.dll" | "reshade32.dll" | "reshade.ini") {
                has_reshade_file = true;
                return;
            }
            if matches!(
                lower,
                "specialk64.dll" | "specialk32.dll" | "specialk.dll" | "specialk.ini"
            ) {
                has_specialk_file = true;
                return;
            }

            if let Some(&canonical_dlss) = DLSS_DLLS.iter().find(|&&d| d == lower) {
                if !inspection.dlss.iter().any(|d| d == canonical_dlss) {
                    inspection.dlss.push(canonical_dlss.to_string());
                }
                return;
            }

            if let Some(&canonical_proxy) = PROXY_DLLS.iter().find(|&&p| p == lower)
                && !inspection.proxies.iter().any(|p| p == canonical_proxy)
            {
                inspection.proxies.push(canonical_proxy.to_string());
            }

            if LOADER_CANDIDATE_DLLS.contains(&lower) {
                let size = entry.metadata().map(|m| m.len()).unwrap_or(0);
                candidate_loader_dlls.push((lower.to_string(), entry.path(), size));
                return;
            }

            if ext == *b"exe" && !is_ignored_exe(name_str, rel_dir) {
                let size = entry.metadata().map(|m| m.len()).unwrap_or(0);
                inspection.exes.push(ExeCandidate {
                    name: name_cow.clone().into_owned(),
                    size,
                });
            }
        });
    }

    let has_emulator = inspection.exes.iter().any(|e| emulator_name_for_exe(&e.name).is_some());

    if !inspection.exes.is_empty() {
        inspection.has_receipt =
            has_retoque_dir && dir.join(".retoque/receipt.txt").is_file() && !is_standalone_dlss_receipt(dir);
        let (loaders, has_optiscaler_proxy) = detect_loaders(
            dir,
            has_retoque_companion || inspection.has_receipt,
            has_sm86_ini,
            sm86_dir_path.as_deref(),
            has_reshade_file,
            has_specialk_file,
            &candidate_loader_dlls,
        );
        inspection.has_retoque = inspection.has_receipt || has_retoque_companion || has_optiscaler_proxy;
        if let Ok(mut map) = observed_retoque_dirs().lock() {
            map.insert(dir.to_path_buf(), inspection.has_retoque);
        }
        inspection.dlss.sort();
        inspection.proxies.sort();
        inspection.loaders = loaders;
        out.insert(dir.to_path_buf(), inspection);
    }

    // Visit high-priority binary directories first so the entry budget is never exhausted by
    // asset folders before reaching `Binaries/Win64` or `bin/x64`.
    subdirs.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| a.1.cmp(&b.1)));
    for (_, sub) in subdirs {
        if *budget == 0 {
            break;
        }
        if has_emulator
            && sub
                .file_name()
                .is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case("games"))
        {
            continue;
        }
        walk_dir(root, &sub, depth + 1, budget, out);
    }
}

fn detect_loaders(
    dir: &Path,
    has_retoque_companion: bool,
    has_sm86_ini: bool,
    sm86_dir: Option<&Path>,
    mut has_reshade: bool,
    mut has_specialk: bool,
    candidate_dlls: &[(String, PathBuf, u64)],
) -> (Vec<String>, bool) {
    let has_sm86_runtime_dir = sm86_dir.is_some_and(|d| {
        fs::read_dir(d).is_ok_and(|rd| {
            rd.flatten().any(|e| {
                e.file_type().is_ok_and(|ft| ft.is_dir())
                    && e.file_name().to_string_lossy().eq_ignore_ascii_case("runtime")
            })
        })
    });

    let mut has_sm86_dll_marker = false;
    let mut has_monolithic_sm86_dll = false;
    let mut has_optiscaler_proxy = false;

    for (lower_name, dll_path, size) in candidate_dlls {
        // A >= 20 MiB dxgi.dll/winmm.dll alongside Retoque companions is OptiScaler (~30 MiB);
        // skip reading 30 MiB from disk during discovery when indicator files already cover sm86.
        if has_retoque_companion
            && *size >= 20 * 1024 * 1024
            && (lower_name == "dxgi.dll" || lower_name == "winmm.dll")
            && !has_sm86_ini
        {
            has_optiscaler_proxy = true;
            continue;
        }
        // Heuristic for monolithic 0.3.x version.dll (~30 MB) when alongside dlssg_sm86.ini or named version.dll.
        if *size >= 15 * 1024 * 1024 && (has_sm86_ini || lower_name == "version.dll") && !has_sm86_runtime_dir {
            has_monolithic_sm86_dll = true;
            if has_sm86_ini {
                continue;
            }
        }
        if (has_sm86_ini || has_sm86_runtime_dir) && has_reshade && has_specialk && has_optiscaler_proxy {
            continue;
        }
        let Ok(bytes) = read_bounded_bytes(dll_path, MAX_DLL_PROBE_BYTES) else {
            continue;
        };
        if contains_subslice(&bytes, MARK_OPTISCALER) || contains_subslice(&bytes, MARK_FORWARDER) {
            has_optiscaler_proxy = true;
            continue;
        }
        if contains_subslice(&bytes, MARK_SM86_ROLE) || contains_subslice(&bytes, MARK_SM86_INFO) {
            has_sm86_dll_marker = true;
            if *size >= 5 * 1024 * 1024 || !has_sm86_runtime_dir {
                has_monolithic_sm86_dll = !has_sm86_runtime_dir;
            }
            continue;
        }
        if contains_subslice(&bytes, MARK_RESHADE) {
            has_reshade = true;
        }
        if contains_subslice(&bytes, MARK_SPECIALK_1) || contains_subslice(&bytes, MARK_SPECIALK_2) {
            has_specialk = true;
        }
    }

    let mut loaders = Vec::new();
    if has_sm86_ini || sm86_dir.is_some() || has_sm86_dll_marker || has_monolithic_sm86_dll {
        let is_modern = if has_sm86_runtime_dir {
            true
        } else if has_monolithic_sm86_dll {
            false
        } else if sm86_dir.is_some() {
            true
        } else if has_sm86_ini {
            // Check whether a version.dll/winmm.dll is present without dlssg_sm86\runtime (0.3.x layout).
            let has_version_or_winmm = candidate_dlls
                .iter()
                .any(|(n, _, _)| n == "version.dll" || n == "winmm.dll");
            if has_version_or_winmm {
                false
            } else {
                // Only dlssg_sm86.ini present: check if it contains the 0.4.x [DLSSG] section, else default to 0.4.
                let ini_bytes = read_bounded_bytes(&dir.join("dlssg_sm86.ini"), 1024 * 1024).unwrap_or_default();
                !contains_subslice(&ini_bytes, b"[FrameGeneration]\nOptimized")
            }
        } else {
            false
        };

        if is_modern {
            loaders.push("dlssg_for_sm86".to_string());
        } else {
            loaders.push("dlssg_for_sm86-legacy".to_string());
        }
    }

    if has_reshade {
        loaders.push("reshade".to_string());
    }
    if has_specialk {
        loaders.push("special_k".to_string());
    }

    loaders.sort();
    loaders.dedup();
    (loaders, has_optiscaler_proxy)
}

fn contains_subslice(haystack: &[u8], needle: &[u8]) -> bool {
    let n = needle.len();
    if n == 0 || haystack.len() < n {
        return false;
    }
    let first = needle[0];
    let last = needle[n - 1];
    let end = haystack.len() - n;
    let mut i = 0;
    while i <= end {
        if haystack[i] == first && haystack[i + n - 1] == last && &haystack[i..i + n] == needle {
            return true;
        }
        i += 1;
    }
    false
}

fn subdir_walk_priority(lower_name: &str) -> i32 {
    match lower_name {
        "binaries" | "win64" | "bin" | "bin64" | "x64" | "x64_dx12" | "game" | "defed" => 100,
        "ph_ft" | "work" | "retail" | "release" | "shipping" => 50,
        _ => 0,
    }
}

fn should_skip_subdir_lower(lower: &str) -> bool {
    if lower.ends_with("_data") || lower.ends_with(".pak") || lower.starts_with("dw_dlc") {
        return true;
    }
    matches!(
        lower,
        "dlssg_sm86"
            | "_commonredist"
            | "redist"
            | "redistributables"
            | "prereq"
            | "prerequisites"
            | "directx"
            | "vcredist"
            | "dotnet"
            | "dotnetcore"
            | "easyanticheat"
            | "battleye"
            | "crashreporter"
            | "crashreport"
            | "optiscaler"
            | "lsfg-cache"
            | "shadercache"
            | "shaders"
            | "content"
            | "paks"
            | "streamingassets"
            | "bundles"
            | "assets"
            | "data"
            | "archives"
            | "packages"
            | "packs"
            | "levels"
            | "worlds"
            | "maps"
            | "textures"
            | "meshes"
            | "characters"
            | "monobleedingedge"
            | "movies"
            | "video"
            | "videos"
            | "sound"
            | "sounds"
            | "audio"
            | "music"
            | "localization"
            | "locales"
            | "licenses"
            | "third_party_licenses"
            | "logs"
            | "saves"
            | "mods"
            | "mods instalados"
            | "modengine"
            | "modengine2"
            | "me2"
            | "me3"
            | "randomizer"
            | "scyllahide"
            | "convergenceer"
            | "isos"
            | "iso"
            | "roms"
            | "bios"
            | "memcards"
            | "savestates"
            | "sstates"
            | "cheats"
            | "cheats_ws"
            | "cheats_ni"
    )
}

fn should_skip_subdir(root: &Path, sub_path: &Path, dir_name: &str) -> bool {
    if dir_name.starts_with('.') {
        return true;
    }
    if with_ascii_lower(dir_name, should_skip_subdir_lower) {
        return true;
    }

    // Unreal Engine's `Engine/` tree only contains engine helpers (CrashReportClient,
    // UnrealCEFSubProcess, EpicWebHelper, UEPrereqSetup) and never the game executable.
    if let Ok(rel) = sub_path.strip_prefix(root) {
        let mut comps = rel.components();
        if let Some(first) = comps.next()
            && first.as_os_str().to_string_lossy().eq_ignore_ascii_case("engine")
        {
            return true;
        }
    }

    false
}

/// Returns `true` if `exe_name` in `rel_dir` is a known non-game executable (crash reporter,
/// redistributable/prerequisite installer, anti-cheat setup, launcher helper, or uninstaller).
pub fn is_ignored_exe(exe_name: &str, rel_dir: &Path) -> bool {
    with_ascii_lower(exe_name, |lower| {
        let stem = lower.strip_suffix(".exe").unwrap_or(lower);

        // Check directory components for non-game and mod-tool subdirectories.
        let mut in_loader_dir = false;
        let mut in_server_dir = false;
        for comp in rel_dir.components() {
            let comp_cow = comp.as_os_str().to_string_lossy();
            let skip_dir = with_ascii_lower(&comp_cow, |cl| {
                if cl == "loader" {
                    in_loader_dir = true;
                }
                if cl == "server" || cl.contains("server redirector") {
                    in_server_dir = true;
                }
                matches!(
                    cl,
                    "_commonredist"
                        | "redist"
                        | "redistributables"
                        | "prereq"
                        | "prerequisites"
                        | "directx"
                        | "vcredist"
                        | "dotnet"
                        | "dotnetcore"
                        | "easyanticheat"
                        | "battleye"
                        | "crashreporter"
                        | "crashreport"
                        | "installer"
                        | "__installer"
                        | "modengine"
                        | "modengine2"
                        | "me2"
                        | "me3"
                        | "randomizer"
                        | "scyllahide"
                        | "convergenceer"
                        | "mods"
                        | "mods instalados"
                )
            });
            if skip_dir {
                return true;
            }
        }

        if (stem == "loader" && in_loader_dir) || (stem == "server" && in_server_dir) {
            return true;
        }

        // Mod-tool, randomizer, injector, downloader, and seamless-coop launcher binaries.
        if stem.starts_with("modengine")
            || stem == "me3"
            || stem.starts_with("me3-")
            || stem.starts_with("me3_")
            || stem.ends_with("sc_launcher")
            || stem.starts_with("injectorcli")
            || stem.contains("downloader")
            || stem.contains("customserverlauncher")
            || stem.contains("randomizer")
        {
            return true;
        }

        // Crash reporters and handlers.
        if stem.contains("crashreport")
            || stem.contains("crashhandler")
            || stem.starts_with("unitycrashhandler")
            || stem.contains("crashpad")
            || stem.contains("errorreporter")
            || stem.contains("bugreport")
            || stem.starts_with("crs-handler")
            || stem.starts_with("crs-uploader")
            || stem.contains("senddebug")
            || stem.contains("minidump")
        {
            return true;
        }

        // Redistributable and prerequisite installers.
        if stem.starts_with("vc_redist")
            || stem.starts_with("vcredist")
            || stem.starts_with("ueprereqsetup")
            || stem.starts_with("ue4prereqsetup")
            || stem.contains("prereqsetup")
            || stem.contains("redist")
            || stem == "dxsetup"
            || stem.starts_with("dotnetfx")
            || stem.starts_with("ndp4")
            || stem.starts_with("windowsdesktop-runtime")
            || stem == "oalinst"
            || stem.starts_with("physx")
            || stem.starts_with("directx")
        {
            return true;
        }

        // EasyAntiCheat and BattlEye setup / service binaries.
        if stem.contains("easyanticheat")
            || stem.contains("eac_setup")
            || stem.starts_with("beservice")
            || stem.starts_with("beclient")
            || stem.contains("battleye")
            || stem.contains("eaanticheat")
        {
            return true;
        }

        // Uninstallers.
        if stem.starts_with("unins") || stem.contains("uninstall") {
            return true;
        }

        // Known launcher and engine helper binaries.
        matches!(
            stem,
            "7za"
                | "7z"
                | "cefsharp.browsersubprocess"
                | "unrealcefsubprocess"
                | "epicwebhelper"
                | "steamerrorreporter"
                | "d3d12stateobjectcompiler"
                | "d3dconfig"
                | "driverversionchecker"
                | "layerschecker"
                | "supporttool"
                | "redprelauncher"
                | "rockstar-games-launcher"
                | "social-club-setup"
                | "activationui"
                | "touchup"
                | "cleanup"
                | "shadercompileworker"
                | "zenserver"
                | "benchmark"
        )
    })
}

fn is_unreal_win64_dir(root: &Path, dir: &Path) -> bool {
    let Ok(rel) = dir.strip_prefix(root) else {
        return false;
    };
    let mut comps = rel.components();
    let Some(last) = comps.next_back() else {
        return false;
    };
    let Some(prev) = comps.next_back() else {
        return false;
    };
    if let Some(first) = rel.components().next()
        && first.as_os_str().to_string_lossy().eq_ignore_ascii_case("engine")
    {
        return false;
    }
    let last_s = last.as_os_str().to_string_lossy();
    let prev_s = prev.as_os_str().to_string_lossy();
    prev_s.eq_ignore_ascii_case("binaries")
        && (last_s.eq_ignore_ascii_case("win64") || last_s.eq_ignore_ascii_case("wingdk"))
}

fn is_unreal_shipping_exe(exe_name: &str) -> bool {
    with_ascii_lower(exe_name, |lower| {
        lower.ends_with("-win64-shipping.exe") || lower.ends_with("-wingdk-shipping.exe")
    })
}

fn score_single_exe(exe_name: &str, size: u64, game_name: &str, hinted_exe: Option<&str>) -> i32 {
    with_ascii_lower(exe_name, |lower| {
        let mut score = 0;
        let stem = lower.strip_suffix(".exe").unwrap_or(lower);

        if hinted_exe.is_some_and(|h| h == lower) {
            score += 100;
        }
        if emulator_name_for_exe(exe_name).is_some() {
            score += 80;
        }
        if lower.ends_with("-win64-shipping.exe") || lower.ends_with("-wingdk-shipping.exe") {
            score += 60;
        }
        if exe_matches_game_name(stem, game_name) {
            score += 30;
        }
        if stem.contains("launcher") || stem == "start_protected_game" || stem.ends_with("_be") {
            score -= 20;
        }
        if size >= 50 * 1024 * 1024 {
            score += 30;
        } else if size >= 10 * 1024 * 1024 {
            score += 20;
        } else if size >= 1024 * 1024 {
            score += 10;
        }
        score
    })
}

#[allow(clippy::too_many_arguments)]
fn score_directory(
    root: &Path,
    dir: &Path,
    info: &DirInspection,
    game_name: &str,
    has_strong_subdir: bool,
    hinted_parent: Option<&Path>,
    hinted_exe: Option<&str>,
) -> i32 {
    let mut score = 0;

    // 1. Hinted target directory and executable from store metadata.
    if hinted_parent.is_some_and(|hp| hp == dir) {
        score += 120;
        if let Some(hexe) = hinted_exe
            && info.exes.iter().any(|e| e.name.eq_ignore_ascii_case(hexe))
        {
            score += 80;
        }
    }

    // 2. Unreal `<Game>/Binaries/Win64` and `*-Win64-Shipping.exe`, or known emulator executable.
    if is_unreal_win64_dir(root, dir) {
        score += 50;
    }
    if info.exes.iter().any(|e| is_unreal_shipping_exe(&e.name)) {
        score += 60;
    }
    if info.exes.iter().any(|e| emulator_name_for_exe(&e.name).is_some()) {
        score += 80;
    }

    // 3. Common binary directory paths vs bonus/soundtrack/devtools/launcher folders.
    if let Ok(rel) = dir.strip_prefix(root) {
        let mut has_bin = false;
        let mut has_dx12 = false;
        let mut has_penalty = false;
        for c in rel.components() {
            let cow = c.as_os_str().to_string_lossy();
            with_ascii_lower(&cow, |cl| {
                if matches!(cl, "bin" | "bin64" | "x64" | "x64_dx12" | "game" | "defed") {
                    has_bin = true;
                }
                if cl.contains("dx12") {
                    has_dx12 = true;
                }
                if matches!(
                    cl,
                    "devtools"
                        | "artbook"
                        | "artbookost"
                        | "erd_artbookost"
                        | "ost"
                        | "soundtrack"
                        | "bonus"
                        | "classic"
                        | "sandbox"
                        | "doomsandbox"
                        | "launcher"
                        | "tools"
                        | "sdk"
                        | "editor"
                ) {
                    has_penalty = true;
                }
            });
        }
        if has_bin {
            score += 15;
        }
        if has_dx12 {
            score += 5;
        }
        if has_penalty {
            score -= 45;
        }
    }

    // 4. DLSS DLLs present in this directory.
    if !info.dlss.is_empty() {
        score += 40 + (info.dlss.len() as i32 * 5);
    }

    // 5. Existing `.retoque/receipt.txt` or observed Retoque proxy/companion.
    // A root receipt without an OptiScaler proxy when a strong binary subdirectory exists is a
    // root-level DLSS/shared receipt and must not promote the root stub over the binary folder.
    let root_only_receipt = dir == root && has_strong_subdir && info.proxies.is_empty();
    if (info.has_receipt || info.has_retoque) && !root_only_receipt {
        score += 30;
    }

    // 6. Executable name similarity to the game folder name.
    if info.exes.iter().any(|e| {
        let stem = e
            .name
            .strip_suffix(".exe")
            .or_else(|| e.name.strip_suffix(".EXE"))
            .unwrap_or(&e.name);
        exe_matches_game_name(stem, game_name)
    }) {
        score += 25;
    }

    // 7. Executable size (larger executables rank higher than small stub launchers).
    let max_size = info.exes.iter().map(|e| e.size).max().unwrap_or(0);
    if max_size >= 50 * 1024 * 1024 {
        score += 30;
    } else if max_size >= 10 * 1024 * 1024 {
        score += 20;
    } else if max_size >= 1024 * 1024 {
        score += 10;
    } else if dir == root && has_strong_subdir {
        // Root bootstrapper stub (e.g. Unreal's 200 KiB root launcher when Binaries/Win64 exists).
        score -= 25;
    }

    score
}

fn exe_matches_game_name(exe_stem: &str, game_name: &str) -> bool {
    let norm_exe = normalize_alnum(exe_stem);
    let norm_game = normalize_alnum(game_name);
    if norm_exe.is_empty() || norm_game.is_empty() {
        return false;
    }
    if norm_exe == norm_game {
        return true;
    }
    if norm_exe.len() >= 3 && (norm_exe.contains(&norm_game) || norm_game.contains(&norm_exe)) {
        return true;
    }
    // Check initials acronym (e.g. "Green Hell" -> "gh", "Stellar Blade" -> "sb",
    // "Grand Theft Auto V" -> "gtav").
    let initials: String = game_name
        .split(|c: char| !c.is_ascii_alphanumeric())
        .filter(|w| !w.is_empty())
        .filter_map(|w| w.chars().next())
        .map(|c| c.to_ascii_lowercase())
        .collect();
    if initials.len() >= 2 && (norm_exe == initials || norm_exe.starts_with(&initials)) {
        return true;
    }
    false
}

fn normalize_alnum(s: &str) -> String {
    let s = s
        .strip_suffix("-Win64-Shipping")
        .or_else(|| s.strip_suffix("-win64-shipping"))
        .unwrap_or(s);
    s.chars()
        .filter(|c| c.is_ascii_alphanumeric())
        .map(|c| c.to_ascii_lowercase())
        .collect()
}
