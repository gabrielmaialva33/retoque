use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};

use serde_json::Value;

use crate::io::{MAX_TEXT_BYTES, read_bounded_string};
use crate::steam::{normalize_path_buf, normalize_windows_path};
use crate::targets::{find_dlss_anywhere_with_targets, find_targets_with_hint, has_elf_executable, resolve_hint_path};
use crate::{Game, Runtime, Store};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct HeroicInstalledGame {
    pub runner: String,
    pub app_name: String,
    pub title: String,
    pub install_path: PathBuf,
    pub executable: Option<PathBuf>,
    pub platform: Option<String>,
}

/// Parse a Heroic installed list JSON (`installed.json` for Legendary, GOG, or Nile).
pub fn parse_heroic_installed_json(text: &str, runner: &str) -> Result<Vec<HeroicInstalledGame>, String> {
    if text.len() > MAX_TEXT_BYTES {
        return Err(format!(
            "Heroic {runner} JSON size ({}) exceeds limit ({MAX_TEXT_BYTES})",
            text.len()
        ));
    }
    let val: Value = serde_json::from_str(text).map_err(|e| format!("invalid Heroic {runner} JSON: {e}"))?;

    let mut out = Vec::new();
    match &val {
        Value::Object(map) => {
            if let Some(Value::Array(installed_arr)) = map.get("installed") {
                for item in installed_arr {
                    if let Some(parsed) = parse_single_heroic_item(item, None, runner) {
                        out.push(parsed);
                    }
                }
            } else {
                for (key, item) in map {
                    if let Some(parsed) = parse_single_heroic_item(item, Some(key.as_str()), runner) {
                        out.push(parsed);
                    }
                }
            }
        }
        Value::Array(arr) => {
            for item in arr {
                if let Some(parsed) = parse_single_heroic_item(item, None, runner) {
                    out.push(parsed);
                }
            }
        }
        _ => return Err(format!("unexpected top-level JSON value for Heroic {runner}")),
    }

    out.sort_by(|a, b| a.app_name.cmp(&b.app_name));
    Ok(out)
}

pub fn parse_heroic_legendary_installed(text: &str) -> Result<Vec<HeroicInstalledGame>, String> {
    parse_heroic_installed_json(text, "legendary")
}

pub fn parse_heroic_gog_installed(text: &str) -> Result<Vec<HeroicInstalledGame>, String> {
    parse_heroic_installed_json(text, "gog")
}

pub fn parse_heroic_nile_installed(text: &str) -> Result<Vec<HeroicInstalledGame>, String> {
    parse_heroic_installed_json(text, "nile")
}

fn parse_single_heroic_item(item: &Value, fallback_key: Option<&str>, runner: &str) -> Option<HeroicInstalledGame> {
    let obj = item.as_object()?;

    let is_dlc = obj
        .get("is_dlc")
        .or_else(|| obj.get("isDLC"))
        .and_then(Value::as_bool)
        .unwrap_or(false);
    if is_dlc {
        return None;
    }

    let app_name = obj
        .get("app_name")
        .or_else(|| obj.get("appName"))
        .or_else(|| obj.get("id"))
        .and_then(Value::as_str)
        .or(fallback_key)?
        .trim()
        .to_string();
    if app_name.is_empty() {
        return None;
    }

    let install_str = obj
        .get("install_path")
        .or_else(|| obj.get("installPath"))
        .or_else(|| obj.get("path"))
        .and_then(Value::as_str)?
        .trim();
    if install_str.is_empty() {
        return None;
    }
    let install_path = PathBuf::from(normalize_windows_path(install_str));

    let title = obj
        .get("title")
        .or_else(|| obj.get("name"))
        .or_else(|| obj.get("app_title"))
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string)
        .unwrap_or_else(|| {
            install_path
                .file_name()
                .and_then(|s| s.to_str())
                .filter(|s| !s.is_empty())
                .unwrap_or(&app_name)
                .to_string()
        });

    let executable = obj
        .get("executable")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(PathBuf::from);

    let platform = obj
        .get("platform")
        .and_then(Value::as_str)
        .map(str::trim)
        .filter(|s| !s.is_empty())
        .map(str::to_string);

    Some(HeroicInstalledGame {
        runner: runner.to_string(),
        app_name,
        title,
        install_path,
        executable,
        platform,
    })
}

/// Pure parser for Heroic `GamesConfig/<app_name>.json` to extract `winePrefix`.
pub fn parse_heroic_game_config(text: &str, app_name: &str) -> Result<Option<PathBuf>, String> {
    if text.len() > MAX_TEXT_BYTES {
        return Err(format!(
            "Heroic GamesConfig JSON size ({}) exceeds limit ({MAX_TEXT_BYTES})",
            text.len()
        ));
    }
    let val: Value = serde_json::from_str(text).map_err(|e| format!("invalid Heroic GamesConfig JSON: {e}"))?;
    let Some(obj) = val.as_object() else {
        return Err("Heroic GamesConfig JSON is not an object".to_string());
    };

    if let Some(sub) = obj.get(app_name).and_then(Value::as_object)
        && let Some(pfx) = extract_wine_prefix(sub)
    {
        return Ok(Some(pfx));
    }

    if let Some(pfx) = extract_wine_prefix(obj) {
        return Ok(Some(pfx));
    }

    for (_, child) in obj {
        if let Some(sub) = child.as_object()
            && let Some(pfx) = extract_wine_prefix(sub)
        {
            return Ok(Some(pfx));
        }
    }

    Ok(None)
}

