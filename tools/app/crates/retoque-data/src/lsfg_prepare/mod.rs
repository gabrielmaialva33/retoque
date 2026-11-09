//! Local LSFG preparation, independent of game writes. Caller runs this on the heavy worker lane.
//! A completed cache only becomes a game setting via the normal reviewed settings plan.
mod cross;
pub mod download;
pub mod dxc;
mod pe;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    sync::atomic::{AtomicBool, Ordering},
};
const MAX_DLL: usize = 128 * 1024 * 1024;
const PIPELINE: &str = "lsfg-sm60-cross-0.7.2-sys-aa217ae-dxc-1.8.2505.1-v1";
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Error {
    MissingCompiler,
    Cancelled,
    Failed(String),
}
impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::MissingCompiler => write!(f, "Official DXC compiler is not installed"),
            Self::Cancelled => write!(f, "Preparation cancelled"),
            Self::Failed(s) => write!(f, "{s}"),
        }
    }
}
impl std::error::Error for Error {}
impl From<std::io::Error> for Error {
    fn from(e: std::io::Error) -> Self {
        Self::Failed(e.to_string())
    }
}
pub type Result<T> = std::result::Result<T, Error>;
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PreparedCache {
    pub directory: PathBuf,
    pub dll_sha256: String,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Progress {
    Inspecting,
    Downloading,
    Compiling { done: usize, total: usize },
    Ready,
}
#[derive(Serialize, Deserialize)]
struct Manifest {
    pipeline: String,
    dll_sha256: String,
    shaders: Vec<String>,
}
pub fn data_dir() -> Result<PathBuf> {
    #[cfg(windows)]
    let path = std::env::var_os("LOCALAPPDATA").map(|p| PathBuf::from(p).join("Retoque"));
    #[cfg(not(windows))]
    let path = std::env::var_os("XDG_DATA_HOME")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .map(|p| p.join("retoque"))
        .or_else(|| std::env::var_os("HOME").map(|p| PathBuf::from(p).join(".local/share/retoque")));
    path.filter(|p| p.is_absolute())
        .ok_or_else(|| Error::Failed("Application data directory is unavailable".into()))
}
fn check_cancel(cancel: &AtomicBool) -> Result<()> {
    if cancel.load(Ordering::Relaxed) {
        Err(Error::Cancelled)
    } else {
        Ok(())
    }
}
fn read_bounded(path: &Path, max: usize) -> Result<Vec<u8>> {
    let (file, meta) = retoque_discovery::io::open_regular_file(path).map_err(Error::Failed)?;
    if meta.len() > max as u64 {
        return Err(Error::Failed("Input exceeds size limit".into()));
    }
    let mut b = Vec::new();
    file.take(max as u64 + 1).read_to_end(&mut b)?;
    if b.len() > max {
        return Err(Error::Failed("Input exceeds size limit".into()));
    }
    Ok(b)
}
fn digest(b: &[u8]) -> String {
    format!("{:x}", Sha256::digest(b))
}
fn verified_cache(path: &Path, hash: &str) -> bool {
    if !fs::symlink_metadata(path).is_ok_and(|m| m.is_dir()) {
        return false;
    }
    let Some(manifest) = read_bounded(&path.join("manifest.json"), 32 * 1024)
        .ok()
        .and_then(|b| serde_json::from_slice::<Manifest>(&b).ok())
    else {
        return false;
    };
    manifest.pipeline == PIPELINE
        && manifest.dll_sha256 == hash
        && manifest.shaders.len() == 99
        && manifest.shaders.iter().enumerate().all(|(rid, hash)| {
            read_bounded(&path.join(format!("{rid}.cso")), 4 * 1024 * 1024)
                .is_ok_and(|b| b.len() >= 32 && b.starts_with(b"DXBC") && digest(&b) == *hash)
        })
}
/// Returns a verified app cache without modifying anything. Does not imply the game's ShaderDir points at it.
pub fn prepared_cache(lossless: &Path, data: &Path) -> Result<Option<PreparedCache>> {
    let hash = digest(&read_bounded(&lossless.join("lsfg-vk.dll"), MAX_DLL)?);
    let directory = data.join("lsfg-cache").join(&hash);
    Ok(verified_cache(&directory, &hash).then_some(PreparedCache {
        directory,
        dll_sha256: hash,
    }))
}
/// Idempotent per DLL content hash. Consent authorizes ONLY the pinned compiler download, never game writes.
/// Cancellation is checked between stages and shaders; an in-flight compiler call must finish before stopping.
pub fn ensure_lsfg_shaders(
    lossless: &Path,
    data: &Path,
    executable_dir: &Path,
    download_consent: bool,
    cancel: &AtomicBool,
    progress: &mut dyn FnMut(Progress),
) -> Result<PreparedCache> {
    ensure_with_fetcher(
        lossless,
        data,
        executable_dir,
        download_consent,
        &download::OfficialFetcher,
        cancel,
        progress,
    )
}
pub fn ensure_with_fetcher(
    lossless: &Path,
    data: &Path,
    executable_dir: &Path,
    download_consent: bool,
    fetcher: &dyn download::Fetcher,
    cancel: &AtomicBool,
    progress: &mut dyn FnMut(Progress),
) -> Result<PreparedCache> {
    check_cancel(cancel)?;
    progress(Progress::Inspecting);
    if !data.is_absolute() {
        return Err(Error::Failed("App data path must be absolute".into()));
    }
    let dll = read_bounded(&lossless.join("lsfg-vk.dll"), MAX_DLL)?;
    let hash = digest(&dll);
    let directory = data.join("lsfg-cache").join(&hash);
    if verified_cache(&directory, &hash) {
        check_cancel(cancel)?;
        progress(Progress::Ready);
        return Ok(PreparedCache {
            directory,
            dll_sha256: hash,
        });
    }
    let modules = pe::extract(&dll)?;
    check_cancel(cancel)?;
    let compiler_dir = match dxc::find(executable_dir, data) {
        Some(path) => path,
        None if download_consent => {
            progress(Progress::Downloading);
            download::install(data, true, fetcher, cancel)?
        }
        None => return Err(Error::MissingCompiler),
    };
    let compiler = dxc::Dxc::load(&compiler_dir)?;
    publish_cache(data, hash, &modules, cancel, progress, &mut |module| {
        let (hlsl, bindings) = cross::hlsl(module)?;
        Ok((compiler.compile(&hlsl)?, bindings))
    })
}
type Converted = (Vec<u8>, std::collections::BTreeMap<u32, String>);
fn publish_cache(
    data: &Path,
    hash: String,
    modules: &[Vec<u32>],
    cancel: &AtomicBool,
    progress: &mut dyn FnMut(Progress),
    convert: &mut dyn FnMut(&[u32]) -> Result<Converted>,
) -> Result<PreparedCache> {
    if modules.len() != 99 {
        return Err(Error::Failed("Expected exactly 99 shaders".into()));
    }
    let directory = data.join("lsfg-cache").join(&hash);
    let root = data.join("lsfg-cache");
    fs::create_dir_all(&root)?;
    let temp = tempfile::Builder::new().prefix(".prepare-").tempdir_in(&root)?;
    let mut shaders = Vec::with_capacity(99);
    let mut layout = std::collections::BTreeMap::new();
    for (rid, module) in modules.iter().enumerate() {
        check_cancel(cancel)?;
        progress(Progress::Compiling { done: rid, total: 99 });
        let (bytes, bindings) = convert(module).map_err(|e| Error::Failed(format!("Shader {rid}: {e}")))?;
        if bytes.len() < 32 || bytes.len() > 4 * 1024 * 1024 || !bytes.starts_with(b"DXBC") {
            return Err(Error::Failed(format!("Shader {rid}: invalid DXIL container")));
        }
        shaders.push(digest(&bytes));
        layout.insert(rid.to_string(), bindings);
        fs::write(temp.path().join(format!("{rid}.cso")), bytes)?;
    }
    check_cancel(cancel)?;
    let manifest = Manifest {
        pipeline: PIPELINE.into(),
        dll_sha256: hash.clone(),
        shaders,
    };
    fs::write(
        temp.path().join("manifest.json"),
        serde_json::to_vec(&manifest).map_err(|e| Error::Failed(e.to_string()))?,
    )?;
    fs::write(
        temp.path().join("layout.json"),
        serde_json::to_vec(&layout).map_err(|e| Error::Failed(e.to_string()))?,
    )?;
    check_cancel(cancel)?;
    // Never replace a directory a running game might use. A bad existing cache is a visible error.
    if directory.exists() {
        if !verified_cache(&directory, &hash) {
            return Err(Error::Failed(
                "Existing shader cache failed verification; choose a fresh app data cache".into(),
            ));
        }
    } else if let Err(e) = fs::rename(temp.path(), &directory)
        && !verified_cache(&directory, &hash)
    {
        return Err(e.into());
    }
    progress(Progress::Ready);
    Ok(PreparedCache {
        directory,
        dll_sha256: hash,
    })
}
/// Settings proposal only. Caller must review/apply with retoque_core::plan_settings.
pub fn shader_dir_edit(cache: &PreparedCache, wine: bool) -> (String, String, String) {
    let path = cache.directory.to_string_lossy();
    let value = if wine && path.starts_with('/') {
        format!("Z:{}", path.replace('/', "\\"))
    } else {
        path.into_owned()
    };
    ("LSFG".into(), "ShaderDir".into(), value)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn cache_requires_manifest_hashes_and_all_99_modules() {
        let temp = tempfile::tempdir().unwrap();
        let p = temp.path();
        let b = b"DXBC synthetic shader container padding";
        let manifest = Manifest {
            pipeline: PIPELINE.into(),
            dll_sha256: "abc".into(),
            shaders: vec![digest(b); 99],
        };
        for rid in 0..99 {
            fs::write(p.join(format!("{rid}.cso")), b).unwrap();
        }
        assert!(!verified_cache(p, "abc"));
        fs::write(p.join("manifest.json"), serde_json::to_vec(&manifest).unwrap()).unwrap();
        assert!(verified_cache(p, "abc"));
        assert!(!verified_cache(p, "different"));
        fs::write(p.join("98.cso"), b"DXBC corrupt").unwrap();
        assert!(!verified_cache(p, "abc"));
    }
    #[test]
    fn publication_is_atomic_and_cancel_or_failure_cleans_partial_output() {
        let temp = tempfile::tempdir().unwrap();
        let cancel = AtomicBool::new(false);
        let modules = vec![vec![0]; 99];
        let bytes = b"DXBC synthetic shader container padding".to_vec();
        let mut count = 0;
        let failed = publish_cache(temp.path(), "hash".into(), &modules, &cancel, &mut |_| {}, &mut |_| {
            count += 1;
            if count == 42 {
                return Err(Error::Failed("injected compile failure".into()));
            }
            assert!(!temp.path().join("lsfg-cache/hash").exists());
            Ok((bytes.clone(), Default::default()))
        });
        assert!(failed.unwrap_err().to_string().contains("injected compile failure"));
        assert_eq!(fs::read_dir(temp.path().join("lsfg-cache")).unwrap().count(), 0);
        let cancelled = publish_cache(
            temp.path(),
            "hash".into(),
            &modules,
            &cancel,
            &mut |p| {
                if p == (Progress::Compiling { done: 10, total: 99 }) {
                    cancel.store(true, Ordering::Relaxed);
                }
            },
            &mut |_| Ok((bytes.clone(), Default::default())),
        );
        assert_eq!(cancelled.unwrap_err(), Error::Cancelled);
        assert_eq!(fs::read_dir(temp.path().join("lsfg-cache")).unwrap().count(), 0);
        cancel.store(false, Ordering::Relaxed);
        let ready = publish_cache(temp.path(), "hash".into(), &modules, &cancel, &mut |_| {}, &mut |_| {
            Ok((bytes.clone(), Default::default()))
        })
        .unwrap();
        assert!(verified_cache(&ready.directory, "hash"));
        assert_eq!(fs::read_dir(temp.path().join("lsfg-cache")).unwrap().count(), 1);
    }
    #[test]
    fn verified_cache_reuse_does_not_need_compiler_or_network() {
        let temp = tempfile::tempdir().unwrap();
        let ls = temp.path().join("LS");
        fs::create_dir(&ls).unwrap();
        fs::write(ls.join("lsfg-vk.dll"), b"synthetic source").unwrap();
        let hash = digest(b"synthetic source");
        let cancel = AtomicBool::new(false);
        let ready = publish_cache(temp.path(), hash, &vec![vec![0]; 99], &cancel, &mut |_| {}, &mut |_| {
            Ok((b"DXBC synthetic shader container padding".to_vec(), Default::default()))
        })
        .unwrap();
        let again = ensure_lsfg_shaders(
            &ls,
            temp.path(),
            &temp.path().join("no-compiler"),
            false,
            &cancel,
            &mut |_| {},
        )
        .unwrap();
        assert_eq!(ready, again);
        assert_eq!(fs::read_dir(&ls).unwrap().count(), 1);
    }
    #[test]
    fn shader_setting_is_proposal_and_wine_mapped() {
        let c = PreparedCache {
            directory: PathBuf::from("/synthetic/cache"),
            dll_sha256: "a".into(),
        };
        assert_eq!(shader_dir_edit(&c, true).2, "Z:\\synthetic\\cache");
    }
}
