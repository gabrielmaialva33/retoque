//! Only the pinned official DXC asset may be fetched, after explicit consent. No network in tests.
use super::{Error, Result, check_cancel};
use sha2::{Digest, Sha256};
use std::{
    io::{Cursor, Read, Write},
    path::{Path, PathBuf},
    sync::atomic::AtomicBool,
    time::Duration,
};
pub const RELEASE: &str = "v1.8.2505.1";
pub const WINDOWS_URL: &str =
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/dxc_2025_07_14.zip";
pub const LINUX_URL: &str = "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.8.2505.1/linux_dxc_2025_07_14.x86_64.tar.gz";
pub const WINDOWS_SHA256: &str = "9ad895a6b039e3a8f8c22a1009f866800b840a74b50db9218d13319e215ea8a4";
pub const LINUX_SHA256: &str = "f2213da1fc99dc8778c8823078e16ba97c7f80f86a1d4520ab1adf4b462bc48c";
pub const MAX_DOWNLOAD: u64 = 64 * 1024 * 1024;
const MAX_LIBRARY: u64 = 64 * 1024 * 1024;
#[derive(Clone, Copy)]
pub enum Platform {
    Windows,
    Linux,
}
impl Platform {
    pub fn current() -> Self {
        if cfg!(windows) { Self::Windows } else { Self::Linux }
    }
    pub fn url(self) -> &'static str {
        match self {
            Self::Windows => WINDOWS_URL,
            Self::Linux => LINUX_URL,
        }
    }
    pub fn sha256(self) -> &'static str {
        match self {
            Self::Windows => WINDOWS_SHA256,
            Self::Linux => LINUX_SHA256,
        }
    }
    fn members(self) -> &'static [(&'static str, &'static str)] {
        match self {
            Self::Windows => &[
                ("bin/x64/dxcompiler.dll", "dxcompiler.dll"),
                ("bin/x64/dxil.dll", "dxil.dll"),
                ("LICENSE-LLVM.txt", "LICENSE-LLVM.txt"),
                ("ReleaseNotes.md", "ReleaseNotes.md"),
                ("LICENSE-MS.txt", "LICENSE-MS.txt"),
                ("LICENSE-MIT.txt", "LICENSE-MIT.txt"),
            ],
            Self::Linux => &[
                ("lib/libdxcompiler.so", "libdxcompiler.so"),
                ("lib/libdxil.so", "libdxil.so"),
                ("LICENSE-LLVM.txt", "LICENSE-LLVM.txt"),
                ("ReleaseNotes.md", "ReleaseNotes.md"),
                ("LICENSE-MS.txt", "LICENSE-MS.txt"),
            ],
        }
    }
}
/// Injected transport receives a fixed asset URL. Production limits redirects before following them.
pub trait Fetcher {
    fn fetch(&self, url: &str, out: &mut dyn Write, cancel: &AtomicBool) -> Result<()>;
}
pub struct OfficialFetcher;
fn allowed_redirect(url: &str) -> bool {
    // GitHub serves release assets through this dedicated CDN; never accept arbitrary GitHub/user URLs.
    url.starts_with("https://release-assets.githubusercontent.com/")
        && !url.split('/').nth(2).unwrap_or_default().contains(['@', ':'])
}
impl Fetcher for OfficialFetcher {
    fn fetch(&self, url: &str, out: &mut dyn Write, cancel: &AtomicBool) -> Result<()> {
        if url != WINDOWS_URL && url != LINUX_URL {
            return Err(Error::Failed("DXC URL is not pinned".into()));
        }
        let config = ureq::Agent::config_builder()
            .max_redirects(0)
            .timeout_connect(Some(Duration::from_secs(20)))
            .timeout_recv_response(Some(Duration::from_secs(60)))
            // The transfer is cancellable from the UI, so a slow line gets time instead of a hard failure.
            .timeout_global(Some(Duration::from_secs(900)))
            .build();
        let agent = ureq::Agent::new_with_config(config);
        let mut current = url.to_string();
        for redirect in 0..=2 {
            check_cancel(cancel)?;
            let mut response = agent.get(&current).call().map_err(|e| Error::Failed(e.to_string()))?;
            if response.status().is_redirection() {
                let next = response
                    .headers()
                    .get("location")
                    .and_then(|v| v.to_str().ok())
                    .ok_or_else(|| Error::Failed("DXC redirect has no location".into()))?;
                if redirect == 2 || !allowed_redirect(next) {
                    return Err(Error::Failed("DXC redirect is outside official release assets".into()));
                }
                current = next.into();
                continue;
            }
            if response.status() != 200 {
                return Err(Error::Failed("DXC download did not return HTTP 200".into()));
            }
            if response
                .headers()
                .get("content-length")
                .and_then(|v| v.to_str().ok())
                .and_then(|v| v.parse::<u64>().ok())
                .is_some_and(|n| n > MAX_DOWNLOAD)
            {
                return Err(Error::Failed("DXC download too large".into()));
            }
            let mut reader = response.body_mut().as_reader();
            let mut buf = [0; 65536];
            let mut count = 0u64;
            loop {
                check_cancel(cancel)?;
                let n = reader.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                count += n as u64;
                if count > MAX_DOWNLOAD {
                    return Err(Error::Failed("DXC download too large".into()));
                }
                out.write_all(&buf[..n])?;
            }
            return Ok(());
        }
        Err(Error::Failed("DXC redirect limit".into()))
    }
}
struct Limited {
    bytes: Vec<u8>,
}
impl Write for Limited {
    fn write(&mut self, b: &[u8]) -> std::io::Result<usize> {
        if (self.bytes.len() as u64) + (b.len() as u64) > MAX_DOWNLOAD {
            return Err(std::io::Error::other("DXC download too large"));
        }
        self.bytes.extend_from_slice(b);
        Ok(b.len())
    }
    fn flush(&mut self) -> std::io::Result<()> {
        Ok(())
    }
}
fn write_member(reader: &mut dyn Read, size: u64, out: &Path, cancel: &AtomicBool) -> Result<()> {
    if size == 0 || size > MAX_LIBRARY {
        return Err(Error::Failed("DXC member size is invalid".into()));
    }
    let mut file = std::fs::OpenOptions::new().write(true).create_new(true).open(out)?;
    let mut remaining = size;
    let mut buf = [0; 65536];
    while remaining > 0 {
        check_cancel(cancel)?;
        let n = reader.read(&mut buf[..remaining.min(65536) as usize])?;
        if n == 0 {
            return Err(Error::Failed("Truncated DXC member".into()));
        }
        file.write_all(&buf[..n])?;
        remaining -= n as u64;
    }
    file.sync_all()?;
    Ok(())
}
/// Exact member allowlist, no archive path is ever joined to the destination, no links accepted.
fn unpack(bytes: &[u8], platform: Platform, dir: &Path, cancel: &AtomicBool) -> Result<()> {
    let members = platform.members();
    let mut seen = std::collections::HashSet::new();
    match platform {
        Platform::Windows => {
            let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|e| Error::Failed(e.to_string()))?;
            if zip.len() > 256 {
                return Err(Error::Failed("Too many DXC archive members".into()));
            }
            for i in 0..zip.len() {
                let mut entry = zip.by_index(i).map_err(|e| Error::Failed(e.to_string()))?;
                if let Some((_, name)) = members.iter().find(|(source, _)| *source == entry.name()) {
                    if !seen.insert(*name)
                        || entry.is_dir()
                        || entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000)
                    {
                        return Err(Error::Failed("Invalid DXC archive entry".into()));
                    }
                    let size = entry.size();
                    write_member(&mut entry, size, &dir.join(name), cancel)?;
                }
            }
        }
        Platform::Linux => {
            let decoder = flate2::read::GzDecoder::new(Cursor::new(bytes));
            // Also bound total decompressed bytes, including ignored entries.
            let mut archive = tar::Archive::new(decoder.take(256 * 1024 * 1024));
            for (i, entry) in archive.entries()?.enumerate() {
                if i >= 256 {
                    return Err(Error::Failed("Too many DXC archive members".into()));
                }
                let mut entry = entry?;
                let path = entry.path()?;
                let path = path.strip_prefix(".").unwrap_or(path.as_ref());
                if let Some((_, name)) = members.iter().find(|(source, _)| Path::new(source) == path) {
                    if !seen.insert(*name) || !entry.header().entry_type().is_file() {
                        return Err(Error::Failed("Invalid DXC archive entry".into()));
                    }
                    let size = entry.size();
                    write_member(&mut entry, size, &dir.join(name), cancel)?;
                }
            }
        }
    }
    if seen.len() != members.len() {
        return Err(Error::Failed("DXC archive is incomplete".into()));
    }
    Ok(())
}
pub fn install(data: &Path, consent: bool, fetcher: &dyn Fetcher, cancel: &AtomicBool) -> Result<PathBuf> {
    if !consent {
        return Err(Error::MissingCompiler);
    }
    check_cancel(cancel)?;
    let platform = Platform::current();
    let mut response = Limited { bytes: Vec::new() };
    fetcher.fetch(platform.url(), &mut response, cancel)?;
    check_cancel(cancel)?;
    if format!("{:x}", Sha256::digest(&response.bytes)) != platform.sha256() {
        return Err(Error::Failed("DXC SHA-256 mismatch".into()));
    }
    let root = data.join("dxc");
    std::fs::create_dir_all(&root)?;
    let temp = tempfile::Builder::new().prefix(".download-").tempdir_in(&root)?;
    unpack(&response.bytes, platform, temp.path(), cancel)?;
    check_cancel(cancel)?;
    let target = root.join(RELEASE);
    if target.exists() {
        // Do not overwrite a potentially in-use library or follow an unexpected destination.
        if super::dxc::verified_libraries(&target) {
            return Ok(target);
        }
        return Err(Error::Failed("DXC destination already exists but is incomplete".into()));
    }
    std::fs::rename(temp.path(), &target)?;
    Ok(target)
}
#[cfg(test)]
mod tests {
    use super::*;
    struct NoNetwork;
    impl Fetcher for NoNetwork {
        fn fetch(&self, _: &str, _: &mut dyn Write, _: &AtomicBool) -> Result<()> {
            panic!("must not fetch")
        }
    }
    struct Corrupt;
    impl Fetcher for Corrupt {
        fn fetch(&self, url: &str, out: &mut dyn Write, _: &AtomicBool) -> Result<()> {
            assert_eq!(url, Platform::current().url());
            out.write_all(b"corrupt")?;
            Ok(())
        }
    }
    #[test]
    fn consent_cancel_and_digest_fail_closed() {
        let temp = tempfile::tempdir().unwrap();
        let cancel = AtomicBool::new(false);
        assert_eq!(
            install(temp.path(), false, &NoNetwork, &cancel).unwrap_err(),
            Error::MissingCompiler
        );
        assert!(
            install(temp.path(), true, &Corrupt, &cancel)
                .unwrap_err()
                .to_string()
                .contains("SHA-256")
        );
        cancel.store(true, std::sync::atomic::Ordering::Relaxed);
        assert_eq!(
            install(temp.path(), true, &NoNetwork, &cancel).unwrap_err(),
            Error::Cancelled
        );
        assert_eq!(std::fs::read_dir(temp.path()).unwrap().count(), 0);
    }
    #[test]
    fn redirect_allowlist() {
        assert!(allowed_redirect("https://release-assets.githubusercontent.com/a"));
        for u in [
            "http://release-assets.githubusercontent.com/a",
            "https://evil.test/a",
            "https://release-assets.githubusercontent.com.evil/a",
            "https://release-assets.githubusercontent.com@evil/a",
        ] {
            assert!(!allowed_redirect(u));
        }
    }
    #[test]
    fn both_archive_layouts_extract_only_expected_regular_members() {
        for platform in [Platform::Linux, Platform::Windows] {
            let members = platform.members();
            let mut buffer = Vec::new();
            match platform {
                Platform::Linux => {
                    let encoder = flate2::write::GzEncoder::new(&mut buffer, flate2::Compression::default());
                    let mut tar = tar::Builder::new(encoder);
                    for (source, _) in members {
                        let mut h = tar::Header::new_gnu();
                        h.set_mode(0o644);
                        h.set_size(4);
                        h.set_cksum();
                        tar.append_data(&mut h, format!("./{source}"), Cursor::new(b"fake"))
                            .unwrap();
                    }
                    tar.into_inner().unwrap().finish().unwrap();
                }
                Platform::Windows => {
                    let mut zip = zip::ZipWriter::new(Cursor::new(&mut buffer));
                    for (source, _) in members {
                        zip.start_file(*source, zip::write::SimpleFileOptions::default())
                            .unwrap();
                        zip.write_all(b"fake").unwrap();
                    }
                    zip.finish().unwrap();
                }
            }
            let temp = tempfile::tempdir().unwrap();
            unpack(&buffer, platform, temp.path(), &AtomicBool::new(false)).unwrap();
            assert_eq!(std::fs::read_dir(temp.path()).unwrap().count(), members.len());
            for (_, name) in members {
                assert_eq!(std::fs::read(temp.path().join(name)).unwrap(), b"fake");
            }
        }
    }
    #[test]
    fn archive_traversal_and_links_never_extracted() {
        let mut buffer = Cursor::new(Vec::new());
        {
            let mut z = zip::ZipWriter::new(&mut buffer);
            z.start_file("../escape", zip::write::SimpleFileOptions::default())
                .unwrap();
            z.write_all(b"bad").unwrap();
            z.finish().unwrap();
        }
        let temp = tempfile::tempdir().unwrap();
        assert!(
            unpack(
                buffer.get_ref(),
                Platform::Windows,
                temp.path(),
                &AtomicBool::new(false)
            )
            .is_err()
        );
        assert_eq!(std::fs::read_dir(temp.path()).unwrap().count(), 0);
    }
}
