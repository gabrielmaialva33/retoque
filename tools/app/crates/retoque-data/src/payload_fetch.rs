//! Fetch, verify and unpack Retoque's own release payload (`OptiScaler.dll`, `nvngx.dll_dlssnr.dll`,
//! `OptiScaler.ini`, `Licenses/`, `translations/`, optional `OptiScaler/`) from `gabrielmaialva33/retoque`.
//!
//! Callers must obtain explicit user confirmation before calling `latest` or `Cache::download`.
//! Tests inject a `Fetcher` and never access the network.

use crate::components::{HeldDir, open_regular};
use cap_fs_ext::DirExt;
use cap_std::fs::{Dir, OpenOptions};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    cmp::Ordering,
    collections::{BTreeMap, HashSet},
    io::{self, Cursor, Read, Write},
    path::{Path, PathBuf},
};

pub const REPO: &str = "gabrielmaialva33/retoque";
pub const RELEASES_URL: &str = "https://api.github.com/repos/gabrielmaialva33/retoque/releases";
pub const LATEST_URL: &str = "https://api.github.com/repos/gabrielmaialva33/retoque/releases/latest";
pub const ASSET_PREFIX: &str = "https://github.com/gabrielmaialva33/retoque/releases/download/";
pub const MAX_API_BYTES: u64 = 2 * 1024 * 1024;
pub const MAX_SUMS_BYTES: u64 = 64 * 1024;
pub const MAX_DOWNLOAD: u64 = 256 * 1024 * 1024;
const MAX_UNPACKED: u64 = 512 * 1024 * 1024;
const MAX_ENTRIES: usize = 2048;

pub const REQUIRED_FILES: [&str; 6] = [
    "OptiScaler.dll",
    "nvngx.dll_dlssnr.dll",
    "OptiScaler.ini",
    "Licenses/RenoDX_ATTRIBUTION.txt",
    "translations/README.md",
    "translations/optional/pt-BR.lang",
];

const FORWARDER_EXPORTS: [&[u8]; 4] = [
    b"dlssnr_call_create",
    b"dlssnr_call_evaluate",
    b"dlssnr_call_set_extras",
    b"dlssnr_abi_version",
];

fn invalid(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, s)
}

