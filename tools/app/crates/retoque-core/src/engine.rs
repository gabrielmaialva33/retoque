#[path = "components.rs"]
pub(crate) mod components;
#[path = "dlss.rs"]
pub(crate) mod dlss;
#[path = "reshade.rs"]
pub mod reshade;

use crate::{
    ini::Ini,
    receipt::{Receipt, ReceiptStatus, Record, safe_rel},
    *,
};
use chrono::Utc;
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::{Read, Write},
};

const MARK_OPTI: &[u8] = b"OptiScaler working as";
const MARK_FORWARDER: &[u8] = b"dlssnr_call_";
const RECEIPT: &str = ".retoque/receipt.txt";
const PENDING: &str = ".retoque/pending-receipt.txt";

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Execution {
    operation: Operation,
    pub(crate) settings_undo: Vec<ini::IniUndo>,
    exact_undo: Vec<ini::IniUndo>,
    // Lifecycle INI edits preserve user ownership and optionally replay exact undo.
    user_ini: BTreeMap<usize, Option<ini::IniUndo>>,
    lifecycle_removal: bool,
    component_only: bool,
    process_roots: Vec<PathBuf>,
    component_completion: Option<ReceiptStatus>,
    proxy: Option<String>,
    forget: BTreeMap<usize, Record>,
    snapshots: BTreeMap<PathBuf, Fingerprint>,
    observed_files: BTreeMap<PathBuf, Option<(PathBuf, Fingerprint)>>,
    refusals: Vec<Refusal>,
    sealed: bool,
    steps: Vec<Step>,
    game_dir: PathBuf,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
enum Operation {
    #[default]
    Install,
    Uninstall,
    Settings,
}

#[derive(Clone, Debug, PartialEq, Eq)]
enum Fingerprint {
    Missing,
    File(String, u32),
    Link(PathBuf),
    Directory(Vec<String>),
    Other,
}
fn io(path: &Path, source: std::io::Error) -> Error {
    Error::Io {
        path: path.to_owned(),
        source,
    }
}
fn exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}
fn linked(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|m| {
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            m.file_attributes() & 0x400 != 0
        }
        #[cfg(not(windows))]
        {
            m.file_type().is_symlink()
        }
    })
}
fn regular(path: &Path) -> bool {
    !linked(path) && path.is_file()
}
fn read(path: &Path) -> Result<Vec<u8>, Error> {
    fs::read(path).map_err(|e| io(path, e))
}
fn hash(path: &Path) -> Result<String, Error> {
    let mut file = fs::File::open(path).map_err(|e| io(path, e))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 65536];
    loop {
        let n = file.read(&mut buffer).map_err(|e| io(path, e))?;
        if n == 0 {
            break;
        }
        digest.update(&buffer[..n]);
    }
    Ok(format!("{:x}", digest.finalize()))
}
fn sha_or_dash(path: &Path) -> String {
    hash(path).unwrap_or_else(|_| "-".into())
}
fn marker(path: &Path, needle: &[u8]) -> bool {
    read(path).is_ok_and(|b| b.windows(needle.len()).any(|w| w == needle))
}
fn fingerprint(path: &Path) -> Result<Fingerprint, Error> {
    let meta = match fs::symlink_metadata(path) {
        Ok(m) => m,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(Fingerprint::Missing),
        Err(e) => return Err(io(path, e)),
    };
    if linked(path) {
        return Ok(Fingerprint::Link(fs::read_link(path).map_err(|e| io(path, e))?));
    }
    if meta.is_file() {
        #[cfg(unix)]
        let mode = {
            use std::os::unix::fs::PermissionsExt;
            meta.permissions().mode()
        };
        #[cfg(not(unix))]
        let mode = u32::from(meta.permissions().readonly());
        return Ok(Fingerprint::File(hash(path)?, mode));
    }
    if meta.is_dir() {
        let mut names = fs::read_dir(path)
            .map_err(|e| io(path, e))?
            .map(|e| e.map(|e| e.file_name().to_string_lossy().into_owned()))
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| io(path, e))?;
        names.sort();
        return Ok(Fingerprint::Directory(names));
    }
    Ok(Fingerprint::Other)
}
fn foreign(path: impl Into<String>) -> Refusal {
    Refusal::Foreign {
        path: path.into(),
        what: Foreign::Unknown,
    }
}
fn check_parents(game: &Path, rel: &str) -> Result<(), Error> {
    check_game_directory(game)?;
    if !safe_rel(rel) {
        return Err(Error::Refused(vec![Refusal::Receipt(format!("unsafe path: {rel}"))]));
    }
    let mut parent = Path::new(rel).parent();
    while let Some(p) = parent {
        if p.as_os_str().is_empty() {
            break;
        }
        let full = game.join(p);
        if linked(&full) || (exists(&full) && !full.is_dir()) {
            return Err(Error::Refused(vec![foreign(format!(
                "{}: receipt parent is a link or not a directory",
                p.display()
            ))]));
        }
        parent = p.parent();
    }
    Ok(())
}
fn read_receipt_file(game: &Path, rel: &str) -> Result<Option<Receipt>, Error> {
    check_parents(game, rel)?;
    let path = game.join(rel);
    if !exists(&path) {
        return Ok(None);
    }
    if !regular(&path) {
        return Err(Error::Refused(vec![foreign(format!("{rel} is not a regular file"))]));
    }
    let text = fs::read_to_string(&path).map_err(|e| io(&path, e))?;
    let receipt = Receipt::parse(&text).map_err(|s| Error::Refused(vec![Refusal::Receipt(s)]))?;
    for record in &receipt.records {
        for path in record.paths() {
            check_parents(game, path)?;
        }
    }
    Ok(Some(receipt))
}
fn load_receipt(game: &Path) -> Result<Option<Receipt>, Error> {
    let before = read_receipt_file(game, RECEIPT)?;
    let Some(mut pending) = read_receipt_file(game, PENDING)? else {
        return Ok(before);
    };
    let Some(before) = before else {
        return Err(Error::Refused(vec![Refusal::Receipt(
            "pending mutation without its base receipt; preserve state for recovery".into(),
        )]));
    };
    if pending.status != ReceiptStatus::Partial {
        return Err(Error::Refused(vec![Refusal::Receipt(
            "invalid pending install receipt status".into(),
        )]));
    }
    // A process may stop on either side of the atomic replacement. Keep previous ownership when the
    // old bytes remain; otherwise the intended hash protects completed writes and preserves user edits.
    for record in &mut pending.records {
        if let Record::File { path, sha256 } = record {
            let file = game.join(&*path);
            if let Some(Record::File { sha256: old, .. }) = before
                .records
                .iter()
                .find(|r| matches!(r, Record::File { path: p, .. } if p == path))
                && regular(&file)
                && hash(&file)? == *old
            {
                *sha256 = old.clone();
            }
        }
    }
    Ok(Some(pending))
}
fn empty_receipt() -> Receipt {
    Receipt {
        status: ReceiptStatus::Partial,
        updated: String::new(),
        records: Vec::new(),
    }
}
fn owned(receipt: &Receipt, rel: &str, sha: &str) -> bool {
    receipt
        .records
        .iter()
        .any(|r| matches!(r, Record::File { path, sha256 } if path == rel && sha256 == sha))
}
fn is_model(rel: &str) -> bool {
    let low = rel.to_ascii_lowercase();
    let base = low.rsplit('/').next().unwrap_or("");
    (base.starts_with("nvngx_dlssnr") && base.ends_with(".dll")) || low.split('/').any(|part| part == "dlssnrmodels")
}
fn variants(game: &Path, rel: &str) -> Vec<String> {
    let mut candidates = vec![String::new()];
    for part in rel.split('/') {
        let mut next = Vec::new();
        for base in &candidates {
            if let Ok(entries) = fs::read_dir(game.join(base)) {
                for entry in entries.flatten() {
                    let name = entry.file_name().to_string_lossy().into_owned();
                    if name.eq_ignore_ascii_case(part) {
                        next.push(if base.is_empty() {
                            name
                        } else {
                            format!("{base}/{name}")
                        });
                    }
                }
            }
        }
        candidates = next;
    }
    candidates.sort();
    candidates
}
fn canonical(game: &Path) -> Result<PathBuf, Error> {
    if !game.is_dir() {
        return Err(Error::Other(format!(
            "Target directory does not exist: {}",
            game.display()
        )));
    }
    // Match Bash's cd -P: the selected alias and its ancestors are trusted. Pin the resolved
    // destination once; only that root and paths beneath it belong to our link-safety boundary.
    let resolved = game.canonicalize().map_err(|e| io(game, e))?;
    check_game_directory(&resolved)?;
    Ok(resolved)
}
fn new_plan(game: &Path, operation: Operation) -> Result<Plan, Error> {
    let game = canonical(game)?;
    Ok(Plan {
        game_dir: game.clone(),
        steps: Vec::new(),
        refusals: Vec::new(),
        notes: Vec::new(),
        execution: Execution {
            operation,
            game_dir: game,
            ..Default::default()
        },
    })
}
fn backup_dir(game: &Path) -> String {
    let base = format!(".retoque/backup/{}", Utc::now().format("%Y%m%dT%H%M%SZ"));
    let mut result = base.clone();
    let mut count = 1;
    while exists(&game.join(&result)) {
        count += 1;
        result = format!("{base}-{count}");
    }
    result
}
fn step_paths(step: &Step) -> Vec<&str> {
    match step {
        Step::Copy { to, .. } => vec![to],
        Step::Backup { path, copy } => vec![path, copy],
        Step::Rename { from, to } => vec![from, to],
        Step::SetIni { file, .. } | Step::UnsetIni { file, .. } => vec![file],
        Step::Remove { path } | Step::Keep { path, .. } => vec![path],
        Step::Restore { copy, to } => vec![copy, to],
    }
}
fn seal(mut plan: Plan, receipt: &Receipt) -> Result<Plan, Error> {
    let mut paths = if plan.execution.operation == Operation::Settings {
        Vec::new()
    } else {
        vec![RECEIPT.to_string(), PENDING.to_string()]
    };
    for s in &plan.steps {
        paths.extend(step_paths(s).into_iter().map(String::from));
    }
    for r in &receipt.records {
        paths.extend(r.paths().into_iter().map(String::from));
    }
    for path in paths {
        check_parents(&plan.game_dir, &path)?;
        let mut current = plan.game_dir.join(path);
        loop {
            let f = fingerprint(&current)?;
            plan.execution.snapshots.insert(current.clone(), f);
            if current == plan.game_dir {
                break;
            }
            current.pop();
        }
    }
    for s in &plan.steps {
        if let Step::Copy { from, .. } = s {
            plan.execution.snapshots.insert(from.clone(), fingerprint(from)?);
            plan.execution.observed_files.insert(from.clone(), observed_file(from)?);
        }
    }
    #[cfg(windows)]
    {
        let paths: Vec<_> = plan
            .steps
            .iter()
            .flat_map(step_paths)
            .chain((plan.execution.operation != Operation::Settings).then_some(RECEIPT))
            .collect();
        let denied = crate::process::denied_writes(&plan.game_dir, &paths);
        if !denied.is_empty() {
            plan.notes.push(format!(
                "Windows denies write access to {}; apply will refuse until permissions are corrected",
                denied.join(", ")
            ));
        }
    }
    plan.refusals.retain(|r| !matches!(r, Refusal::GameRunning { .. }));
    let processes = plan_processes(&plan);
    if !processes.is_empty() {
        plan.refusals.insert(0, Refusal::GameRunning { processes });
    }
    plan.execution.steps = plan.steps.clone();
    plan.execution.refusals = plan.refusals.clone();
    plan.execution.sealed = true;
    Ok(plan)
}
struct Installer<'a> {
    plan: Plan,
    options: &'a InstallOptions,
    receipt: Receipt,
    backup: String,
    reshade: bool,
}
impl Installer<'_> {
    fn backup(&mut self, rel: &str) {
        self.plan.steps.push(Step::Backup {
            path: rel.into(),
            copy: format!("{}/{rel}", self.backup),
        });
    }
    fn copy(&mut self, from: &Path, rel: &str) {
        self.plan.steps.push(Step::Copy {
            from: from.to_owned(),
            to: rel.into(),
        });
    }
    fn dll(&mut self, rel: &str, src: &Path, mark: &[u8], proxy: bool) -> Result<(), Error> {
        check_parents(&self.plan.game_dir, rel)?;
        let target = self.plan.game_dir.join(rel);
        let src_sha = hash(src)?;
        if !cfg!(windows)
            && let Some(hit) = variants(&self.plan.game_dir, rel).iter().find(|n| n.as_str() != rel)
        {
            self.plan.refusals.push(foreign(format!(
                "{hit} is already here, and to Wine it is the same name"
            )));
            return Ok(());
        }
        if exists(&target) && !target.is_file() && !linked(&target) {
            self.plan
                .refusals
                .push(foreign(format!("{rel}: it is not a regular file")));
            return Ok(());
        }
        if !exists(&target) {
            self.copy(src, rel);
            return Ok(());
        }
        let sha = sha_or_dash(&target);
        if !linked(&target) && (sha == src_sha || owned(&self.receipt, rel, &sha)) {
            if sha == src_sha {
                self.plan.steps.push(Step::Keep {
                    path: rel.into(),
                    why: "already this build, left as it is".into(),
                });
            } else {
                self.copy(src, rel);
            }
            if proxy
                && self.options.keep_reshade
                && self
                    .receipt
                    .records
                    .iter()
                    .any(|r| matches!(r,Record::Rename { to,.. } if to.to_ascii_lowercase().ends_with("reshade64.dll")))
            {
                self.reshade = true;
            }
        } else if !linked(&target) && !mark.is_empty() && marker(&target, mark) {
            self.backup(rel);
            self.copy(src, rel);
        } else if !linked(&target) && marker(&target, b"ReShade") {
            if proxy && self.options.keep_reshade {
                let new = match rel.to_ascii_lowercase().as_str() {
                    "dxgi.dll" => "ReShade64.dll",
                    "d3d12.dll" => "d3d12-ReShade64.dll",
                    _ => {
                        self.plan.refusals.push(Refusal::Foreign {
                            path: format!("{rel}: --keep-reshade handles dxgi.dll and d3d12.dll only"),
                            what: Foreign::ReShade,
                        });
                        return Ok(());
                    }
                };
                let taken: Vec<_> = ["ReShade64.dll", "d3d12-ReShade64.dll"]
                    .iter()
                    .flat_map(|n| variants(&self.plan.game_dir, n))
                    .collect();
                if !taken.is_empty() {
                    self.plan.refusals.push(foreign(format!(
                        "{} is already here; cannot keep ReShade",
                        taken.join(", ")
                    )));
                    return Ok(());
                }
                self.plan.steps.push(Step::Rename {
                    from: rel.into(),
                    to: new.into(),
                });
                self.reshade = true;
                self.copy(src, rel);
            } else if self.options.replace_foreign {
                self.backup(rel);
                self.copy(src, rel);
            } else {
                self.plan.refusals.push(Refusal::Foreign {
                    path: rel.into(),
                    what: Foreign::ReShade,
                });
            }
        } else if self.options.replace_foreign {
            self.backup(rel);
            self.copy(src, rel);
        } else {
            self.plan.refusals.push(foreign(rel));
        }
        Ok(())
    }
    fn ini(&mut self) -> Result<(), Error> {
        let current = match read_ini_at(&self.plan.game_dir) {
            Ok(value) => value,
            Err(Error::Refused(refusals)) => {
                for refusal in refusals {
                    self.plan.refusals.push(foreign(match refusal {
                        Refusal::Usage(message) => message,
                        other => format!("{other:?}"),
                    }));
                }
                return Ok(());
            }
            Err(error) => return Err(error),
        };
        let rel = current
            .as_ref()
            .map(|(path, _)| path.file_name().unwrap().to_string_lossy().into_owned())
            .unwrap_or_else(|| INI_NAME.into());
        let path = self.plan.game_dir.join(&rel);
        let present = current.is_some();
        let mut ini = if let Some((_, bytes)) = current {
            Ini::parse(&bytes)
        } else {
            let mut src = self.options.source_dir.join(INI_NAME);
            if !src.is_file()
                && let Some(repo) = repository()
            {
                src = repo.join(INI_NAME);
            }
            if src.is_file() {
                self.copy(&src, INI_NAME);
                Ini::parse(&read(&src)?)
            } else {
                Ini::default()
            }
        };
        let mut backed = false;
        for (want, section, key) in [
            (self.reshade, "Plugins", "LoadReshade"),
            (self.options.enable_nr, "DlssNr", "Enabled"),
        ] {
            if !want {
                continue;
            }
            let values = ini.values(section, key);
            if values.len() == 1 && values[0].eq_ignore_ascii_case(b"true") {
                continue;
            }
            if present && !backed && !owned(&self.receipt, &rel, &sha_or_dash(&path)) {
                self.backup(&rel);
                backed = true;
            }
            self.plan.steps.push(Step::SetIni {
                file: rel.clone(),
                section: section.into(),
                key: key.into(),
                old: ini.get(section, key).map(str::to_owned),
                value: "true".into(),
            });
            ini.set(section, key, "true");
        }
        Ok(())
    }
    fn support(&mut self) -> Result<(), Error> {
        let root = self.options.source_dir.join("OptiScaler");
        let mut files = Vec::new();
        collect_support(&root, &mut files)?;
        files.sort();
        for file in files {
            let rel = file
                .strip_prefix(&self.options.source_dir)
                .map_err(|e| Error::Other(e.to_string()))?
                .to_string_lossy()
                .replace('\\', "/");
            if is_model(&rel) || rel.to_ascii_lowercase().ends_with(".disabled") {
                continue;
            }
            if !safe_rel(&rel) {
                self.plan.refusals.push(foreign(format!("unsafe support path: {rel}")));
                continue;
            }
            check_parents(&self.plan.game_dir, &rel)?;
            let parts: Vec<_> = rel.split('/').collect();
            let mut bad = false;
            for i in 1..parts.len() {
                let parent = parts[..i].join("/");
                if !cfg!(windows) && variants(&self.plan.game_dir, &parent).iter().any(|p| p != &parent) {
                    self.plan
                        .refusals
                        .push(foreign(format!("{parent}: conflicting directory spelling")));
                    bad = true;
                    break;
                }
            }
            if bad {
                continue;
            }
            if rel.eq_ignore_ascii_case("OptiScaler/libxell.dll")
                && self.plan.game_dir.join("libxell.dll").is_file()
                && sha_or_dash(&file) != sha_or_dash(&self.plan.game_dir.join("libxell.dll"))
            {
                self.plan
                    .notes
                    .push("libxell.dll: the game ships its own, so OptiScaler/libxell.dll is not installed".into());
                continue;
            }
            self.dll(&rel, &file, b"", false)?;
        }
        Ok(())
    }
}
fn collect_support(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), Error> {
    if !dir.is_dir() || linked(dir) {
        return Ok(());
    }
    for entry in fs::read_dir(dir).map_err(|e| io(dir, e))? {
        let entry = entry.map_err(|e| io(dir, e))?;
        let path = entry.path();
        if linked(&path) {
            continue;
        }
        if path.is_dir() {
            collect_support(&path, out)?;
        } else if path.is_file() {
            out.push(path);
        }
    }
    Ok(())
}
fn repository() -> Option<PathBuf> {
    let cwd = std::env::current_dir().ok()?;
    // Runtime search only: never embed a developer's checkout path in the shipped binary.
    let exe = std::env::current_exe().ok();
    cwd.ancestors()
        .chain(exe.as_deref().into_iter().flat_map(Path::ancestors))
        .find(|p| p.join("OptiScaler.ini").is_file() && p.join("tools/deploy-to-game.sh").is_file())
        .map(Path::to_owned)
}

