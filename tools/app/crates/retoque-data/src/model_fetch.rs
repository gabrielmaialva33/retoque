//! Community NR model catalog and verified archive import (`RankFTW/rhi-repo` via `ApolloF/DLSS5-Updater-catalog`).
//! There is no official NVIDIA download of `nvngx_dlssnr.dll`. Callers must show that the source is a community
//! redistribution and obtain explicit confirmation before calling `refresh_catalog` or `download_and_import`.
//! Tests inject a `Fetcher` and never touch the network.

pub use crate::sm86::Fetcher;
use crate::{
    gpu::Gpu,
    model_compat::{Compatibility, MAX_MODEL_BYTES, ModelCompatibility},
    models::{Catalog, Model},
};
use serde::Deserialize;
use sha2::{Digest, Sha256};
use std::{
    collections::HashSet,
    io::{self, Cursor, Read, Write},
    time::Duration,
};

pub const CATALOG_URL: &str = "https://raw.githubusercontent.com/ApolloF/DLSS5-Updater-catalog/main/catalog.json";
pub const ASSET_PREFIX: &str = "https://github.com/RankFTW/rhi-repo/releases/download/";
pub const SOURCE_LABEL: &str = "RankFTW/rhi-repo (ApolloF/DLSS5-Updater-catalog)";
pub const MAX_CATALOG_BYTES: u64 = 8 * 1024 * 1024;
pub const MAX_ARCHIVE_BYTES: u64 = MAX_MODEL_BYTES;
pub const MAX_UNPACKED_BYTES: u64 = MAX_MODEL_BYTES;
const MAX_CATALOG_ENTRIES: usize = 64;
const MAX_ARCHIVE_ENTRIES: usize = 64;
const MODEL_DLL_NAME: &str = "nvngx_dlssnr.dll";

fn invalid(message: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, message)
}

fn valid_sha256(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit())
}

fn version_ok(v: &str) -> bool {
    !v.is_empty()
        && v.len() <= 32
        && v.split('.').count() >= 3
        && v.split('.').count() <= 4
        && v.split('.')
            .all(|part| !part.is_empty() && part.len() <= 8 && part.bytes().all(|b| b.is_ascii_digit()))
}

fn safe_segment(seg: &str) -> bool {
    !seg.is_empty()
        && seg.len() <= 128
        && seg != "."
        && seg != ".."
        && !seg.ends_with(['.', ' '])
        && seg
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
}

pub fn allowed_asset_url(value: &str) -> bool {
    let Ok(u) = value.parse::<ureq::http::Uri>() else {
        return false;
    };
    let Some(host) = u.host() else { return false };
    let Some(authority) = u.authority() else { return false };
    if u.scheme_str() != Some("https")
        || host != "github.com"
        || !(authority.as_str() == host || authority.as_str() == "github.com:443")
        || value.contains('#')
        || u.query().is_some()
        || u.path().contains('%')
        || !value.starts_with(ASSET_PREFIX)
    {
        return false;
    }
    let rest = &value[ASSET_PREFIX.len()..];
    let Some((tag, asset)) = rest.split_once('/') else {
        return false;
    };
    safe_segment(tag) && safe_segment(asset) && asset.ends_with(".zip")
}

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
    if !redirected_asset && value == CATALOG_URL {
        return true;
    }
    if allowed_asset_url(value) {
        return true;
    }
    redirected_asset
        && matches!(
            host,
            "release-assets.githubusercontent.com" | "objects.githubusercontent.com"
        )
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct RemoteModel {
    pub version: String,
    pub variant: String,
    pub label: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    pub compatibility: ModelCompatibility,
}

impl RemoteModel {
    pub fn validate(&self) -> io::Result<()> {
        if !version_ok(&self.version)
            || self.variant.is_empty()
            || self.variant.len() > 64
            || self.variant.chars().any(char::is_control)
            || self.label.is_empty()
            || self.label.len() > 128
            || self.label.chars().any(char::is_control)
            || !valid_sha256(&self.sha256)
            || self.size == 0
            || self.size > MAX_UNPACKED_BYTES
            || !allowed_asset_url(&self.url)
        {
            return Err(invalid("invalid model catalog metadata"));
        }
        let lower_hash = self.sha256.to_ascii_lowercase();
        if let Some((_, probed_size, _, _, _)) = PROBED_VARIANTS.iter().find(|(h, ..)| *h == lower_hash)
            && self.size != *probed_size
        {
            return Err(invalid("pinned model metadata mismatch"));
        }
        if let Some(pinned) = PINNED_TABLE.iter().find(|p| p.sha256 == lower_hash)
            && (self.size != pinned.size || self.url != pinned.url || lower_hash != pinned.sha256)
        {
            return Err(invalid("pinned model metadata mismatch"));
        }
        Ok(())
    }
}

