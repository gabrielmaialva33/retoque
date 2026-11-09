//! Retoque's install engine: the rules of `tools/deploy-to-game.sh` and `tools/install-windows.ps1` as one
//! implementation (docs/superpowers/specs/2026-11-05-retoque-app.md).
//!
//! The public types below are the contract the CLI, the app and the other crates are written against. Change
//! them only together with their callers.

use std::path::{Path, PathBuf};

mod engine;
pub mod ini;
pub mod pe_version;
mod process;
pub mod receipt;
pub use process::running_processes;

/// The names OptiScaler answers to when a game loads it (dllmain.cpp, dllNames), lower case.
pub const PROXY_NAMES: [&str; 7] = [
    "dxgi.dll",
    "winmm.dll",
    "version.dll",
    "dbghelp.dll",
    "d3d12.dll",
    "winhttp.dll",
    "wininet.dll",
];
pub const FORWARDER_NAME: &str = "nvngx.dll_dlssnr.dll";
pub const INI_NAME: &str = "OptiScaler.ini";
pub const MODEL_NAME: &str = "nvngx_dlssnr.dll";
pub const STATE_DIR: &str = ".retoque";

/// What an install is asked to do. Field-for-field the scripts' flags.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct InstallOptions {
    pub game_dir: PathBuf,
    /// One of `PROXY_NAMES`, `OptiScaler.dll` or `<name>.asi`; default `dxgi.dll`.
    pub proxy: String,
    /// Where `OptiScaler.dll`, the forwarder, `OptiScaler.ini` and the `OptiScaler/` support folder are taken from.
    pub source_dir: PathBuf,
    pub enable_nr: bool,
    pub keep_reshade: bool,
    pub replace_foreign: bool,
    pub allow_second_proxy: bool,
    /// Linux only: copy the driver's Wine `_nvngx.dll` (the scripts' default; `--no-nvngx` turns it off).
    pub copy_nvngx: bool,
}

impl Default for InstallOptions {
    fn default() -> Self {
        Self {
            game_dir: PathBuf::new(),
            proxy: "dxgi.dll".into(),
            source_dir: PathBuf::new(),
            enable_nr: false,
            keep_reshade: false,
            replace_foreign: false,
            allow_second_proxy: false,
            copy_nvngx: cfg!(target_os = "linux"),
        }
    }
}

/// One change, in the order it will be made.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Step {
    Copy {
        from: PathBuf,
        to: String,
    },
    Backup {
        path: String,
        copy: String,
    },
    Rename {
        from: String,
        to: String,
    },
    SetIni {
        file: String,
        section: String,
        key: String,
        /// Last effective value immediately before this step, or None when the key is absent.
        old: Option<String>,
        value: String,
    },
    Remove {
        path: String,
    },
    Restore {
        copy: String,
        to: String,
    },
    /// A file the run leaves alone, with the reason (identical content, changed by the user since install, …).
    Keep {
        path: String,
        why: String,
    },
}