pub(crate) fn plan_install(options: &InstallOptions) -> Result<Plan, Error> {
    let mut plan = new_plan(&options.game_dir, Operation::Install)?;
    let proxy = if options.proxy.is_empty() {
        "dxgi.dll"
    } else {
        &options.proxy
    };
    let low = proxy.to_ascii_lowercase();
    if !safe_rel(proxy)
        || proxy.contains('/')
        || !(PROXY_NAMES.contains(&low.as_str()) || low == "optiscaler.dll" || (low.ends_with(".asi") && low.len() > 4))
    {
        plan.refusals
            .push(Refusal::Usage(format!("{proxy} is not a name OptiScaler answers to")));
        return Ok(plan);
    }
    plan.execution.proxy = Some(proxy.into());
    let receipt = load_receipt(&plan.game_dir)?.unwrap_or_else(empty_receipt);
    if receipt.status == ReceiptStatus::Partial && !receipt.records.is_empty() {
        plan.notes.push(
            "The last deploy here stopped part-way; its records are kept and this one carries on from them".into(),
        );
    }
    let backup = backup_dir(&plan.game_dir);
    let mut installer = Installer {
        plan,
        options,
        receipt,
        backup,
        reshade: false,
    };
    let opti = options.source_dir.join("OptiScaler.dll");
    let fwd = options.source_dir.join(FORWARDER_NAME);
    if !opti.is_file() || !fwd.is_file() {
        return Err(Error::Other(
            "OptiScaler.dll or nvngx.dll_dlssnr.dll not found in source directory".into(),
        ));
    }
    installer.dll(proxy, &opti, MARK_OPTI, true)?;
    if low != "optiscaler.dll" {
        installer.dll("OptiScaler.dll", &opti, MARK_OPTI, false)?;
    }
    installer.dll(FORWARDER_NAME, &fwd, MARK_FORWARDER, false)?;
    for name in PROXY_NAMES {
        if name == low {
            continue;
        }
        for hit in variants(&installer.plan.game_dir, name) {
            let path = installer.plan.game_dir.join(&hit);
            installer
                .plan
                .execution
                .snapshots
                .insert(path.clone(), fingerprint(&path)?);
            installer
                .plan
                .execution
                .observed_files
                .insert(path.clone(), observed_file(&path)?);
            if marker(&installer.plan.game_dir.join(&hit), MARK_OPTI) {
                if options.allow_second_proxy {
                    installer.plan.notes.push(format!(
                        "{hit} also holds an OptiScaler build; deploying anyway (--allow-second-proxy)"
                    ));
                } else {
                    installer.plan.refusals.push(Refusal::SecondProxy { name: hit });
                }
            }
        }
    }
    installer.ini()?;
    installer.support()?;
    #[cfg(target_os = "linux")]
    if options.copy_nvngx {
        let candidates: Vec<PathBuf> = match std::env::var_os("RETOQUE_DEPLOY_TEST_NVNGX").filter(|s| !s.is_empty()) {
            Some(path) => vec![path.into()],
            None => [
                "/usr/lib/nvidia/wine/_nvngx.dll",
                "/usr/lib64/nvidia/wine/_nvngx.dll",
                "/usr/lib/x86_64-linux-gnu/nvidia/wine/_nvngx.dll",
            ]
            .iter()
            .map(PathBuf::from)
            .collect(),
        };
        if let Some(driver) = candidates.iter().find(|p| p.is_file()) {
            installer.dll("_nvngx.dll", driver, b"", false)?;
        } else {
            installer.plan.notes.push("NVIDIA Wine _nvngx.dll not found; install your driver Wine NGX component or supply it beside the game.".into());
        }
    }
    let ours = installer.plan.game_dir.join("OptiScaler/libxell.dll");
    let theirs = installer.plan.game_dir.join("libxell.dll");
    if ours.is_file()
        && theirs.is_file()
        && sha_or_dash(&ours) != sha_or_dash(&theirs)
        && !exists(&ours.with_extension("dll.disabled"))
    {
        check_parents(&installer.plan.game_dir, "OptiScaler/libxell.dll")?;
        installer.plan.steps.push(Step::Rename {
            from: "OptiScaler/libxell.dll".into(),
            to: "OptiScaler/libxell.dll.disabled".into(),
        });
    }
    if !installer.plan.game_dir.join(MODEL_NAME).is_file() {
        installer.plan.notes.push("Supply your own nvngx_dlssnr.dll beside the game (or name another copy with [DlssNr] ModelFile); the model is not included.".into());
    }
    #[cfg(target_os = "linux")]
    installer.plan.notes.push(
        if installer.plan.game_dir.join("optiscaler_skip_vulkan_hooks").is_file() {
            "Vulkan hooks: skipped via marker file"
        } else if installer
            .plan
            .game_dir
            .join("optiscaler_skip_vulkan_hooks.disabled")
            .is_file()
        {
            "Vulkan hooks: ACTIVE (marker present but disabled)"
        } else {
            "Vulkan hooks: ACTIVE (no marker); set [Vulkan] SkipHooks=true if the device is lost"
        }
        .into(),
    );
    seal(installer.plan, &installer.receipt)
}

