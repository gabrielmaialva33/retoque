//! Locates the user's Lossless Scaling files and checks existing converted shaders. Never copies or converts.

use retoque_discovery::{
    io::read_bounded_string,
    steam::{parse_appmanifest_acf, parse_library_folders_vdf},
};
use sha2::{Digest, Sha256};
use std::io::Read;
use std::path::{Path, PathBuf};

/// Deterministic filesystem helper: roots and libraryfolders are supplied by the caller, no global discovery.
pub fn find_in_steam_roots(roots: &[PathBuf]) -> Option<PathBuf> {
    let mut libraries = roots.to_vec();
    for root in roots {
        for relative in ["steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"] {
            if let Ok(text) = read_bounded_string(&root.join(relative), 1024 * 1024)
                && let Ok(paths) = parse_library_folders_vdf(&text)
            {
                libraries.extend(paths);
            }
        }
    }
    let mut seen = std::collections::HashSet::new();
    for library in libraries {
        let canonical = std::fs::canonicalize(&library).unwrap_or(library.clone());
        if !seen.insert(canonical) {
            continue;
        }
        let apps = library.join("steamapps");
        let named = read_bounded_string(&apps.join("appmanifest_993090.acf"), 1024 * 1024)
            .ok()
            .and_then(|s| parse_appmanifest_acf(&s).ok())
            .filter(|m| m.app_id == 993090)
            .filter(|m| {
                !m.install_dir.is_empty()
                    && m.install_dir != "."
                    && m.install_dir != ".."
                    && !m.install_dir.contains(['/', '\\', ':', '\0'])
            });
        let dir = apps.join("common").join(
            named
                .map(|m| m.install_dir)
                .unwrap_or_else(|| "Lossless Scaling".into()),
        );
        if dir.join("Lossless.dll").is_file() {
            return Some(dir);
        }
    }
    None
}

