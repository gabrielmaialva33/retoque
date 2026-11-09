//! The artwork Steam already keeps on disk for each game (`appcache/librarycache/<appid>/`), so the app can show a
//! game's capsule, hero banner and logo without asking any service. Newer Steam clients keep some images one folder
//! deeper (a hashed or language subfolder), so that level is searched too.

use std::path::{Path, PathBuf};

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Artwork {
    /// Portrait cover, `library_600x900.jpg` or `library_capsule.jpg`.
    pub capsule: Option<PathBuf>,
    /// Wide background, `library_hero.jpg`.
    pub hero: Option<PathBuf>,
    /// Transparent title logo, `logo.png`.
    pub logo: Option<PathBuf>,
}

/// Steam's own folders (not its libraries) where `appcache` lives.
fn steam_roots(home: Option<&Path>) -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if let Some(h) = home {
        roots.push(h.join(".local/share/Steam"));
        roots.push(h.join(".steam/root"));
        roots.push(h.join(".var/app/com.valvesoftware.Steam/.local/share/Steam"));
    }
    #[cfg(windows)]
    roots.extend(crate::windows::windows_steam_roots());
    roots
}

fn find(dir: &Path, name: &str) -> Option<PathBuf> {
    let direct = dir.join(name);
    if direct.is_file() {
        return Some(direct);
    }
    let mut deeper: Vec<PathBuf> = std::fs::read_dir(dir)
        .ok()?
        .flatten()
        .map(|e| e.path().join(name))
        .filter(|p| p.is_file())
        .collect();
    deeper.sort();
    deeper.into_iter().next()
}

/// Artwork for a Steam app id, from the first Steam folder that has any. Reads only a few directory entries.
pub fn steam_artwork(app_id: u32, home: Option<&Path>) -> Artwork {
    for root in steam_roots(home) {
        let dir = root.join("appcache/librarycache").join(app_id.to_string());
        if !dir.is_dir() {
            continue;
        }
        let art = Artwork {
            // Older clients name the cover library_600x900.jpg; newer ones library_capsule.jpg in a subfolder.
            capsule: find(&dir, "library_600x900.jpg").or_else(|| find(&dir, "library_capsule.jpg")),
            hero: find(&dir, "library_hero.jpg"),
            logo: find(&dir, "logo.png"),
        };
        if art != Artwork::default() {
            return art;
        }
    }
    Artwork::default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_direct_and_nested_images() {
        let home = tempfile_dir();
        let dir = home.join(".local/share/Steam/appcache/librarycache/42");
        std::fs::create_dir_all(dir.join("abc")).unwrap();
        std::fs::write(dir.join("library_600x900.jpg"), b"x").unwrap();
        std::fs::write(dir.join("abc/logo.png"), b"x").unwrap();
        let art = steam_artwork(42, Some(&home));
        assert_eq!(art.capsule, Some(dir.join("library_600x900.jpg")));
        assert_eq!(art.logo, Some(dir.join("abc/logo.png")));
        assert_eq!(art.hero, None);
        assert_eq!(steam_artwork(7, Some(&home)), Artwork::default());
        std::fs::remove_dir_all(&home).unwrap();
    }

    fn tempfile_dir() -> PathBuf {
        let d = std::env::temp_dir().join(format!("retoque-art-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }
}