pub(crate) fn plan_uninstall(game: &Path) -> Result<Plan, Error> {
    let mut plan = new_plan(game, Operation::Uninstall)?;
    let receipt =
        load_receipt(&plan.game_dir)?.ok_or_else(|| Error::Other("no receipt; nothing was touched".into()))?;
    reshade::before_uninstall(&mut plan, &receipt)?;
    uninstall_steps(plan, &receipt)
}

fn uninstall_steps(mut plan: Plan, receipt: &Receipt) -> Result<Plan, Error> {
    let mut changed: BTreeMap<String, bool> = BTreeMap::new();
    let exists_after = |path: &str, changed: &BTreeMap<String, bool>| {
        changed
            .get(path)
            .copied()
            .unwrap_or_else(|| exists(&plan.game_dir.join(path)))
    };
    for record in &receipt.records {
        if let Record::File { path, sha256 } = record {
            let full = plan.game_dir.join(path);
            if is_model(path) || !exists(&full) {
                plan.execution.forget.insert(plan.steps.len(), record.clone());
                plan.steps.push(Step::Keep {
                    path: path.clone(),
                    why: if is_model(path) {
                        "NVIDIA's model, never removed; left in place"
                    } else {
                        "already gone"
                    }
                    .into(),
                });
            } else if regular(&full) && hash(&full)? == *sha256 {
                plan.steps.push(Step::Remove { path: path.clone() });
                changed.insert(path.clone(), false);
            } else {
                plan.steps.push(Step::Keep {
                    path: path.clone(),
                    why: "left in place, it changed since this script wrote it".into(),
                });
            }
        }
    }
    for record in receipt.records.iter().rev() {
        if let Record::Backup { path, copy, sha256 } = record {
            if is_model(path) || is_model(copy) || !exists(&plan.game_dir.join(copy)) {
                plan.execution.forget.insert(plan.steps.len(), record.clone());
                plan.steps.push(Step::Keep {
                    path: copy.clone(),
                    why: "not restored (model or missing backup)".into(),
                });
            } else if !linked(&plan.game_dir.join(copy))
                && (!regular(&plan.game_dir.join(copy)) || hash(&plan.game_dir.join(copy))? != *sha256)
            {
                plan.steps.push(Step::Keep {
                    path: copy.clone(),
                    why: "backup is incomplete or changed; not restored".into(),
                });
            } else if exists_after(path, &changed) {
                plan.steps.push(Step::Keep {
                    path: copy.clone(),
                    why: format!("{path}: not restored, a file is still there"),
                });
            } else {
                plan.steps.push(Step::Restore {
                    copy: copy.clone(),
                    to: path.clone(),
                });
                changed.insert(path.clone(), true);
            }
        }
    }
    for record in receipt.records.iter().rev() {
        if let Record::Rename { sha256, from, to } = record {
            let full = plan.game_dir.join(to);
            if is_model(from) || is_model(to) || !exists(&full) {
                plan.execution.forget.insert(plan.steps.len(), record.clone());
                plan.steps.push(Step::Keep {
                    path: to.clone(),
                    why: "not renamed (model or already gone)".into(),
                });
            } else if exists_after(from, &changed) {
                plan.steps.push(Step::Keep {
                    path: to.clone(),
                    why: format!("not renamed back, {from} is in the way"),
                });
            } else if regular(&full) && hash(&full)? == *sha256 {
                plan.steps.push(Step::Rename {
                    from: to.clone(),
                    to: from.clone(),
                });
                changed.insert(from.clone(), true);
                changed.insert(to.clone(), false);
            } else {
                plan.steps.push(Step::Keep {
                    path: to.clone(),
                    why: "not renamed back, it changed since".into(),
                });
            }
        }
    }
    seal(plan, receipt)
}