fn extract_wine_prefix(obj: &serde_json::Map<String, Value>) -> Option<PathBuf> {
    let raw = obj
        .get("winePrefix")
        .or_else(|| obj.get("wine_prefix"))
        .and_then(Value::as_str)?
        .trim();
    if raw.is_empty() { None } else { Some(PathBuf::from(raw)) }
}

fn parse_gog_library_titles(text: &str) -> HashMap<String, String> {
    let mut titles = HashMap::new();
    let Ok(val) = serde_json::from_str::<Value>(text) else {
        return titles;
    };
    let Some(games) = val
        .get("games")
        .or_else(|| val.get("library"))
        .and_then(Value::as_array)
    else {
        return titles;
    };
    for g in games {
        if let Some(obj) = g.as_object()
            && let Some(app_name) = obj
                .get("app_name")
                .or_else(|| obj.get("appName"))
                .and_then(Value::as_str)
            && let Some(title) = obj.get("title").and_then(Value::as_str)
            && !app_name.is_empty()
            && !title.is_empty()
        {
            titles.insert(app_name.to_string(), title.to_string());
        }
    }
    titles
}

pub fn discover_heroic(home: Option<&Path>) -> (Vec<Game>, Vec<String>) {
    let Some(h) = home else {
        return (Vec::new(), Vec::new());
    };

    let candidate_roots = [
        h.join(".config/heroic"),
        h.join(".var/app/com.heroicgameslauncher.hgl/config/heroic"),
    ];

    let mut seen_roots: HashSet<PathBuf> = HashSet::new();
    let mut roots: Vec<PathBuf> = Vec::new();
    for root in candidate_roots {
        if !root.exists() {
            continue;
        }
        let canon = fs::canonicalize(&root).unwrap_or_else(|_| root.clone());
        if seen_roots.insert(canon) {
            roots.push(root);
        }
    }

    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    let mut skipped = Vec::new();

    for root in roots {
        let gog_titles = {
            let cache_path = root.join("store_cache/gog_library.json");
            if let Ok(text) = read_bounded_string(&cache_path, MAX_TEXT_BYTES) {
                parse_gog_library_titles(&text)
            } else {
                HashMap::new()
            }
        };

        let sources: [(&str, &[&str]); 3] = [
            (
                "legendary",
                &["legendaryConfig/legendary/installed.json", "legendary/installed.json"],
            ),
            ("gog", &["gog_store/installed.json", "gog/installed.json"]),
            ("nile", &["nile_config/nile/installed.json", "nile/installed.json"]),
        ];

        for (runner, rel_paths) in sources {
            let Some(json_path) = rel_paths.iter().map(|rel| root.join(rel)).find(|p| p.exists()) else {
                continue;
            };

            let text = match read_bounded_string(&json_path, MAX_TEXT_BYTES) {
                Ok(t) => t,
                Err(e) => {
                    skipped.push(format!("heroic ({}): {e}", json_path.display()));
                    continue;
                }
            };

            let parsed_list = match parse_heroic_installed_json(&text, runner) {
                Ok(list) => list,
                Err(e) => {
                    skipped.push(format!("heroic ({}): {e}", json_path.display()));
                    continue;
                }
            };

            for mut entry in parsed_list {
                if runner == "gog"
                    && let Some(cached_title) = gog_titles.get(&entry.app_name)
                    && (entry.title == entry.app_name
                        || entry
                            .install_path
                            .file_name()
                            .and_then(|s| s.to_str())
                            .is_some_and(|f| f == entry.title))
                {
                    entry.title = cached_title.clone();
                }

                let install_dir = normalize_path_buf(&entry.install_path);
                if !install_dir.is_dir() {
                    skipped.push(format!(
                        "heroic ({}): install directory not found: {}",
                        entry.title,
                        install_dir.display()
                    ));
                    continue;
                }

                let cfg_path = root.join("GamesConfig").join(format!("{}.json", entry.app_name));
                let mut prefix = None;
                if cfg_path.exists() {
                    match read_bounded_string(&cfg_path, MAX_TEXT_BYTES) {
                        Ok(cfg_text) => match parse_heroic_game_config(&cfg_text, &entry.app_name) {
                            Ok(p) => prefix = p,
                            Err(e) => skipped.push(format!("heroic ({}): {e}", cfg_path.display())),
                        },
                        Err(e) => skipped.push(format!("heroic ({}): {e}", cfg_path.display())),
                    }
                }

                let resolved_exe = entry
                    .executable
                    .as_deref()
                    .map(|exe| resolve_hint_path(&install_dir, prefix.as_deref(), exe));
                let game_targets = find_targets_with_hint(&install_dir, resolved_exe.as_deref());
                let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);

                let is_linux_platform = entry
                    .platform
                    .as_deref()
                    .is_some_and(|p| p.eq_ignore_ascii_case("linux"));

                let runtime = if is_linux_platform
                    || (prefix.is_none() && game_targets.is_empty() && has_elf_executable(&install_dir))
                {
                    Runtime::NativeLinux
                } else {
                    Runtime::Wine { prefix }
                };

                let id = format!("heroic:{}:{}", entry.runner, entry.app_name);
                games.entry(id.clone()).or_insert(Game {
                    id,
                    name: entry.title,
                    store: Store::Heroic,
                    install_dir,
                    runtime,
                    targets: game_targets,
                    dlss_anywhere,
                    registered: true,
                });
            }
        }
    }

    (games.into_values().collect(), skipped)
}
