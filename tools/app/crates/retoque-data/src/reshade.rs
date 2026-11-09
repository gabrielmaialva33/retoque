//! Official opt-in ReShade provider. Only the x64 DLL and upstream legal text enter the cache.
use crate::components::{Component, ComponentKind, HeldDir, open_regular};
use cap_fs_ext::DirExt;
use cap_std::fs::OpenOptions;
use retoque_core::reshade::{Build, build};
use sha2::{Digest, Sha256};
use std::{
    fs,
    io::{self, Cursor, Read, Write},
    path::{Path, PathBuf},
};

pub const URL: &str = "https://reshade.me/downloads/ReShade_Setup_6.8.0_Addon.exe";
pub const SETUP_SHA: &str = "afe4c8f13048306307983b8b3d41d5bf00a86820440b0e57dea10950e1176445";
pub const DLL_SHA: &str = "0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7";
pub const SETUP_SIZE: u64 = 4_318_424;
pub const DLL_SIZE: u64 = 5_592_064;
pub const MAX_INPUT: usize = 128 * 1024 * 1024;
pub const OFFICIAL_LABEL: &str = "ReShade 6.8.0 (with add-on support), official, reshade.me";
// https://github.com/crosire/reshade/blob/v6.8.0/LICENSE.md (BSD-3-Clause).
pub const LICENSE: &str = r#"Copyright 2014 Patrick Mours. All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

  * Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
  * Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"#;