// All 9 locally probed NR variants from nr-probe.json (sha256, byte size, version, variant label, CUDA SASS targets).
const PROBED_VARIANTS: &[(&str, u64, &str, &str, &[u16])] = &[
    (
        "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed",
        276_162_560,
        "310.9.16",
        "compat-fp16",
        &[75, 86, 89, 120],
    ),
    (
        "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f",
        266_424_944,
        "310.9.16",
        "official",
        &[120],
    ),
    (
        "f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc",
        165_840_496,
        "310.8.3",
        "lecram-untested",
        &[75, 86, 89, 120],
    ),
    (
        "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927",
        165_830_144,
        "310.8.2",
        "SF-v2",
        &[75, 86, 89, 120],
    ),
    (
        "4b8d19bc3eff58a084f5eca7489c921501c203450169fb82ff4f649a4482ba05",
        165_840_496,
        "310.8.0",
        "RTX40",
        &[89, 120],
    ),
    (
        "e67dee209320cdafe0e93e45675d7aa34323a53acc57a72b2e40a181581c989a",
        165_840_496,
        "310.8.0",
        "compat-fp16-openNR",
        &[75, 86, 89, 120],
    ),
    (
        "89fa32d64952fef1845ba6de7f0141d3f7c809929ff17890cf7f27e049a35a10",
        165_830_144,
        "310.8.0",
        "devcamo-ada",
        &[89, 120],
    ),
    (
        "7e042cdfefe611e1aea2049e381fd5f757e15ff3953b3fecea72ab09f0d4a603",
        165_830_144,
        "310.8.0",
        "devcamo-ampere",
        &[86, 120],
    ),
    (
        "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
        165_840_496,
        "310.8.0",
        "official",
        &[120],
    ),
];

struct PinnedEntry {
    version: &'static str,
    variant: &'static str,
    url: &'static str,
    sha256: &'static str,
    size: u64,
    archs: &'static [u16],
}

// Built-in pinned table of downloadable NR model entries on RankFTW/rhi-repo with probed CUDA archs.
const PINNED_TABLE: &[PinnedEntry] = &[
    PinnedEntry {
        version: "310.9.16",
        variant: "compat-fp16",
        url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.16/nvngx_dlssnr_310.9.16.zip",
        sha256: "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed",
        size: 276_162_560,
        archs: &[75, 86, 89, 120],
    },
    PinnedEntry {
        version: "310.8.3",
        variant: "lecram-untested",
        url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.Lecram/nvngx_dlssnr_310.8.Lecram.zip",
        sha256: "f95feb54137ea11979f9b4ec4f00afd84b5c98a5624d3388fbf6a87714a39fcc",
        size: 165_840_496,
        archs: &[75, 86, 89, 120],
    },
    PinnedEntry {
        version: "310.8.2",
        variant: "SF-v2",
        url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF-v2/nvngx_dlssnr_310.8.SF-v2.zip",
        sha256: "6eb209e764f39872625debd6abaf45e2bb6322f6f270f781f70c059ae30b3927",
        size: 165_830_144,
        archs: &[75, 86, 89, 120],
    },
    PinnedEntry {
        version: "310.8.0",
        variant: "RTX40",
        url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.0-RTX40/nvngx_dlssnr_310.8.0-RTX40.zip",
        sha256: "4b8d19bc3eff58a084f5eca7489c921501c203450169fb82ff4f649a4482ba05",
        size: 165_840_496,
        archs: &[89, 120],
    },
    PinnedEntry {
        version: "310.8.0",
        variant: "official",
        url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.0/nvngx_dlssnr_310.8.0.zip",
        sha256: "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e",
        size: 165_840_496,
        archs: &[120],
    },
];

pub fn pinned_entries() -> Vec<RemoteModel> {
    PINNED_TABLE
        .iter()
        .map(|p| RemoteModel {
            version: p.version.into(),
            variant: p.variant.into(),
            label: format!("DLSS NR {} ({})", p.version, p.variant),
            url: p.url.into(),
            sha256: p.sha256.into(),
            size: p.size,
            compatibility: ModelCompatibility {
                archs: p.archs.to_vec(),
                known: true,
            },
        })
        .collect()
}

