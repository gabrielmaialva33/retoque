//! The only sm86 network source is the upstream GitHub release. No game-folder access here.
//! Callers must obtain consent before calling `latest`/`download`; tests inject a Fetcher.
use crate::components::{self, Component, ComponentCatalog, HeldDir, open_regular};
use cap_fs_ext::DirExt;
use cap_std::fs::{Dir, OpenOptions};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    io::{self, Cursor, Read, Write},
    path::{Path, PathBuf},
};

pub const LATEST: &str = "https://api.github.com/repos/sdli1995/dlssg_for_sm86/releases/latest";
pub const MAX_DOWNLOAD: u64 = 256 * 1024 * 1024;
const MAX_UNPACKED: u64 = 1024 * 1024 * 1024;
const MAX_ENTRIES: usize = 8192;
const ASSET_PREFIX: &str = "https://github.com/sdli1995/dlssg_for_sm86/releases/download/";
fn invalid(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, s)
}
fn sha(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn hash_ok(hash: &str) -> bool {
    hash.len() == 64 && hash.bytes().all(|b| b.is_ascii_hexdigit())
}
fn version_ok(v: &str) -> bool {
    let v = v.strip_prefix('v').unwrap_or(v);
    let parts: Vec<_> = v.split('.').collect();
    parts.len() == 3
        && parts
            .iter()
            .all(|s| !s.is_empty() && s.len() <= 8 && s.bytes().all(|b| b.is_ascii_digit()))
        && (parts[0] != "0" || parts[1].parse::<u32>().is_ok_and(|minor| minor >= 4))
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Release {
    pub version: String,
    pub published_at: String,
    pub asset: String,
    pub url: String,
    pub size: u64,
    pub digest: Option<String>,
}
impl Release {
    fn validate(&self) -> io::Result<()> {
        let number = self.version.strip_prefix('v').unwrap_or(&self.version);
        if !version_ok(&self.version)
            || self.asset != format!("dlssg-release-{number}-x64.zip")
            || self.url != format!("{ASSET_PREFIX}{}/{}", self.version, self.asset)
            || self.size == 0
            || self.size > MAX_DOWNLOAD
            || self.published_at.len() > 40
            || !self.published_at.ends_with('Z')
            || self.published_at.chars().any(char::is_control)
            || self.digest.as_ref().is_some_and(|d| !hash_ok(d))
        {
            return Err(invalid("invalid official sm86 release metadata"));
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Record {
    pub release: Release,
    pub sha256: String,
    pub component_id: String,
    pub ignored: Vec<String>,
}

/// A bounded response, including redirect validation, never an arbitrary HTTP client.
pub trait Fetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>>;
}
pub struct GithubFetcher;
fn allowed_url(value: &str, redirected_asset: bool) -> bool {
    let Ok(u) = value.parse::<ureq::http::Uri>() else {
        return false;
    };
    let Some(host) = u.host() else { return false };
    let Some(authority) = u.authority() else { return false };
    if u.scheme_str() != Some("https")
        || value.contains('#')
        || !(authority.as_str() == host || authority.as_str() == format!("{host}:443"))
    {
        return false;
    }
    if value == LATEST {
        return true;
    }
    if u.host() == Some("github.com")
        && value.starts_with(ASSET_PREFIX)
        && u.query().is_none()
        && !u.path().contains('%')
    {
        let rest = &value[ASSET_PREFIX.len()..];
        if let Some((tag, asset)) = rest.split_once('/') {
            return version_ok(tag)
                && asset == format!("dlssg-release-{}-x64.zip", tag.strip_prefix('v').unwrap_or(tag));
        }
    }
    // Only a redirect from a validated repository asset may reach GitHub's blob hosts.
    redirected_asset
        && matches!(
            u.host(),
            Some("release-assets.githubusercontent.com" | "objects.githubusercontent.com")
        )
}
impl Fetcher for GithubFetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
        if !allowed_url(url, false) {
            return Err(invalid("non-official sm86 URL"));
        }
        let asset = url.starts_with(ASSET_PREFIX);
        crate::fetch::get(url, limit, progress, |next, redirect| {
            (!redirect || asset) && allowed_url(next, asset && redirect)
        })
    }
}

pub fn latest(fetcher: &dyn Fetcher) -> io::Result<Release> {
    let bytes = fetcher.get(LATEST, 1024 * 1024, &mut |_| {})?;
    if bytes.len() > 1024 * 1024 {
        return Err(invalid("release metadata exceeds size limit"));
    }
    #[derive(Deserialize)]
    struct Api {
        tag_name: String,
        published_at: String,
        draft: bool,
        prerelease: bool,
        assets: Vec<Asset>,
    }
    #[derive(Deserialize)]
    struct Asset {
        name: String,
        browser_download_url: String,
        size: u64,
        digest: Option<String>,
    }
    let api: Api = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
    if api.draft || api.prerelease || !version_ok(&api.tag_name) {
        return Err(invalid("unsupported sm86 release"));
    }
    let wanted = format!(
        "dlssg-release-{}-x64.zip",
        api.tag_name.strip_prefix('v').unwrap_or(&api.tag_name)
    );
    let mut matching = api.assets.into_iter().filter(|a| a.name == wanted);
    let a = matching
        .next()
        .ok_or_else(|| invalid("documented sm86 release asset missing"))?;
    if matching.next().is_some() {
        return Err(invalid("ambiguous sm86 release asset"));
    }
    let digest = a
        .digest
        .map(|d| {
            d.strip_prefix("sha256:")
                .map(str::to_ascii_lowercase)
                .ok_or_else(|| invalid("unsupported release digest"))
        })
        .transpose()?;
    let release = Release {
        version: api.tag_name,
        published_at: api.published_at,
        asset: a.name,
        url: a.browser_download_url,
        size: a.size,
        digest,
    };
    release.validate()?;
    Ok(release)
}

fn safe_zip_path(path: &str) -> bool {
    let path = path.strip_suffix('/').unwrap_or(path);
    !path.is_empty()
        && path.len() <= 1024
        && !path.contains('\\')
        && path.split('/').count() <= 16
        && path.split('/').all(|part| {
            // Unlike deployed paths, ignored documentation can contain spaces and Unicode.
            let stem = part
                .split('.')
                .next()
                .unwrap_or_default()
                .trim_end()
                .to_ascii_uppercase();
            !part.is_empty()
                && part != "."
                && part != ".."
                && !part.ends_with(['.', ' '])
                && !part
                    .chars()
                    .any(|c| c.is_control() || matches!(c, ':' | '<' | '>' | '"' | '|' | '?' | '*'))
                && !matches!(
                    stem.as_str(),
                    "CON" | "PRN" | "AUX" | "NUL" | "CLOCK$" | "CONIN$" | "CONOUT$"
                )
                && !["COM", "LPT"].iter().any(|p| {
                    stem.strip_prefix(p).is_some_and(|s| {
                        matches!(s, "1" | "2" | "3" | "4" | "5" | "6" | "7" | "8" | "9" | "¹" | "²" | "³")
                    })
                })
        })
}
fn notice(path: &str) -> bool {
    // Preserve legal texts separately, never as a game payload or an executable from archive/alternatives.
    let name = path.rsplit('/').next().unwrap_or_default().to_ascii_lowercase();
    (name.starts_with("license")
        || name.starts_with("licence")
        || name.starts_with("third_party_notices")
        || name.starts_with("notice"))
        && (name.ends_with(".txt") || name.ends_with(".md") || !name.contains('.'))
}
fn write_new(dir: &Dir, name: &str, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = Path::new(name).parent()
        && !parent.as_os_str().is_empty()
    {
        dir.create_dir_all(parent)?;
    }
    let mut f = dir.open_with(name, OpenOptions::new().write(true).create_new(true))?;
    f.write_all(bytes)?;
    f.sync_all()
}
fn unpack(bytes: &[u8], stage: &Dir) -> io::Result<Vec<String>> {
    let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(io::Error::other)?;
    if zip.len() > MAX_ENTRIES {
        return Err(invalid("too many archive entries"));
    }
    let mut names = HashSet::new();
    let mut files = Vec::new();
    let mut total = 0u64;
    let mut ignored = Vec::new();
    // Validate even entries which will not be extracted, before creating any payload files.
    for i in 0..zip.len() {
        let file = zip.by_index(i).map_err(io::Error::other)?;
        let path = file.name();
        if !safe_zip_path(path) || !names.insert(path.trim_end_matches('/').to_lowercase()) {
            return Err(invalid("unsafe or colliding archive path"));
        }
        let kind = file.unix_mode().unwrap_or(0) & 0o170000;
        if file.encrypted()
            || !matches!(kind, 0 | 0o100000 | 0o040000)
            || (kind == 0o040000 && !file.is_dir())
            || (kind == 0o100000 && file.is_dir())
        {
            return Err(invalid("archive link, encryption or special file refused"));
        }
        total = total
            .checked_add(file.size())
            .ok_or_else(|| invalid("archive size overflow"))?;
        if total > MAX_UNPACKED {
            return Err(invalid("archive exceeds unpacked size limit"));
        }
        if !file.is_dir() {
            files.push(path.to_owned());
        }
    }
    if components::REQUIRED.iter().any(|p| !files.iter().any(|f| f == p)) {
        return Err(invalid("incomplete sm86 release"));
    }
    let mut actual = 0u64;
    for i in 0..zip.len() {
        let mut file = zip.by_index(i).map_err(io::Error::other)?;
        let path = file.name().to_owned();
        if file.is_dir() {
            ignored.push(path);
            continue;
        }
        let destination = if components::allowed_path(&path) {
            Some(format!("payload/{path}"))
        } else if notice(&path) {
            Some(format!("notices/{path}"))
        } else {
            ignored.push(path);
            None
        };
        // Stream every entry to verify CRC/truncation and enforce actual expanded byte limits.
        let mut out = if let Some(dest) = destination {
            if let Some(parent) = Path::new(&dest).parent() {
                stage.create_dir_all(parent)?;
            }
            Some(stage.open_with(dest, OpenOptions::new().write(true).create_new(true))?)
        } else {
            None
        };
        let mut buf = [0u8; 65536];
        let mut size = 0u64;
        loop {
            let n = file.read(&mut buf)?;
            if n == 0 {
                break;
            }
            actual += n as u64;
            size += n as u64;
            if actual > MAX_UNPACKED {
                return Err(invalid("archive exceeds unpacked size limit"));
            }
            if let Some(f) = &mut out {
                f.write_all(&buf[..n])?;
            }
        }
        if size != file.size() {
            return Err(invalid("truncated archive entry"));
        }
        if let Some(out) = out {
            out.sync_all()?;
        }
    }
    Ok(ignored)
}

pub fn default_cache() -> io::Result<PathBuf> {
    let base = std::env::var_os(if cfg!(windows) { "LOCALAPPDATA" } else { "HOME" })
        .filter(|v| !v.is_empty())
        .ok_or_else(|| invalid("app data unavailable"))?;
    Ok(PathBuf::from(base).join(if cfg!(windows) {
        "Retoque/sm86-releases"
    } else {
        ".local/share/retoque/sm86-releases"
    }))
}
/// Cache is independent of game folders. Each version records its first accepted archive hash permanently.
pub struct Cache {
    held: HeldDir,
}
impl Cache {
    pub fn open(root: &Path) -> io::Result<Self> {
        std::fs::create_dir_all(root)?;
        Ok(Self {
            held: HeldDir::open(root)?,
        })
    }
    pub fn records(&self) -> io::Result<Vec<Record>> {
        self.held.current()?;
        let mut records = Vec::new();
        for entry in self.held.dir.entries()? {
            let name = entry?.file_name();
            if let Some(name) = name.to_str().filter(|n| version_ok(n)) {
                let dir = self.held.dir.open_dir_nofollow(name)?;
                let mut bytes = Vec::new();
                open_regular(&dir, "release.json")?
                    .take(16_777_217)
                    .read_to_end(&mut bytes)?;
                if bytes.len() > 16_777_216 {
                    return Err(invalid("release record exceeds limit"));
                }
                let r: Record = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
                r.release.validate()?;
                if r.release.version != name || !hash_ok(&r.sha256) {
                    return Err(invalid("invalid release record"));
                }
                records.push(r);
            }
        }
        Ok(records)
    }
    /// Prefer the newest locally verified official release after restart; explicit UI selections still win.
    pub fn prefer_latest(&self, catalog: &mut [Component]) -> io::Result<()> {
        let records = self.records()?;
        let key = |c: &Component| {
            records
                .iter()
                .filter(|r| r.component_id == c.id)
                .map(|r| {
                    let version = r.release.version.strip_prefix('v').unwrap_or(&r.release.version);
                    let parts: Vec<u32> = version.split('.').filter_map(|n| n.parse().ok()).collect();
                    (parts[0], parts[1], parts[2])
                })
                .max()
        };
        catalog.sort_by_cached_key(|c| std::cmp::Reverse(key(c)));
        Ok(())
    }

    pub fn contains(&self, release: &Release, catalog: &[Component]) -> io::Result<bool> {
        Ok(self
            .records()?
            .iter()
            .any(|r| &r.release == release && catalog.iter().any(|c| c.id == r.component_id)))
    }
    pub fn download(
        &self,
        fetcher: &dyn Fetcher,
        release: &Release,
        catalog: &ComponentCatalog,
        progress: &mut dyn FnMut(u64),
    ) -> io::Result<(Component, Record)> {
        release.validate()?;
        let bytes = fetcher.get(&release.url, release.size, progress)?;
        self.import_archive(release, &bytes, catalog)
    }
    pub fn import_archive(
        &self,
        release: &Release,
        bytes: &[u8],
        catalog: &ComponentCatalog,
    ) -> io::Result<(Component, Record)> {
        release.validate()?;
        self.held.current()?;
        if bytes.len() as u64 != release.size || bytes.len() as u64 > MAX_DOWNLOAD {
            return Err(invalid("release size mismatch"));
        }
        let hash = sha(bytes);
        if release.digest.as_ref().is_some_and(|d| !d.eq_ignore_ascii_case(&hash)) {
            return Err(invalid("release SHA-256 mismatch"));
        }
        let old = self
            .records()?
            .into_iter()
            .find(|r| r.release.version == release.version);
        if let Some(old) = &old
            && old.sha256 != hash
        {
            return Err(invalid("release changed since first download"));
        }
        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let name = format!(
            ".unpack-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        );
        self.held.dir.create_dir(&name)?;
        let result = (|| {
            let stage = self.held.dir.open_dir_nofollow(&name)?;
            let ignored = unpack(bytes, &stage)?;
            self.held.current()?;
            let payload = self.held.root.join(&name).join("payload");
            let label = format!("DLSS-G for sm86 {} · {}", release.version, release.published_at);
            let component = catalog.import_dir(&payload, Some(&label))?;
            let record = Record {
                release: release.clone(),
                sha256: hash,
                component_id: component.id.clone(),
                ignored,
            };
            write_new(
                &stage,
                "release.json",
                &serde_json::to_vec_pretty(&record).map_err(io::Error::other)?,
            )?;
            write_new(&stage, "ignored-files.log", record.ignored.join("\n").as_bytes())?;
            // Payload is content-addressed in the catalog; cache retains provenance and legal texts only.
            stage.remove_dir_all("payload")?;
            drop(stage);
            self.held.current()?;
            if old.is_none() {
                // A directory with release.json is nonempty: rename cannot overwrite a racing accepted version.
                if let Err(e) = self.held.dir.rename(&name, &self.held.dir, &release.version) {
                    let accepted = self
                        .records()?
                        .into_iter()
                        .find(|r| r.release.version == release.version);
                    if !accepted.is_some_and(|r| r.sha256 == record.sha256) {
                        return Err(e);
                    }
                }
            }
            Ok((component, record))
        })();
        let _ = self.held.dir.remove_dir_all(&name);
        result
    }
}

/// Kit-builder entry point: keep the exact verified official archive and metadata, never commit binaries.
/// The GUI imports this cache into app data after its first-run confirmation, without a network request.
pub fn build_first_run_cache(destination: &Path, fetcher: &dyn Fetcher) -> io::Result<()> {
    let release = latest(fetcher)?;
    let bytes = fetcher.get(&release.url, release.size, &mut |_| {})?;
    if bytes.len() as u64 != release.size || release.digest.as_ref().is_none_or(|d| *d != sha(&bytes)) {
        return Err(invalid("kit cache requires a matching GitHub SHA-256 digest"));
    }
    // The kit redistributes the raw archive, unlike the selective app import. Never allow an
    // upstream layout change to smuggle a protected NR model or Lossless Scaling binary into it.
    let mut zip = zip::ZipArchive::new(Cursor::new(&bytes)).map_err(io::Error::other)?;
    for index in 0..zip.len() {
        let entry = zip.by_index(index).map_err(io::Error::other)?;
        let name = entry.name().rsplit('/').next().unwrap_or_default().to_ascii_lowercase();
        if name.starts_with("nvngx_dlssnr") || matches!(name.as_str(), "lossless.dll" | "lsfg-vk.dll") {
            return Err(invalid("protected binary in kit archive"));
        }
    }
    std::fs::create_dir_all(destination)?;
    let held = HeldDir::open(destination)?;
    write_new(&held.dir, "release.zip", &bytes)?;
    write_new(
        &held.dir,
        "release.json",
        &serde_json::to_vec_pretty(&release).map_err(io::Error::other)?,
    )
}
pub fn import_first_run_cache(
    source: &Path,
    cache: &Cache,
    catalog: &ComponentCatalog,
) -> io::Result<(Component, Record)> {
    let held = HeldDir::open(source)?;
    let mut json = Vec::new();
    open_regular(&held.dir, "release.json")?
        .take(1_048_577)
        .read_to_end(&mut json)?;
    if json.len() > 1_048_576 {
        return Err(invalid("kit metadata too large"));
    }
    let release: Release = serde_json::from_slice(&json).map_err(io::Error::other)?;
    if release.digest.is_none() {
        return Err(invalid("kit cache requires a GitHub digest"));
    }
    let mut bytes = Vec::new();
    open_regular(&held.dir, "release.zip")?
        .take(MAX_DOWNLOAD + 1)
        .read_to_end(&mut bytes)?;
    cache.import_archive(&release, &bytes, catalog)
}

#[cfg(test)]
mod tests {
    use super::*;
    use zip::write::SimpleFileOptions;
    fn archive(extra: Option<(&str, &[u8])>) -> Vec<u8> {
        let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
        for path in components::REQUIRED.iter().copied().chain([
            "dlssg_sm86/sr/sr_kernels.dll",
            "dlssg_sm86/sr/nvngx_dlss.dll",
            "LICENSE.md",
            "THIRD_PARTY_NOTICES.txt",
        ]) {
            z.start_file(path, SimpleFileOptions::default()).unwrap();
            z.write_all(b"MZ synthetic fixture").unwrap();
        }
        if let Some((path, bytes)) = extra {
            z.start_file(path, SimpleFileOptions::default()).unwrap();
            z.write_all(bytes).unwrap();
        }
        z.finish().unwrap().into_inner()
    }
    fn release(bytes: &[u8]) -> Release {
        Release {
            version: "0.4.1".into(),
            published_at: "2026-10-27T06:27:00Z".into(),
            asset: "dlssg-release-0.4.1-x64.zip".into(),
            url: format!("{ASSET_PREFIX}0.4.1/dlssg-release-0.4.1-x64.zip"),
            size: bytes.len() as u64,
            digest: Some(sha(bytes)),
        }
    }
    struct Fixture {
        bytes: Vec<u8>,
        metadata: Vec<u8>,
    }
    impl Fetcher for Fixture {
        fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
            let bytes = if url == LATEST {
                &self.metadata
            } else {
                assert!(url.starts_with(ASSET_PREFIX));
                &self.bytes
            };
            assert!(bytes.len() as u64 <= limit);
            progress(bytes.len() as u64);
            Ok(bytes.clone())
        }
    }
    #[test]
    fn synthetic_download_hash_import_sr_notices_and_ignored_members() {
        let bytes = archive(Some(("unknown/future.txt", b"not extracted")));
        let release = release(&bytes);
        let metadata = serde_json::json!({"tag_name":release.version,"published_at":release.published_at,"draft":false,"prerelease":false,
            "assets":[{"name":release.asset,"browser_download_url":release.url,"size":release.size,"digest":format!("sha256:{}",sha(&bytes))}]}).to_string().into_bytes();
        let fetcher = Fixture { bytes, metadata };
        assert_eq!(latest(&fetcher).unwrap(), release);
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let catalog = ComponentCatalog::open(Some(&t.path().join("catalog"))).unwrap();
        let (c, r) = cache.download(&fetcher, &release, &catalog, &mut |_| {}).unwrap();
        assert!(c.files.contains(&"dlssg_sm86/sr/sr_kernels.dll".into()));
        assert!(c.files.contains(&"dlssg_sm86/sr/nvngx_dlss.dll".into()));
        assert!(!c.root.join("LICENSE.md").exists());
        assert!(!t.path().join("cache/0.4.1/unknown").exists());
        assert!(t.path().join("cache/0.4.1/notices/LICENSE.md").is_file());
        assert_eq!(r.ignored, ["unknown/future.txt"]);
        assert!(cache.contains(&release, &catalog.list().unwrap()).unwrap());
        assert_eq!(cache.records().unwrap(), [r]);
        assert_eq!(
            cache.download(&fetcher, &release, &catalog, &mut |_| {}).unwrap().0.id,
            c.id
        );
    }
    #[test]
    fn latest_catalog_choice_uses_numeric_versions_and_update_checks_local_presence() {
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let catalog = ComponentCatalog::open(Some(&t.path().join("catalog"))).unwrap();
        let old = archive(None);
        let (old_component, _) = cache.import_archive(&release(&old), &old, &catalog).unwrap();
        let new = archive(Some(("dlssg_sm86/runtime/extra.dll", b"MZ newer")));
        let mut r = release(&new);
        r.version = "0.4.10".into();
        r.asset = "dlssg-release-0.4.10-x64.zip".into();
        r.url = format!("{ASSET_PREFIX}{}/{}", r.version, r.asset);
        assert!(!cache.contains(&r, &catalog.list().unwrap()).unwrap());
        let (new_component, _) = cache.import_archive(&r, &new, &catalog).unwrap();
        let mut choices = vec![old_component, new_component.clone()];
        cache.prefer_latest(&mut choices).unwrap();
        assert_eq!(choices[0].id, new_component.id);
        assert!(cache.contains(&r, &choices).unwrap());
        catalog.remove(&new_component.id).unwrap();
        assert!(!cache.contains(&r, &catalog.list().unwrap()).unwrap());
    }
    #[test]
    fn kit_builder_refuses_protected_binaries_before_writing_cache() {
        let bytes = archive(Some(("extra/nvngx_dlssnr.dll", b"MZ never package")));
        let r = release(&bytes);
        let metadata = serde_json::json!({"tag_name":r.version,"published_at":r.published_at,"draft":false,"prerelease":false,
            "assets":[{"name":r.asset,"browser_download_url":r.url,"size":r.size,"digest":format!("sha256:{}",sha(&bytes))}]}).to_string().into_bytes();
        let fetcher = Fixture { bytes, metadata };
        let t = tempfile::tempdir().unwrap();
        let destination = t.path().join("kit");
        assert!(
            build_first_run_cache(&destination, &fetcher)
                .unwrap_err()
                .to_string()
                .contains("protected binary")
        );
        assert!(!destination.exists());
    }

    #[test]
    fn wrong_hash_truncation_missing_members_and_first_seen_hash_fail_closed() {
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let catalog = ComponentCatalog::open(Some(&t.path().join("catalog"))).unwrap();
        let bytes = archive(None);
        let mut r = release(&bytes);
        r.digest = Some("0".repeat(64));
        assert!(
            cache
                .import_archive(&r, &bytes, &catalog)
                .unwrap_err()
                .to_string()
                .contains("SHA-256")
        );
        let truncated = &bytes[..bytes.len() - 50];
        assert!(cache.import_archive(&release(truncated), truncated, &catalog).is_err());
        let empty = zip::ZipWriter::new(Cursor::new(Vec::new()))
            .finish()
            .unwrap()
            .into_inner();
        assert!(cache.import_archive(&release(&empty), &empty, &catalog).is_err());
        assert!(catalog.list().unwrap().is_empty());
        let mut r = release(&bytes);
        r.digest = None;
        cache.import_archive(&r, &bytes, &catalog).unwrap();
        let changed = archive(Some(("extra", b"different")));
        let mut r = release(&changed);
        r.digest = None;
        assert!(
            cache
                .import_archive(&r, &changed, &catalog)
                .unwrap_err()
                .to_string()
                .contains("first download")
        );
    }
    #[test]
    fn dangerous_entries_refuse_even_outside_payload() {
        for path in [
            "../escape",
            "/absolute",
            "C:/drive",
            "safe/../escape",
            "safe\\escape",
            "safe/NUL.txt",
            "COM1/x",
            "safe/CONIN$",
            "safe/evil:stream",
            "version.DLL",
        ] {
            let bytes = archive(Some((path, b"evil")));
            let t = tempfile::tempdir().unwrap();
            let cache = Cache::open(&t.path().join("cache")).unwrap();
            let catalog = ComponentCatalog::open(Some(&t.path().join("catalog"))).unwrap();
            assert!(
                cache.import_archive(&release(&bytes), &bytes, &catalog).is_err(),
                "{path}"
            );
            assert!(catalog.list().unwrap().is_empty());
            assert_eq!(std::fs::read_dir(t.path().join("cache")).unwrap().count(), 0);
        }
        let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
        z.add_symlink("ignored-link", "outside", SimpleFileOptions::default())
            .unwrap();
        let bytes = z.finish().unwrap().into_inner();
        let t = tempfile::tempdir().unwrap();
        let d = Dir::open_ambient_dir(t.path(), cap_std::ambient_authority()).unwrap();
        assert!(unpack(&bytes, &d).unwrap_err().to_string().contains("link"));
    }
    #[test]
    fn expanded_size_and_corrupt_entry_are_rejected_before_import() {
        let t = tempfile::tempdir().unwrap();
        let dir = Dir::open_ambient_dir(t.path(), cap_std::ambient_authority()).unwrap();
        let mut oversized = archive(None);
        let central = oversized.windows(4).position(|w| w == b"PK\x01\x02").unwrap();
        oversized[central + 24..central + 28].copy_from_slice(&((MAX_UNPACKED + 1) as u32).to_le_bytes());
        assert!(unpack(&oversized, &dir).unwrap_err().to_string().contains("size limit"));
        let mut corrupt = archive(None);
        let central = corrupt.windows(4).position(|w| w == b"PK\x01\x02").unwrap();
        corrupt[central + 16] ^= 1; // Wrong stored CRC, independently of the compression method.
        assert!(unpack(&corrupt, &dir).is_err());
    }

    #[test]
    fn official_endpoints_only_and_no_api_redirects() {
        assert!(allowed_url(LATEST, false));
        assert!(allowed_url(&release(b"x").url, false));
        for url in [
            "http://api.github.com/repos/sdli1995/dlssg_for_sm86/releases/latest",
            "https://api.github.com/other",
            "https://github.com/other/repo/releases/download/v1/payload.zip",
            "https://github.com.evil/sdli1995/dlssg_for_sm86/releases/download/0.4.1/dlssg-release-0.4.1-x64.zip",
            "https://user@release-assets.githubusercontent.com/blob",
            "https://release-assets.githubusercontent.com:8443/blob",
            "https://evil.example/blob",
        ] {
            assert!(!allowed_url(url, true), "{url}");
        }
        assert!(!allowed_url("https://release-assets.githubusercontent.com/blob", false));
        assert!(allowed_url(
            "https://release-assets.githubusercontent.com/blob?signature=synthetic",
            true
        ));
        assert!(!allowed_url("https://objects.githubusercontent.com/blob", false));
    }
    #[test]
    fn kit_is_verified_again_and_imports_without_network() {
        let t = tempfile::tempdir().unwrap();
        let bytes = archive(None);
        let r = release(&bytes);
        let kit = t.path().join("kit");
        std::fs::create_dir(&kit).unwrap();
        std::fs::write(kit.join("release.json"), serde_json::to_vec(&r).unwrap()).unwrap();
        std::fs::write(kit.join("release.zip"), &bytes).unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let catalog = ComponentCatalog::open(Some(&t.path().join("catalog"))).unwrap();
        import_first_run_cache(&kit, &cache, &catalog).unwrap();
        std::fs::write(kit.join("release.zip"), b"changed").unwrap();
        assert!(import_first_run_cache(&kit, &cache, &catalog).is_err());
    }
}
