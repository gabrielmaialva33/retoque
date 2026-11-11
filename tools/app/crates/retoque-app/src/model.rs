//! What the background jobs do and what they send back. Every function here runs off the UI thread. Results carry
//! the folder the GUI asked about (never the engine's canonical spelling of it), so they are routed to the view that
//! asked, and a late answer for a folder the user has left is dropped.

use crate::error::AppError;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::Sender;

use retoque_core::{InstallOptions, Plan, Status, Step, StepDone};
use retoque_data::logs::Summary;
use retoque_data::models::{Catalog, Model};
use retoque_discovery::{Artwork, Game};

/// One settings change: (section, key, value).
pub type Edit = (String, String, String);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlanKind {
    Install,
    Uninstall,
    /// OptiScaler.ini keys, from the Settings or the Model tab.
    Settings,
}

pub enum Outcome {
    UpdateAll(crate::update_all::Event),
    ReShade(Box<crate::reshade_ui::Event>),
    Dlss(crate::dlss_ui::Event),
    Lsfg(crate::lsfg_ui::Event),
    PayloadFetch(Box<crate::payload_ui::Event>),
    NrFetch(crate::tabs::model::NrFetchEvent),
    HistoryWritten(Result<(), AppError>),
    /// What the startup cleanup removed from the app's data folder.
    AppDataCleaned(retoque_data::app_data::Cleaned),
    /// Bytes in the app's data folder.
    AppDataUsage(u64),
    DropClassified {
        request: u64,
        paths: Vec<(PathBuf, crate::drop_import::Route)>,
    },
    Doctor(Box<crate::intelligence::Assessment>),
    DoctorBadge {
        generation: u64,
        target: PathBuf,
        count: usize,
    },
    Watch {
        generation: u64,
        target: PathBuf,
        result: Result<crate::watcher::Changes, String>,
    },
    Scoped {
        epoch: Option<u64>,
        request: u64,
        name: &'static str,
        outcome: Box<Outcome>,
    },
    Payload(PathBuf, Result<String, AppError>),
    Library(u64, PathBuf, Box<Status>),
    LibraryDone(u64),
    ModelInspection(crate::app::ModelInspection),
    ModelUsage(HashMap<PathBuf, usize>),
    Auto(Box<crate::autotune::Event>),
    Discovered {
        games: Vec<Game>,
        skipped: Vec<String>,
        art: HashMap<String, Artwork>,
        /// Folders a store uninstalled but Retoque's files kept, already inspected (only those with something to remove).
        leftovers: Vec<retoque_core::leftovers::Leftover>,
    },
    /// The reviewed leftover cleanup finished (totals over every folder), or stopped at the first failure.
    LeftoversRemoved(Result<retoque_core::leftovers::Removed, AppError>),
    Status {
        target: PathBuf,
        result: Result<Status, AppError>,
    },
    /// The ini's actual path (its spelling may differ in case) and bytes, or None when the folder has none.
    Ini {
        target: PathBuf,
        result: Result<Option<(PathBuf, Vec<u8>)>, AppError>,
    },
    Plan {
        target: PathBuf,
        kind: PlanKind,
        /// Which preview this answers; only the newest one is shown.
        request: u64,
        result: Result<Plan, AppError>,
        groups: Vec<usize>,
    },
    Progress {
        target: PathBuf,
        done: StepDone,
    },
    Applied {
        target: PathBuf,
        kind: PlanKind,
        /// For a settings plan, the edits it carried, so only those leave the pending list.
        edits: Vec<Edit>,
        result: Result<(), AppError>,
    },
    Logs {
        latest: bool,
        target: PathBuf,
        files: Vec<PathBuf>,
        result: Result<Option<Summary>, AppError>,
    },
    Bundle {
        target: PathBuf,
        result: Result<PathBuf, AppError>,
    },
    Catalog {
        result: Result<Vec<Model>, AppError>,
    },
    /// Worker snapshot order; the hub discards stale lists without discarding mutation completion events.
    CatalogSnapshot {
        revision: u64,
        result: Result<Vec<Model>, AppError>,
    },
    /// A job panicked. `target` lets the app release the folder's write lock and reload it.
    Failed {
        job: &'static str,
        target: Option<PathBuf>,
        message: String,
    },
    Finished,
}

pub fn discover(manual: Vec<PathBuf>, libraries: Vec<PathBuf>) -> Outcome {
    let (mut games, skipped, leftover_dirs) = retoque_discovery::discover_with_leftovers(&retoque_discovery::Config {
        manual,
        libraries,
        home: None,
    });
    let leftovers = leftover_dirs
        .iter()
        .filter_map(|dir| retoque_core::leftovers::inspect(dir).ok())
        .filter(|l| !l.remove.is_empty())
        .collect();
    // The engine refuses a folder reached through a link (a receipt must not follow one), so a legitimate alias
    // such as ~/.steam/root is resolved once, here, and every later call uses the real path.
    for game in &mut games {
        for target in &mut game.targets {
            if let Ok(real) = std::fs::canonicalize(&target.path) {
                target.path = real;
            }
        }
        if let Ok(real) = std::fs::canonicalize(&game.install_dir) {
            game.install_dir = real;
        }
    }
    let home = dirs::home_dir();
    let art = games
        .iter()
        .filter_map(|g| {
            let app_id = g.id.strip_prefix("steam:")?.parse().ok()?;
            Some((g.id.clone(), retoque_discovery::steam_artwork(app_id, home.as_deref())))
        })
        .collect();
    Outcome::Discovered {
        games,
        skipped,
        art,
        leftovers,
    }
}