fn invalid(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, s)
}
pub fn verify(bytes: &[u8], size: u64, sha: &str) -> io::Result<()> {
    if bytes.len() as u64 != size || format!("{:x}", Sha256::digest(bytes)) != sha {
        return Err(invalid("ReShade size or SHA-256 mismatch"));
    }
    Ok(())
}
pub struct OfficialFetcher;
impl crate::sm86::Fetcher for OfficialFetcher {
    fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> io::Result<Vec<u8>> {
        crate::fetch::get(url, limit, progress, |u, _| u == URL)
    }
}
pub fn extract(bytes: &[u8]) -> io::Result<Vec<u8>> {
    if bytes.len() > MAX_INPUT {
        return Err(invalid("ReShade input exceeds size cap"));
    }
    if !bytes.starts_with(b"MZ") {
        return Err(invalid("ReShade input is not a PE executable"));
    }
    let mut zip = zip::ZipArchive::new(Cursor::new(bytes)).map_err(|_| invalid("ReShade setup has no appended ZIP"))?;
    if zip.len() > 64 {
        return Err(invalid("ReShade ZIP has too many entries"));
    }
    let mut selected = None;
    for index in 0..zip.len() {
        let entry = zip.by_index(index).map_err(io::Error::other)?;
        if entry.name().eq_ignore_ascii_case("ReShade64.dll") {
            if selected.is_some()
                || entry.name() != "ReShade64.dll"
                || entry.is_dir()
                || entry.unix_mode().is_some_and(|m| m & 0o170000 == 0o120000)
            {
                return Err(invalid("ambiguous or linked ReShade64.dll"));
            }
            selected = Some(index);
        }
    }
    let entry = zip
        .by_index(selected.ok_or_else(|| invalid("ReShade64.dll missing from setup"))?)
        .map_err(io::Error::other)?;
    if entry.size() > MAX_INPUT as u64 {
        return Err(invalid("ReShade DLL exceeds size cap"));
    }
    let mut dll = Vec::new();
    entry.take(MAX_INPUT as u64 + 1).read_to_end(&mut dll)?;
    if dll.len() > MAX_INPUT || !retoque_core::reshade::is_x64_dll(&dll) {
        return Err(invalid("invalid ReShade DLL"));
    }
    Ok(dll)
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cached {
    pub component: Component,
    pub build: Build,
}
impl Cached {
    pub fn dll(&self) -> PathBuf {
        self.component.root.join("ReShade64.dll")
    }
}
fn id(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn describe(root: PathBuf, bytes: &[u8]) -> Cached {
    let hash = id(bytes);
    let version = retoque_core::pe_version::read_version(&mut Cursor::new(bytes));
    let label = if hash == DLL_SHA {
        OFFICIAL_LABEL.into()
    } else {
        format!("ReShade {} (local)", version.as_deref().unwrap_or("?"))
    };
    Cached {
        build: build(bytes),
        component: Component {
            id: hash,
            kind: ComponentKind::ReShade,
            version,
            label,
            root,
            files: vec!["ReShade64.dll".into()],
        },
    }
}
fn read_at(dir: &cap_std::fs::Dir, name: &str, limit: usize) -> io::Result<Vec<u8>> {
    let mut bytes = Vec::new();
    open_regular(dir, name)?
        .take(limit as u64 + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() > limit {
        return Err(invalid("ReShade file exceeds size cap"));
    }
    Ok(bytes)
}
pub fn cached(data: &Path, sha: &str) -> io::Result<Option<Cached>> {
    if sha.len() != 64 || !sha.bytes().all(|c| c.is_ascii_hexdigit()) {
        return Err(invalid("invalid ReShade catalog id"));
    }
    let root = data.join("reshade");
    if !root.try_exists()? {
        return Ok(None);
    }
    let parent = HeldDir::open(&root)?;
    if !parent.dir.try_exists(sha)? {
        return Ok(None);
    }
    let dir = parent.dir.open_dir_nofollow(sha)?;
    let bytes = read_at(&dir, "ReShade64.dll", MAX_INPUT)?;
    if id(&bytes) != sha || read_at(&dir, "LICENSE.txt", 16384)? != LICENSE.as_bytes() {
        return Err(invalid("ReShade catalog integrity failure"));
    }
    parent.current()?;
    Ok(Some(describe(root.join(sha), &bytes)))
}
fn store(data: &Path, bytes: &[u8]) -> io::Result<Cached> {
    if bytes.len() > MAX_INPUT
        || !retoque_core::reshade::is_x64_dll(bytes)
        || (build(bytes) == Build::Unknown && !bytes.windows(7).any(|w| w == b"ReShade"))
    {
        return Err(invalid(
            "unrecognized ReShade DLL; require an x64 DLL with ReShade identification",
        ));
    }
    let sha = id(bytes);
    if let Some(c) = cached(data, &sha)? {
        return Ok(c);
    }
    fs::create_dir_all(data)?;
    let data = HeldDir::open(data)?;
    match data.dir.create_dir("reshade") {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::AlreadyExists => {}
        Err(e) => return Err(e),
    }
    let parent = data.dir.open_dir_nofollow("reshade")?;
    // Reuse the catalog's capability-relative no-follow file access and exclusive publication.
    static NEXT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let staging = format!(
        ".import-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    );
    parent.create_dir(&staging)?;
    let result = (|| {
        let dir = parent.open_dir_nofollow(&staging)?;
        for (name, content) in [("ReShade64.dll", bytes), ("LICENSE.txt", LICENSE.as_bytes())] {
            let mut file = dir.open_with(name, OpenOptions::new().create_new(true).write(true))?;
            file.write_all(content)?;
            file.sync_all()?;
        }
        drop(dir);
        data.current()?;
        if !parent.try_exists(&sha)? {
            parent.rename(&staging, &parent, &sha)?;
        }
        cached(&data.root, &sha)?.ok_or_else(|| invalid("ReShade cache disappeared"))
    })();
    let _ = parent.remove_dir_all(&staging);
    result
}
pub fn import(data: &Path, source: &Path) -> io::Result<Cached> {
    let root = source
        .parent()
        .filter(|p| !p.as_os_str().is_empty())
        .unwrap_or(Path::new("."));
    let dir = HeldDir::open(root)?;
    let name = source
        .file_name()
        .and_then(|s| s.to_str())
        .ok_or_else(|| invalid("invalid ReShade filename"))?;
    let bytes = read_at(&dir.dir, name, MAX_INPUT)?;
    dir.current()?;
    let dll = if name.to_ascii_lowercase().ends_with(".exe") {
        extract(&bytes)?
    } else {
        bytes
    };
    store(data, &dll)
}
pub fn download(
    data: &Path,
    consent: bool,
    fetcher: &dyn crate::sm86::Fetcher,
    progress: &mut dyn FnMut(u64),
) -> io::Result<Cached> {
    if !consent {
        return Err(invalid("ReShade download requires confirmation"));
    }
    if let Some(c) = cached(data, DLL_SHA)? {
        return Ok(c);
    }
    let bytes = fetcher.get(URL, SETUP_SIZE, progress)?;
    verify(&bytes, SETUP_SIZE, SETUP_SHA)?;
    let dll = extract(&bytes)?;
    verify(&dll, DLL_SIZE, DLL_SHA)?;
    store(data, &dll)
}

#[derive(Clone, Debug)]
pub struct Snapshot {
    pub addons: Vec<retoque_core::reshade::Addon>,
    pub build: Option<Build>,
    pub managed: bool,
    pub nr_enabled: bool,
    pub yield_to_addons: Option<String>,
    pub load_enabled: bool,
    /// ReShade is here and its overlay takes the mouse from the Retoque menu while open (InputProcessing 2/absent).
    pub menu_input_blocked: bool,
}
impl Snapshot {
    pub fn owner(&self) -> retoque_core::reshade::NrOwner {
        if self.build == Some(Build::Full) && self.load_enabled {
            retoque_core::reshade::nr_owner(self.nr_enabled, self.yield_to_addons.as_deref(), &self.addons)
        } else {
            retoque_core::reshade::nr_owner(self.nr_enabled, Some("false"), &self.addons)
        }
    }
}
pub fn inspect(game: &Path) -> Result<Snapshot, retoque_core::Error> {
    let ini = retoque_core::read_ini(game)?
        .map(|(_, b)| retoque_core::ini::Ini::parse(&b))
        .unwrap_or_default();
    let on = |s: &str, k: &str| {
        ini.get(s, k)
            .is_some_and(|v| v.eq_ignore_ascii_case("true") || v == "1")
    };
    let build = retoque_core::reshade::installed_build(game)?;
    Ok(Snapshot {
        addons: retoque_core::reshade::addons(game)?,
        build,
        managed: retoque_core::reshade::managed(game)?,
        nr_enabled: on("DlssNr", "Enabled"),
        yield_to_addons: ini.get("DlssNr", "YieldToAddons").map(str::to_owned),
        load_enabled: on("Plugins", "LoadReshade"),
        menu_input_blocked: build.is_some() && retoque_core::reshade::menu_input_blocked_in(game)?,
    })
}
pub fn recommendation(snapshot: &Snapshot) -> Option<crate::recommend::Recommendation> {
    use crate::recommend::*;
    if snapshot.addons.is_empty() || snapshot.build == Some(Build::Full) {
        return None;
    }
    Some(Recommendation {
        id: "reshade_addons",
        title_key: "rs.recommend",
        why_key: "rs.recommend_why",
        actions: vec![Action::InstallComponent(ComponentKind::ReShade)],
        needs: Some(Need::Component(ComponentKind::ReShade)),
        evidence: vec![
            FactEvidence::new(
                "rs.addons",
                snapshot
                    .addons
                    .iter()
                    .map(|a| a.file.as_str())
                    .collect::<Vec<_>>()
                    .join(", "),
            ),
            FactEvidence::new("rs.build", format!("{:?}", snapshot.build)),
        ],
        confidence: Confidence::StaticMatchUntested,
        caveat_key: Some("rs.static"),
    })
}