// Dependencies that are read through links need both the resolved identity and the file contents.
fn observed_file(path: &Path) -> Result<Option<(PathBuf, Fingerprint)>, Error> {
    if !path.is_file() {
        return Ok(None);
    }
    let resolved = path.canonicalize().map_err(|e| io(path, e))?;
    Ok(Some((resolved.clone(), fingerprint(&resolved)?)))
}
fn checked_payload(plan: &Plan, path: &Path) -> Result<Vec<u8>, Error> {
    let Some(Some((resolved, Fingerprint::File(expected, _)))) = plan.execution.observed_files.get(path) else {
        return Err(Error::Other("copy source was not a regular file when planned".into()));
    };
    let bytes = read(path)?;
    if path.canonicalize().map_err(|e| io(path, e))? != *resolved
        || format!("{:x}", Sha256::digest(&bytes)) != *expected
    {
        return Err(Error::StalePlan("source changed since planning; plan again".into()));
    }
    Ok(bytes)
}
fn check_state(game: &Path) -> Result<(), Error> {
    for rel in [RECEIPT, PENDING] {
        check_parents(game, rel)?;
        let file = game.join(rel);
        if exists(&file) && !regular(&file) {
            return Err(usage_error(format!("{rel}: state file is a link or not regular")));
        }
    }
    Ok(())
}
fn check_target(game: &Path, target: &Path) -> Result<(), Error> {
    let rel = target
        .strip_prefix(game)
        .map_err(|_| usage_error("write target is outside game"))?;
    let rel = rel
        .components()
        .map(|c| c.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/");
    check_parents(game, &rel)
}
fn sync_directory(path: &Path) -> Result<(), Error> {
    #[cfg(unix)]
    fs::File::open(path)
        .and_then(|file| file.sync_all())
        .map_err(|e| io(path, e))?;
    #[cfg(not(unix))]
    let _ = path; // std does not provide a portable Windows directory durability barrier.
    Ok(())
}
fn put_atomic(game: &Path, target: &Path, bytes: &[u8], permissions: Option<fs::Permissions>) -> Result<(), Error> {
    check_state(game)?;
    check_target(game, target)?;
    let state = game.join(STATE_DIR);
    fs::create_dir_all(&state).map_err(|e| io(&state, e))?;
    let mut builder = tempfile::Builder::new();
    builder.prefix("incoming.");
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        builder.permissions(fs::Permissions::from_mode(0o666));
    }
    let mut temp = builder.tempfile_in(&state).map_err(|e| io(&state, e))?;
    temp.write_all(bytes).map_err(|e| io(target, e))?;
    if let Some(mode) = permissions {
        temp.as_file().set_permissions(mode).map_err(|e| io(target, e))?;
    }
    temp.as_file().sync_all().map_err(|e| io(target, e))?;
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|e| io(parent, e))?;
    }
    check_state(game)?;
    check_target(game, target)?;
    temp.persist(target).map_err(|e| io(target, e.error))?;
    sync_directory(target.parent().unwrap())?;
    sync_directory(&state)?;
    sync_directory(game)?;
    Ok(())
}
fn write_receipt(game: &Path, receipt: &mut Receipt) -> Result<(), Error> {
    receipt.updated = Utc::now().format("%Y-%m-%dT%H:%M:%SZ").to_string();
    put_atomic(game, &game.join(RECEIPT), receipt.to_text().as_bytes(), None)
}
// Like FAIL_AFTER_STEP, this is an explicit subprocess-only fault injector for recovery tests.
// Exiting bypasses all Rust cleanup; no caller callback or unwinding can complete the transaction.
fn crash_checkpoint(point: &str, index: usize) {
    if std::env::var("RETOQUE_DEPLOY_TEST_CRASH_AT").ok().as_deref() == Some(&format!("{point}:{index}")) {
        std::process::exit(86);
    }
}
fn write_intent(game: &Path, receipt: &Receipt, index: usize) -> Result<(), Error> {
    crash_checkpoint("before-intent", index);
    put_atomic(game, &game.join(PENDING), receipt.to_text().as_bytes(), None)?;
    crash_checkpoint("after-intent", index);
    Ok(())
}
fn clear_intent(game: &Path) -> Result<(), Error> {
    check_state(game)?;
    let path = game.join(PENDING);
    if exists(&path) {
        fs::remove_file(&path).map_err(|e| io(&path, e))?;
        sync_directory(&game.join(STATE_DIR))?;
    }
    Ok(())
}
fn remember(receipt: &mut Receipt, path: &str, sha: String) {
    if let Some(Record::File { sha256, .. }) = receipt
        .records
        .iter_mut()
        .find(|r| matches!(r,Record::File { path: p,.. } if p == path))
    {
        *sha256 = sha;
    } else {
        receipt.records.push(Record::File {
            path: path.into(),
            sha256: sha,
        });
    }
}
fn backup_file(game: &Path, path: &str, copy: &str) -> Result<(), Error> {
    let from = game.join(path);
    let to = game.join(copy);
    if exists(&to) {
        return Err(Error::Other(format!("{copy} appeared while deploying; stopping")));
    }
    fs::create_dir_all(to.parent().unwrap()).map_err(|e| io(&to, e))?;
    #[cfg(unix)]
    if linked(&from) {
        let target = fs::read_link(&from).map_err(|e| io(&from, e))?;
        return std::os::unix::fs::symlink(target, &to).map_err(|e| io(&to, e));
    }
    #[cfg(windows)]
    if linked(&from) {
        let target = fs::read_link(&from).map_err(|e| io(&from, e))?;
        return std::os::windows::fs::symlink_file(target, &to).map_err(|e| io(&to, e));
    }
    let mut src = fs::File::open(&from).map_err(|e| io(&from, e))?;
    let mut dst = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&to)
        .map_err(|e| io(&to, e))?;
    std::io::copy(&mut src, &mut dst).map_err(|e| io(&to, e))?;
    dst.set_permissions(src.metadata().map_err(|e| io(&from, e))?.permissions())
        .map_err(|e| io(&to, e))?;
    let metadata = src.metadata().map_err(|e| io(&from, e))?;
    let mut times = fs::FileTimes::new();
    if let Ok(time) = metadata.modified() {
        times = times.set_modified(time);
    }
    if let Ok(time) = metadata.accessed() {
        times = times.set_accessed(time);
    }
    dst.set_times(times).map_err(|e| io(&to, e))?;
    dst.sync_all().map_err(|e| io(&to, e))
}
fn move_no_replace(from: &Path, to: &Path) -> Result<(), Error> {
    if exists(to) {
        return Err(Error::Other(format!(
            "{} appeared while deploying; stopping",
            to.display()
        )));
    }
    if let Some(parent) = to.parent() {
        fs::create_dir_all(parent).map_err(|e| io(parent, e))?;
    }
    fs::rename(from, to).map_err(|e| io(to, e))
}
fn perform(plan: &Plan, step: &Step, index: usize, receipt: &mut Receipt) -> Result<(), Error> {
    let game = &plan.game_dir;
    for path in step_paths(step) {
        check_parents(game, path)?;
        if !matches!(step, Step::Keep { .. }) && is_model(path) {
            return Err(Error::Other("NVIDIA model paths are never changed".into()));
        }
    }
    match step {
        Step::Copy { from, to } => {
            let bytes = checked_payload(plan, from)?;
            remember(receipt, to, format!("{:x}", Sha256::digest(&bytes)));
            write_intent(game, receipt, index)?;
            put_atomic(game, &game.join(to), &bytes, None)?;
        }
        Step::Backup { path, copy } => {
            let sha256 = sha_or_dash(&game.join(path));
            receipt.records.push(Record::Backup {
                sha256,
                path: path.clone(),
                copy: copy.clone(),
            });
            write_intent(game, receipt, index)?;
            backup_file(game, path, copy)?;
            sync_directory(game.join(copy).parent().unwrap())?;
        }
        Step::Rename { from, to } => {
            let sha256 = sha_or_dash(&game.join(from));
            if plan.execution.operation != Operation::Uninstall {
                receipt.records.push(Record::Rename {
                    sha256,
                    from: from.clone(),
                    to: to.clone(),
                });
                write_intent(game, receipt, index)?;
            }
            move_no_replace(&game.join(from), &game.join(to))?;
            sync_directory(game.join(from).parent().unwrap())?;
            sync_directory(game.join(to).parent().unwrap())?;
            if plan.execution.operation == Operation::Uninstall {
                receipt
                    .records
                    .retain(|r| !matches!(r,Record::Rename { from: f,to: t,.. } if f == to && t == from));
            }
        }
        Step::SetIni { file, section, key, .. } | Step::UnsetIni { file, section, key, .. } => {
            let value = match step {
                Step::SetIni { value, .. } => Some(value.as_str()),
                _ => None,
            };
            let path = game.join(file);
            let mut ini = if exists(&path) {
                Ini::parse(&read(&path)?)
            } else {
                Ini::default()
            };
            if file.eq_ignore_ascii_case("ReShade.ini") {
                ini = Ini::parse_sensitive(&ini.to_bytes());
            }
            if let Some(Some(undo)) = plan.execution.user_ini.get(&index) {
                if !undo.matches_inverse(section, key, value) || !ini.undo(undo) {
                    return Err(usage_error("invalid lifecycle INI undo"));
                }
            } else if let Some(value) = value {
                ini.set(section, key, value);
            } else {
                ini.unset(section, key);
            }
            let mode = fs::metadata(&path).ok().map(|m| m.permissions());
            let bytes = ini.to_bytes();
            if !plan.execution.user_ini.contains_key(&index) {
                remember(receipt, file, format!("{:x}", Sha256::digest(&bytes)));
            }
            write_intent(game, receipt, index)?;
            put_atomic(game, &path, &bytes, mode)?;
        }
        Step::Remove { path } => {
            fs::remove_file(game.join(path)).map_err(|e| io(&game.join(path), e))?;
            sync_directory(game.join(path).parent().unwrap())?;
            receipt
                .records
                .retain(|r| !matches!(r,Record::File { path: p,.. } if p == path));
        }
        Step::Restore { copy, to } => {
            move_no_replace(&game.join(copy), &game.join(to))?;
            sync_directory(game.join(copy).parent().unwrap())?;
            sync_directory(game.join(to).parent().unwrap())?;
            receipt
                .records
                .retain(|r| !matches!(r,Record::Backup { path,copy: c,.. } if path == to && c == copy));
        }
        Step::Keep { path, .. } => {
            if let Some(record) = plan.execution.forget.get(&index) {
                receipt.records.retain(|r| r != record);
            } else if plan.execution.operation == Operation::Install {
                remember(receipt, path, hash(&game.join(path))?);
            }
        }
    }
    Ok(())
}
fn cleanup_empty(game: &Path, dir: &Path) -> Result<(), Error> {
    check_state(game)?;
    check_target(game, &dir.join("entry"))?;
    if let Ok(entries) = fs::read_dir(dir) {
        for entry in entries {
            let entry = entry.map_err(|e| io(dir, e))?;
            if entry.path().is_dir() && !linked(&entry.path()) {
                cleanup_empty(game, &entry.path())?;
            }
        }
        check_state(game)?;
        check_target(game, &dir.join("entry"))?;
        let _ = fs::remove_dir(dir); // A nonempty backup tree remains for manual recovery.
    }
    Ok(())
}
fn plan_processes(plan: &Plan) -> Vec<(u32, String)> {
    let paths = plan.execution.snapshots.keys().cloned().collect::<Vec<_>>();
    let mut processes = crate::process::running_for(&plan.game_dir, &paths);
    for root in &plan.execution.process_roots {
        processes.extend(crate::process::running_for(root, &paths));
    }
    processes.sort();
    processes.dedup();
    processes
}

