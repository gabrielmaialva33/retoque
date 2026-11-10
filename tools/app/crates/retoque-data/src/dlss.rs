//! NVIDIA's pinned DLSS SR/RR/FG catalog. Never ships DLLs; explicit consent precedes downloads.
use crate::components::{HeldDir, open_regular};
pub use crate::sm86::Fetcher;
use cap_fs_ext::DirExt;
use cap_std::fs::{Dir, OpenOptions};
use sha2::{Digest, Sha256};
use std::io::{Read, Write};
use std::{
    fs, io,
    path::{Path, PathBuf},
};
pub const VERSION: &str = "310.9.16";
const PREFIX: &str = "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/";
const LICENSE: &str = "LICENSE.txt";
pub const MARKER: &[u8] = b"Retoque DLSS receipt v1\n";
#[derive(Clone, Copy)]
pub struct Asset {
    pub name: &'static str,
    pub sha256: &'static str,
    pub size: u64,
}
pub const ASSETS: &[Asset] = &[
    Asset {
        name: "nvngx_dlss.dll",
        sha256: "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        size: 58_956_912,
    },
    Asset {
        name: "nvngx_dlssd.dll",
        sha256: "b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee",
        size: 48_343_664,
    },
    Asset {
        name: "nvngx_dlssg.dll",
        sha256: "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7",
        size: 7_506_544,
    },
    Asset {
        name: LICENSE,
        sha256: "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
        size: 26_620,
    },
];
fn error(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, s)
}
fn url(asset: &Asset) -> String {
    format!(
        "{PREFIX}{}{}",
        if asset.name == LICENSE {
            ""
        } else {
            "lib/Windows_x86_64/rel/"
        },
        asset.name
    )
}
fn allowed(value: &str, redirect: bool) -> bool {
    if ASSETS.iter().any(|a| value == url(a)) {
        return true;
    }
    let Ok(u) = value.parse::<ureq::http::Uri>() else {
        return false;
    };
    let Some(host) = u.host() else { return false };
    redirect
        && u.scheme_str() == Some("https")
        && u.authority().is_some_and(|a| a.as_str() == host)
        && !value.contains('#')
        && matches!(
            host,
            "media.githubusercontent.com" | "objects.githubusercontent.com" | "release-assets.githubusercontent.com"
        )
}
pub struct NvidiaFetcher;
impl Fetcher for NvidiaFetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
        crate::fetch::get(url, limit, progress, allowed)
    }
}
fn verify_bytes(bytes: &[u8], asset: &Asset) -> io::Result<()> {
    if bytes.len() as u64 != asset.size || format!("{:x}", Sha256::digest(bytes)) != asset.sha256 {
        return Err(error("DLSS size or SHA-256 mismatch"));
    }
    Ok(())
}
fn checked_dir(path: &Path) -> io::Result<()> {
    if let Ok(m) = fs::symlink_metadata(path) {
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if m.file_attributes() & 0x400 != 0 {
                return Err(error("linked DLSS catalog"));
            }
        }
        if !m.is_dir() || m.file_type().is_symlink() {
            return Err(error("invalid DLSS catalog directory"));
        }
    }
    Ok(())
}
/// Bytes a full catalog download receives, for progress.
pub fn download_size() -> u64 {
    ASSETS.iter().map(|a| a.size).sum()
}
pub fn catalog_path(data: &Path) -> PathBuf {
    data.join("dlss").join(VERSION)
}
pub fn verified_catalog(data: &Path) -> io::Result<Option<PathBuf>> {
    checked_dir(&data.join("dlss"))?;
    let root = catalog_path(data);
    if !root.try_exists()? {
        return Ok(None);
    }
    checked_dir(&root)?;
    let held = HeldDir::open(&root)?;
    for a in ASSETS {
        let file = open_regular(&held.dir, a.name)?;
        if file.metadata()?.len() != a.size {
            return Err(error("invalid DLSS catalog file"));
        }
        let mut bytes = Vec::new();
        file.take(a.size + 1).read_to_end(&mut bytes)?;
        verify_bytes(&bytes, a)?;
    }
    let mut marker = Vec::new();
    open_regular(&held.dir, "receipt-marker.txt")?
        .take(128)
        .read_to_end(&mut marker)?;
    if marker != MARKER {
        return Err(error("invalid DLSS catalog marker"));
    }
    held.current()?;
    Ok(Some(root))
}
/// Downloads all three variants and the original license. Never overwrites a damaged catalog silently.
pub fn download(
    data: &Path,
    consent: bool,
    fetcher: &(dyn Fetcher + Sync),
    progress: &mut dyn FnMut(u64),
) -> io::Result<PathBuf> {
    if !consent {
        return Err(error("DLSS download requires confirmation"));
    }
    if let Some(path) = verified_catalog(data)? {
        return Ok(path);
    }
    let parent = data.join("dlss");
    checked_dir(&parent)?;
    fs::create_dir_all(&parent)?;
    let held = HeldDir::open(&parent)?;
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let name = format!(
        ".download-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    );
    held.dir.create_dir(&name)?;
    let result = (|| {
        let stage = held.dir.open_dir_nofollow(&name)?;
        fetch_artifacts(&stage, ASSETS, fetcher, progress)?;
        drop(stage);
        held.current()?;
        if held.dir.symlink_metadata(VERSION).is_ok() {
            return Err(error("DLSS catalog appeared during download"));
        }
        held.dir.rename(&name, &held.dir, VERSION)?;
        Ok(catalog_path(data))
    })();
    let _ = held.dir.remove_dir_all(&name);
    result
}
fn write_new(directory: &Dir, name: &str, bytes: &[u8]) -> io::Result<()> {
    let mut file = directory.open_with(name, OpenOptions::new().write(true).create_new(true))?;
    file.write_all(bytes)?;
    file.sync_all()
}
fn fetch_artifacts(
    directory: &Dir,
    assets: &[Asset],
    fetcher: &(dyn Fetcher + Sync),
    progress: &mut dyn FnMut(u64),
) -> io::Result<()> {
    // GitHub serves each connection at a few hundred KiB/s, so the files come down side by side. Each file is
    // verified before it is written to the private stage, which is only published after all of them pass.
    use std::sync::atomic::{AtomicU64, Ordering};
    let received: Vec<AtomicU64> = assets.iter().map(|_| AtomicU64::new(0)).collect();
    let results = std::thread::scope(|scope| {
        let handles: Vec<_> = assets
            .iter()
            .zip(&received)
            .map(|(a, counter)| {
                scope.spawn(move || {
                    let bytes = fetcher.get(&url(a), a.size, &mut |n| counter.store(n, Ordering::Relaxed))?;
                    verify_bytes(&bytes, a)?;
                    write_new(directory, a.name, &bytes)
                })
            })
            .collect();
        let mut reported = u64::MAX;
        while handles.iter().any(|h| !h.is_finished()) {
            let total = received.iter().map(|c| c.load(Ordering::Relaxed)).sum();
            if total != reported {
                reported = total;
                progress(total);
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        handles
            .into_iter()
            .map(|h| {
                h.join()
                    .unwrap_or_else(|_| Err(error("DLSS download stopped unexpectedly")))
            })
            .collect::<Vec<_>>()
    });
    results.into_iter().collect::<io::Result<()>>()?;
    progress(assets.iter().map(|a| a.size).sum());
    write_new(directory, "receipt-marker.txt", MARKER)?;
    let manifest = assets
        .iter()
        .map(|a| format!("{} {} {}\n", a.sha256, a.size, a.name))
        .collect::<String>();
    write_new(
        directory,
        "SHA256SUMS.txt",
        format!("NVIDIA DLSS {VERSION}\n{PREFIX}\n{manifest}").as_bytes(),
    )?;
    Ok(())
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GameDll {
    pub path: String,
    pub version: Option<String>,
}
pub fn inspect(install_dir: &Path) -> io::Result<Vec<GameDll>> {
    retoque_discovery::find_dlss_paths(install_dir)?
        .into_iter()
        .map(|path| {
            Ok(GameDll {
                version: retoque_core::pe_version::file_version(&install_dir.join(&path)),
                path,
            })
        })
        .collect()
}
/// True only for a readable version that is the catalog's or newer; unknown versions are never "current".
pub fn current(version: &str) -> bool {
    let parts = version.split('.').collect::<Vec<_>>();
    (3..=4).contains(&parts.len()) && parts.iter().all(|p| p.parse::<u32>().is_ok()) && !older(version)
}
pub fn older(version: &str) -> bool {
    let mut numbers = [0u32; 4];
    let parts = version.split('.').collect::<Vec<_>>();
    if !(3..=4).contains(&parts.len()) {
        return false;
    }
    for (i, p) in parts.iter().enumerate() {
        let Ok(n) = p.parse() else { return false };
        numbers[i] = n;
    }
    numbers < [310, 9, 16, 0]
}
#[derive(Clone, Debug)]
pub struct Update {
    pub install_dir: PathBuf,
    pub data_dir: PathBuf,
}
pub fn plan(target: &Path, update: &Update) -> Result<retoque_core::Plan, String> {
    let catalog = verified_catalog(&update.data_dir)
        .map_err(|e| e.to_string())?
        .ok_or("DLSS catalog is not downloaded")?;
    let files = inspect(&update.install_dir)
        .map_err(|e| e.to_string())?
        .into_iter()
        .filter(|d| d.version.as_deref().is_none_or(older))
        .map(|d| d.path)
        .collect::<Vec<_>>();
    let plan =
        retoque_core::plan_dlss_update(&update.install_dir, target, &catalog, &files).map_err(|e| e.to_string())?;
    // Tie the core's source snapshots to the pinned catalog, including a change during planning.
    verified_catalog(&update.data_dir)
        .map_err(|e| e.to_string())?
        .ok_or("DLSS catalog disappeared")?;
    retoque_core::validate_plan(&plan).map_err(|e| e.to_string())?;
    Ok(plan)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn injected_good_bad_and_oversize_downloads() {
        struct Fake(&'static [u8]);
        impl Fetcher for Fake {
            fn get(&self, _: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
                assert_eq!(limit, 3);
                progress(self.0.len() as u64);
                Ok(self.0.to_vec())
            }
        }
        let pins = [Asset {
            name: "nvngx_dlss.dll",
            sha256: "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            size: 3,
        }];
        for (bytes, valid) in [
            (b"abc".as_slice(), true),
            (b"abd".as_slice(), false),
            (b"abcd".as_slice(), false),
        ] {
            let temp = tempfile::tempdir().unwrap();
            assert_eq!(
                fetch_artifacts(
                    &Dir::open_ambient_dir(temp.path(), cap_std::ambient_authority()).unwrap(),
                    &pins,
                    &Fake(bytes),
                    &mut |_| {}
                )
                .is_ok(),
                valid
            );
            assert_eq!(temp.path().join("nvngx_dlss.dll").exists(), valid);
        }
    }
    #[test]
    fn discovery_keeps_all_plugin_copies_and_excludes_loader_catalogs() {
        let temp = tempfile::tempdir().unwrap();
        let expected = [
            "Engine/Plugins/A/nvngx_dlss.dll",
            "Engine/Plugins/B/nvngx_dlss.dll",
            "Project/Binaries/Win64/nvngx_dlssg.dll",
        ];
        for file in expected
            .into_iter()
            .chain(["dlssg_sm86/runtime/nvngx_dlssg.dll", ".retoque/backup/nvngx_dlss.dll"])
        {
            let p = temp.path().join(file);
            fs::create_dir_all(p.parent().unwrap()).unwrap();
            fs::write(p, b"synthetic unknown PE").unwrap();
        }
        let files = inspect(temp.path()).unwrap();
        assert_eq!(files.iter().map(|d| d.path.as_str()).collect::<Vec<_>>(), expected);
        assert!(files.iter().all(|d| d.version.is_none()));
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(temp.path().join("Engine"), temp.path().join("LinkedEngine")).unwrap();
            assert_eq!(inspect(temp.path()).unwrap(), files);
        }
    }
    #[test]
    fn doctor_reports_info_and_partial_root_with_translations() {
        let files = [GameDll {
            path: "plugin/nvngx_dlss.dll".into(),
            version: Some("310.1.0.0".into()),
        }];
        let findings = crate::doctor::diagnose_dlss(&files, true);
        assert_eq!(findings.len(), 2);
        assert_eq!(findings[1].severity, crate::doctor::Severity::Info);
        assert_eq!(findings[1].repair, Some(crate::doctor::Repair::UpdateDlss));
        for f in findings {
            for language in [
                crate::messages::Language::English,
                crate::messages::Language::Portuguese,
            ] {
                assert_ne!(crate::messages::text(f.title_key, language), f.title_key);
                assert_ne!(crate::messages::text(f.why_key, language), f.why_key);
            }
        }
    }
    #[test]
    fn pins_size_hash_and_redirects() {
        let a = Asset {
            name: "fixture",
            sha256: "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            size: 3,
        };
        assert!(verify_bytes(b"abc", &a).is_ok());
        assert!(verify_bytes(b"abd", &a).is_err());
        assert!(verify_bytes(b"abcd", &a).is_err());
        assert!(allowed(&url(&ASSETS[0]), false));
        assert!(allowed("https://media.githubusercontent.com/a", true));
        for u in [
            "http://media.githubusercontent.com/a",
            "https://media.githubusercontent.com.evil/a",
            "https://media.githubusercontent.com@evil/a",
            "https://evil.test/a",
            "https://github.com/other/repo",
        ] {
            assert!(!allowed(u, true));
        }
        assert!(!allowed("https://media.githubusercontent.com/a", false));
    }
    #[test]
    fn consent_and_bad_download_never_publish() {
        struct Fake;
        impl Fetcher for Fake {
            fn get(&self, _: &str, limit: u64, _: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
                assert_eq!(limit, ASSETS[0].size);
                Ok(b"not a DLL".to_vec())
            }
        }
        let temp = tempfile::tempdir().unwrap();
        assert!(download(temp.path(), false, &Fake, &mut |_| {}).is_err());
        assert!(!temp.path().join("dlss").exists());
        assert!(download(temp.path(), true, &Fake, &mut |_| {}).is_err());
        assert!(!catalog_path(temp.path()).exists());
    }
    #[test]
    fn current_needs_a_readable_version_at_least_the_catalogs() {
        assert!(super::current("310.9.16.0"));
        assert!(super::current("310.10.0"));
        assert!(!super::current("310.6.0.0"));
        assert!(!super::current("garbage"));
        assert!(!super::current("310.x.16"));
    }
    #[test]
    fn numeric_versions_and_unknown_are_not_guessed() {
        assert!(older("310.9.15.0"));
        assert!(older("3.10.4.0"));
        for s in ["unknown", "310.9.16.0", "310.10.0", "311.0.0"] {
            assert!(!older(s));
        }
    }
}
