//! Per-user desktop app installation. Never uses game targets or the app's data directory.
use retoque_data::{app_release, payload_fetch};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs, io,
    path::{Path, PathBuf},
};

#[cfg(windows)]
#[path = "app_install_windows.rs"]
pub mod windows;

pub const EXE: &str = app_release::EXE;
pub const OLD_EXE: &str = "retoque-app.old.exe";
pub const UNINSTALL_KEY: &str = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\Retoque";
pub const ROOTS: [&str; 7] = [
    EXE,
    "OptiScaler.dll",
    "nvngx.dll_dlssnr.dll",
    "OptiScaler.ini",
    "Licenses",
    "translations",
    "OptiScaler",
];
/// The packager checks this embedded marker against the requested release version.
pub fn version() -> &'static str {
    std::hint::black_box(concat!("RETOQUE_APP_VERSION=", env!("CARGO_PKG_VERSION"), ";"))
        .strip_prefix("RETOQUE_APP_VERSION=")
        .unwrap()
        .strip_suffix(';')
        .unwrap()
}

#[derive(Clone, Debug)]
pub struct Paths {
    pub install: PathBuf,
    pub shortcuts: [PathBuf; 2],
}
impl Paths {
    pub fn new(local: &Path, programs: &Path, desktop: &Path) -> io::Result<Self> {
        if [local, programs, desktop].iter().any(|p| !p.is_absolute()) {
            return Err(io::Error::other("Windows known folder must be absolute"));
        }
        Ok(Self {
            install: local.join("Programs/Retoque"),
            shortcuts: [programs.join("Retoque.lnk"), desktop.join("Retoque.lnk")],
        })
    }
    pub fn exe(&self) -> PathBuf {
        self.install.join(EXE)
    }
    pub fn is_installed(&self, exe: &Path) -> bool {
        fs::canonicalize(exe)
            .ok()
            .zip(fs::canonicalize(self.exe()).ok())
            .is_some_and(|(a, b)| a == b)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum RegistryValue {
    Text(String),
    Dword(u32),
}

pub fn registry_values(paths: &Paths, version: &str, bytes: u64) -> BTreeMap<&'static str, RegistryValue> {
    use RegistryValue::{Dword, Text};
    BTreeMap::from([
        ("DisplayName", Text("Retoque".into())),
        ("DisplayVersion", Text(version.into())),
        ("Publisher", Text("Retoque contributors".into())),
        ("InstallLocation", Text(paths.install.to_string_lossy().into())),
        ("DisplayIcon", Text(format!("\"{}\",0", paths.exe().display()))),
        (
            "UninstallString",
            Text(format!("\"{}\" --uninstall-app", paths.exe().display())),
        ),
        ("EstimatedSize", Dword(bytes.div_ceil(1024).min(u32::MAX as u64) as u32)),
        ("NoModify", Dword(1)),
        ("NoRepair", Dword(1)),
    ])
}

/// Reject links/junctions in every existing component; no recursive operation follows them.
pub fn plain_path(path: &Path) -> io::Result<()> {
    for part in path.ancestors() {
        match fs::symlink_metadata(part) {
            Ok(meta) => {
                #[cfg(windows)]
                let reparse = {
                    use std::os::windows::fs::MetadataExt;
                    meta.file_attributes() & 0x400 != 0
                };
                #[cfg(not(windows))]
                let reparse = false;
                if meta.file_type().is_symlink() || reparse {
                    return Err(io::Error::other("linked app path refused"));
                }
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

#[derive(Clone, Debug)]
pub struct File {
    pub source: PathBuf,
    pub name: String,
    pub sha256: String,
    pub size: u64,
}
fn record(source: &Path, name: &str) -> io::Result<File> {
    plain_path(source)?;
    let meta = fs::metadata(source)?;
    if !meta.is_file() || meta.len() == 0 || meta.len() > payload_fetch::MAX_DOWNLOAD * 2 {
        return Err(io::Error::other("invalid app source file"));
    }
    let mut hash = Sha256::new();
    io::copy(&mut fs::File::open(source)?, &mut hash)?;
    Ok(File {
        source: source.into(),
        name: name.into(),
        size: meta.len(),
        sha256: format!("{:x}", hash.finalize()),
    })
}

fn collect(root: &Path, name: &str, files: &mut Vec<File>) -> io::Result<()> {
    let path = root.join(name);
    plain_path(&path)?;
    let meta = fs::metadata(&path)?;
    if files.len() >= 2048 {
        return Err(io::Error::other("too many app files"));
    }
    if meta.is_dir() {
        // Limit depth before recursion, even when a directory has no files.
        if name.split('/').count() > 6 {
            return Err(io::Error::other("app directory too deep"));
        }
        for entry in fs::read_dir(path)? {
            let entry = entry?;
            let leaf = entry
                .file_name()
                .into_string()
                .map_err(|_| io::Error::other("invalid app file name"))?;
            collect(root, &format!("{name}/{leaf}"), files)?;
        }
    } else {
        if !app_release::allowed_file(name) {
            return Err(io::Error::other("unexpected file in app bundle"));
        }
        files.push(record(&path, name)?);
    }
    Ok(())
}

#[derive(Clone, Debug)]
pub struct InstallPlan {
    pub paths: Paths,
    pub files: Vec<File>,
}
impl InstallPlan {
    pub fn prepare(paths: Paths, exe: &Path) -> io::Result<Self> {
        let source = exe.parent().ok_or_else(|| io::Error::other("missing app parent"))?;
        let mut files = vec![record(exe, EXE)?];
        for root in &ROOTS[1..] {
            if *root == "OptiScaler" && !source.join(root).try_exists()? {
                continue;
            }
            collect(source, root, &mut files)?;
        }
        if payload_fetch::REQUIRED_FILES
            .iter()
            .any(|p| !files.iter().any(|f| f.name == *p))
        {
            return Err(io::Error::other("the complete payload must be beside the app"));
        }
        if files.iter().map(|f| f.size).sum::<u64>() > payload_fetch::MAX_DOWNLOAD * 2 {
            return Err(io::Error::other("app bundle exceeds size limit"));
        }
        let plan = Self { paths, files };
        plan.check_destination()?;
        Ok(plan)
    }
    pub fn check_destination(&self) -> io::Result<()> {
        for path in std::iter::once(&self.paths.install).chain(self.paths.shortcuts.iter()) {
            plain_path(path)?;
            if path.try_exists()? {
                return Err(io::Error::new(
                    io::ErrorKind::AlreadyExists,
                    "app destination already exists",
                ));
            }
        }
        Ok(())
    }
    pub fn bytes(&self) -> u64 {
        self.files.iter().map(|f| f.size).sum()
    }
    /// Stage verified copies, then publish to a previously absent program folder.
    pub fn copy(&self) -> io::Result<()> {
        self.check_destination()?;
        let parent = self
            .paths
            .install
            .parent()
            .ok_or_else(|| io::Error::other("missing Programs folder"))?;
        fs::create_dir_all(parent)?;
        plain_path(parent)?;
        let stage = tempfile::Builder::new()
            .prefix(".retoque-install-")
            .tempdir_in(parent)?;
        for file in &self.files {
            let current = record(&file.source, &file.name)?;
            if current.sha256 != file.sha256 || current.size != file.size {
                return Err(io::Error::other("app source changed after review"));
            }
            let dest = stage.path().join(&file.name);
            fs::create_dir_all(dest.parent().unwrap())?;
            let mut out = fs::OpenOptions::new().write(true).create_new(true).open(&dest)?;
            io::copy(&mut fs::File::open(&file.source)?, &mut out)?;
            out.sync_all()?;
            if record(&dest, &file.name)?.sha256 != file.sha256 {
                return Err(io::Error::other("app copy verification failed"));
            }
        }
        self.check_destination()?;
        fs::rename(stage.path(), &self.paths.install)?;
        Ok(())
    }
}

#[derive(Clone, Debug, serde::Serialize, serde::Deserialize, PartialEq, Eq)]
pub struct Move {
    pub from: PathBuf,
    pub to: PathBuf,
}

/// All sources/destinations are relative to the fixed program folder. Old files survive until launch succeeds.
pub fn replacement_steps(install: &Path, stage: &Path) -> io::Result<Vec<Move>> {
    plain_path(install)?;
    plain_path(stage)?;
    if stage.parent() != Some(install)
        || !stage
            .file_name()
            .is_some_and(|n| n.to_string_lossy().starts_with(".retoque-update-"))
    {
        return Err(io::Error::other("update staging must be inside the installed app"));
    }
    if install.join(OLD_EXE).try_exists()? {
        return Err(io::Error::other(
            "previous app backup still exists; close other app instances and restart",
        ));
    }
    let mut steps = Vec::new();
    for root in ROOTS {
        let old = install.join(root);
        let new = stage.join("payload").join(root);
        if old.try_exists()? {
            let mut files = Vec::new();
            collect(install, root, &mut files)?;
            steps.push(Move {
                from: old.clone(),
                to: if root == EXE {
                    install.join(OLD_EXE)
                } else {
                    stage.join("backup").join(root)
                },
            });
        }
        if new.try_exists()? {
            steps.push(Move { from: new, to: old });
        }
    }
    Ok(steps)
}

/// Roll back in reverse order on an ordinary failure. A durable journal records intent before every rename.
pub fn replace(steps: &[Move], journal: &Path) -> io::Result<()> {
    let mut completed: Vec<Move> = Vec::new();
    for step in steps {
        let result = (|| {
            plain_path(&step.from)?;
            plain_path(&step.to)?;
            if step.to.try_exists()? {
                return Err(io::Error::other("app replacement destination appeared"));
            }
            fs::create_dir_all(step.to.parent().unwrap())?;
            let mut intent = completed.clone();
            intent.push(step.clone());
            let mut f = fs::File::create(journal)?;
            use io::Write;
            f.write_all(&serde_json::to_vec(&intent)?)?;
            f.sync_all()?;
            fs::rename(&step.from, &step.to)
        })();
        if let Err(error) = result {
            rollback(&completed)?;
            match fs::remove_file(journal) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                Err(e) => return Err(e),
            }
            return Err(error);
        }
        completed.push(step.clone());
    }
    Ok(())
}

pub fn rollback(completed: &[Move]) -> io::Result<()> {
    for step in completed.iter().rev() {
        plain_path(&step.from)?;
        plain_path(&step.to)?;
        if step.from.try_exists()? {
            return Err(io::Error::other("cannot roll back onto an existing app file"));
        }
        fs::rename(&step.to, &step.from)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture(root: &Path) {
        for file in payload_fetch::REQUIRED_FILES.into_iter().chain([EXE]) {
            let path = root.join(file);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, b"original").unwrap();
        }
    }
    #[test]
    fn paths_registry_and_install_review() {
        let t = tempfile::tempdir().unwrap();
        let paths = Paths::new(t.path(), &t.path().join("start"), &t.path().join("desktop")).unwrap();
        assert_eq!(paths.install, t.path().join("Programs/Retoque"));
        assert_ne!(paths.install, t.path().join("Retoque"));
        assert_eq!(version(), env!("CARGO_PKG_VERSION"));
        let values = registry_values(&paths, "1.2.3", 1025);
        assert_eq!(values["EstimatedSize"], RegistryValue::Dword(2));
        assert_eq!(values["NoRepair"], RegistryValue::Dword(1));
        assert_eq!(values["DisplayVersion"], RegistryValue::Text("1.2.3".into()));
        assert_eq!(
            values["UninstallString"],
            RegistryValue::Text(format!("\"{}\" --uninstall-app", paths.exe().display()))
        );
        let source = t.path().join("portable");
        fixture(&source);
        let plan = InstallPlan::prepare(paths.clone(), &source.join(EXE)).unwrap();
        assert!(!paths.install.exists());
        plan.copy().unwrap();
        assert!(paths.is_installed(&paths.exe()));
        assert!(plan.copy().is_err());
        assert!(!t.path().join("Retoque").exists());
    }
    #[test]
    fn stale_review_and_missing_payload_refuse_without_installing() {
        let t = tempfile::tempdir().unwrap();
        let source = t.path().join("portable");
        fixture(&source);
        let paths = Paths::new(t.path(), &t.path().join("start"), &t.path().join("desktop")).unwrap();
        let plan = InstallPlan::prepare(paths.clone(), &source.join(EXE)).unwrap();
        fs::write(source.join(EXE), b"changed").unwrap();
        assert!(plan.copy().is_err());
        assert!(!paths.install.exists());
        fs::remove_file(source.join("nvngx.dll_dlssnr.dll")).unwrap();
        assert!(InstallPlan::prepare(paths, &source.join(EXE)).is_err());
    }
    #[test]
    fn replace_order_and_rollback_preserve_both_versions() {
        let t = tempfile::tempdir().unwrap();
        fixture(t.path());
        let stage = t.path().join(".retoque-update-test");
        fixture(&stage.join("payload"));
        fs::write(stage.join("payload").join(EXE), b"new exe").unwrap();
        let steps = replacement_steps(t.path(), &stage).unwrap();
        assert_eq!(
            steps[0],
            Move {
                from: t.path().join(EXE),
                to: t.path().join(OLD_EXE)
            }
        );
        replace(&steps, &stage.join("journal.json")).unwrap();
        assert_eq!(fs::read(t.path().join(EXE)).unwrap(), b"new exe");
        assert_eq!(fs::read(t.path().join(OLD_EXE)).unwrap(), b"original");
        rollback(&steps).unwrap();
        assert_eq!(fs::read(t.path().join(EXE)).unwrap(), b"original");
        assert_eq!(fs::read(stage.join("payload").join(EXE)).unwrap(), b"new exe");
        // A missing source after the exe swap must restore the original, not leave half an update.
        fs::remove_file(stage.join("payload/OptiScaler.dll")).unwrap();
        assert!(replace(&steps, &stage.join("journal.json")).is_err());
        assert_eq!(fs::read(t.path().join(EXE)).unwrap(), b"original");
        assert!(replacement_steps(t.path(), &t.path().join("outside")).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn symlink_source_and_destination_are_refused() {
        let t = tempfile::tempdir().unwrap();
        let source = t.path().join("portable");
        fixture(&source);
        fs::remove_file(source.join("OptiScaler.dll")).unwrap();
        std::os::unix::fs::symlink(source.join(EXE), source.join("OptiScaler.dll")).unwrap();
        let paths = Paths::new(t.path(), &t.path().join("start"), &t.path().join("desktop")).unwrap();
        assert!(InstallPlan::prepare(paths, &source.join(EXE)).is_err());
    }
}

#[cfg(all(test, not(windows)))]
mod ui_tests {
    #[test]
    fn linux_preferences_have_no_app_install_or_update_controls() {
        use crate::{
            app::RetoqueApp,
            i18n::{Lang, t},
        };
        use egui_kittest::{Harness, kittest::Queryable};
        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.prefs.open = true;
            let mut harness = Harness::builder()
                .with_size([1180.0, 760.0])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.prefs_dialog(ui.ctx()), app);
            harness.run_steps(2);
            harness.get_by_label(t(lang, "prefs.title"));
            for key in [
                "app_install.install",
                "app_install.check",
                "app_install.update",
                "app_install.heading",
            ] {
                assert!(harness.query_by_label(t(lang, key)).is_none());
            }
        }
    }
}