fn validate_snapshots(plan: &Plan, expected: &BTreeMap<PathBuf, Fingerprint>) -> Result<(), Error> {
    check_game_directory(&plan.game_dir)?;
    for root in &plan.execution.process_roots {
        check_game_directory(root)?;
    }
    let processes = plan_processes(plan);
    if !processes.is_empty() {
        return Err(Error::Refused(vec![Refusal::GameRunning { processes }]));
    }
    for (path, before) in expected {
        if fingerprint(path)? != *before {
            return Err(Error::StalePlan(format!(
                "{} changed since planning; plan again",
                path.display()
            )));
        }
    }
    for (path, before) in &plan.execution.observed_files {
        if observed_file(path)? != *before {
            return Err(Error::StalePlan(format!(
                "{} source changed since planning; plan again",
                path.display()
            )));
        }
    }
    Ok(())
}
// Refresh only paths changed by our own operation, before handing control to progress callbacks.
fn refresh_expected(game: &Path, expected: &mut BTreeMap<PathBuf, Fingerprint>, paths: &[&str]) -> Result<(), Error> {
    for rel in paths {
        let mut path = game.join(rel);
        loop {
            expected.insert(path.clone(), fingerprint(&path)?);
            if path == game {
                break;
            }
            path.pop();
        }
    }
    Ok(())
}
pub(crate) fn verify(plan: &Plan) -> Result<(), Error> {
    if !plan.refusals.is_empty() {
        return Err(Error::Refused(plan.refusals.clone()));
    }
    if !plan.execution.sealed
        || plan.refusals != plan.execution.refusals
        || plan.steps != plan.execution.steps
        || plan.game_dir != plan.execution.game_dir
    {
        return Err(Error::StalePlan("plan was modified; plan again before applying".into()));
    }
    Ok(())
}
pub(crate) fn apply(plan: &Plan, progress: &mut dyn FnMut(StepDone)) -> Result<(), Error> {
    verify(plan)?;
    validate_snapshots(plan, &plan.execution.snapshots)?;
    #[cfg(windows)]
    {
        let paths: Vec<_> = plan
            .steps
            .iter()
            .flat_map(step_paths)
            .chain((plan.execution.operation != Operation::Settings).then_some(RECEIPT))
            .collect();
        let denied = crate::process::denied_writes(&plan.game_dir, &paths);
        if !denied.is_empty() {
            return Err(Error::Refused(vec![Refusal::Usage(format!(
                "Windows denies write access to {}",
                denied.join(", ")
            ))]));
        }
    }
    if plan.execution.operation == Operation::Settings {
        return apply_settings(plan, progress);
    }
    let mut receipt = load_receipt(&plan.game_dir)?.unwrap_or_else(empty_receipt);
    if let Some(proxy) = &plan.execution.proxy {
        let record = Record::Proxy { name: proxy.clone() };
        if !receipt.records.contains(&record) {
            receipt.records.push(record);
        }
    }
    receipt.status = if plan.execution.operation == Operation::Uninstall {
        ReceiptStatus::UninstallIncomplete
    } else {
        ReceiptStatus::Partial
    };
    write_receipt(&plan.game_dir, &mut receipt)?;
    clear_intent(&plan.game_dir)?;
    let mut expected = plan.execution.snapshots.clone();
    refresh_expected(&plan.game_dir, &mut expected, &[RECEIPT, PENDING])?;
    for (index, step) in plan.steps.iter().enumerate() {
        validate_snapshots(plan, &expected)?;
        check_state(&plan.game_dir)?;
        if let Err(e) = perform(plan, step, index, &mut receipt) {
            // The last durable receipt is already incomplete. Never write through a failed boundary.
            return Err(Error::Other(format!(
                "{e}; status {}",
                if plan.execution.operation == Operation::Uninstall {
                    "uninstall-incomplete"
                } else {
                    "partial"
                }
            )));
        }
        crash_checkpoint("after-mutation", index);
        crash_checkpoint("before-receipt", index);
        write_receipt(&plan.game_dir, &mut receipt)?;
        crash_checkpoint("after-receipt", index);
        clear_intent(&plan.game_dir)?;
        let mut changed = step_paths(step);
        changed.extend([RECEIPT, PENDING]);
        refresh_expected(&plan.game_dir, &mut expected, &changed)?;
        progress(StepDone {
            index,
            total: plan.steps.len(),
            step: step.clone(),
        });
        if std::env::var("RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP").ok().as_deref() == Some(&(index + 1).to_string()) {
            return Err(Error::Other(format!(
                "RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP: stopped part-way; status {}",
                if plan.execution.operation == Operation::Uninstall {
                    "uninstall-incomplete"
                } else {
                    "partial"
                }
            )));
        }
    }
    validate_snapshots(plan, &expected)?;
    check_state(&plan.game_dir)?;
    if plan.execution.operation == Operation::Uninstall {
        cleanup_empty(&plan.game_dir, &plan.game_dir.join(".retoque/backup"))?;
        if receipt.records.iter().all(|r| matches!(r, Record::Proxy { .. }))
            && (!plan.execution.component_only || receipt.records.is_empty())
        {
            fs::remove_file(plan.game_dir.join(RECEIPT)).map_err(|e| io(&plan.game_dir.join(RECEIPT), e))?;
            let _ = fs::remove_dir(plan.game_dir.join(STATE_DIR));
        } else {
            if plan.execution.lifecycle_removal
                || (plan.execution.component_only && !exists(&plan.game_dir.join(components::INDEX)))
            {
                receipt.status = plan.execution.component_completion.unwrap_or(ReceiptStatus::Complete);
            }
            write_receipt(&plan.game_dir, &mut receipt)?;
        }
    } else {
        receipt.status = plan.execution.component_completion.unwrap_or(ReceiptStatus::Complete);
        write_receipt(&plan.game_dir, &mut receipt)?;
    }
    Ok(())
}
pub(crate) fn status(game: &Path) -> Result<Status, Error> {
    let game = canonical(game)?;
    let receipt = load_receipt(&game)?;
    let mut proxies = Vec::new();
    for name in PROXY_NAMES {
        for hit in variants(&game, name) {
            if marker(&game.join(&hit), MARK_OPTI) {
                proxies.push(hit);
            }
        }
    }
    if let Some(receipt) = &receipt {
        for r in &receipt.records {
            if let Record::Proxy { name } = r
                && !proxies.contains(name)
                && marker(&game.join(name), MARK_OPTI)
            {
                proxies.push(name.clone());
            }
        }
    }
    let proxy = receipt
        .as_ref()
        .and_then(|r| {
            r.records.iter().find_map(|r| {
                if let Record::Proxy { name } = r {
                    Some(name)
                } else {
                    None
                }
            })
        })
        .or_else(|| proxies.first());
    let build_sha256 = proxy
        .and_then(|p| hash(&game.join(p)).ok())
        .or_else(|| hash(&game.join("OptiScaler.dll")).ok());
    let launch_option = if cfg!(target_os = "linux") {
        proxy.map(|p| {
            format!(
                "WINEDLLOVERRIDES=\"{}=n,b\" %command%",
                p.rsplit_once('.').map(|(name, _)| name).unwrap_or(p)
            )
        })
    } else {
        None
    };
    let ini = variants(&game, INI_NAME)
        .first()
        .and_then(|name| fs::read(game.join(name)).ok())
        .map(|bytes| Ini::parse(&bytes));
    let nr_enabled =
        ini.as_ref()
            .and_then(|ini| ini.get("DlssNr", "Enabled"))
            .and_then(|v| match v.to_ascii_lowercase().as_str() {
                "true" | "1" => Some(true),
                "false" | "0" => Some(false),
                _ => None,
            });
    let proxy_version = proxy.and_then(|name| crate::pe_version::file_version(&game.join(name)));
    let forwarder_version = variants(&game, FORWARDER_NAME)
        .first()
        .and_then(|name| crate::pe_version::file_version(&game.join(name)));
    Ok(Status {
        proxy_version,
        forwarder_version,
        receipt,
        optiscaler_proxies: proxies,
        build_sha256,
        nr_enabled,
        model_present: game.join(MODEL_NAME).is_file(),
        launch_option,
    })
}

