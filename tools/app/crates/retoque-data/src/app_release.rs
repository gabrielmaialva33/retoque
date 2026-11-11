//! Windows app releases. Nothing here performs network access without an explicit caller request.
//! Extraction shares the payload verifier; staging stays inside the program folder, never in a game.
use crate::payload_fetch::{self as payload, Fetcher, FileRecord, Release};
use cap_std::{ambient_authority, fs::Dir};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{collections::BTreeMap, fs, io, path::Path};

pub const EXE: &str = "retoque-app.exe";

pub fn newer(version: &str, current: &str) -> bool {
    payload::compare_versions(version, current) == Some(std::cmp::Ordering::Greater)
}

pub fn latest(fetcher: &dyn Fetcher) -> io::Result<Option<Release>> {
    match fetcher.get(payload::RELEASES_URL, payload::MAX_API_BYTES, &mut |_| {}) {
        Ok(bytes) => payload::parse_releases_kind(&bytes, true),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

pub fn allowed_file(name: &str) -> bool {
    name == EXE || payload::is_allowed_payload_path(name)
}

/// A small bounded PE header check, independent of the app's optional VERSIONINFO resources.
pub fn is_x64_exe(bytes: &[u8]) -> bool {
    if bytes.len() < 64 || &bytes[..2] != b"MZ" {
        return false;
    }
    let offset = u32::from_le_bytes(bytes[60..64].try_into().unwrap()) as usize;
    let Some(header) = bytes.get(offset..offset.saturating_add(26)) else {
        return false;
    };
    &header[..4] == b"PE\0\0"
        && header[4..6] == [0x64, 0x86]
        && header[24..26] == [0x0b, 0x02]
        && u16::from_le_bytes(header[22..24].try_into().unwrap()) & 0x2002 == 0x0002
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Manifest {
    pub release: Release,
    pub files: BTreeMap<String, FileRecord>,
}

impl Manifest {
    /// Recheck staged bytes just before replacing anything, including after the parent exits.
    pub fn verify(&self, bundle: &Path) -> io::Result<()> {
        self.release.validate_kind(true)?;
        if !self.files.contains_key(EXE) || payload::REQUIRED_FILES.iter().any(|p| !self.files.contains_key(*p)) {
            return Err(io::Error::other("incomplete app manifest"));
        }
        let held = crate::components::HeldDir::open(bundle)?;
        fn list(
            dir: &Dir,
            prefix: &str,
            names: &mut std::collections::BTreeSet<String>,
            depth: usize,
        ) -> io::Result<()> {
            use cap_fs_ext::DirExt;
            if depth > 6 {
                return Err(io::Error::other("staged app tree too deep"));
            }
            for entry in dir.entries()? {
                let entry = entry?;
                let name = entry
                    .file_name()
                    .into_string()
                    .map_err(|_| io::Error::other("invalid staged name"))?;
                let path = if prefix.is_empty() {
                    name.clone()
                } else {
                    format!("{prefix}/{name}")
                };
                if entry.file_type()?.is_dir() {
                    list(&dir.open_dir_nofollow(&name)?, &path, names, depth + 1)?;
                } else {
                    if names.len() >= 2048 {
                        return Err(io::Error::other("too many staged files"));
                    }
                    names.insert(path);
                }
            }
            Ok(())
        }
        let mut names = std::collections::BTreeSet::new();
        list(&held.dir, "", &mut names, 0)?;
        if names != self.files.keys().cloned().collect() {
            return Err(io::Error::other("unexpected staged app file"));
        }
        let mut total = 0u64;
        for (name, rec) in &self.files {
            if !allowed_file(name) || rec.size == 0 || rec.size > payload::MAX_DOWNLOAD * 2 {
                return Err(io::Error::other("invalid app manifest entry"));
            }
            total = total
                .checked_add(rec.size)
                .filter(|n| *n <= payload::MAX_DOWNLOAD * 2)
                .ok_or_else(|| io::Error::other("staged app exceeds size limit"))?;
            let mut file = crate::components::open_regular(&held.dir, name)?;
            let mut hash = Sha256::new();
            let size = io::copy(&mut std::io::Read::by_ref(&mut file).take(rec.size + 1), &mut hash)?;
            if size != rec.size || format!("{:x}", hash.finalize()) != rec.sha256 {
                return Err(io::Error::other("staged app file changed"));
            }
        }
        let mut exe = crate::components::open_regular(&held.dir, EXE)?;
        let mut header = [0u8; 4096];
        let read = exe.read(&mut header)?;
        if !is_x64_exe(&header[..read]) {
            return Err(io::Error::other("staged app is not Windows x64"));
        }
        // Avoid accepting a mislabeled binary that would offer the same update forever.
        let marker = format!("RETOQUE_APP_VERSION={};", self.release.version);
        let mut reader = crate::components::open_regular(&held.dir, EXE)?;
        let mut carry = Vec::new();
        let mut chunk = [0u8; 65536];
        let mut found = false;
        loop {
            let n = reader.read(&mut chunk)?;
            if n == 0 {
                break;
            }
            carry.extend_from_slice(&chunk[..n]);
            if carry.windows(marker.len()).any(|w| w == marker.as_bytes()) {
                found = true;
                break;
            }
            let keep = marker.len().min(carry.len());
            carry.drain(..carry.len() - keep);
        }
        if !found {
            return Err(io::Error::other("app version marker does not match release"));
        }
        held.current()
    }
}

/// Owns the temporary directory until the user confirms restart; dropping cancels the download.
#[derive(Debug)]
pub struct Staged {
    pub dir: tempfile::TempDir,
    pub manifest: Manifest,
}

pub fn download(fetcher: &dyn Fetcher, release: &Release, install: &Path) -> io::Result<Staged> {
    release.validate_kind(true)?;
    let sums = fetcher.get(&release.sums_url, release.sums_size, &mut |_| {})?;
    let bytes = fetcher.get(&release.url, release.size, &mut |_| {})?;
    stage(release, &bytes, &sums, install)
}

pub fn stage(release: &Release, bytes: &[u8], sums: &[u8], install: &Path) -> io::Result<Staged> {
    payload::verify_archive(release, bytes, sums, true)?;
    let held = crate::components::HeldDir::open(install)?;
    let dir = tempfile::Builder::new()
        .prefix(".retoque-update-")
        .tempdir_in(install)?;
    let cap = Dir::open_ambient_dir(dir.path(), ambient_authority())?;
    let unpacked = payload::unpack_kind(bytes, &cap, true)?;
    let manifest = Manifest {
        release: release.clone(),
        files: unpacked.files,
    };
    held.current()?;
    manifest.verify(&dir.path().join("payload"))?;
    fs::write(dir.path().join("manifest.json"), serde_json::to_vec(&manifest)?)?;
    Ok(Staged { dir, manifest })
}

use std::io::Read;

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{Cursor, Write};

    fn fixture(extra: Option<&str>) -> (Release, Vec<u8>, Vec<u8>) {
        let (mut release, payload_zip, _, _) = payload::synthetic_release_fixture("v1.2.3", (1, 2, 3, 0), None);
        let mut out = zip::ZipWriter::new(Cursor::new(Vec::new()));
        let options = zip::write::SimpleFileOptions::default();
        let mut input = zip::ZipArchive::new(Cursor::new(payload_zip)).unwrap();
        for i in 0..input.len() {
            let mut entry = input.by_index(i).unwrap();
            out.start_file(entry.name(), options).unwrap();
            io::copy(&mut entry, &mut out).unwrap();
        }
        let mut exe = payload::versioned_pe_fixture((1, 2, 3, 0), b"RETOQUE_APP_VERSION=1.2.3;");
        exe[68..70].copy_from_slice(&0x8664u16.to_le_bytes());
        exe[86..88].copy_from_slice(&2u16.to_le_bytes());
        out.start_file(EXE, options).unwrap();
        out.write_all(&exe).unwrap();
        if let Some(extra) = extra {
            out.start_file(extra, options).unwrap();
            out.write_all(b"bad").unwrap();
        }
        let bytes = out.finish().unwrap().into_inner();
        release.asset = payload::asset_name(&release.version, true);
        release.url = format!("{}{}/{}", payload::ASSET_PREFIX, release.tag, release.asset);
        release.size = bytes.len() as u64;
        release.digest = format!("{:x}", Sha256::digest(&bytes));
        let sums = format!("{}  {}\n", release.digest, release.asset).into_bytes();
        release.sums_size = sums.len() as u64;
        release.sums_digest = None;
        (release, bytes, sums)
    }

    #[test]
    fn versions_and_paths() {
        assert!(newer("retoque-v1.10.0", "1.9.9"));
        assert!(newer("1.0.0", "1.0.0-rc.2"));
        for v in ["1.0.0", "0.9.0", "1.0.0-rc.1", "invalid"] {
            assert!(!newer(v, "1.0.0"));
        }
        for bad in [
            "../retoque-app.exe",
            "RETOQUE-APP.EXE",
            "nvngx_dlssnr.dll",
            "Licenses/evil.exe",
            "translations/CON.txt",
            "updater.exe",
            "retoque-app.old.exe",
        ] {
            assert!(!allowed_file(bad), "{bad}");
        }
    }

    #[test]
    fn stage_rechecks_files_and_cleans_cancelled_stage() {
        let tmp = tempfile::tempdir().unwrap();
        let (release, bytes, sums) = fixture(None);
        let staged = stage(&release, &bytes, &sums, tmp.path()).unwrap();
        let path = staged.dir.path().to_owned();
        staged.manifest.verify(&path.join("payload")).unwrap();
        fs::write(path.join("payload/Licenses/unlisted.txt"), b"unexpected").unwrap();
        assert!(
            staged
                .manifest
                .verify(&path.join("payload"))
                .unwrap_err()
                .to_string()
                .contains("unexpected")
        );
        fs::remove_file(path.join("payload/Licenses/unlisted.txt")).unwrap();
        fs::write(path.join("payload").join(EXE), b"tampered").unwrap();
        assert!(staged.manifest.verify(&path.join("payload")).is_err());
        drop(staged);
        assert!(!path.exists());
    }

    #[test]
    fn both_digests_and_allowlist_are_enforced() {
        let tmp = tempfile::tempdir().unwrap();
        let (mut release, bytes, mut sums) = fixture(None);
        release.digest = "0".repeat(64);
        assert!(
            stage(&release, &bytes, &sums, tmp.path())
                .unwrap_err()
                .to_string()
                .contains("GitHub")
        );
        release.digest = format!("{:x}", Sha256::digest(&bytes));
        sums[0] = if sums[0] == b'0' { b'1' } else { b'0' };
        assert!(
            stage(&release, &bytes, &sums, tmp.path())
                .unwrap_err()
                .to_string()
                .contains("SHA256SUMS")
        );
        for bad in [
            "../outside",
            "nvngx_dlssnr.dll",
            "retoque-app.old.exe",
            "RETOQUE-APP.exe",
        ] {
            let (release, bytes, sums) = fixture(Some(bad));
            assert!(stage(&release, &bytes, &sums, tmp.path()).is_err(), "{bad}");
        }
        assert_eq!(fs::read_dir(tmp.path()).unwrap().count(), 0);
    }

    #[test]
    fn metadata_requires_an_unambiguous_app_digest_and_matching_version() {
        let (_, _, _, json) = payload::synthetic_release_fixture("v1.2.3", (1, 2, 3, 0), None);
        let (release, bytes, sums) = fixture(None);
        let mut value: serde_json::Value = serde_json::from_slice(&json).unwrap();
        let item = if value.is_array() { &mut value[0] } else { &mut value };
        let assets = item["assets"].as_array_mut().unwrap();
        assets.push(
            serde_json::json!({"name": release.asset, "browser_download_url": release.url,
            "size": release.size, "digest": format!("sha256:{}", release.digest)}),
        );
        let encoded = serde_json::to_vec(&value).unwrap();
        assert!(payload::parse_releases_kind(&encoded, true).unwrap().is_some());
        let item = if value.is_array() { &mut value[0] } else { &mut value };
        let assets = item["assets"].as_array_mut().unwrap();
        assets.retain(|asset| !asset["name"].as_str().unwrap().starts_with("retoque-payload-"));
        assert!(
            payload::parse_releases_json(&serde_json::to_vec(&value).unwrap())
                .unwrap()
                .is_none()
        );
        let item = if value.is_array() { &mut value[0] } else { &mut value };
        let assets = item["assets"].as_array_mut().unwrap();
        assets.last_mut().unwrap()["digest"] = serde_json::Value::Null;
        assert!(payload::parse_releases_kind(&serde_json::to_vec(&value).unwrap(), true).is_err());
        let tmp = tempfile::tempdir().unwrap();
        let stage = stage(&release, &bytes, &sums, tmp.path()).unwrap();
        let exe = stage.dir.path().join("payload").join(EXE);
        let data = fs::read(&exe).unwrap();
        let data = data
            .windows(b"RETOQUE_APP_VERSION=1.2.3;".len())
            .position(|w| w == b"RETOQUE_APP_VERSION=1.2.3;")
            .unwrap();
        let mut bytes = fs::read(&exe).unwrap();
        bytes[data + "RETOQUE_APP_VERSION=".len()] = b'9';
        fs::write(&exe, &bytes).unwrap();
        let mut manifest = stage.manifest.clone();
        manifest.files.get_mut(EXE).unwrap().sha256 = format!("{:x}", Sha256::digest(&bytes));
        assert!(
            manifest
                .verify(&stage.dir.path().join("payload"))
                .unwrap_err()
                .to_string()
                .contains("version marker")
        );
        assert!(payload::parse_sha256sums(&format!("{}é  file", "a".repeat(63))).is_err());
    }

    #[test]
    fn optional_app_asset_and_official_urls() {
        let (_, _, _, json) = payload::synthetic_release_fixture("v1.2.3", (1, 2, 3, 0), None);
        assert!(payload::parse_releases_kind(&json, true).unwrap().is_none());
        assert!(payload::allowed_url(
            "https://github.com/gabrielmaialva33/retoque/releases/download/v1.2.3/retoque-app-1.2.3-windows-x64.zip",
            false
        ));
        assert!(!payload::allowed_url(
            "https://github.com/evil/retoque/releases/download/v1.2.3/retoque-app-1.2.3-windows-x64.zip",
            false
        ));
    }
}
