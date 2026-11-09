use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use crate::io::{MAX_TEXT_BYTES, read_bounded_string};
use crate::targets::{find_dlss_anywhere_with_targets, find_targets, has_elf_executable};
use crate::{Game, Runtime, Store};

const MAX_VDF_DEPTH: usize = 64;

/// Normalize a path string when it starts with a Windows drive prefix (`c:/...` or `C:\...`),
/// uppercasing the drive letter and converting mixed `/` and `\` separators to `\`.
pub fn normalize_windows_path(raw: &str) -> String {
    let trimmed = raw.trim();
    let bytes = trimmed.as_bytes();
    if bytes.len() >= 2
        && bytes[0].is_ascii_alphabetic()
        && bytes[1] == b':'
        && (bytes.len() == 2 || bytes[2] == b'/' || bytes[2] == b'\\')
    {
        let drive = (bytes[0] as char).to_ascii_uppercase();
        let rest = &trimmed[2..];
        let segs: Vec<&str> = rest.split(['/', '\\']).filter(|s| !s.is_empty() && *s != ".").collect();
        if segs.is_empty() {
            format!("{drive}:\\")
        } else {
            format!("{drive}:\\{}", segs.join("\\"))
        }
    } else {
        trimmed.to_string()
    }
}

pub fn normalize_path_buf(path: &Path) -> PathBuf {
    let s = path.to_string_lossy();
    let norm = normalize_windows_path(&s);
    if norm == s.as_ref() {
        path.to_path_buf()
    } else {
        PathBuf::from(norm)
    }
}