/// Remove each reviewed leftover; `retoque_core::leftovers::remove` refuses one that changed since the review.
pub fn remove_leftovers(reviewed: Vec<retoque_core::leftovers::Leftover>) -> Outcome {
    let mut total = retoque_core::leftovers::Removed::default();
    for leftover in &reviewed {
        match retoque_core::leftovers::remove(leftover) {
            Ok(done) => {
                total.files += done.files;
                total.bytes += done.bytes;
            }
            Err(error) => return Outcome::LeftoversRemoved(Err(AppError::from(error))),
        }
    }
    Outcome::LeftoversRemoved(Ok(total))
}

pub fn status(target: PathBuf) -> Outcome {
    let result = retoque_core::status(&target).map_err(|e| AppError::from(e).for_target(target.clone()));
    Outcome::Status { target, result }
}

pub fn read_ini(target: PathBuf) -> Outcome {
    let result = retoque_core::read_ini(&target).map_err(|e| AppError::from(e).for_target(target.clone()));
    Outcome::Ini { target, result }
}

pub fn plan(kind: PlanKind, request: u64, target: PathBuf, options: InstallOptions, edits: Vec<Edit>) -> Outcome {
    let result = match kind {
        PlanKind::Install => retoque_core::plan_install(&options),
        PlanKind::Uninstall => retoque_core::plan_uninstall(&target),
        PlanKind::Settings => retoque_core::plan_settings(&target, &edits),
    }
    .map_err(|e| AppError::from(e).for_target(target.clone()));
    let groups = result
        .as_ref()
        .map(|plan| {
            plan.steps()
                .iter()
                .map(|step| {
                    let exists = matches!(step, Step::Copy { to, .. } if plan.game_dir().join(to).exists());
                    crate::app::step_group(step, plan.steps(), exists)
                })
                .collect()
        })
        .unwrap_or_default();
    Outcome::Plan {
        groups,
        target,
        kind,
        request,
        result,
    }
}

pub fn apply(
    kind: PlanKind,
    plan: Plan,
    target: PathBuf,
    edits: Vec<Edit>,
    progress: &Sender<Outcome>,
    ctx: &egui::Context,
) -> Outcome {
    let result = plan
        .into_verified()
        .and_then(|plan| {
            retoque_core::apply(&plan, &mut |done| {
                let _ = progress.send(Outcome::Progress {
                    target: target.clone(),
                    done,
                });
                ctx.request_repaint();
            })
        })
        .map_err(|e| AppError::from(e).for_target(target.clone()));
    Outcome::Applied {
        target,
        kind,
        edits,
        result,
    }
}

pub fn logs(target: PathBuf) -> Outcome {
    let files = retoque_data::logs::find(&target);
    let result = match files.first() {
        Some(newest) => retoque_data::logs::summarize(newest)
            .map(Some)
            .map_err(|e| AppError::io(newest.clone(), e).for_target(target.clone())),
        None => Ok(None),
    };
    Outcome::Logs {
        latest: true,
        target,
        files,
        result,
    }
}

pub fn bundle(target: PathBuf, status_text: String, out_dir: PathBuf) -> Outcome {
    let result = retoque_data::logs::bundle(&target, &status_text, &out_dir)
        .map_err(|e| AppError::io(out_dir, e).for_target(target.clone()));
    Outcome::Bundle { target, result }
}

pub fn catalog_list() -> Outcome {
    Outcome::Catalog {
        result: Catalog::open(None).and_then(|c| c.list()).map_err(AppError::from),
    }
}

pub fn catalog_import(file: PathBuf) -> Outcome {
    let result = Catalog::open(None)
        .and_then(|c| c.import(&file, None).and_then(|_| c.list()))
        .map_err(|e| AppError::io(file, e));
    Outcome::Catalog { result }
}

pub fn catalog_remove(sha256: String) -> Outcome {
    let result = Catalog::open(None)
        .and_then(|c| c.remove(&sha256).and_then(|_| c.list()))
        .map_err(AppError::from);
    Outcome::Catalog { result }
}

/// Where the release files (OptiScaler.dll, the forwarder, OptiScaler/) are: beside the app in a release, or the
/// build output when the app runs from a checkout.
pub fn find_payload() -> Option<PathBuf> {
    if let Some(dir) = std::env::var_os("RETOQUE_SOURCE_DIR").map(PathBuf::from) {
        return Some(dir);
    }
    let exe = std::env::current_exe().ok()?;
    let beside = exe.parent()?.to_path_buf();
    if is_payload(&beside) {
        return Some(beside);
    }
    exe.ancestors()
        .map(|a| a.join("x64").join("out"))
        .find(|d| is_payload(d))
}

/// A release folder holds the ini beside the DLLs; a checkout's `x64/out` holds only the DLLs, and the engine takes
/// the ini from the repository root, as tools/deploy-to-game.sh does.
pub fn is_payload(dir: &Path) -> bool {
    dir.join("OptiScaler.dll").is_file() && dir.join(retoque_core::FORWARDER_NAME).is_file()
}

/// The diagnostic bundle goes to the user's Downloads (or home) folder, where they will look for it.
pub fn bundle_dir() -> PathBuf {
    dirs::download_dir()
        .or_else(dirs::home_dir)
        .unwrap_or_else(std::env::temp_dir)
}

/// Opens a folder in the system file manager.
pub fn open_folder(dir: &Path) {
    #[cfg(windows)]
    let program = "explorer";
    #[cfg(not(windows))]
    let program = "xdg-open";
    let _ = std::process::Command::new(program).arg(dir).spawn();
}