fn version_key(version: &str) -> (u32, u32, u32, u32) {
    let mut parts = version.split('.').filter_map(|p| p.parse::<u32>().ok());
    (
        parts.next().unwrap_or(0),
        parts.next().unwrap_or(0),
        parts.next().unwrap_or(0),
        parts.next().unwrap_or(0),
    )
}

/// Picks the newest known entry whose probed CUDA targets cover `gpu`.
/// Unknown entries (`known == false`) and unknown/unsupported GPUs return `None`.
pub fn pick_for_gpu<'a>(entries: &'a [RemoteModel], gpu: Option<&Gpu>) -> Option<&'a RemoteModel> {
    let gpu = gpu?;
    let (major, minor) = gpu.compute_cap?;
    let exact_sm = (major * 10 + minor) as u16;
    entries
        .iter()
        .enumerate()
        .filter(|(_, entry)| entry.compatibility.for_gpu(Some(gpu)) == Compatibility::Compatible)
        .max_by_key(|(index, entry)| {
            (
                version_key(&entry.version),
                entry.compatibility.archs.contains(&exact_sm),
                std::cmp::Reverse(*index),
            )
        })
        .map(|(_, entry)| entry)
}

pub fn pick_pinned_for_gpu(gpu: Option<&Gpu>) -> Option<RemoteModel> {
    let entries = pinned_entries();
    pick_for_gpu(&entries, gpu).cloned()
}

pub struct CommunityFetcher;

impl Fetcher for CommunityFetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
        if !allowed_url(url, false) {
            return Err(invalid("non-rhi-repo model URL"));
        }
        let asset = url.starts_with(ASSET_PREFIX);
        let agent: ureq::Agent = ureq::Agent::config_builder()
            .https_only(true)
            .max_redirects(0)
            .proxy(None)
            .timeout_global(Some(Duration::from_secs(180)))
            .user_agent("Retoque-nr-model-fetch")
            .build()
            .into();
        let mut current = url.to_owned();
        for _ in 0..5 {
            if !allowed_url(&current, asset) {
                return Err(invalid("refused model redirect"));
            }
            let mut response = agent
                .get(&current)
                .call()
                .map_err(|_| invalid("model HTTPS request failed"))?;
            if response.status().is_redirection() {
                if !asset {
                    return Err(invalid("model catalog redirect refused"));
                }
                let location = response
                    .headers()
                    .get("location")
                    .and_then(|s| s.to_str().ok())
                    .ok_or_else(|| invalid("missing model redirect"))?;
                current = location.to_owned();
                continue;
            }
            if response.status().as_u16() != 200 {
                return Err(invalid("unexpected model HTTP status"));
            }
            if response
                .headers()
                .get("content-length")
                .and_then(|v| v.to_str().ok())
                .and_then(|v| v.parse::<u64>().ok())
                .is_some_and(|n| n > limit)
            {
                return Err(invalid("model response exceeds size limit"));
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
                    return Err(invalid("model response exceeds size limit"));
                }
                out.extend_from_slice(&buf[..n]);
                progress(out.len() as u64);
            }
            return Ok(out);
        }
        Err(invalid("too many model redirects"))
    }
}

fn variant_from_url(url: &str, version: &str) -> String {
    let rest = url.strip_prefix(ASSET_PREFIX).unwrap_or_default();
    let tag = rest.split('/').next().unwrap_or("community");
    let stripped = tag.strip_prefix("dlssnr-").unwrap_or(tag);
    if stripped == version {
        "community".into()
    } else if let Some(suffix) = stripped.strip_prefix(&format!("{version}-")) {
        suffix.into()
    } else {
        stripped.into()
    }
}