fn usage_error(message: impl Into<String>) -> Error {
    Error::Refused(vec![Refusal::Usage(message.into())])
}

// The canonical root can itself be replaced after planning. Refuse that replacement, but do not
// inspect trusted ancestors: Wine marks volume/subvolume boundaries there as reparse points too.
fn check_game_directory(game: &Path) -> Result<(), Error> {
    if linked(game) || !game.is_dir() {
        return Err(usage_error(format!(
            "{}: resolved game directory is a link or not a directory",
            game.display()
        )));
    }
    Ok(())
}

pub(crate) fn read_ini(game_dir: &Path) -> Result<Option<(PathBuf, Vec<u8>)>, Error> {
    read_ini_at(&canonical(game_dir)?)
}

// Internal callers already have a pinned canonical game directory; never resolve it a second time.
fn read_ini_at(game: &Path) -> Result<Option<(PathBuf, Vec<u8>)>, Error> {
    check_game_directory(game)?;
    let mut hits = Vec::new();
    for entry in fs::read_dir(game).map_err(|e| io(game, e))? {
        let entry = entry.map_err(|e| io(game, e))?;
        if entry
            .file_name()
            .to_str()
            .is_some_and(|name| name.eq_ignore_ascii_case(INI_NAME))
        {
            hits.push(entry.path());
        }
    }
    if hits.len() > 1 {
        return Err(usage_error(
            "ambiguous OptiScaler.ini variants; keep one spelling before editing",
        ));
    }
    let Some(path) = hits.pop() else {
        return Ok(None);
    };
    if !regular(&path) {
        return Err(usage_error("OptiScaler.ini is a link or not a regular file"));
    }
    let bytes = read(&path)?;
    Ok(Some((path, bytes)))
}