fn canonical_dedup_key(path: &Path) -> String {
    if let Ok(canon) = fs::canonicalize(path) {
        let norm = normalize_path_buf(&canon);
        let s = norm.to_string_lossy();
        if cfg!(windows) || s.as_bytes().get(1) == Some(&b':') {
            return s.to_ascii_lowercase();
        }
        return s.into_owned();
    }
    let norm = normalize_path_buf(path);
    let s = norm.to_string_lossy();
    if cfg!(windows) || s.as_bytes().get(1) == Some(&b':') {
        s.to_ascii_lowercase()
    } else {
        s.into_owned()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct SteamAppManifest {
    pub app_id: u32,
    pub name: String,
    pub install_dir: String,
    pub uses_proton_override: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum VdfValue {
    Str(String),
    Map(Vec<(String, VdfValue)>),
}

impl VdfValue {
    pub fn as_str(&self) -> Option<&str> {
        match self {
            VdfValue::Str(s) => Some(s),
            VdfValue::Map(_) => None,
        }
    }

    pub fn as_map(&self) -> Option<&[(String, VdfValue)]> {
        match self {
            VdfValue::Map(m) => Some(m),
            VdfValue::Str(_) => None,
        }
    }

    pub fn get(&self, key: &str) -> Option<&VdfValue> {
        let entries = self.as_map()?;
        entries
            .iter()
            .find(|(k, _)| k.eq_ignore_ascii_case(key))
            .map(|(_, v)| v)
    }

    pub fn get_str(&self, key: &str) -> Option<&str> {
        self.get(key)?.as_str()
    }
}

/// Parse Valve KeyValues (VDF / ACF) text into a structured tree using an explicit bounded stack.
pub fn parse_vdf(text: &str) -> Result<Vec<(String, VdfValue)>, String> {
    if text.len() > MAX_TEXT_BYTES {
        return Err(format!("VDF size ({}) exceeds limit ({MAX_TEXT_BYTES})", text.len()));
    }
    let tokens = tokenize_vdf(text)?;
    parse_vdf_iterative(tokens)
}

#[derive(Debug, PartialEq, Eq)]
enum Token {
    Str(String),
    OpenBrace,
    CloseBrace,
}

fn tokenize_vdf(text: &str) -> Result<Vec<Token>, String> {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let bytes = text.as_bytes();
    let n = bytes.len();
    let mut tokens = Vec::with_capacity(n / 16);
    let mut i = 0;

    while i < n {
        let b = bytes[i];
        if b.is_ascii_whitespace() {
            i += 1;
            continue;
        }
        if b >= 0x80 {
            let ch = text[i..].chars().next().unwrap();
            if ch.is_whitespace() {
                i += ch.len_utf8();
                continue;
            }
        }
        if b == b'/' && i + 1 < n && bytes[i + 1] == b'/' {
            i += 2;
            while i < n && bytes[i] != b'\n' {
                i += 1;
            }
            continue;
        }
        if b == b'{' {
            tokens.push(Token::OpenBrace);
            i += 1;
            continue;
        }
        if b == b'}' {
            tokens.push(Token::CloseBrace);
            i += 1;
            continue;
        }
        if b == b'"' {
            i += 1;
            let start = i;
            let mut has_escape = false;
            let mut closed = false;
            while i < n {
                let cur = bytes[i];
                if cur == b'"' {
                    closed = true;
                    break;
                }
                if cur == b'\\' {
                    has_escape = true;
                    i += 1;
                    if i < n {
                        let ch = text[i..].chars().next().unwrap();
                        i += ch.len_utf8();
                    }
                    continue;
                }
                i += 1;
            }
            if !closed {
                return Err("unterminated quoted string in VDF".to_string());
            }
            let slice = &text[start..i];
            i += 1; // consume closing '"'
            if !has_escape {
                tokens.push(Token::Str(slice.to_string()));
            } else {
                let mut s = String::with_capacity(slice.len());
                let mut chars = slice.chars();
                while let Some(ch) = chars.next() {
                    if ch == '\\' {
                        match chars.next() {
                            Some('\\') => s.push('\\'),
                            Some('"') => s.push('"'),
                            Some('n') => s.push('\n'),
                            Some('t') => s.push('\t'),
                            Some(other) => {
                                s.push('\\');
                                s.push(other);
                            }
                            None => s.push('\\'),
                        }
                    } else {
                        s.push(ch);
                    }
                }
                tokens.push(Token::Str(s));
            }
            continue;
        }

        // Unquoted token fallback.
        let start = i;
        while i < n {
            let cur = bytes[i];
            if cur.is_ascii_whitespace() || cur == b'{' || cur == b'}' || cur == b'"' {
                break;
            }
            if cur >= 0x80 {
                let ch = text[i..].chars().next().unwrap();
                if ch.is_whitespace() {
                    break;
                }
                i += ch.len_utf8();
            } else {
                i += 1;
            }
        }
        if i > start {
            tokens.push(Token::Str(text[start..i].to_string()));
        }
    }

    Ok(tokens)
}

fn parse_vdf_iterative(tokens: Vec<Token>) -> Result<Vec<(String, VdfValue)>, String> {
    let mut root: Vec<(String, VdfValue)> = Vec::new();
    let mut stack: Vec<(String, Vec<(String, VdfValue)>)> = Vec::new();
    let mut iter = tokens.into_iter();

    while let Some(tok) = iter.next() {
        match tok {
            Token::CloseBrace => {
                let Some((key, entries)) = stack.pop() else {
                    return Err("unexpected '}' in VDF".to_string());
                };
                let target = stack.last_mut().map(|(_, e)| e).unwrap_or(&mut root);
                target.push((key, VdfValue::Map(entries)));
            }
            Token::OpenBrace => {
                return Err("unexpected '{' in VDF where key was expected".to_string());
            }
            Token::Str(key) => {
                let Some(next_tok) = iter.next() else {
                    return Err(format!("unexpected end of VDF after key '{key}'"));
                };
                match next_tok {
                    Token::Str(val) => {
                        let target = stack.last_mut().map(|(_, e)| e).unwrap_or(&mut root);
                        target.push((key, VdfValue::Str(val)));
                    }
                    Token::OpenBrace => {
                        if stack.len() >= MAX_VDF_DEPTH {
                            return Err(format!("VDF nesting depth exceeds limit ({MAX_VDF_DEPTH})"));
                        }
                        stack.push((key, Vec::new()));
                    }
                    Token::CloseBrace => {
                        return Err(format!("unexpected '}}' after key '{key}' in VDF"));
                    }
                }
            }
        }
    }

    if !stack.is_empty() {
        return Err("unclosed '{' in VDF".to_string());
    }
    Ok(root)
}

/// Pure parser for Steam `steamapps/libraryfolders.vdf`.
pub fn parse_library_folders_vdf(text: &str) -> Result<Vec<PathBuf>, String> {
    let root = parse_vdf(text)?;
    let root_map = VdfValue::Map(root);
    let lf = root_map
        .get("libraryfolders")
        .ok_or_else(|| "missing 'libraryfolders' section in VDF".to_string())?;
    let entries = lf
        .as_map()
        .ok_or_else(|| "'libraryfolders' is not a map in VDF".to_string())?;

    let mut out = Vec::new();
    for (key, val) in entries {
        let raw_path = match val {
            VdfValue::Map(_) => val.get_str("path"),
            VdfValue::Str(s) if key.chars().all(|c| c.is_ascii_digit()) => Some(s.as_str()),
            VdfValue::Str(_) => None,
        };
        if let Some(p_str) = raw_path
            && !p_str.trim().is_empty()
        {
            let p = PathBuf::from(normalize_windows_path(p_str));
            if !out.contains(&p) {
                out.push(p);
            }
        }
    }
    Ok(out)
}

/// Pure parser for Steam `steamapps/appmanifest_*.acf`.
pub fn parse_appmanifest_acf(text: &str) -> Result<SteamAppManifest, String> {
    let root = parse_vdf(text)?;
    let root_map = VdfValue::Map(root);
    let app_state = root_map
        .get("AppState")
        .ok_or_else(|| "missing 'AppState' section in ACF".to_string())?;

    let app_id_str = app_state
        .get_str("appid")
        .ok_or_else(|| "missing 'appid' in AppState".to_string())?;
    let app_id: u32 = app_id_str
        .trim()
        .parse()
        .map_err(|_| format!("invalid appid '{app_id_str}' in ACF"))?;

    let name = app_state
        .get_str("name")
        .ok_or_else(|| format!("missing 'name' in AppState for appid {app_id}"))?
        .to_string();

    let install_dir = app_state
        .get_str("installdir")
        .ok_or_else(|| format!("missing 'installdir' in AppState for appid {app_id}"))?
        .to_string();

    let uses_proton_override = ["UserConfig", "MountedConfig"].iter().any(|sec| {
        app_state.get(sec).is_some_and(|cfg| {
            cfg.get_str("platform_override_source")
                .is_some_and(|s| s.eq_ignore_ascii_case("windows"))
                || cfg.get_str("compat_tool").is_some_and(is_proton_or_wine_tool)
        })
    });

    Ok(SteamAppManifest {
        app_id,
        name,
        install_dir,
        uses_proton_override,
    })
}

/// Pure parser for `CompatToolMapping` inside Steam's `config/config.vdf` (or standalone VDF).
pub fn parse_compat_tool_mapping_vdf(text: &str) -> Result<HashMap<u32, String>, String> {
    let root = parse_vdf(text)?;
    let root_val = VdfValue::Map(root);
    let mut map = HashMap::new();
    if let Some(mapping_node) = find_vdf_key_bounded(&root_val, "CompatToolMapping", 0)
        && let Some(entries) = mapping_node.as_map()
    {
        for (app_id_str, val) in entries {
            let Ok(app_id) = app_id_str.trim().parse::<u32>() else {
                continue;
            };
            let tool_name = match val {
                VdfValue::Map(_) => val.get_str("name").map(str::to_string),
                VdfValue::Str(s) => Some(s.clone()),
            };
            if let Some(name) = tool_name
                && !name.trim().is_empty()
            {
                map.insert(app_id, name);
            }
        }
    }
    Ok(map)
}

fn find_vdf_key_bounded<'a>(node: &'a VdfValue, target_key: &str, depth: usize) -> Option<&'a VdfValue> {
    if depth > MAX_VDF_DEPTH {
        return None;
    }
    if let Some(found) = node.get(target_key) {
        return Some(found);
    }
    if let Some(entries) = node.as_map() {
        for (_, child) in entries {
            if let Some(found) = find_vdf_key_bounded(child, target_key, depth + 1) {
                return Some(found);
            }
        }
    }
    None
}

/// Returns `true` if the tool name in `CompatToolMapping` is a Proton or Wine compatibility tool
/// rather than a native Linux runtime container.
pub fn is_proton_or_wine_tool(tool_name: &str) -> bool {
    let lower = tool_name.trim().to_ascii_lowercase();
    if lower.is_empty() {
        return false;
    }
    if lower.starts_with("steamlinuxruntime") || lower == "sniper" || lower == "soldier" || lower == "scout" {
        return false;
    }
    true
}

/// Returns `true` if a Steam app is a runtime or redistributable tool rather than a game.
pub fn is_steam_tool_or_runtime(app_id: u32, name: &str, install_dir: &str) -> bool {
    if app_id == 228980 {
        return true;
    }
    let name_lower = name.trim().to_ascii_lowercase();
    let dir_lower = install_dir.trim().to_ascii_lowercase();

    if name_lower == "proton"
        || name_lower.starts_with("proton ")
        || name_lower.starts_with("proton-")
        || dir_lower == "proton"
        || dir_lower.starts_with("proton ")
        || dir_lower.starts_with("proton-")
    {
        return true;
    }

    if name_lower.starts_with("steam linux runtime") || dir_lower.starts_with("steamlinuxruntime") {
        return true;
    }

    if name_lower == "steamworks common redistributables"
        || dir_lower == "steamworks shared"
        || dir_lower == "steam controller configs"
    {
        return true;
    }

    false
}

pub fn discover_steam(home: Option<&Path>, extra_roots: &[PathBuf], windows_runtime: bool) -> (Vec<Game>, Vec<String>) {
    let mut candidate_roots: Vec<PathBuf> = Vec::new();
    if let Some(h) = home {
        candidate_roots.push(h.join(".local/share/Steam"));
        candidate_roots.push(h.join(".steam/root"));
        candidate_roots.push(h.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"));
    }
    for r in extra_roots {
        candidate_roots.push(normalize_path_buf(r));
    }

    let mut seen_roots: HashSet<String> = HashSet::new();
    let mut steam_roots: Vec<PathBuf> = Vec::new();
    for root in candidate_roots {
        if !root.exists() {
            continue;
        }
        let key = canonical_dedup_key(&root);
        if seen_roots.insert(key) {
            steam_roots.push(normalize_path_buf(&root));
        }
    }

    let mut skipped = Vec::new();
    let mut compat_tools: HashMap<u32, String> = HashMap::new();
    let mut library_dirs: Vec<PathBuf> = Vec::new();
    let mut seen_libs: HashSet<String> = HashSet::new();

    for root in &steam_roots {
        let config_vdf = root.join("config").join("config.vdf");
        if config_vdf.exists()
            && let Ok(text) = read_bounded_string(&config_vdf, MAX_TEXT_BYTES)
            && let Ok(mapping) = parse_compat_tool_mapping_vdf(&text)
        {
            for (k, v) in mapping {
                compat_tools.entry(k).or_insert(v);
            }
        }

        let root_key = canonical_dedup_key(root);
        if seen_libs.insert(root_key) {
            library_dirs.push(normalize_path_buf(root));
        }

        let lf_vdf = root.join("steamapps").join("libraryfolders.vdf");
        if lf_vdf.exists() {
            match read_bounded_string(&lf_vdf, MAX_TEXT_BYTES) {
                Ok(text) => match parse_library_folders_vdf(&text) {
                    Ok(paths) => {
                        for p in paths {
                            let norm_p = normalize_path_buf(&p);
                            let key = canonical_dedup_key(&norm_p);
                            if seen_libs.insert(key) {
                                library_dirs.push(norm_p);
                            }
                        }
                    }
                    Err(e) => skipped.push(format!("steam ({}): {e}", lf_vdf.display())),
                },
                Err(e) => skipped.push(format!("steam ({}): {e}", lf_vdf.display())),
            }
        }
    }

    let mut games_by_appid: BTreeMap<u32, Game> = BTreeMap::new();
    let mut claimed_common_dirs: HashSet<String> = HashSet::new();
    let mut orphan_games: Vec<Game> = Vec::new();

    for lib in &library_dirs {
        let steamapps = lib.join("steamapps");
        if !steamapps.exists() {
            continue;
        }
        let read_dir = match fs::read_dir(&steamapps) {
            Ok(rd) => rd,
            Err(e) => {
                skipped.push(format!("steam ({}): {e}", steamapps.display()));
                continue;
            }
        };

        let mut acf_paths: Vec<PathBuf> = read_dir
            .flatten()
            .map(|e| e.path())
            .filter(|p| {
                p.file_name()
                    .and_then(|n| n.to_str())
                    .is_some_and(|n| n.starts_with("appmanifest_") && n.ends_with(".acf"))
            })
            .collect();
        acf_paths.sort();

        for acf_path in acf_paths {
            let text = match read_bounded_string(&acf_path, MAX_TEXT_BYTES) {
                Ok(t) => t,
                Err(e) => {
                    skipped.push(format!("steam ({}): {e}", acf_path.display()));
                    continue;
                }
            };
            let manifest = match parse_appmanifest_acf(&text) {
                Ok(m) => m,
                Err(e) => {
                    skipped.push(format!("steam ({}): {e}", acf_path.display()));
                    continue;
                }
            };

            claimed_common_dirs.insert(manifest.install_dir.to_ascii_lowercase());

            if is_steam_tool_or_runtime(manifest.app_id, &manifest.name, &manifest.install_dir) {
                continue;
            }

            let install_dir = normalize_path_buf(&steamapps.join("common").join(&manifest.install_dir));
            if !install_dir.is_dir() {
                skipped.push(format!(
                    "steam ({}): install directory not found: {}",
                    manifest.name,
                    install_dir.display()
                ));
                continue;
            }
            if games_by_appid.contains_key(&manifest.app_id) {
                continue;
            }

            let game_targets = find_targets(&install_dir);
            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let runtime = if windows_runtime {
                Runtime::Windows
            } else {
                resolve_steam_linux_runtime(
                    manifest.app_id,
                    manifest.uses_proton_override,
                    lib,
                    &library_dirs,
                    &compat_tools,
                    &install_dir,
                    !game_targets.is_empty(),
                )
            };

            games_by_appid.insert(
                manifest.app_id,
                Game {
                    id: format!("steam:{}", manifest.app_id),
                    name: manifest.name,
                    store: Store::Steam,
                    install_dir,
                    runtime,
                    targets: game_targets,
                    dlss_anywhere,
                    registered: true,
                },
            );
        }
    }

    // Scan `<library>/steamapps/common/*` for unregistered game folders without an appmanifest.
    for lib in &library_dirs {
        let common_dir = lib.join("steamapps").join("common");
        let Ok(read_dir) = fs::read_dir(&common_dir) else {
            continue;
        };

        let mut subdirs: Vec<PathBuf> = read_dir
            .flatten()
            .filter(|e| e.file_type().is_ok_and(|ft| ft.is_dir()))
            .map(|e| e.path())
            .collect();
        subdirs.sort();

        for folder_path in subdirs {
            let Some(folder_os) = folder_path.file_name() else {
                continue;
            };
            let folder_name = folder_os.to_string_lossy().into_owned();
            if folder_name.is_empty()
                || claimed_common_dirs.contains(&folder_name.to_ascii_lowercase())
                || is_steam_tool_or_runtime(0, &folder_name, &folder_name)
            {
                continue;
            }

            let install_dir = normalize_path_buf(&folder_path);
            let game_targets = find_targets(&install_dir);
            if game_targets.is_empty() {
                continue;
            }

            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);
            let runtime = if windows_runtime {
                Runtime::Windows
            } else {
                Runtime::Wine { prefix: None }
            };

            claimed_common_dirs.insert(folder_name.to_ascii_lowercase());
            orphan_games.push(Game {
                id: format!("steam-folder:{}/{folder_name}", lib.display()),
                name: folder_name,
                store: Store::Steam,
                install_dir,
                runtime,
                targets: game_targets,
                dlss_anywhere,
                registered: false,
            });
        }
    }

    let mut out: Vec<Game> = games_by_appid.into_values().collect();
    out.extend(orphan_games);
    (out, skipped)
}

fn resolve_steam_linux_runtime(
    app_id: u32,
    uses_proton_override: bool,
    current_lib: &Path,
    all_libs: &[PathBuf],
    compat_tools: &HashMap<u32, String>,
    install_dir: &Path,
    has_exe_targets: bool,
) -> Runtime {
    let app_id_str = app_id.to_string();
    let local_pfx = current_lib
        .join("steamapps")
        .join("compatdata")
        .join(&app_id_str)
        .join("pfx");

    let prefix = if local_pfx.is_dir() {
        Some(local_pfx)
    } else {
        all_libs
            .iter()
            .map(|l| l.join("steamapps").join("compatdata").join(&app_id_str).join("pfx"))
            .find(|p| p.is_dir())
    };

    if prefix.is_some() {
        return Runtime::Proton { app_id, prefix };
    }

    if let Some(tool) = compat_tools.get(&app_id) {
        if is_proton_or_wine_tool(tool) {
            return Runtime::Proton { app_id, prefix: None };
        } else if !has_exe_targets {
            return Runtime::NativeLinux;
        }
    }

    if uses_proton_override {
        return Runtime::Proton { app_id, prefix: None };
    }

    if !has_exe_targets && has_elf_executable(install_dir) {
        return Runtime::NativeLinux;
    }

    if has_exe_targets {
        return Runtime::Proton { app_id, prefix: None };
    }

    Runtime::Unknown
}