fn sha(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn hash_ok(hash: &str) -> bool {
    hash.len() == 64 && hash.bytes().all(|b| b.is_ascii_hexdigit())
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct ParsedVersion {
    numbers: [u32; 4],
    parts_count: usize,
    prerelease: Option<String>,
}

fn parse_version(raw: &str) -> Option<ParsedVersion> {
    let stripped = if let Some(rest) = raw.strip_prefix("retoque-") {
        rest.strip_prefix('v')?
    } else {
        raw.strip_prefix('v').unwrap_or(raw)
    };
    if stripped.is_empty() || stripped.len() > 64 {
        return None;
    }
    let (core, pre) = match stripped.split_once('-') {
        Some((c, p)) => (c, Some(p)),
        None => (stripped, None),
    };
    let parts: Vec<&str> = core.split('.').collect();
    if !(3..=4).contains(&parts.len()) {
        return None;
    }
    let mut numbers = [0u32; 4];
    for (idx, part) in parts.iter().enumerate() {
        if part.is_empty()
            || part.len() > 8
            || !part.bytes().all(|b| b.is_ascii_digit())
            || (part.len() > 1 && part.starts_with('0'))
        {
            return None;
        }
        numbers[idx] = part.parse().ok()?;
    }
    if let Some(pre) = pre
        && (pre.is_empty()
            || pre.len() > 32
            || !pre.as_bytes()[0].is_ascii_alphanumeric()
            || pre
                .split('.')
                .any(|seg| seg.is_empty() || !seg.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')))
    {
        return None;
    }
    Some(ParsedVersion {
        numbers,
        parts_count: parts.len(),
        prerelease: pre.map(str::to_owned),
    })
}

/// Normalize a release tag (`retoque-v0.3.0`, `v0.3.0`, or `0.3.0`) to `0.3.0` (with optional `-prerelease`).
pub fn normalize_version(raw: &str) -> Option<String> {
    let parsed = parse_version(raw)?;
    if parsed.parts_count != 3 {
        return None;
    }
    let base = format!("{}.{}.{}", parsed.numbers[0], parsed.numbers[1], parsed.numbers[2]);
    Some(match parsed.prerelease {
        Some(pre) => format!("{base}-{pre}"),
        None => base,
    })
}

pub fn version_ok(raw: &str) -> bool {
    normalize_version(raw).is_some()
}

fn compare_pre_ident(a: &str, b: &str) -> Ordering {
    match (a.parse::<u64>(), b.parse::<u64>()) {
        (Ok(na), Ok(nb)) => na.cmp(&nb),
        (Ok(_), Err(_)) => Ordering::Less,
        (Err(_), Ok(_)) => Ordering::Greater,
        (Err(_), Err(_)) => a.cmp(b),
    }
}

/// Compare two version strings (`retoque-v0.3.0`, `v0.3.0`, `0.3.0`, or 4-part PE `10.0.0.1`).
pub fn compare_versions(a: &str, b: &str) -> Option<Ordering> {
    let va = parse_version(a)?;
    let vb = parse_version(b)?;
    match va.numbers.cmp(&vb.numbers) {
        Ordering::Equal => {}
        ord => return Some(ord),
    }
    Some(match (&va.prerelease, &vb.prerelease) {
        (None, None) => Ordering::Equal,
        (None, Some(_)) => Ordering::Greater,
        (Some(_), None) => Ordering::Less,
        (Some(pa), Some(pb)) => {
            let mut ia = pa.split('.');
            let mut ib = pb.split('.');
            loop {
                match (ia.next(), ib.next()) {
                    (Some(sa), Some(sb)) => match compare_pre_ident(sa, sb) {
                        Ordering::Equal => continue,
                        ord => break ord,
                    },
                    (None, None) => break Ordering::Equal,
                    (None, Some(_)) => break Ordering::Less,
                    (Some(_), None) => break Ordering::Greater,
                }
            }
        }
    })
}

pub(crate) fn asset_name(version: &str, app: bool) -> String {
    if app {
        format!("retoque-app-{version}-windows-x64.zip")
    } else {
        format!("retoque-payload-{version}-x64.zip")
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Release {
    pub tag: String,
    pub version: String,
    pub published_at: String,
    pub asset: String,
    pub url: String,
    pub size: u64,
    pub digest: String,
    pub sums_url: String,
    pub sums_size: u64,
    pub sums_digest: Option<String>,
}

impl Release {
    pub fn validate(&self) -> io::Result<()> {
        self.validate_kind(false)
    }

    pub(crate) fn validate_kind(&self, app: bool) -> io::Result<()> {
        let Some(norm) = normalize_version(&self.tag) else {
            return Err(invalid("invalid payload release tag"));
        };
        if self.version != norm
            || self.asset != asset_name(&self.version, app)
            || self.url != format!("{ASSET_PREFIX}{}/{}", self.tag, self.asset)
            || self.sums_url != format!("{ASSET_PREFIX}{}/SHA256SUMS", self.tag)
            || self.size == 0
            || self.size > MAX_DOWNLOAD
            || self.sums_size == 0
            || self.sums_size > MAX_SUMS_BYTES
            || self.published_at.is_empty()
            || self.published_at.len() > 40
            || !self.published_at.ends_with('Z')
            || self.published_at.chars().any(char::is_control)
            || !hash_ok(&self.digest)
            || self.sums_digest.as_ref().is_some_and(|d| !hash_ok(d))
        {
            return Err(invalid("invalid official payload release metadata"));
        }
        Ok(())
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct FileRecord {
    pub sha256: String,
    pub size: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq, Eq)]
pub struct Record {
    pub release: Release,
    pub sha256: String,
    pub optiscaler_sha256: String,
    pub forwarder_sha256: String,
    pub file_version: Option<String>,
    pub payload_dir: PathBuf,
    pub files: BTreeMap<String, FileRecord>,
}

/// A bounded HTTPS response reader with strict host and redirect validation.
pub trait Fetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>>;
}

pub struct GithubFetcher;

pub fn allowed_url(value: &str, redirected_asset: bool) -> bool {
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
    if value == RELEASES_URL || value == LATEST_URL {
        return !redirected_asset;
    }
    if u.host() == Some("github.com")
        && value.starts_with(ASSET_PREFIX)
        && u.query().is_none()
        && !u.path().contains('%')
    {
        let rest = &value[ASSET_PREFIX.len()..];
        if let Some((tag, asset)) = rest.split_once('/')
            && let Some(ver) = normalize_version(tag)
        {
            return asset == "SHA256SUMS" || asset == asset_name(&ver, false) || asset == asset_name(&ver, true);
        }
    }
    redirected_asset
        && matches!(
            u.host(),
            Some("release-assets.githubusercontent.com" | "objects.githubusercontent.com")
        )
}

impl Fetcher for GithubFetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
        if !allowed_url(url, false) {
            return Err(invalid("non-official payload URL"));
        }
        let asset = url.starts_with(ASSET_PREFIX);
        let agent = crate::fetch::agent(limit, "Retoque-payload-release");
        let mut current = url.to_owned();
        for hop in 0..5 {
            if !allowed_url(&current, asset && hop > 0) {
                return Err(invalid("refused payload release redirect"));
            }
            let mut response = agent
                .get(&current)
                .call()
                .map_err(|_| invalid("payload HTTPS request failed"))?;
            if response.status().is_redirection() {
                if !asset {
                    return Err(invalid("release API redirect refused"));
                }
                let location = response
                    .headers()
                    .get("location")
                    .and_then(|s| s.to_str().ok())
                    .ok_or_else(|| invalid("missing release redirect"))?;
                current = location.to_owned();
                continue;
            }
            let status = response.status().as_u16();
            if status == 404 && !asset {
                return Err(io::Error::new(io::ErrorKind::NotFound, "no release published yet"));
            }
            if status != 200 {
                return Err(invalid("unexpected release HTTP status"));
            }
            if response
                .headers()
                .get("content-length")
                .and_then(|v| v.to_str().ok())
                .and_then(|v| v.parse::<u64>().ok())
                .is_some_and(|n| n > limit)
            {
                return Err(invalid("release response exceeds size limit"));
            }
            let mut out = Vec::new();
            let mut reader = response.body_mut().as_reader();
            let mut buf = [0u8; 65536];
            loop {
                let n = reader.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                if out.len() as u64 + n as u64 > limit {
                    return Err(invalid("release response exceeds size limit"));
                }
                out.extend_from_slice(&buf[..n]);
                progress(out.len() as u64);
            }
            return Ok(out);
        }
        Err(invalid("too many release redirects"))
    }
}

#[derive(Deserialize)]
struct ApiRelease {
    tag_name: String,
    published_at: Option<String>,
    #[serde(default)]
    draft: bool,
    #[serde(default)]
    prerelease: bool,
    assets: Vec<ApiAsset>,
}

#[derive(Deserialize)]
struct ApiAsset {
    name: String,
    browser_download_url: String,
    size: u64,
    digest: Option<String>,
}

fn parse_digest_field(digest: Option<String>) -> io::Result<Option<String>> {
    digest
        .map(|d| {
            let hex = d
                .strip_prefix("sha256:")
                .map(str::to_ascii_lowercase)
                .ok_or_else(|| invalid("unsupported release digest"))?;
            if !hash_ok(&hex) {
                return Err(invalid("invalid release SHA-256 digest"));
            }
            Ok(hex)
        })
        .transpose()
}

fn convert_api_release(api: ApiRelease, app: bool) -> io::Result<Option<Release>> {
    if api.draft || api.prerelease {
        return Ok(None);
    }
    let version = normalize_version(&api.tag_name).ok_or_else(|| invalid("unsupported payload release tag"))?;
    let published_at = api
        .published_at
        .ok_or_else(|| invalid("missing release publication timestamp"))?;
    let wanted_zip = asset_name(&version, app);
    // Older releases may only carry a payload; newer app-only releases must not break payload discovery.
    if !api.assets.iter().any(|a| a.name == wanted_zip)
        && (app || api.assets.iter().any(|a| a.name == asset_name(&version, true)))
    {
        return Ok(None);
    }

    let mut matching_zip = api.assets.iter().filter(|a| a.name == wanted_zip);
    let zip_asset = matching_zip
        .next()
        .ok_or_else(|| invalid("documented payload release asset missing"))?;
    if matching_zip.next().is_some() {
        return Err(invalid("ambiguous payload release asset"));
    }

    let mut matching_sums = api.assets.iter().filter(|a| a.name == "SHA256SUMS");
    let sums_asset = matching_sums
        .next()
        .ok_or_else(|| invalid("documented SHA256SUMS release asset missing"))?;
    if matching_sums.next().is_some() {
        return Err(invalid("ambiguous SHA256SUMS release asset"));
    }

    let digest = parse_digest_field(zip_asset.digest.clone())?
        .ok_or_else(|| invalid("payload release asset is missing GitHub SHA-256 digest"))?;
    let sums_digest = parse_digest_field(sums_asset.digest.clone())?;

    let release = Release {
        tag: api.tag_name,
        version,
        published_at,
        asset: zip_asset.name.clone(),
        url: zip_asset.browser_download_url.clone(),
        size: zip_asset.size,
        digest,
        sums_url: sums_asset.browser_download_url.clone(),
        sums_size: sums_asset.size,
        sums_digest,
    };
    release.validate_kind(app)?;
    Ok(Some(release))
}

/// Parse GitHub release JSON (either a `/releases` array or a `/releases/latest` object).
/// Returns `Ok(None)` when no published non-draft release exists yet.
pub fn parse_releases_json(bytes: &[u8]) -> io::Result<Option<Release>> {
    parse_releases_kind(bytes, false)
}

pub(crate) fn parse_releases_kind(bytes: &[u8], app: bool) -> io::Result<Option<Release>> {
    if bytes.len() as u64 > MAX_API_BYTES {
        return Err(invalid("release metadata exceeds size limit"));
    }
    let value: serde_json::Value = serde_json::from_slice(bytes).map_err(|_| invalid("invalid release JSON"))?;
    match value {
        serde_json::Value::Array(items) => {
            if items.is_empty() {
                return Ok(None);
            }
            let mut candidates = Vec::new();
            for item in items {
                let api: ApiRelease = serde_json::from_value(item).map_err(|_| invalid("invalid release item"))?;
                if let Some(release) = convert_api_release(api, app)? {
                    candidates.push(release);
                }
            }
            candidates.sort_by(|a, b| compare_versions(&a.version, &b.version).unwrap_or(Ordering::Equal));
            Ok(candidates.pop())
        }
        serde_json::Value::Object(map) => {
            if !map.contains_key("tag_name")
                && map
                    .get("message")
                    .and_then(|v| v.as_str())
                    .is_some_and(|m| m.eq_ignore_ascii_case("not found"))
            {
                return Ok(None);
            }
            let api: ApiRelease = serde_json::from_value(serde_json::Value::Object(map))
                .map_err(|_| invalid("invalid release object"))?;
            convert_api_release(api, app)
        }
        _ => Err(invalid("unexpected release JSON root")),
    }
}

/// Query GitHub Releases for `gabrielmaialva33/retoque`. Returns `Ok(None)` when no release exists yet.
pub fn latest(fetcher: &dyn Fetcher) -> io::Result<Option<Release>> {
    match fetcher.get(RELEASES_URL, MAX_API_BYTES, &mut |_| {}) {
        Ok(bytes) => parse_releases_json(&bytes),
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(None),
        Err(e) => Err(e),
    }
}

fn safe_zip_path(path: &str) -> bool {
    let path = path.strip_suffix('/').unwrap_or(path);
    !path.is_empty()
        && path.len() <= 512
        && !path.starts_with('/')
        && !path.contains('\\')
        && path.split('/').count() <= 12
        && path.split('/').all(|part| {
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

/// True when a relative archive path names a forbidden NVIDIA, Lossless Scaling, sm86 or build-artifact file.
pub fn is_forbidden_path(path: &str) -> bool {
    let stripped = path.trim_end_matches('/');
    let lower = stripped.to_ascii_lowercase();
    let parts: Vec<&str> = lower.split('/').collect();
    let leaf = parts.last().copied().unwrap_or_default();
    parts.iter().any(|p| *p == "dlssnrmodels" || *p == "dlssg_sm86")
        || leaf.starts_with("nvngx_dlssnr")
        || matches!(
            leaf,
            "_nvngx.dll"
                | "nvngx_dlss.dll"
                | "nvngx_dlssd.dll"
                | "nvngx_dlssg.dll"
                | "lossless.dll"
                | "lsfg-vk.dll"
                | "version.dll"
                | "dlssg_sm86.ini"
                | "optiscaler.lang"
        )
        || leaf.contains("sm86")
        || leaf.contains("latewarp")
        || [".pdb", ".lib", ".exp", ".ilk", ".cso", ".spv"]
            .iter()
            .any(|ext| leaf.ends_with(ext))
}

/// True when a relative archive path is part of the allowed Retoque payload contract.
pub fn is_allowed_payload_path(path: &str) -> bool {
    if !safe_zip_path(path) || path.ends_with('/') || is_forbidden_path(path) {
        return false;
    }
    if matches!(path, "OptiScaler.dll" | "nvngx.dll_dlssnr.dll" | "OptiScaler.ini") {
        return true;
    }
    let parts: Vec<&str> = path.split('/').collect();
    match parts[0] {
        "Licenses" => {
            parts.len() == 2
                && (parts[1].ends_with(".txt") || parts[1].ends_with(".md") || parts[1].eq_ignore_ascii_case("license"))
        }
        "translations" => {
            path == "translations/README.md"
                || (parts.len() == 3 && parts[1] == "optional" && parts[2].ends_with(".lang"))
        }
        "OptiScaler" => parts.len() >= 2 && parts.len() <= 6,
        _ => false,
    }
}

fn is_allowed_dir(path: &str) -> bool {
    let stripped = path.trim_end_matches('/');
    safe_zip_path(stripped)
        && !is_forbidden_path(stripped)
        && (matches!(
            stripped,
            "Licenses" | "translations" | "translations/optional" | "OptiScaler"
        ) || stripped.starts_with("OptiScaler/"))
}

/// Parse a `SHA256SUMS` document (`<64-hex>  <path>` or `<64-hex> *<path>`).
pub fn parse_sha256sums(text: &str) -> io::Result<BTreeMap<String, String>> {
    let mut out = BTreeMap::new();
    for raw_line in text.lines() {
        let line = raw_line.trim_end_matches('\r');
        if line.trim().is_empty() {
            continue;
        }
        if line.len() < 66 {
            return Err(invalid("malformed line in SHA256SUMS"));
        }
        let (hash, rest) = line
            .split_at_checked(64)
            .ok_or_else(|| invalid("invalid UTF-8 checksum boundary"))?;
        if !hash_ok(hash) {
            return Err(invalid("invalid SHA-256 hex in SHA256SUMS"));
        }
        let name = rest
            .strip_prefix("  ")
            .or_else(|| rest.strip_prefix(" *"))
            .or_else(|| rest.strip_prefix(' '))
            .ok_or_else(|| invalid("invalid separator in SHA256SUMS"))?;
        let name = name.strip_prefix("./").unwrap_or(name);
        if !safe_zip_path(name) || name.ends_with('/') {
            return Err(invalid("unsafe path in SHA256SUMS"));
        }
        if out.insert(name.to_owned(), hash.to_ascii_lowercase()).is_some() {
            return Err(invalid("duplicate entry in SHA256SUMS"));
        }
    }
    if out.is_empty() {
        return Err(invalid("SHA256SUMS is empty"));
    }
    Ok(out)
}

fn has_enabled_true(ini_bytes: &[u8]) -> bool {
    for raw in ini_bytes.split(|&b| b == b'\n') {
        let line = raw.strip_suffix(b"\r").unwrap_or(raw);
        let trimmed = line.trim_ascii_start();
        if trimmed.starts_with(b";") || trimmed.starts_with(b"#") {
            continue;
        }
        let Some((key, rest)) = trimmed
            .iter()
            .position(|&b| b == b'=')
            .map(|i| (&trimmed[..i], &trimmed[i + 1..]))
        else {
            continue;
        };
        if !key.trim_ascii().eq_ignore_ascii_case(b"Enabled") {
            continue;
        }
        let val_part = match rest.iter().position(|&b| b == b';' || b == b'#') {
            Some(i) => &rest[..i],
            None => rest,
        };
        if val_part.trim_ascii().eq_ignore_ascii_case(b"true") {
            return true;
        }
    }
    false
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

pub(crate) struct UnpackedPayload {
    pub(crate) files: BTreeMap<String, FileRecord>,
    optiscaler_sha256: String,
    forwarder_sha256: String,
    file_version: Option<String>,
}

fn unpack(bytes: &[u8], stage: &Dir) -> io::Result<UnpackedPayload> {
    unpack_kind(bytes, stage, false)
}

pub(crate) fn unpack_kind(bytes: &[u8], stage: &Dir, app: bool) -> io::Result<UnpackedPayload> {
    let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(io::Error::other)?;
    if zip.len() > MAX_ENTRIES {
        return Err(invalid("too many payload archive entries"));
    }
    let mut names = HashSet::new();
    let mut present_files = HashSet::new();
    let mut total = 0u64;
    for i in 0..zip.len() {
        let file = zip.by_index(i).map_err(io::Error::other)?;
        let path = file.name();
        let stripped = path.trim_end_matches('/');
        if !safe_zip_path(path) || !names.insert(stripped.to_lowercase()) {
            return Err(invalid("unsafe or colliding payload archive path"));
        }
        let kind = file.unix_mode().unwrap_or(0) & 0o170000;
        if file.encrypted()
            || !matches!(kind, 0 | 0o100000 | 0o040000)
            || (kind == 0o040000 && !file.is_dir())
            || (kind == 0o100000 && file.is_dir())
        {
            return Err(invalid("archive link, encryption or special file refused"));
        }
        if is_forbidden_path(stripped) {
            return Err(invalid("forbidden file in payload archive"));
        }
        if file.is_dir() {
            if !is_allowed_dir(path) {
                return Err(invalid("unexpected directory in payload archive"));
            }
            continue;
        }
        if path != "SHA256SUMS" && !(is_allowed_payload_path(path) || app && path == "retoque-app.exe") {
            return Err(invalid("unexpected extra file in payload archive"));
        }
        total = total
            .checked_add(file.size())
            .ok_or_else(|| invalid("archive size overflow"))?;
        if total > MAX_UNPACKED {
            return Err(invalid("archive exceeds unpacked size limit"));
        }
        present_files.insert(path.to_owned());
    }
    if REQUIRED_FILES.iter().any(|req| !present_files.contains(*req))
        || app && !present_files.contains("retoque-app.exe")
    {
        return Err(invalid("incomplete payload release archive"));
    }

    let mut actual = 0u64;
    let mut files = BTreeMap::new();
    let mut internal_sums: Option<Vec<u8>> = None;
    let mut opti_bytes = Vec::new();
    let mut fwd_bytes = Vec::new();
    let mut ini_bytes = Vec::new();
    let mut app_header = Vec::new();

    for i in 0..zip.len() {
        let mut file = zip.by_index(i).map_err(io::Error::other)?;
        let path = file.name().to_owned();
        if file.is_dir() {
            continue;
        }
        if path == "SHA256SUMS" {
            let expected_size = file.size();
            if expected_size > MAX_SUMS_BYTES {
                return Err(invalid("internal SHA256SUMS exceeds size limit"));
            }
            let mut buf = Vec::new();
            file.take(MAX_SUMS_BYTES + 1).read_to_end(&mut buf)?;
            if buf.len() as u64 != expected_size {
                return Err(invalid("truncated internal SHA256SUMS"));
            }
            internal_sums = Some(buf);
            continue;
        }
        let dest = format!("payload/{path}");
        if let Some(parent) = Path::new(&dest).parent() {
            stage.create_dir_all(parent)?;
        }
        let mut out = stage.open_with(&dest, OpenOptions::new().write(true).create_new(true))?;
        let mut hasher = Sha256::new();
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
            hasher.update(&buf[..n]);
            out.write_all(&buf[..n])?;
            match path.as_str() {
                "OptiScaler.dll" => opti_bytes.extend_from_slice(&buf[..n]),
                "nvngx.dll_dlssnr.dll" => fwd_bytes.extend_from_slice(&buf[..n]),
                "OptiScaler.ini" => ini_bytes.extend_from_slice(&buf[..n]),
                "retoque-app.exe" if app_header.len() < 4096 => {
                    app_header.extend_from_slice(&buf[..n.min(4096 - app_header.len())]);
                }
                _ => {}
            }
        }
        if size == 0 || size != file.size() {
            return Err(invalid("empty or truncated payload archive entry"));
        }
        out.sync_all()?;
        files.insert(
            path,
            FileRecord {
                sha256: format!("{:x}", hasher.finalize()),
                size,
            },
        );
    }

    if let Some(sums_raw) = internal_sums {
        let text = std::str::from_utf8(&sums_raw).map_err(|_| invalid("internal SHA256SUMS is not UTF-8"))?;
        let sums = parse_sha256sums(text)?;
        for (name, expected_hash) in &sums {
            let Some(rec) = files.get(name) else {
                return Err(invalid("internal SHA256SUMS references missing file"));
            };
            if &rec.sha256 != expected_hash {
                return Err(invalid("internal SHA256SUMS member hash mismatch"));
            }
        }
    }

    if !opti_bytes.starts_with(b"MZ")
        || !opti_bytes
            .windows(b"OptiScaler working as".len())
            .any(|w| w == b"OptiScaler working as")
    {
        return Err(invalid("OptiScaler.dll is missing DOS header or marker"));
    }
    if !fwd_bytes.starts_with(b"MZ")
        || FORWARDER_EXPORTS
            .iter()
            .any(|exp| !fwd_bytes.windows(exp.len()).any(|w| w == *exp))
    {
        return Err(invalid("nvngx.dll_dlssnr.dll is missing required exports"));
    }
    if app && !crate::app_release::is_x64_exe(&app_header) {
        return Err(invalid("app is not a Windows x64 executable"));
    }
    if has_enabled_true(&ini_bytes) {
        return Err(invalid("packaged OptiScaler.ini has Enabled=true by default"));
    }

    let opti_ver = retoque_core::pe_version::read_version(&mut Cursor::new(&opti_bytes));
    let fwd_ver = retoque_core::pe_version::read_version(&mut Cursor::new(&fwd_bytes));
    if opti_ver != fwd_ver {
        return Err(invalid(
            "OptiScaler.dll and nvngx.dll_dlssnr.dll file versions do not match",
        ));
    }

    let optiscaler_sha256 = files["OptiScaler.dll"].sha256.clone();
    let forwarder_sha256 = files["nvngx.dll_dlssnr.dll"].sha256.clone();

    Ok(UnpackedPayload {
        files,
        optiscaler_sha256,
        forwarder_sha256,
        file_version: opti_ver,
    })
}

pub(crate) fn verify_archive(release: &Release, zip_bytes: &[u8], sums_bytes: &[u8], app: bool) -> io::Result<String> {
    release.validate_kind(app)?;
    if zip_bytes.len() as u64 != release.size || zip_bytes.len() as u64 > MAX_DOWNLOAD {
        return Err(invalid("payload archive size mismatch"));
    }
    if sums_bytes.len() as u64 != release.sums_size || sums_bytes.len() as u64 > MAX_SUMS_BYTES {
        return Err(invalid("SHA256SUMS size mismatch"));
    }
    if let Some(sums_digest) = &release.sums_digest {
        let actual_sums_hash = sha(sums_bytes);
        if !sums_digest.eq_ignore_ascii_case(&actual_sums_hash) {
            return Err(invalid("SHA256SUMS digest mismatch"));
        }
    }
    let zip_hash = sha(zip_bytes);
    if !release.digest.eq_ignore_ascii_case(&zip_hash) {
        return Err(invalid("payload SHA-256 does not match GitHub asset digest"));
    }
    let sums_text = std::str::from_utf8(sums_bytes).map_err(|_| invalid("SHA256SUMS is not UTF-8"))?;
    let sums = parse_sha256sums(sums_text)?;
    let Some(expected_from_sums) = sums.get(&release.asset) else {
        return Err(invalid("SHA256SUMS does not list payload archive"));
    };
    if !expected_from_sums.eq_ignore_ascii_case(&zip_hash) {
        return Err(invalid("payload SHA-256 does not match SHA256SUMS"));
    }

    Ok(zip_hash)
}

pub fn default_cache() -> io::Result<PathBuf> {
    #[cfg(windows)]
    let base = std::env::var_os("LOCALAPPDATA")
        .filter(|v| !v.is_empty())
        .map(|p| PathBuf::from(p).join("Retoque/payload-releases"));
    #[cfg(not(windows))]
    let base = std::env::var_os("XDG_DATA_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .map(|p| p.join("retoque/payload-releases"))
        .or_else(|| {
            std::env::var_os("HOME")
                .filter(|v| !v.is_empty())
                .map(|p| PathBuf::from(p).join(".local/share/retoque/payload-releases"))
        });
    base.ok_or_else(|| invalid("app data unavailable"))
}

/// Verified payload releases unpacked in app data. Never touches game folders.
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

    fn verify_entry(&self, name: &str) -> io::Result<Record> {
        let dir = self.held.dir.open_dir_nofollow(name)?;
        let mut bytes = Vec::new();
        open_regular(&dir, "release.json")?
            .take(4_194_305)
            .read_to_end(&mut bytes)?;
        if bytes.len() > 4_194_304 {
            return Err(invalid("payload release record exceeds limit"));
        }
        let mut record: Record = serde_json::from_slice(&bytes).map_err(io::Error::other)?;
        record.release.validate()?;
        if record.release.version != name
            || !hash_ok(&record.sha256)
            || !hash_ok(&record.optiscaler_sha256)
            || !hash_ok(&record.forwarder_sha256)
            || record.sha256 != record.release.digest
            || REQUIRED_FILES.iter().any(|req| !record.files.contains_key(*req))
        {
            return Err(invalid("invalid payload release record"));
        }
        let payload_dir = dir.open_dir_nofollow("payload")?;
        for (rel, expected) in &record.files {
            if !is_allowed_payload_path(rel) || !hash_ok(&expected.sha256) {
                return Err(invalid("invalid file entry in payload record"));
            }
            let mut file = open_regular(&payload_dir, rel)?;
            let mut hasher = Sha256::new();
            let mut buf = [0u8; 65536];
            let mut size = 0u64;
            loop {
                let n = file.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                size += n as u64;
                if size > expected.size {
                    return Err(invalid("cached payload file size mismatch"));
                }
                hasher.update(&buf[..n]);
            }
            if size != expected.size || format!("{:x}", hasher.finalize()) != expected.sha256 {
                return Err(invalid("cached payload file integrity check failed"));
            }
        }
        record.payload_dir = self.held.root.join(name).join("payload");
        Ok(record)
    }

    pub fn records(&self) -> io::Result<Vec<Record>> {
        self.held.current()?;
        let mut records = Vec::new();
        for entry in self.held.dir.entries()? {
            let name = entry?.file_name();
            if let Some(name) = name.to_str().filter(|n| version_ok(n)) {
                records.push(self.verify_entry(name)?);
            }
        }
        records.sort_by(|a, b| compare_versions(&b.release.version, &a.release.version).unwrap_or(Ordering::Equal));
        Ok(records)
    }

    pub fn latest_local(&self) -> io::Result<Option<Record>> {
        Ok(self.records()?.into_iter().next())
    }

    pub fn contains(&self, release: &Release) -> io::Result<bool> {
        Ok(self
            .records()?
            .iter()
            .any(|r| &r.release == release && r.sha256 == release.digest))
    }

    pub fn download(
        &self,
        fetcher: &dyn Fetcher,
        release: &Release,
        progress: &mut dyn FnMut(u64),
    ) -> io::Result<Record> {
        release.validate()?;
        let sums_bytes = fetcher.get(&release.sums_url, release.sums_size, &mut |_| {})?;
        let zip_bytes = fetcher.get(&release.url, release.size, progress)?;
        self.import_archive(release, &zip_bytes, &sums_bytes)
    }

    pub fn import_archive(&self, release: &Release, zip_bytes: &[u8], sums_bytes: &[u8]) -> io::Result<Record> {
        release.validate()?;
        self.held.current()?;
        let zip_hash = verify_archive(release, zip_bytes, sums_bytes, false)?;

        let existing = self
            .records()?
            .into_iter()
            .find(|r| r.release.version == release.version);
        if let Some(old) = &existing {
            if old.sha256 != zip_hash {
                return Err(invalid("payload release changed since first download"));
            }
            return Ok(old.clone());
        }

        static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        let stage_name = format!(
            ".unpack-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        );
        self.held.dir.create_dir(&stage_name)?;
        let result = (|| {
            let stage = self.held.dir.open_dir_nofollow(&stage_name)?;
            let unpacked = unpack(zip_bytes, &stage)?;
            self.held.current()?;
            let payload_dir = self.held.root.join(&release.version).join("payload");
            let record = Record {
                release: release.clone(),
                sha256: zip_hash,
                optiscaler_sha256: unpacked.optiscaler_sha256,
                forwarder_sha256: unpacked.forwarder_sha256,
                file_version: unpacked.file_version,
                payload_dir,
                files: unpacked.files,
            };
            write_new(
                &stage,
                "release.json",
                &serde_json::to_vec_pretty(&record).map_err(io::Error::other)?,
            )?;
            drop(stage);
            self.held.current()?;
            if let Err(e) = self.held.dir.rename(&stage_name, &self.held.dir, &release.version) {
                let accepted = self
                    .records()?
                    .into_iter()
                    .find(|r| r.release.version == release.version);
                if let Some(accepted) = accepted
                    && accepted.sha256 == record.sha256
                {
                    return Ok(accepted);
                }
                return Err(e);
            }
            self.verify_entry(&release.version)
        })();
        let _ = self.held.dir.remove_dir_all(&stage_name);
        result
    }
}

#[doc(hidden)]
pub fn versioned_pe_fixture(ver: (u16, u16, u16, u16), tail: &[u8]) -> Vec<u8> {
    fn put16(b: &mut [u8], n: usize, v: u16) {
        b[n..n + 2].copy_from_slice(&v.to_le_bytes());
    }
    fn put32(b: &mut [u8], n: usize, v: u32) {
        b[n..n + 4].copy_from_slice(&v.to_le_bytes());
    }
    let mut b = vec![0; 1024];
    b[..2].copy_from_slice(b"MZ");
    put32(&mut b, 60, 64);
    b[64..68].copy_from_slice(b"PE\0\0");
    put16(&mut b, 70, 1);
    put16(&mut b, 84, 240);
    put16(&mut b, 88, 0x20b);
    let d = 88 + 112;
    put32(&mut b, d - 4, 16);
    put32(&mut b, d + 16, 0x1000);
    put32(&mut b, d + 20, 256);
    let s = 88 + 240;
    put32(&mut b, s + 12, 0x1000);
    put32(&mut b, s + 16, 256);
    put32(&mut b, s + 20, 512);
    let r = &mut b[512..];
    for (at, id, to) in [(0, 16, 0x80000020), (32, 1, 0x80000040), (64, 1033, 96)] {
        put16(r, at + 14, 1);
        put32(r, at + 16, id);
        put32(r, at + 20, to);
    }
    put32(r, 96, 0x1080);
    put32(r, 100, 92);
    put16(r, 128, 92);
    put16(r, 130, 52);
    for (i, c) in "VS_VERSION_INFO\0".encode_utf16().enumerate() {
        put16(r, 134 + i * 2, c);
    }
    put32(r, 168, 0xfeef04bd);
    put32(r, 172, 0x10000);
    put32(r, 176, (u32::from(ver.0) << 16) | u32::from(ver.1));
    put32(r, 180, (u32::from(ver.2) << 16) | u32::from(ver.3));
    b.extend_from_slice(tail);
    b
}

#[doc(hidden)]
pub fn synthetic_release_fixture(
    tag: &str,
    ver: (u16, u16, u16, u16),
    extra: Option<(&str, &[u8])>,
) -> (Release, Vec<u8>, Vec<u8>, Vec<u8>) {
    use zip::write::SimpleFileOptions;
    let opti = versioned_pe_fixture(ver, b"OptiScaler working as synthetic");
    let fwd = versioned_pe_fixture(
        ver,
        b"dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version",
    );
    let ini = b"[Log]\nLogToFile=true\nLogLevel=2\n[Menu]\nShortcutKey=0x24\n[DlssNr]\nEnabled=auto\nModelFile=auto\n";
    let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
    let entries: [(&str, &[u8]); 7] = [
        ("OptiScaler.dll", &opti),
        ("nvngx.dll_dlssnr.dll", &fwd),
        ("OptiScaler.ini", ini),
        ("Licenses/RenoDX_ATTRIBUTION.txt", b"RenoDX attribution text"),
        ("translations/README.md", b"Translations readme"),
        ("translations/optional/pt-BR.lang", b"pt-BR lang dictionary"),
        ("OptiScaler/support.txt", b"optional support file"),
    ];
    for (path, data) in entries {
        z.start_file(path, SimpleFileOptions::default()).unwrap();
        z.write_all(data).unwrap();
    }
    if let Some((path, data)) = extra {
        z.start_file(path, SimpleFileOptions::default()).unwrap();
        z.write_all(data).unwrap();
    }
    let zip_bytes = z.finish().unwrap().into_inner();
    let version = normalize_version(tag).unwrap();
    let asset = format!("retoque-payload-{version}-x64.zip");
    let zip_sha = sha(&zip_bytes);
    let sums = format!("{zip_sha}  {asset}\n").into_bytes();
    let sums_sha = sha(&sums);
    let release = Release {
        tag: tag.into(),
        version,
        published_at: "2026-11-09T12:00:00Z".into(),
        asset: asset.clone(),
        url: format!("{ASSET_PREFIX}{tag}/{asset}"),
        size: zip_bytes.len() as u64,
        digest: zip_sha.clone(),
        sums_url: format!("{ASSET_PREFIX}{tag}/SHA256SUMS"),
        sums_size: sums.len() as u64,
        sums_digest: Some(sums_sha.clone()),
    };
    let api_json = serde_json::json!([
        {
            "tag_name": release.tag,
            "published_at": release.published_at,
            "draft": false,
            "prerelease": false,
            "assets": [
                {
                    "name": release.asset,
                    "browser_download_url": release.url,
                    "size": release.size,
                    "digest": format!("sha256:{zip_sha}")
                },
                {
                    "name": "SHA256SUMS",
                    "browser_download_url": release.sums_url,
                    "size": release.sums_size,
                    "digest": format!("sha256:{sums_sha}")
                }
            ]
        }
    ])
    .to_string()
    .into_bytes();
    (release, zip_bytes, sums, api_json)
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;
    use zip::write::SimpleFileOptions;

    pub(crate) fn versioned_pe(major: u16, minor: u16, patch: u16, build: u16, tail: &[u8]) -> Vec<u8> {
        versioned_pe_fixture((major, minor, patch, build), tail)
    }

    pub(crate) fn synthetic_archive(ver: (u16, u16, u16, u16), extra: Option<(&str, &[u8])>) -> Vec<u8> {
        synthetic_release_fixture("retoque-v0.3.0", ver, extra).1
    }

    pub(crate) fn release_for(tag: &str, zip_bytes: &[u8]) -> (Release, Vec<u8>, Vec<u8>) {
        let version = normalize_version(tag).unwrap();
        let asset = format!("retoque-payload-{version}-x64.zip");
        let zip_sha = sha(zip_bytes);
        let sums = format!("{zip_sha}  {asset}\n").into_bytes();
        let sums_sha = sha(&sums);
        let release = Release {
            tag: tag.into(),
            version,
            published_at: "2026-11-09T12:00:00Z".into(),
            asset: asset.clone(),
            url: format!("{ASSET_PREFIX}{tag}/{asset}"),
            size: zip_bytes.len() as u64,
            digest: zip_sha.clone(),
            sums_url: format!("{ASSET_PREFIX}{tag}/SHA256SUMS"),
            sums_size: sums.len() as u64,
            sums_digest: Some(sums_sha.clone()),
        };
        let api_json = serde_json::json!([
            {
                "tag_name": release.tag,
                "published_at": release.published_at,
                "draft": false,
                "prerelease": false,
                "assets": [
                    {
                        "name": release.asset,
                        "browser_download_url": release.url,
                        "size": release.size,
                        "digest": format!("sha256:{zip_sha}")
                    },
                    {
                        "name": "SHA256SUMS",
                        "browser_download_url": release.sums_url,
                        "size": release.sums_size,
                        "digest": format!("sha256:{sums_sha}")
                    }
                ]
            }
        ])
        .to_string()
        .into_bytes();
        (release, sums, api_json)
    }

    pub(crate) struct SyntheticFetcher {
        pub api_json: Option<Vec<u8>>,
        pub zip_bytes: Vec<u8>,
        pub sums_bytes: Vec<u8>,
    }

    impl Fetcher for SyntheticFetcher {
        fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
            let data = if url == RELEASES_URL || url == LATEST_URL {
                match &self.api_json {
                    Some(b) => b,
                    None => {
                        return Err(io::Error::new(io::ErrorKind::NotFound, "no release published yet"));
                    }
                }
            } else if url.ends_with("/SHA256SUMS") {
                assert!(allowed_url(url, false));
                &self.sums_bytes
            } else {
                assert!(allowed_url(url, false));
                &self.zip_bytes
            };
            assert!(data.len() as u64 <= limit);
            progress(data.len() as u64);
            Ok(data.clone())
        }
    }

    #[test]
    fn release_json_parsing_picks_newest_stable_and_handles_no_release_cases() {
        let old_zip = synthetic_archive((10, 0, 0, 1), None);
        let new_zip = synthetic_archive((10, 0, 0, 2), None);
        let (old_rel, _, _) = release_for("retoque-v0.3.0", &old_zip);
        let (new_rel, _, _) = release_for("retoque-v0.3.2", &new_zip);
        let (draft_rel, _, _) = release_for("retoque-v0.4.0", &new_zip);
        let json = serde_json::json!([
            {
                "tag_name": old_rel.tag,
                "published_at": old_rel.published_at,
                "draft": false,
                "prerelease": false,
                "assets": [
                    {"name": old_rel.asset, "browser_download_url": old_rel.url, "size": old_rel.size, "digest": format!("sha256:{}", old_rel.digest)},
                    {"name": "SHA256SUMS", "browser_download_url": old_rel.sums_url, "size": old_rel.sums_size, "digest": format!("sha256:{}", old_rel.sums_digest.as_ref().unwrap())}
                ]
            },
            {
                "tag_name": draft_rel.tag,
                "published_at": draft_rel.published_at,
                "draft": true,
                "prerelease": false,
                "assets": []
            },
            {
                "tag_name": new_rel.tag,
                "published_at": new_rel.published_at,
                "draft": false,
                "prerelease": false,
                "assets": [
                    {"name": new_rel.asset, "browser_download_url": new_rel.url, "size": new_rel.size, "digest": format!("sha256:{}", new_rel.digest)},
                    {"name": "SHA256SUMS", "browser_download_url": new_rel.sums_url, "size": new_rel.sums_size, "digest": format!("sha256:{}", new_rel.sums_digest.as_ref().unwrap())}
                ]
            }
        ])
        .to_string()
        .into_bytes();

        let parsed = parse_releases_json(&json).unwrap().unwrap();
        assert_eq!(parsed, new_rel);

        // No-release cases: empty array, GitHub 404 JSON object, only drafts, and HTTP 404 NotFound.
        assert_eq!(parse_releases_json(b"[]").unwrap(), None);
        assert_eq!(
            parse_releases_json(br#"{"message":"Not Found","documentation_url":"https://docs.github.com/rest"}"#)
                .unwrap(),
            None
        );
        assert_eq!(
            parse_releases_json(br#"[{"tag_name":"retoque-v0.3.0","draft":true,"prerelease":false,"assets":[]}]"#)
                .unwrap(),
            None
        );
        let no_rel_fetcher = SyntheticFetcher {
            api_json: None,
            zip_bytes: Vec::new(),
            sums_bytes: Vec::new(),
        };
        assert_eq!(latest(&no_rel_fetcher).unwrap(), None);
    }

    #[test]
    fn version_comparison_handles_tags_numeric_order_and_prereleases() {
        assert_eq!(compare_versions("retoque-v0.3.0", "v0.3.0"), Some(Ordering::Equal));
        assert_eq!(compare_versions("0.3.0", "v0.3.0"), Some(Ordering::Equal));
        assert_eq!(
            compare_versions("retoque-v0.3.10", "retoque-v0.3.2"),
            Some(Ordering::Greater)
        );
        assert_eq!(compare_versions("0.4.0", "0.3.99"), Some(Ordering::Greater));
        assert_eq!(compare_versions("0.3.0", "0.3.0-rc.1"), Some(Ordering::Greater));
        assert_eq!(compare_versions("0.3.0-rc.2", "0.3.0-rc.1"), Some(Ordering::Greater));
        assert_eq!(compare_versions("10.0.0.2", "10.0.0.1"), Some(Ordering::Greater));
        assert_eq!(compare_versions("retoque-0.3.0", "0.3.0"), None);
        assert_eq!(compare_versions("invalid", "0.3.0"), None);
    }

    #[test]
    fn digest_and_sha256sums_mismatches_are_refused() {
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let zip_bytes = synthetic_archive((10, 0, 0, 1), None);
        let (release, sums, _) = release_for("retoque-v0.3.0", &zip_bytes);

        // 1. Wrong GitHub asset digest.
        let mut bad_digest = release.clone();
        bad_digest.digest = "0".repeat(64);
        assert!(
            cache
                .import_archive(&bad_digest, &zip_bytes, &sums)
                .unwrap_err()
                .to_string()
                .contains("GitHub asset digest")
        );

        // 2. Wrong hash in SHA256SUMS.
        let bad_sums = format!("{}  {}\n", "a".repeat(64), release.asset).into_bytes();
        let mut r2 = release.clone();
        r2.sums_size = bad_sums.len() as u64;
        r2.sums_digest = Some(sha(&bad_sums));
        assert!(
            cache
                .import_archive(&r2, &zip_bytes, &bad_sums)
                .unwrap_err()
                .to_string()
                .contains("SHA256SUMS")
        );

        // 3. Wrong SHA256SUMS asset digest.
        let mut bad_sums_digest = release.clone();
        bad_sums_digest.sums_digest = Some("b".repeat(64));
        assert!(
            cache
                .import_archive(&bad_sums_digest, &zip_bytes, &sums)
                .unwrap_err()
                .to_string()
                .contains("SHA256SUMS digest")
        );

        // 4. Internal SHA256SUMS inside zip with mismatched member hash.
        let bad_internal = synthetic_archive(
            (10, 0, 0, 1),
            Some(("SHA256SUMS", format!("{}  OptiScaler.dll\n", "c".repeat(64)).as_bytes())),
        );
        let (r_int, sums_int, _) = release_for("retoque-v0.3.0", &bad_internal);
        assert!(
            cache
                .import_archive(&r_int, &bad_internal, &sums_int)
                .unwrap_err()
                .to_string()
                .contains("internal SHA256SUMS")
        );

        // Nothing was published to the cache.
        assert!(cache.records().unwrap().is_empty());
    }

    #[test]
    fn extra_and_forbidden_files_and_version_mismatches_are_refused() {
        for (bad_path, expected_err) in [
            ("nvngx_dlssnr.dll", "forbidden"),
            ("OptiScaler/nvngx_dlssnr_v2.dll", "forbidden"),
            ("DlssNrModels/model.bin", "forbidden"),
            ("_nvngx.dll", "forbidden"),
            ("nvngx_dlssg.dll", "forbidden"),
            ("OptiScaler/dlssg_sm86.dll", "forbidden"),
            ("version.dll", "forbidden"),
            ("dlssg_sm86.ini", "forbidden"),
            ("lossless.dll", "forbidden"),
            ("lsfg-vk.dll", "forbidden"),
            ("OptiScaler/shader.cso", "forbidden"),
            ("OptiScaler/debug.pdb", "forbidden"),
            ("OptiScaler/import.lib", "forbidden"),
            ("OptiScaler/latewarp.dll", "forbidden"),
            ("OptiScaler.lang", "forbidden"),
            ("extra/unexpected.txt", "unexpected extra file"),
            ("setup_windows.bat", "unexpected extra file"),
            ("../escape.dll", "unsafe"),
        ] {
            let zip_bytes = synthetic_archive((10, 0, 0, 1), Some((bad_path, b"MZ bad")));
            let (release, sums, _) = release_for("retoque-v0.3.0", &zip_bytes);
            let t = tempfile::tempdir().unwrap();
            let cache = Cache::open(&t.path().join("cache")).unwrap();
            let err = cache
                .import_archive(&release, &zip_bytes, &sums)
                .unwrap_err()
                .to_string();
            assert!(
                err.contains(expected_err),
                "path {bad_path} gave unexpected error: {err}"
            );
            assert!(cache.records().unwrap().is_empty());
        }

        // Mismatched OptiScaler.dll vs forwarder PE VERSIONINFO must be refused.
        let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
        let opti = versioned_pe(10, 0, 0, 1, b"OptiScaler working as synthetic");
        let fwd = versioned_pe(
            10,
            0,
            0,
            2,
            b"dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version",
        );
        for (p, b) in [
            ("OptiScaler.dll", opti.as_slice()),
            ("nvngx.dll_dlssnr.dll", fwd.as_slice()),
            ("OptiScaler.ini", b"[DlssNr]\nEnabled=auto\n".as_slice()),
            ("Licenses/RenoDX_ATTRIBUTION.txt", b"attr".as_slice()),
            ("translations/README.md", b"readme".as_slice()),
            ("translations/optional/pt-BR.lang", b"lang".as_slice()),
        ] {
            z.start_file(p, SimpleFileOptions::default()).unwrap();
            z.write_all(b).unwrap();
        }
        let mismatched = z.finish().unwrap().into_inner();
        let (rel, sums, _) = release_for("retoque-v0.3.0", &mismatched);
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        assert!(
            cache
                .import_archive(&rel, &mismatched, &sums)
                .unwrap_err()
                .to_string()
                .contains("versions do not match")
        );
    }

    #[test]
    fn allowed_hosts_only_and_no_api_redirects() {
        assert!(allowed_url(RELEASES_URL, false));
        assert!(allowed_url(LATEST_URL, false));
        assert!(!allowed_url(RELEASES_URL, true));
        assert!(allowed_url(
            "https://github.com/gabrielmaialva33/retoque/releases/download/retoque-v0.3.0/retoque-payload-0.3.0-x64.zip",
            false
        ));
        assert!(allowed_url(
            "https://github.com/gabrielmaialva33/retoque/releases/download/retoque-v0.3.0/SHA256SUMS",
            false
        ));
        assert!(allowed_url(
            "https://release-assets.githubusercontent.com/blob?sig=1",
            true
        ));
        assert!(!allowed_url(
            "https://release-assets.githubusercontent.com/blob?sig=1",
            false
        ));
        for bad in [
            "http://api.github.com/repos/gabrielmaialva33/retoque/releases",
            "https://api.github.com/repos/other/repo/releases",
            "https://github.com/other/repo/releases/download/v0.3.0/retoque-payload-0.3.0-x64.zip",
            "https://github.com/gabrielmaialva33/retoque/releases/download/retoque-v0.3.0/evil.zip",
            "https://evil.example/retoque-payload-0.3.0-x64.zip",
        ] {
            assert!(!allowed_url(bad, false), "{bad}");
            assert!(!allowed_url(bad, true), "{bad}");
        }
    }

    #[test]
    fn downloaded_payload_is_used_by_install_plan_and_restores_on_uninstall() {
        let t = tempfile::tempdir().unwrap();
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let zip_bytes = synthetic_archive((10, 0, 0, 1), None);
        let (release, sums, api_json) = release_for("retoque-v0.3.0", &zip_bytes);
        let fetcher = SyntheticFetcher {
            api_json: Some(api_json),
            zip_bytes,
            sums_bytes: sums,
        };
        let found = latest(&fetcher).unwrap().unwrap();
        assert_eq!(found, release);
        let record = cache.download(&fetcher, &found, &mut |_| {}).unwrap();
        assert_eq!(record.file_version.as_deref(), Some("10.0.0.1"));
        assert!(cache.contains(&found).unwrap());

        let game = t.path().join("game");
        std::fs::create_dir_all(&game).unwrap();
        std::fs::write(game.join("Game.exe"), b"MZ game exe").unwrap();

        let plan = retoque_core::plan_install(&retoque_core::InstallOptions {
            game_dir: game.clone(),
            source_dir: record.payload_dir.clone(),
            proxy: "dxgi.dll".into(),
            enable_nr: true,
            copy_nvngx: false,
            ..Default::default()
        })
        .unwrap();
        assert!(plan.refusals().is_empty());
        assert!(!game.join("dxgi.dll").exists(), "plan must not write");

        let verified = plan.into_verified().unwrap();
        retoque_core::apply(&verified, &mut |_| {}).unwrap();

        let status = retoque_core::status(&game).unwrap();
        assert_eq!(
            status.receipt.as_ref().unwrap().status,
            retoque_core::receipt::ReceiptStatus::Complete
        );
        assert_eq!(status.build_sha256.as_deref(), Some(record.optiscaler_sha256.as_str()));
        assert_eq!(status.proxy_version.as_deref(), Some("10.0.0.1"));
        assert_eq!(status.forwarder_version.as_deref(), Some("10.0.0.1"));
        assert!(!status.version_mismatch());
        assert_eq!(status.nr_enabled, Some(true));

        let uninstall = retoque_core::plan_uninstall(&game).unwrap().into_verified().unwrap();
        retoque_core::apply(&uninstall, &mut |_| {}).unwrap();
        assert!(!game.join("dxgi.dll").exists());
        assert!(!game.join("OptiScaler.dll").exists());
        assert!(!game.join("nvngx.dll_dlssnr.dll").exists());
        assert!(!game.join(".retoque/receipt.txt").exists());
        assert_eq!(std::fs::read(game.join("Game.exe")).unwrap(), b"MZ game exe");
    }

    #[cfg(unix)]
    #[test]
    fn make_payload_release_script_packages_synthetic_out_and_enforces_guards() {
        use std::process::Command;

        let repo_root = Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(4).unwrap();
        let script = repo_root.join("tools/app/scripts/make-payload-release.sh");
        let t = tempfile::tempdir().unwrap();
        let out_dir = t.path().join("x64/out");
        let dest_dir = t.path().join("release-out");
        std::fs::create_dir_all(out_dir.join("OptiScaler")).unwrap();

        let opti = versioned_pe(10, 0, 0, 1, b"OptiScaler working as synthetic");
        let fwd = versioned_pe(
            10,
            0,
            0,
            1,
            b"dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version",
        );
        std::fs::write(out_dir.join("OptiScaler.dll"), &opti).unwrap();
        std::fs::write(out_dir.join("nvngx.dll_dlssnr.dll"), &fwd).unwrap();
        std::fs::write(out_dir.join("OptiScaler/support.txt"), b"support").unwrap();

        let output = Command::new("bash")
            .arg(&script)
            .arg("--build-dir")
            .arg(&out_dir)
            .arg("--out")
            .arg(&dest_dir)
            .arg("--version")
            .arg("retoque-v0.3.0")
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "script failed: {}",
            String::from_utf8_lossy(&output.stderr)
        );
        let stdout = String::from_utf8_lossy(&output.stdout);
        assert!(stdout.contains("gh release create retoque-v0.3.0 --repo gabrielmaialva33/retoque"));

        let zip_path = dest_dir.join("retoque-payload-0.3.0-x64.zip");
        let sums_path = dest_dir.join("SHA256SUMS");
        assert!(zip_path.is_file());
        assert!(sums_path.is_file());

        let zip_bytes = std::fs::read(&zip_path).unwrap();
        let sums_bytes = std::fs::read(&sums_path).unwrap();
        let release = Release {
            tag: "retoque-v0.3.0".into(),
            version: "0.3.0".into(),
            published_at: "2026-11-09T12:00:00Z".into(),
            asset: "retoque-payload-0.3.0-x64.zip".into(),
            url: format!("{ASSET_PREFIX}retoque-v0.3.0/retoque-payload-0.3.0-x64.zip"),
            size: zip_bytes.len() as u64,
            digest: sha(&zip_bytes),
            sums_url: format!("{ASSET_PREFIX}retoque-v0.3.0/SHA256SUMS"),
            sums_size: sums_bytes.len() as u64,
            sums_digest: Some(sha(&sums_bytes)),
        };
        let cache = Cache::open(&t.path().join("cache")).unwrap();
        let record = cache.import_archive(&release, &zip_bytes, &sums_bytes).unwrap();
        assert_eq!(record.file_version.as_deref(), Some("10.0.0.1"));
        assert!(record.payload_dir.join("OptiScaler/support.txt").is_file());
        // Every optional menu text pack ships, byte for byte, and none is active at the archive root.
        for pack in ["pt-BR", "de", "es", "fr", "ja", "ko", "pl", "ru", "zh-CN"] {
            let rel = format!("translations/optional/{pack}.lang");
            assert!(is_allowed_payload_path(&rel), "{rel} must be allowed");
            assert!(record.files.contains_key(&rel), "{rel} missing from the payload record");
            assert_eq!(
                std::fs::read(record.payload_dir.join(&rel)).unwrap(),
                std::fs::read(repo_root.join("retoque").join(&rel)).unwrap(),
                "{rel} differs from the repository pack"
            );
        }
        assert!(!record.payload_dir.join("OptiScaler.lang").exists());
        assert!(!is_allowed_payload_path("translations/ja.lang"));
        assert!(!is_allowed_payload_path("translations/optional/ja/ja.lang"));

        // Guard 1: Mismatched forwarder version must fail.
        let bad_fwd = versioned_pe(
            10,
            0,
            0,
            2,
            b"dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version",
        );
        std::fs::write(out_dir.join("nvngx.dll_dlssnr.dll"), &bad_fwd).unwrap();
        let mismatch_out = Command::new("bash")
            .arg(&script)
            .arg("--build-dir")
            .arg(&out_dir)
            .arg("--out")
            .arg(t.path().join("mismatch-out"))
            .output()
            .unwrap();
        assert!(!mismatch_out.status.success());
        assert!(String::from_utf8_lossy(&mismatch_out.stderr).contains("does not match"));
        std::fs::write(out_dir.join("nvngx.dll_dlssnr.dll"), &fwd).unwrap();

        // Guard 2: Forbidden files in build output must fail.
        for forbidden in ["nvngx_dlssnr.dll", "lsfg-vk.dll", "dlssg_sm86.dll"] {
            let p = out_dir.join(forbidden);
            std::fs::write(&p, b"MZ forbidden").unwrap();
            let res = Command::new("bash")
                .arg(&script)
                .arg("--build-dir")
                .arg(&out_dir)
                .arg("--out")
                .arg(t.path().join("forbidden-out"))
                .output()
                .unwrap();
            std::fs::remove_file(&p).unwrap();
            assert!(!res.status.success(), "{forbidden} should be refused");
            assert!(
                String::from_utf8_lossy(&res.stderr).contains("forbidden"),
                "{forbidden} stderr: {}",
                String::from_utf8_lossy(&res.stderr)
            );
        }
    }
}