pub fn find_lossless_scaling(home: Option<&Path>) -> Option<PathBuf> {
    let default_home = std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" }).map(PathBuf::from);
    let root = home.or(default_home.as_deref());
    let mut roots = Vec::new();
    if let Some(home) = root {
        roots.extend([
            home.join(".local/share/Steam"),
            home.join(".steam/root"),
            home.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"),
        ]);
    }
    #[cfg(windows)]
    if home.is_none() {
        roots.extend(retoque_discovery::windows::windows_steam_roots());
    }
    find_in_steam_roots(&roots)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LsfgReadiness {
    Ready,
    NeedsShaderCache { hint: String },
    NoLosslessScaling,
}

/// Without a game folder only an explicit ShaderDir can be confirmed. A cache elsewhere is not used implicitly.
pub fn lsfg_readiness(lossless_dir: Option<&Path>, ini_shader_dir: Option<&str>, home: Option<&Path>) -> LsfgReadiness {
    lsfg_readiness_for_game(lossless_dir, ini_shader_dir, None, home)
}

/// Also inspects the runtime's default `<proxy folder>/lsfg-cache/<sha256(lsfg-vk.dll)>/`.
/// Ready means the 99 expected DXIL containers exist, not GPU/driver/format compatibility.
pub fn lsfg_readiness_for_game(
    lossless_dir: Option<&Path>,
    ini_shader_dir: Option<&str>,
    game_dir: Option<&Path>,
    home: Option<&Path>,
) -> LsfgReadiness {
    if let Some(value) = ini_shader_dir
        .map(str::trim)
        .filter(|v| !v.is_empty() && !v.eq_ignore_ascii_case("auto"))
    {
        let host = shader_host_path(value);
        if host.as_deref().is_some_and(complete_cache) {
            return LsfgReadiness::Ready;
        }
        return cache_needed(); // An explicit invalid override must not silently fall back.
    }
    let discovered = if lossless_dir.is_none() {
        find_lossless_scaling(home)
    } else {
        None
    };
    let Some(dir) = lossless_dir.or(discovered.as_deref()) else {
        return LsfgReadiness::NoLosslessScaling;
    };
    if !dir.join("Lossless.dll").is_file() && !dir.join("lsfg-vk.dll").is_file() {
        return LsfgReadiness::NoLosslessScaling;
    }
    if let Some(game) = game_dir
        && let Ok((mut file, _)) = retoque_discovery::io::open_regular_file(&dir.join("lsfg-vk.dll"))
    {
        let mut digest = Sha256::new();
        let mut block = [0; 65536];
        loop {
            match file.read(&mut block) {
                Ok(0) => break,
                Ok(n) => digest.update(&block[..n]),
                Err(_) => return cache_needed(),
            }
        }
        let cache = game.join("lsfg-cache").join(format!("{:x}", digest.finalize()));
        if complete_cache(&cache) {
            return LsfgReadiness::Ready;
        }
    }
    cache_needed()
}

fn cache_needed() -> LsfgReadiness {
    // This is a command template, never executed here. It contains no user paths or licensed content.
    LsfgReadiness::NeedsShaderCache { hint: concat!(
        "From the Retoque source checkout, run: python3 tools/lsfg-study/convert.py \"<Lossless Scaling>/lsfg-vk.dll\" --out \"<shader folder>\". ",
        "Requires spirv-cross and dxc. Then set [LSFG] ShaderDir to that folder (0.cso through 98.cso), ",
        "or use --cache-dir \"<game proxy folder>/lsfg-cache\" to create the runtime's hash-keyed cache. ",
        "The app does not copy or convert Lossless Scaling files."
    ).into() }
}

fn shader_host_path(value: &str) -> Option<PathBuf> {
    #[cfg(windows)]
    {
        let p = PathBuf::from(value);
        if p.is_absolute() {
            return Some(p);
        }
    }
    #[cfg(not(windows))]
    {
        if let Some(rest) = value.strip_prefix("Z:\\").or_else(|| value.strip_prefix("z:\\")) {
            return Some(PathBuf::from(format!("/{}", rest.replace('\\', "/"))));
        }
        if value.starts_with('/') {
            return Some(PathBuf::from(value));
        }
    }
    if value.contains(':') || value.starts_with('\\') {
        return None;
    }
    // The runtime resolves a relative override against its process working directory, which is unknown here.
    None
}

fn complete_cache(dir: &Path) -> bool {
    (0..99).all(|rid| {
        let Ok((mut file, meta)) = retoque_discovery::io::open_regular_file(&dir.join(format!("{rid}.cso"))) else {
            return false;
        };
        if !(32..=4 * 1024 * 1024).contains(&meta.len()) {
            return false;
        }
        let mut magic = [0; 4];
        file.read_exact(&mut magic).is_ok() && magic == *b"DXBC"
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn cache(dir: &Path) {
        std::fs::create_dir_all(dir).unwrap();
        let mut bytes = vec![0; 32];
        bytes[..4].copy_from_slice(b"DXBC");
        for i in 0..99 {
            std::fs::write(dir.join(format!("{i}.cso")), &bytes).unwrap();
        }
    }
    #[test]
    fn finds_app_in_second_library_and_rejects_manifest_traversal() {
        let temp = tempfile::tempdir().unwrap();
        let root = temp.path().join("Steam");
        let library = temp.path().join("library");
        std::fs::create_dir_all(root.join("steamapps")).unwrap();
        let dir = library.join("steamapps/common/Named LS");
        std::fs::create_dir_all(&dir).unwrap();
        std::fs::write(dir.join("Lossless.dll"), "synthetic").unwrap();
        std::fs::write(
            root.join("steamapps/libraryfolders.vdf"),
            format!(
                r#""libraryfolders" {{ "1" {{ "path" "{}" }} }}"#,
                library.display().to_string().replace('\\', "\\\\")
            ),
        )
        .unwrap();
        let manifest = library.join("steamapps/appmanifest_993090.acf");
        std::fs::write(
            &manifest,
            r#""AppState" { "appid" "993090" "name" "LS" "installdir" "Named LS" }"#,
        )
        .unwrap();
        assert_eq!(find_in_steam_roots(std::slice::from_ref(&root)), Some(dir));
        std::fs::write(
            &manifest,
            r#""AppState" { "appid" "993090" "name" "LS" "installdir" "../../escape" }"#,
        )
        .unwrap();
        assert_eq!(find_in_steam_roots(&[root]), None);
    }
    #[test]
    fn readiness_requires_complete_cache_and_follows_override_precedence() {
        let temp = tempfile::tempdir().unwrap();
        let ls = temp.path().join("LS");
        let game = temp.path().join("game");
        std::fs::create_dir(&ls).unwrap();
        std::fs::write(ls.join("Lossless.dll"), "synthetic").unwrap();
        std::fs::write(ls.join("lsfg-vk.dll"), "synthetic shaders").unwrap();
        assert!(matches!(
            lsfg_readiness(Some(&ls), None, Some(temp.path())),
            LsfgReadiness::NeedsShaderCache { .. }
        ));
        let cached = game
            .join("lsfg-cache")
            .join(format!("{:x}", Sha256::digest(b"synthetic shaders")));
        cache(&cached);
        assert_eq!(
            lsfg_readiness_for_game(Some(&ls), None, Some(&game), Some(temp.path())),
            LsfgReadiness::Ready
        );
        assert!(matches!(
            lsfg_readiness_for_game(Some(&ls), Some("missing"), Some(&game), Some(temp.path())),
            LsfgReadiness::NeedsShaderCache { .. }
        ));
        let path = cached.to_string_lossy();
        assert_eq!(
            lsfg_readiness(None, Some(&path), Some(temp.path())),
            LsfgReadiness::Ready
        );
        std::fs::remove_file(cached.join("98.cso")).unwrap();
        assert!(matches!(
            lsfg_readiness(None, Some(&path), Some(temp.path())),
            LsfgReadiness::NeedsShaderCache { .. }
        ));
        assert_eq!(
            lsfg_readiness(Some(&temp.path().join("missing")), None, Some(temp.path())),
            LsfgReadiness::NoLosslessScaling
        );
    }
}