fn valid_edit(section: &str, key: &str, value: &str) -> bool {
    !section.trim().is_empty()
        && !key.trim().is_empty()
        && !section.contains(['\r', '\n', '\0', '[', ']'])
        && !key.contains(['\r', '\n', '\0', '='])
        && !key.trim_start().starts_with([';', '#'])
        && !value.contains(['\r', '\n', '\0'])
}

pub(crate) fn plan_settings(game_dir: &Path, edits: &[(String, String, String)]) -> Result<Plan, Error> {
    let edits = edits
        .iter()
        .map(|(s, k, v)| (s.clone(), k.clone(), Some(v.clone())))
        .collect::<Vec<_>>();
    plan_settings_edits(game_dir, &edits, None)
}

pub(crate) fn plan_settings_edits(
    game_dir: &Path,
    edits: &[(String, String, Option<String>)],
    provenance: Option<&[ini::IniUndo]>,
) -> Result<Plan, Error> {
    let mut plan = new_plan(game_dir, Operation::Settings)?;
    let (path, bytes) = match read_ini_at(&plan.game_dir) {
        Ok(Some(value)) => value,
        Ok(None) => {
            plan.refusals.push(Refusal::Usage(
                "OptiScaler.ini is missing; install or supply it before editing settings".into(),
            ));
            return Ok(plan);
        }
        Err(Error::Refused(refusals)) => {
            plan.refusals.extend(refusals);
            return Ok(plan);
        }
        Err(error) => return Err(error),
    };
    let file = path.file_name().unwrap().to_string_lossy().into_owned();
    let mut ini = Ini::parse(&bytes);
    let exact = provenance.filter(|p| p.last().is_some_and(|u| u.matches_after(&bytes)));
    if let Some(provenance) = exact {
        if provenance.len() != edits.len() {
            return Err(usage_error("INI undo provenance does not match the requested edits"));
        }
        plan.execution.exact_undo = provenance.iter().rev().cloned().collect();
    }
    if provenance.is_some() {
        plan.notes.push(if exact.is_some() {
            "INI undo restores the original bytes; the recorded post-apply hash still matches."
        } else {
            "Conservative INI undo: the file changed or exact history is unavailable. Other bytes are preserved; restoration is not byte-for-byte."
        }.into());
    }
    for (index, (section, key, value)) in edits.iter().enumerate() {
        if !valid_edit(section, key, value.as_deref().unwrap_or("")) {
            plan.refusals.push(Refusal::Usage(
                "INI edits require single-line section, key and value without structural separators".into(),
            ));
            continue;
        }
        let old = ini.get(section, key).map(str::to_owned);
        let changed = if let Some(undo) = plan.execution.exact_undo.get(index) {
            if !undo.matches_inverse(section, key, value.as_deref()) || !ini.undo(undo) {
                return Err(usage_error("invalid INI undo provenance; no files were changed"));
            }
            true
        } else if let Some(undo) = ini.edit_with_undo(section, key, value.as_deref()) {
            plan.execution.settings_undo.push(undo);
            true
        } else {
            false
        };
        if changed {
            plan.steps.push(if let Some(value) = value {
                Step::SetIni {
                    file: file.clone(),
                    section: section.clone(),
                    key: key.clone(),
                    old,
                    value: value.clone(),
                }
            } else {
                Step::UnsetIni {
                    file: file.clone(),
                    section: section.clone(),
                    key: key.clone(),
                    old,
                }
            });
        }
    }
    plan = seal(plan, &empty_receipt())?;
    // Even a no-op preview describes this exact INI. Bind its fingerprint to the bytes we actually planned,
    // rather than accepting a different file that happened to arrive during the planning reads.
    let expected = fingerprint(&path)?;
    let planned_hash = format!("{:x}", Sha256::digest(&bytes));
    if !matches!(&expected,Fingerprint::File(hash,_) if hash == &planned_hash) {
        return Err(Error::StalePlan("INI changed while planning; plan again".into()));
    }
    plan.execution.snapshots.insert(path, expected);
    plan.execution
        .snapshots
        .insert(plan.game_dir.clone(), fingerprint(&plan.game_dir)?);
    Ok(plan)
}