/// Strictly parses `ApolloF/DLSS5-Updater-catalog` `catalog.json` bytes and merges valid `RankFTW/rhi-repo`
/// NR model entries with the built-in pinned table. Unknown entries keep `known: false` and empty `archs`
/// until scanned after download, never assumed compatible.
pub fn parse_catalog_json(bytes: &[u8]) -> io::Result<Vec<RemoteModel>> {
    if bytes.is_empty() || bytes.len() as u64 > MAX_CATALOG_BYTES {
        return Err(invalid("model catalog exceeds size limit"));
    }
    #[derive(Deserialize)]
    struct Root {
        schema: u32,
        repository: String,
        sections: Sections,
    }
    #[derive(Deserialize)]
    struct Sections {
        neural: Neural,
    }
    #[derive(Deserialize)]
    struct Neural {
        dlssnr: DlssNrSection,
    }
    #[derive(Deserialize)]
    struct DlssNrSection {
        current: RawEntry,
        #[serde(default)]
        history: Vec<RawEntry>,
    }
    #[derive(Deserialize)]
    struct RawEntry {
        name: String,
        version: String,
        #[serde(default)]
        sha256: String,
        size: u64,
        url: String,
    }

    let root: Root = serde_json::from_slice(bytes).map_err(|_| invalid("invalid model catalog metadata"))?;
    if root.schema != 1 || root.repository != "ApolloF/DLSS5-Updater-catalog" {
        return Err(invalid("invalid model catalog metadata"));
    }
    if root.sections.neural.dlssnr.history.len() > MAX_CATALOG_ENTRIES {
        return Err(invalid("too many model catalog entries"));
    }

    let mut merged = pinned_entries();
    let mut seen: HashSet<String> = merged.iter().map(|e| e.sha256.clone()).collect();

    for raw in std::iter::once(root.sections.neural.dlssnr.current).chain(root.sections.neural.dlssnr.history) {
        if raw.name != MODEL_DLL_NAME || !version_ok(&raw.version) {
            return Err(invalid("invalid model catalog metadata"));
        }
        if !valid_sha256(&raw.sha256) {
            return Err(invalid("invalid or missing model sha256"));
        }
        if raw.size == 0 || raw.size > MAX_UNPACKED_BYTES {
            return Err(invalid("model catalog exceeds size limit"));
        }
        if !allowed_asset_url(&raw.url) {
            return Err(invalid("non-rhi-repo model URL"));
        }
        let sha256 = raw.sha256.to_ascii_lowercase();
        let (variant, compatibility) = if let Some(&(_, probed_size, _, probed_variant, archs)) =
            PROBED_VARIANTS.iter().find(|(h, ..)| *h == sha256)
        {
            if raw.size != probed_size {
                return Err(invalid("pinned model metadata mismatch"));
            }
            (
                probed_variant.to_owned(),
                ModelCompatibility {
                    archs: archs.to_vec(),
                    known: true,
                },
            )
        } else {
            (
                variant_from_url(&raw.url, &raw.version),
                ModelCompatibility {
                    archs: Vec::new(),
                    known: false,
                },
            )
        };
        let entry = RemoteModel {
            label: format!("DLSS NR {} ({variant})", raw.version),
            version: raw.version,
            variant,
            url: raw.url,
            sha256: sha256.clone(),
            size: raw.size,
            compatibility,
        };
        entry.validate()?;
        if seen.insert(sha256) {
            merged.push(entry);
        }
    }
    merged.sort_by_key(|a| std::cmp::Reverse(version_key(&a.version)));
    Ok(merged)
}

pub fn refresh_catalog(fetcher: &dyn Fetcher) -> io::Result<Vec<RemoteModel>> {
    let bytes = fetcher.get(CATALOG_URL, MAX_CATALOG_BYTES, &mut |_| {})?;
    parse_catalog_json(&bytes)
}