/// Why nothing will be written. `exit_code` is the scripts' exit status.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Refusal {
    Usage(String),
    /// A process maps or holds a file in the game folder.
    GameRunning {
        processes: Vec<(u32, String)>,
    },
    /// A file at one of our names is not ours.
    Foreign {
        path: String,
        what: Foreign,
    },
    /// Another proxy name in the folder already holds OptiScaler.
    SecondProxy {
        name: String,
    },
    /// The receipt is unreadable, a newer version, or names a path outside the folder.
    Receipt(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Foreign {
    ReShade,
    Unknown,
}

impl Refusal {
    pub fn exit_code(&self) -> i32 {
        match self {
            Refusal::GameRunning { .. } => 3,
            Refusal::Foreign { .. } => 4,
            Refusal::SecondProxy { .. } => 5,
            Refusal::Usage(_) | Refusal::Receipt(_) => 1,
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    game_dir: PathBuf,
    steps: Vec<Step>,
    /// Empty when the plan may be applied.
    refusals: Vec<Refusal>,
    /// Things the user should know that do not stop the run (model missing, launch option needed, …).
    notes: Vec<String>,
    // The engine retains operation and preconditions; callers cannot construct unchecked plans.
    execution: engine::Execution,
}

/// A sealed, refusal-free preview. No mutable access to its steps is exposed.
///
/// Unchecked plans cannot be applied:
/// ```compile_fail
/// use retoque_core::{Plan, apply};
/// fn bypass(plan: &Plan) { apply(plan, &mut |_| {}).unwrap(); }
/// ```
/// Review callers cannot alter the original steps:
/// ```compile_fail
/// use retoque_core::{Plan, Step};
/// fn bypass(plan: &mut Plan) { plan.steps.push(Step::Remove { path: "nvngx_dlssnr.dll".into() }); }
/// ```
/// Nor remove refusals:
/// ```compile_fail
/// use retoque_core::Plan;
/// fn bypass(plan: &mut Plan) { plan.refusals.clear(); }
/// ```
#[derive(Clone, Debug)]
pub struct VerifiedPlan(Plan);

impl Plan {
    pub fn game_dir(&self) -> &Path {
        &self.game_dir
    }
    pub fn steps(&self) -> &[Step] {
        &self.steps
    }
    pub fn refusals(&self) -> &[Refusal] {
        &self.refusals
    }
    pub fn notes(&self) -> &[String] {
        &self.notes
    }
    /// Verification seals the reviewed value; apply still checks filesystem/process preconditions.
    pub fn into_verified(self) -> Result<VerifiedPlan, Error> {
        engine::verify(&self)?;
        Ok(VerifiedPlan(self))
    }
}
impl VerifiedPlan {
    pub fn plan(&self) -> &Plan {
        &self.0
    }
}

/// Reported by `apply` after each step, for progress display.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StepDone {
    pub index: usize,
    pub total: usize,
    pub step: Step,
}

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("{}: {source}", path.display())]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("refused: {0:?}")]
    Refused(Vec<Refusal>),
    #[error("{0}")]
    Other(String),
    #[error("{0}")]
    StalePlan(String),
}

/// What is installed in a folder, read without writing anything.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Status {
    pub receipt: Option<receipt::Receipt>,
    /// Proxy names in the folder that hold an OptiScaler build, receipt or not.
    pub optiscaler_proxies: Vec<String>,
    /// sha256 of the installed `OptiScaler.dll` (or proxy), lower-case hex.
    pub build_sha256: Option<String>,
    pub nr_enabled: Option<bool>,
    pub model_present: bool,
    pub proxy_version: Option<String>,
    pub forwarder_version: Option<String>,
    /// `WINEDLLOVERRIDES="<name>=n,b" %command%` when the proxy needs it under Proton/Wine.
    pub launch_option: Option<String>,
}

impl Status {
    /// Unknown/missing resources are not reported as a mismatch.
    pub fn version_mismatch(&self) -> bool {
        matches!((&self.proxy_version, &self.forwarder_version), (Some(a), Some(b)) if a != b)
    }
}

/// A validated, user-imported component manifest. No payload is downloaded by the engine.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ComponentFiles {
    pub id: String,
    pub source_root: PathBuf,
    pub files: Vec<String>,
}

pub fn plan_component_install(game_dir: &Path, component: &ComponentFiles) -> Result<Plan, Error> {
    engine::components::plan_install(game_dir, component)
}

pub fn plan_component_uninstall(game_dir: &Path, id: &str) -> Result<Plan, Error> {
    engine::components::plan_uninstall(game_dir, id)
}

pub fn installed_components(game_dir: &Path) -> Result<Vec<String>, Error> {
    engine::components::installed(game_dir)
}

pub fn plan_install(options: &InstallOptions) -> Result<Plan, Error> {
    engine::plan_install(options)
}