// User settings do not acquire receipt ownership. The temp lives beside the INI and is always created
// exclusively with a random name, so a stale or hostile preexisting temp entry can never be truncated.
fn replace_settings_ini(path: &Path, bytes: &[u8]) -> Result<(), Error> {
    if !regular(path) {
        return Err(usage_error("OptiScaler.ini is a link or not a regular file"));
    }
    let permissions = fs::metadata(path).map_err(|e| io(path, e))?.permissions();
    let parent = path
        .parent()
        .ok_or_else(|| usage_error("INI has no parent directory"))?;
    let mut temp = tempfile::Builder::new()
        .prefix(".retoque-ini.")
        .tempfile_in(parent)
        .map_err(|e| io(parent, e))?;
    temp.write_all(bytes).map_err(|e| io(path, e))?;
    // Preserve Unix mode. Windows preflight rejects readonly files; tempfile's Windows persist can
    // normalize attributes, so ACL/arbitrary attribute preservation is not promised.
    temp.as_file().set_permissions(permissions).map_err(|e| io(path, e))?;
    temp.as_file().sync_all().map_err(|e| io(path, e))?;
    temp.persist(path).map_err(|e| io(path, e.error))?;
    Ok(())
}

fn apply_settings(plan: &Plan, progress: &mut dyn FnMut(StepDone)) -> Result<(), Error> {
    let mut expected = plan.execution.snapshots.clone();
    for (index, step) in plan.steps.iter().enumerate() {
        let (file, section, key, old, value) = match step {
            Step::SetIni {
                file,
                section,
                key,
                old,
                value,
            } => (file, section, key, old, Some(value.as_str())),
            Step::UnsetIni {
                file,
                section,
                key,
                old,
            } => (file, section, key, old, None),
            _ => return Err(Error::Other("invalid step in settings plan".into())),
        };
        check_parents(&plan.game_dir, file)?;
        let path = plan.game_dir.join(file);
        let processes = plan_processes(plan);
        if !processes.is_empty() {
            return Err(Error::Refused(vec![Refusal::GameRunning { processes }]));
        }
        if expected.get(&path) != Some(&fingerprint(&path)?) {
            return Err(Error::StalePlan(
                "INI changed since planning or the last settings step; plan again".into(),
            ));
        }
        let mut ini = Ini::parse(&read(&path)?);
        if ini.get(section, key) != old.as_deref() {
            return Err(Error::StalePlan("INI value changed since planning; plan again".into()));
        }
        if let Some(undo) = plan.execution.exact_undo.get(index) {
            if !ini.undo(undo) {
                return Err(Error::StalePlan(
                    "INI undo provenance no longer matches; plan again".into(),
                ));
            }
        } else if let Some(value) = value {
            ini.set(section, key, value);
        } else {
            ini.unset(section, key);
        }
        let bytes = ini.to_bytes();
        replace_settings_ini(&path, &bytes)?;
        if let Some(Fingerprint::File(hash, _)) = expected.get_mut(&path) {
            *hash = format!("{:x}", Sha256::digest(&bytes));
        }
        progress(StepDone {
            index,
            total: plan.steps.len(),
            step: step.clone(),
        });
    }
    Ok(())
}

// Merge disjoint install mutations before review, keeping the original sources and receipt preconditions.
pub(crate) fn combine_installs(mut first: Plan, second: Plan) -> Result<Plan, Error> {
    if first.game_dir != second.game_dir
        || first.execution.operation != Operation::Install
        || second.execution.operation != Operation::Install
    {
        return Err(usage_error("only same-root installs can be combined"));
    }
    let used: std::collections::BTreeSet<_> = first.steps.iter().flat_map(step_paths).collect();
    if second.steps.iter().flat_map(step_paths).any(|p| used.contains(p)) {
        return Err(usage_error("overlapping install plans"));
    }
    for (path, expected) in second.execution.snapshots {
        if first.execution.snapshots.get(&path).is_some_and(|old| old != &expected) {
            return Err(usage_error("install plans observe different files"));
        }
        first.execution.snapshots.insert(path, expected);
    }
    first.execution.observed_files.extend(second.execution.observed_files);
    first.execution.process_roots.extend(second.execution.process_roots);
    first.steps.extend(second.steps);
    first.refusals.extend(second.refusals);
    // Preserve every original fingerprint. Resealing would bless source changes between the plans.
    first.execution.steps = first.steps.clone();
    first.execution.refusals = first.refusals.clone();
    Ok(first)
}

pub(crate) fn validate_plan(plan: &Plan) -> Result<(), Error> {
    verify(plan)?;
    validate_snapshots(plan, &plan.execution.snapshots)
}