fn safe_zip_path(path: &str) -> bool {
    let path = path.strip_suffix('/').unwrap_or(path);
    !path.is_empty()
        && path.len() <= 1024
        && !path.contains('\\')
        && path.split('/').count() <= 16
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

/// Validates every archive entry, extracts only `nvngx_dlssnr.dll`, verifies its exact byte size and SHA-256,
/// and imports it into `catalog` with `entry.label`.
pub fn import_archive(entry: &RemoteModel, archive_bytes: &[u8], catalog: &Catalog) -> io::Result<Model> {
    entry.validate()?;
    if archive_bytes.is_empty() || archive_bytes.len() as u64 > MAX_ARCHIVE_BYTES {
        return Err(invalid("model response exceeds size limit"));
    }
    let mut zip = zip::ZipArchive::new(Cursor::new(archive_bytes)).map_err(io::Error::other)?;
    if zip.is_empty() || zip.len() > MAX_ARCHIVE_ENTRIES {
        return Err(invalid("too many archive entries"));
    }
    let mut names = HashSet::new();
    let mut total_unpacked = 0u64;
    let mut dll_index = None;

    for i in 0..zip.len() {
        let file = zip.by_index(i).map_err(io::Error::other)?;
        let path = file.name();
        if !safe_zip_path(path) || !names.insert(path.trim_end_matches('/').to_ascii_lowercase()) {
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
        total_unpacked = total_unpacked
            .checked_add(file.size())
            .ok_or_else(|| invalid("archive size overflow"))?;
        if total_unpacked > MAX_UNPACKED_BYTES {
            return Err(invalid("archive exceeds unpacked size limit"));
        }
        if !file.is_dir() {
            let file_name = path.rsplit('/').next().unwrap_or(path);
            if file_name.eq_ignore_ascii_case(MODEL_DLL_NAME) {
                if file_name != MODEL_DLL_NAME {
                    return Err(invalid("unsafe or colliding archive path"));
                }
                if dll_index.replace(i).is_some() {
                    return Err(invalid("multiple nvngx_dlssnr.dll entries in archive"));
                }
            }
        }
    }

    let dll_index = dll_index.ok_or_else(|| invalid("nvngx_dlssnr.dll missing from archive"))?;

    // Stream non-DLL entries without writing them to disk so CRC/truncation/size limits are enforced.
    let mut actual_total = 0u64;
    for i in 0..zip.len() {
        if i == dll_index {
            continue;
        }
        let mut file = zip.by_index(i).map_err(io::Error::other)?;
        if file.is_dir() {
            continue;
        }
        let expected = file.size();
        let mut read_bytes = 0u64;
        let mut buf = [0u8; 65536];
        loop {
            let n = file.read(&mut buf)?;
            if n == 0 {
                break;
            }
            read_bytes += n as u64;
            actual_total += n as u64;
            if actual_total > MAX_UNPACKED_BYTES {
                return Err(invalid("archive exceeds unpacked size limit"));
            }
        }
        if read_bytes != expected {
            return Err(invalid("truncated archive entry"));
        }
    }

    let mut dll_entry = zip.by_index(dll_index).map_err(io::Error::other)?;
    if dll_entry.size() != entry.size {
        return Err(invalid("model size mismatch"));
    }

    let stage_dir = tempfile::Builder::new().prefix(".nr-fetch-").tempdir()?;
    let staged_path = stage_dir.path().join(MODEL_DLL_NAME);
    let mut staged_file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&staged_path)?;
    let mut hasher = Sha256::new();
    let mut dll_size = 0u64;
    let mut buf = [0u8; 65536];
    loop {
        let n = dll_entry.read(&mut buf)?;
        if n == 0 {
            break;
        }
        dll_size += n as u64;
        actual_total += n as u64;
        if dll_size > entry.size || actual_total > MAX_UNPACKED_BYTES {
            return Err(invalid("model size mismatch"));
        }
        hasher.update(&buf[..n]);
        staged_file.write_all(&buf[..n])?;
    }
    if dll_size != entry.size {
        return Err(invalid("truncated archive entry"));
    }
    staged_file.sync_all()?;
    drop(staged_file);

    let computed_sha256 = format!("{:x}", hasher.finalize());
    if !computed_sha256.eq_ignore_ascii_case(&entry.sha256) {
        return Err(invalid("model SHA-256 mismatch"));
    }

    catalog.import(&staged_path, Some(&entry.label))
}

pub fn download_and_import(
    fetcher: &dyn Fetcher,
    entry: &RemoteModel,
    catalog: &Catalog,
    progress: &mut dyn FnMut(u64),
) -> io::Result<Model> {
    entry.validate()?;
    let bytes = fetcher.get(&entry.url, MAX_ARCHIVE_BYTES, progress)?;
    import_archive(entry, &bytes, catalog)
}

/// Builds an in-memory `.zip` containing only `nvngx_dlssnr.dll` for offline test fixtures.
pub fn build_single_dll_zip(dll_bytes: &[u8]) -> io::Result<Vec<u8>> {
    let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
    z.start_file(MODEL_DLL_NAME, zip::write::SimpleFileOptions::default())
        .map_err(io::Error::other)?;
    z.write_all(dll_bytes)?;
    Ok(z.finish().map_err(io::Error::other)?.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use zip::write::SimpleFileOptions;

    fn sha(bytes: &[u8]) -> String {
        format!("{:x}", Sha256::digest(bytes))
    }

    fn elf_abi8(sm: u16) -> Vec<u8> {
        let mut b = vec![0; 64];
        b[..4].copy_from_slice(b"\x7fELF");
        b[4..9].copy_from_slice(&[2, 1, 1, 0x41, 8]);
        b[16..18].copy_from_slice(&2u16.to_le_bytes());
        b[18..20].copy_from_slice(&190u16.to_le_bytes());
        b[20..24].copy_from_slice(&1u32.to_le_bytes());
        let flags = u32::from(sm) << 8 | 2;
        b[48..52].copy_from_slice(&flags.to_le_bytes());
        b[52..54].copy_from_slice(&64u16.to_le_bytes());
        b
    }

    fn zip_with(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut z = zip::ZipWriter::new(Cursor::new(Vec::new()));
        for &(name, data) in entries {
            z.start_file(name, SimpleFileOptions::default()).unwrap();
            z.write_all(data).unwrap();
        }
        z.finish().unwrap().into_inner()
    }

    struct MapFetcher {
        catalog: Vec<u8>,
        archive: Vec<u8>,
    }

    impl Fetcher for MapFetcher {
        fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
            let data = if url == CATALOG_URL {
                &self.catalog
            } else {
                assert!(allowed_asset_url(url));
                &self.archive
            };
            if data.len() as u64 > limit {
                return Err(invalid("model response exceeds size limit"));
            }
            progress(data.len() as u64);
            Ok(data.clone())
        }
    }

    #[test]
    fn pick_by_arch_for_rtx_20_30_40_50_and_unknown_gpu() {
        let entries = pinned_entries();
        assert_eq!(entries.len(), 5);
        for (line, expected_sha) in [
            (
                "RTX 2080,7.5,1,8192",
                Some("cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed"),
            ),
            (
                "RTX 3060,8.6,1,12288",
                Some("cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed"),
            ),
            (
                "RTX 4090,8.9,1,24576",
                Some("cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed"),
            ),
            (
                "RTX 5090,12.0,1,32768",
                Some("cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed"),
            ),
            ("RTX 4090,N/A,1,24576", None),
        ] {
            let gpu = crate::gpu::parse_nvidia_smi_csv(line).remove(0);
            assert_eq!(
                pick_for_gpu(&entries, Some(&gpu)).map(|m| m.sha256.as_str()),
                expected_sha,
                "{line}"
            );
        }
        assert!(pick_for_gpu(&entries, None).is_none());

        // When only 310.8.0 official (sm_120) and RTX40 (sm_89/120) are present:
        let v310_8_0: Vec<_> = entries.into_iter().filter(|e| e.version == "310.8.0").collect();
        let rtx30 = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let rtx40 = crate::gpu::parse_nvidia_smi_csv("RTX 4090,8.9,1,24576").remove(0);
        let rtx50 = crate::gpu::parse_nvidia_smi_csv("RTX 5090,12.0,1,32768").remove(0);
        assert!(pick_for_gpu(&v310_8_0, Some(&rtx30)).is_none());
        assert_eq!(pick_for_gpu(&v310_8_0, Some(&rtx40)).unwrap().variant, "RTX40");
        assert!(pick_for_gpu(&v310_8_0, Some(&rtx50)).is_some());
    }

    #[test]
    fn catalog_parsing_accepts_good_and_rejects_hostile_inputs() {
        let unknown_hash = "4c5bd1171c7336b4b04fb394de51da285ab6ead6f922d7afdec163f71c319d74";
        let good = serde_json::json!({
            "schema": 1,
            "repository": "ApolloF/DLSS5-Updater-catalog",
            "sections": {
                "neural": {
                    "dlssnr": {
                        "current": {
                            "name": "nvngx_dlssnr.dll",
                            "version": "310.9.16",
                            "sha256": "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed",
                            "size": 276_162_560u64,
                            "url": "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.16/nvngx_dlssnr_310.9.16.zip"
                        },
                        "history": [
                            {
                                "name": "nvngx_dlssnr.dll",
                                "version": "310.8.1",
                                "sha256": unknown_hash,
                                "size": 165_830_144u64,
                                "url": "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.8.SF/nvngx_dlssnr_310.8.SF.zip"
                            }
                        ]
                    }
                }
            }
        })
        .to_string()
        .into_bytes();
        let parsed = parse_catalog_json(&good).unwrap();
        let added = parsed.iter().find(|e| e.sha256 == unknown_hash).unwrap();
        assert!(!added.compatibility.known);
        assert!(added.compatibility.archs.is_empty());
        let rtx30 = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        assert_eq!(added.compatibility.for_gpu(Some(&rtx30)), Compatibility::Unknown);
        assert!(pick_for_gpu(std::slice::from_ref(added), Some(&rtx30)).is_none());

        // Hostile: wrong host / repository
        for bad_url in [
            "https://evil.example/RankFTW/rhi-repo/releases/download/v1/nvngx_dlssnr.zip",
            "https://github.com/other/rhi-repo/releases/download/v1/nvngx_dlssnr.zip",
            "http://github.com/RankFTW/rhi-repo/releases/download/v1/nvngx_dlssnr.zip",
            "https://github.com/ApolloF/DLSS5-Updater-catalog/releases/download/mirror/nvngx_dlssnr.dll",
        ] {
            let hostile = serde_json::json!({
                "schema": 1,
                "repository": "ApolloF/DLSS5-Updater-catalog",
                "sections": {
                    "neural": {
                        "dlssnr": {
                            "current": {
                                "name": "nvngx_dlssnr.dll",
                                "version": "310.9.99",
                                "sha256": unknown_hash,
                                "size": 1024,
                                "url": bad_url
                            }
                        }
                    }
                }
            })
            .to_string()
            .into_bytes();
            assert!(parse_catalog_json(&hostile).is_err(), "{bad_url}");
        }

        // Hostile: missing or malformed hash
        for bad_hash in ["", "not-a-sha256", "cdfa6c1e"] {
            let hostile = serde_json::json!({
                "schema": 1,
                "repository": "ApolloF/DLSS5-Updater-catalog",
                "sections": {
                    "neural": {
                        "dlssnr": {
                            "current": {
                                "name": "nvngx_dlssnr.dll",
                                "version": "310.9.99",
                                "sha256": bad_hash,
                                "size": 1024,
                                "url": "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.99/nvngx_dlssnr_310.9.99.zip"
                            }
                        }
                    }
                }
            })
            .to_string()
            .into_bytes();
            assert!(parse_catalog_json(&hostile).is_err(), "{bad_hash}");
        }

        // Hostile: huge catalog JSON or huge entry size
        let huge_bytes = vec![b' '; (MAX_CATALOG_BYTES + 1) as usize];
        assert!(parse_catalog_json(&huge_bytes).is_err());
        let huge_entry = serde_json::json!({
            "schema": 1,
            "repository": "ApolloF/DLSS5-Updater-catalog",
            "sections": {
                "neural": {
                    "dlssnr": {
                        "current": {
                            "name": "nvngx_dlssnr.dll",
                            "version": "310.9.99",
                            "sha256": unknown_hash,
                            "size": MAX_UNPACKED_BYTES + 1,
                            "url": "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.99/nvngx_dlssnr_310.9.99.zip"
                        }
                    }
                }
            }
        })
        .to_string()
        .into_bytes();
        assert!(parse_catalog_json(&huge_entry).is_err());
    }

    #[test]
    fn download_verify_unzip_and_import_handle_bad_hash_extra_files_and_slip() {
        let mut dll_bytes = b"MZ synthetic NR model".to_vec();
        dll_bytes.extend(elf_abi8(86));
        dll_bytes.extend(elf_abi8(120));
        let dll_sha = sha(&dll_bytes);
        let entry = RemoteModel {
            version: "310.9.20".into(),
            variant: "community-test".into(),
            label: "DLSS NR 310.9.20 (community-test)".into(),
            url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.20/nvngx_dlssnr_310.9.20.zip"
                .into(),
            sha256: dll_sha.clone(),
            size: dll_bytes.len() as u64,
            compatibility: ModelCompatibility {
                archs: vec![],
                known: false,
            },
        };

        // 1. Valid archive with extra safe files: extracts ONLY nvngx_dlssnr.dll, preserves label, scans archs via model_compat, keeps known=false.
        let archive = zip_with(&[
            ("README.txt", b"community notes"),
            ("nested/extra.bin", b"ignored extra"),
            ("nvngx_dlssnr.dll", &dll_bytes),
        ]);
        let temp = tempfile::tempdir().unwrap();
        let catalog_root = temp.path().join("catalog");
        let catalog = Catalog::open(Some(&catalog_root)).unwrap();
        let fetcher = MapFetcher {
            catalog: Vec::new(),
            archive: archive.clone(),
        };
        let mut progressed = 0;
        let imported = download_and_import(&fetcher, &entry, &catalog, &mut |n| progressed = n).unwrap();
        assert_eq!(progressed, archive.len() as u64);
        assert_eq!(imported.sha256, dll_sha);
        assert_eq!(imported.label, "DLSS NR 310.9.20 (community-test)");
        assert_eq!(imported.size, dll_bytes.len() as u64);
        // Unknown entry scanned by model_compat after download shows archs [86, 120] but stays known=false (Unknown compatibility).
        assert_eq!(imported.compatibility.archs, vec![86, 120]);
        assert!(!imported.compatibility.known);
        let rtx30 = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        assert_eq!(imported.compatibility.for_gpu(Some(&rtx30)), Compatibility::Unknown);
        assert!(!catalog_root.join(&dll_sha).join("README.txt").exists());
        assert!(!catalog_root.join(&dll_sha).join("extra.bin").exists());

        // 2. Bad hash: rejected before importing into catalog.
        let empty_cat = Catalog::open(Some(&temp.path().join("bad-hash-cat"))).unwrap();
        let mut wrong_hash_entry = entry.clone();
        wrong_hash_entry.sha256 = "0".repeat(64);
        assert!(
            import_archive(&wrong_hash_entry, &archive, &empty_cat)
                .unwrap_err()
                .to_string()
                .contains("SHA-256")
        );
        assert!(empty_cat.list().unwrap().is_empty());

        // 3. Size mismatch: rejected before importing.
        let mut wrong_size_entry = entry.clone();
        wrong_size_entry.size += 1;
        assert!(
            import_archive(&wrong_size_entry, &archive, &empty_cat)
                .unwrap_err()
                .to_string()
                .contains("size")
        );
        assert!(empty_cat.list().unwrap().is_empty());

        // 4. Missing nvngx_dlssnr.dll or duplicate nvngx_dlssnr.dll: rejected.
        let only_extra = zip_with(&[("README.txt", b"no model here")]);
        assert!(import_archive(&entry, &only_extra, &empty_cat).is_err());
        let duplicate_dll = zip_with(&[("nvngx_dlssnr.dll", &dll_bytes), ("sub/nvngx_dlssnr.dll", &dll_bytes)]);
        assert!(import_archive(&entry, &duplicate_dll, &empty_cat).is_err());
        assert!(empty_cat.list().unwrap().is_empty());

        // 5. Zip-slip / unsafe paths / symlinks: rejected before touching catalog.
        for bad_path in [
            "../nvngx_dlssnr.dll",
            "../../evil.dll",
            "/abs/nvngx_dlssnr.dll",
            "C:/nvngx_dlssnr.dll",
            "sub\\nvngx_dlssnr.dll",
            "safe/NUL.txt",
            "nvngx_dlssnr.DLL",
        ] {
            let slip = zip_with(&[(bad_path, &dll_bytes), ("nvngx_dlssnr.dll", &dll_bytes)]);
            assert!(import_archive(&entry, &slip, &empty_cat).is_err(), "{bad_path}");
            assert!(empty_cat.list().unwrap().is_empty());
        }
        let mut sym_zip = zip::ZipWriter::new(Cursor::new(Vec::new()));
        sym_zip
            .add_symlink("nvngx_dlssnr.dll", "/etc/passwd", SimpleFileOptions::default())
            .unwrap();
        let sym_bytes = sym_zip.finish().unwrap().into_inner();
        assert!(import_archive(&entry, &sym_bytes, &empty_cat).is_err());
        assert!(empty_cat.list().unwrap().is_empty());
    }

    #[test]
    fn url_allowlist_restricts_hosts_and_redirects() {
        assert!(allowed_url(CATALOG_URL, false));
        assert!(!allowed_url(CATALOG_URL, true));
        for entry in pinned_entries() {
            assert!(allowed_url(&entry.url, false));
            assert!(allowed_url(&entry.url, true));
        }
        assert!(allowed_url(
            "https://release-assets.githubusercontent.com/blob?sig=1",
            true
        ));
        assert!(!allowed_url(
            "https://release-assets.githubusercontent.com/blob?sig=1",
            false
        ));
        for bad in [
            "http://raw.githubusercontent.com/ApolloF/DLSS5-Updater-catalog/main/catalog.json",
            "https://github.com/other/repo/releases/download/v1/nvngx_dlssnr.zip",
            "https://user@github.com/RankFTW/rhi-repo/releases/download/v1/nvngx_dlssnr.zip",
            "https://release-assets.githubusercontent.com:8443/blob",
            "https://evil.example/blob",
        ] {
            assert!(!allowed_url(bad, false), "{bad}");
            assert!(!allowed_url(bad, true), "{bad}");
        }
    }
}