pub fn plan_uninstall(game_dir: &Path) -> Result<Plan, Error> {
    engine::plan_uninstall(game_dir)
}

/// Reads the unique case-insensitive OptiScaler.ini match and returns its actual absolute path and bytes.
/// Returns None if absent. The selected directory (including aliases) is resolved to its canonical target;
/// its ancestors are trusted. Ambiguous variants and linked/non-file INIs are refused.
pub fn read_ini(game_dir: &Path) -> Result<Option<(PathBuf, Vec<u8>)>, Error> {
    engine::read_ini(game_dir)
}

/// Plans user edits as (section, key, value), retaining the old effective value for each changed step.
/// Missing/ambiguous/linked INIs and running games are refused. No-op edits are omitted. Keys may be added.
/// Apply rechecks the file snapshot and processes; these user edits never update or create an install receipt.
pub fn plan_settings(game_dir: &Path, edits: &[(String, String, String)]) -> Result<Plan, Error> {
    engine::plan_settings(game_dir, edits)
}

/// Carries out an unmodified plan with no refusals. Install/uninstall rechecks files and processes between
/// steps. Installs persist a side intent before mutations and commit the v1 receipt afterward; interrupted
/// intents are reconciled by subsequent planning/apply. Failure leaves partial/uninstall-incomplete state. Settings replaces only the INI, preserving permissions and leaving the
/// receipt untouched, and reports progress after each edit. Already completed edits remain if a later edit fails.
/// Refusal-free zero-step plans are accepted: uninstall can still remove an empty/partial receipt, while an
/// empty settings plan writes nothing. Callers must not hide apply solely because the step list is empty.
pub fn apply(plan: &VerifiedPlan, progress: &mut dyn FnMut(StepDone)) -> Result<(), Error> {
    engine::apply(&plan.0, progress)
}

pub fn status(game_dir: &Path) -> Result<Status, Error> {
    engine::status(game_dir)
}

/// A host path as a game under Proton/Wine sees it (`/home/<user>/x` -> `Z:\\home\\<user>\\x`): Wine maps drive Z: to `/`.
/// On Windows the path is returned as it is.
pub fn wine_path(host: &Path) -> String {
    let text = host.to_string_lossy();
    if cfg!(windows) || !text.starts_with('/') {
        return text.into_owned();
    }
    format!("Z:{}", text.replace('/', "\\"))
}

#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;

    #[cfg(not(windows))]
    #[test]
    fn wine_path_maps_root_to_drive_z() {
        assert_eq!(
            wine_path(Path::new("/home/u/Retoque models/a.dll")), // privacy: allow (synthetic test path)
            "Z:\\home\\u\\Retoque models\\a.dll"                  // privacy: allow (synthetic test path)
        );
        assert_eq!(wine_path(Path::new("relative/x")), "relative/x");
    }
}

/// Replace only existing SR/RR/FG DLLs; paths are relative to the discovered installation root.
pub fn plan_dlss_update(install_dir: &Path, target: &Path, catalog: &Path, files: &[String]) -> Result<Plan, Error> {
    engine::dlss::plan(install_dir, target, catalog, files)
}
/// Recognize only the app's standalone DLSS receipt. Foreign root receipts are refused.
pub fn dlss_root_state(install_dir: &Path) -> Result<Option<receipt::ReceiptStatus>, Error> {
    engine::dlss::root_state(install_dir)
}
/// Root receipt first, then executable receipt. Each returned plan must be reviewed before apply.
pub fn plan_game_restore(install_dir: &Path, target: &Path, root_only: bool) -> Result<Vec<Plan>, Error> {
    engine::dlss::restore(install_dir, target, root_only)
}
pub fn combine_install_plans(first: Plan, second: Plan) -> Result<Plan, Error> {
    engine::combine_installs(first, second)
}

/// Read-only preflight of a sealed plan before a compound operation. Apply rechecks every step.
pub fn validate_plan(plan: &Plan) -> Result<(), Error> {
    engine::validate_plan(plan)
}
