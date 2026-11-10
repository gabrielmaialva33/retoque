//! GUI orchestration: one reviewed component operation followed by settings, one writer lock.
use crate::error::AppError;
use crate::model::Edit;
use retoque_core::{ComponentFiles, installed_components, plan_component_install, plan_component_uninstall};
use retoque_core::{Plan, Step, StepDone};
pub use retoque_data::sm86;
pub use retoque_data::{components, gpu, lsfg, recommend};
use std::path::{Path, PathBuf};

#[path = "optimize.rs"]
pub mod optimize;

#[derive(Clone, Debug)]
pub enum ComponentChange {
    ReShade(crate::reshade_ui::Change),
    Install(components::Component),
    Remove(String),
    Retoque {
        options: Box<retoque_core::InstallOptions>,
        component: Option<Box<ComponentChange>>,
    },
}
#[derive(Clone, Debug, Default)]
pub struct Request {
    pub component: Option<ComponentChange>,
    pub edits: Vec<Edit>,
    pub dlss: Option<crate::dlss_ui::Change>,
}
impl Request {
    pub fn with_install(mut self, options: retoque_core::InstallOptions) -> Self {
        self.set_install(options);
        self
    }
    pub fn set_install(&mut self, options: retoque_core::InstallOptions) {
        let existing = match self.component.take() {
            Some(ComponentChange::Retoque { component, .. }) => component,
            other => other.map(Box::new),
        };
        self.component = Some(ComponentChange::Retoque {
            options: Box::new(options),
            component: existing,
        });
    }
    pub fn set_component(&mut self, change: ComponentChange) {
        match &mut self.component {
            Some(ComponentChange::Retoque { component, .. }) => {
                *component = Some(Box::new(change));
            }
            slot => *slot = Some(change),
        }
    }
    pub fn retoque_install(&self) -> Option<&retoque_core::InstallOptions> {
        match &self.component {
            Some(ComponentChange::Retoque { options, .. }) => Some(options),
            _ => None,
        }
    }
    pub fn component_change(&self) -> Option<&ComponentChange> {
        match &self.component {
            Some(ComponentChange::Retoque { component, .. }) => component.as_deref(),
            other => other.as_ref(),
        }
    }
}
#[derive(Clone)]
pub struct Prepared {
    pub target: PathBuf,
    pub request: Request,
    pub plans: Vec<Plan>,
    pub groups: Vec<Vec<usize>>,
}

static STAGED_DIRS: std::sync::Mutex<Vec<tempfile::TempDir>> = std::sync::Mutex::new(Vec::new());

fn remember_staged(temp: tempfile::TempDir) {
    if let Ok(mut guard) = STAGED_DIRS.lock() {
        if guard.len() >= 16 {
            guard.remove(0);
        }
        guard.push(temp);
    }
}

fn plan_post_install_component(
    target: &Path,
    change: &ComponentChange,
    install_options: Option<&retoque_core::InstallOptions>,
) -> Result<Plan, AppError> {
    match change {
        ComponentChange::ReShade(change) => change.plan(target).map_err(AppError::from),
        ComponentChange::Install(c) => {
            let files = ComponentFiles {
                id: c.id.clone(),
                source_root: c.root.clone(),
                files: c.files.clone(),
            };
            if let Some(options) = install_options
                && options.proxy.eq_ignore_ascii_case("version.dll")
                && !target.join("version.dll").exists()
                && options.source_dir.join("OptiScaler.dll").is_file()
            {
                let temp = tempfile::tempdir().map_err(AppError::from)?;
                let staged_proxy = temp.path().join("version.dll");
                std::fs::copy(options.source_dir.join("OptiScaler.dll"), &staged_proxy)
                    .map_err(|e| AppError::io(staged_proxy, e))?;
                let plan = plan_component_install(temp.path(), &files).map_err(AppError::from)?;
                remember_staged(temp);
                return Ok(plan);
            }
            plan_component_install(target, &files).map_err(AppError::from)
        }
        ComponentChange::Remove(id) => plan_component_uninstall(target, id).map_err(AppError::from),
        ComponentChange::Retoque { component, options } => match component.as_deref() {
            Some(inner) => plan_post_install_component(target, inner, Some(options)),
            None => Err("missing inner component change".into()),
        },
    }
}

fn plan_post_install_settings(target: &Path, install_plan: Option<&Plan>, edits: &[Edit]) -> Result<Plan, AppError> {
    let Some(install) = install_plan else {
        return retoque_core::plan_settings(target, edits).map_err(AppError::from);
    };
    let current = match retoque_core::read_ini(target) {
        Ok(value) => value,
        Err(retoque_core::Error::Refused(_)) => {
            return retoque_core::plan_settings(target, edits).map_err(AppError::from);
        }
        Err(error) => return Err(error.into()),
    };
    let install_touches_edits = install.steps().iter().any(|step| {
        matches!(
            step,
            Step::SetIni { section, key, .. }
                if edits.iter().any(|(s, k, _)| s.eq_ignore_ascii_case(section) && k.eq_ignore_ascii_case(key))
        )
    });
    if current.is_some() && !install_touches_edits {
        return retoque_core::plan_settings(target, edits).map_err(AppError::from);
    }
    let copied_ini = install.steps().iter().find_map(|step| match step {
        Step::Copy { from, to } if to.eq_ignore_ascii_case(retoque_core::INI_NAME) => Some(from.as_path()),
        _ => None,
    });
    let has_install_ini_step = copied_ini.is_some()
        || install
            .steps()
            .iter()
            .any(|step| matches!(step, Step::SetIni { file, .. } if file.eq_ignore_ascii_case(retoque_core::INI_NAME)));
    if current.is_none() && !has_install_ini_step {
        return retoque_core::plan_settings(target, edits).map_err(AppError::from);
    }
    // The preview is always a staged copy, never the payload folder the INI comes from: on Windows the app runs from
    // that folder, so the running-file check of a plan against it refuses the whole review (retoque-app.exe is "in use"),
    // and a plan aimed there could only ever describe the payload's INI. The real plan is rebuilt against the game at
    // apply time (replan_intermediate) and must match this preview step for step.
    let mut ini = if let Some((_, bytes)) = current {
        retoque_core::ini::Ini::parse(&bytes)
    } else if let Some(from) = copied_ini {
        let bytes = std::fs::read(from).map_err(|e| AppError::io(from.to_path_buf(), e))?;
        retoque_core::ini::Ini::parse(&bytes)
    } else {
        retoque_core::ini::Ini::default()
    };
    for step in install.steps() {
        if let Step::SetIni {
            file,
            section,
            key,
            value,
            ..
        } = step
            && file.eq_ignore_ascii_case(retoque_core::INI_NAME)
        {
            ini.set(section, key, value);
        }
    }
    let temp = tempfile::tempdir().map_err(AppError::from)?;
    let staged_ini = temp.path().join(retoque_core::INI_NAME);
    std::fs::write(&staged_ini, ini.to_bytes()).map_err(|e| AppError::io(staged_ini, e))?;
    let plan = retoque_core::plan_settings(temp.path(), edits).map_err(AppError::from)?;
    remember_staged(temp);
    Ok(plan)
}

fn is_backup_copy(copy: &str, path: &str) -> bool {
    copy.starts_with(".retoque/backup/") && copy.ends_with(&format!("/{path}"))
}

fn steps_match(a: &[Step], b: &[Step]) -> bool {
    a.len() == b.len()
        && a.iter().zip(b).all(|(left, right)| match (left, right) {
            (Step::Backup { path: pa, copy: ca }, Step::Backup { path: pb, copy: cb }) => {
                pa == pb && (ca == cb || (is_backup_copy(ca, pa) && is_backup_copy(cb, pb)))
            }
            _ => left == right,
        })
}

pub fn prepare(target: &Path, request: Request) -> Result<Prepared, AppError> {
    let mut plans = Vec::new();
    if let Some(options) = request.retoque_install() {
        let mut options = options.clone();
        if options.game_dir.as_os_str().is_empty() {
            options.game_dir = target.to_path_buf();
        }
        plans.push(retoque_core::plan_install(&options).map_err(AppError::from)?);
    }
    let install_offset = usize::from(request.retoque_install().is_some());
    if let Some(change) = request.component_change() {
        let p = plan_post_install_component(target, change, request.retoque_install())?;
        plans.push(p);
    }
    if let Some(change) = &request.dlss {
        match change {
            crate::dlss_ui::Change::Update(update) => {
                let dlss = retoque_data::dlss::plan(target, update).map_err(AppError::from)?;
                if matches!(request.component_change(), Some(ComponentChange::Install(_)))
                    && plans
                        .get(install_offset)
                        .is_some_and(|p| p.game_dir() == dlss.game_dir())
                {
                    let component = plans.remove(install_offset);
                    plans.insert(
                        install_offset,
                        retoque_core::combine_install_plans(component, dlss).map_err(AppError::from)?,
                    );
                } else {
                    plans.insert(install_offset, dlss);
                }
            }
            crate::dlss_ui::Change::Restore { install_dir } => {
                if !plans.is_empty() || !request.edits.is_empty() {
                    return Err("restore cannot be combined with optimization".into());
                }
                plans = retoque_core::plan_game_restore(install_dir, target, false).map_err(AppError::from)?;
            }
        }
    }
    if !request.edits.is_empty() {
        let install_plan = request.retoque_install().and_then(|_| plans.first());
        plans.push(plan_post_install_settings(target, install_plan, &request.edits)?);
    }
    let groups = plans
        .iter()
        .map(|p| {
            p.steps()
                .iter()
                .map(|step| {
                    let exists = matches!(step,Step::Copy{to,..} if p.game_dir().join(to).exists());
                    crate::app::step_group(step, p.steps(), exists)
                })
                .collect()
        })
        .collect();
    Ok(Prepared {
        target: target.into(),
        request,
        plans,
        groups,
    })
}
#[derive(Debug)]
pub struct ApplyFailure {
    pub completed: usize,
    pub detail: AppError,
}
fn replan_intermediate(prepared: &Prepared, index: usize) -> Result<Option<Plan>, ApplyFailure> {
    let has_settings = !prepared.request.edits.is_empty();
    if index > 0 && has_settings && index + 1 == prepared.plans.len() {
        let fresh =
            retoque_core::plan_settings(&prepared.target, &prepared.request.edits).map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?;
        if fresh.steps() != prepared.plans[index].steps() || !fresh.refusals().is_empty() {
            return Err(ApplyFailure {
                completed: index,
                detail: "settings changed since review".into(),
            });
        }
        return Ok(Some(fresh));
    }
    if index == 0 || matches!(prepared.request.dlss, Some(crate::dlss_ui::Change::Restore { .. })) {
        return Ok(None);
    }
    let install_offset = usize::from(prepared.request.retoque_install().is_some());
    let non_settings_count = prepared.plans.len() - usize::from(has_settings);
    let fresh = match (prepared.request.component_change(), &prepared.request.dlss) {
        (Some(ComponentChange::Install(c)), Some(crate::dlss_ui::Change::Update(update)))
            if non_settings_count == install_offset + 1 =>
        {
            let comp = plan_component_install(
                &prepared.target,
                &ComponentFiles {
                    id: c.id.clone(),
                    source_root: c.root.clone(),
                    files: c.files.clone(),
                },
            )
            .map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?;
            let dlss = retoque_data::dlss::plan(&prepared.target, update).map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?;
            retoque_core::combine_install_plans(comp, dlss).map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?
        }
        (_, Some(crate::dlss_ui::Change::Update(update))) if index == install_offset => {
            retoque_data::dlss::plan(&prepared.target, update).map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?
        }
        (Some(ComponentChange::ReShade(change)), _) => change.plan(&prepared.target).map_err(|e| ApplyFailure {
            completed: index,
            detail: e.into(),
        })?,
        (Some(ComponentChange::Install(c)), _) => plan_component_install(
            &prepared.target,
            &ComponentFiles {
                id: c.id.clone(),
                source_root: c.root.clone(),
                files: c.files.clone(),
            },
        )
        .map_err(|e| ApplyFailure {
            completed: index,
            detail: e.into(),
        })?,
        (Some(ComponentChange::Remove(id)), _) => {
            plan_component_uninstall(&prepared.target, id).map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?
        }
        _ => return Ok(None),
    };
    if !steps_match(fresh.steps(), prepared.plans[index].steps()) || !fresh.refusals().is_empty() {
        return Err(ApplyFailure {
            completed: index,
            detail: "component changed since review".into(),
        });
    }
    Ok(Some(fresh))
}
pub fn apply(prepared: &Prepared, progress: &mut dyn FnMut(StepDone)) -> Result<(), ApplyFailure> {
    if prepared.plans.iter().any(|p| !p.refusals().is_empty()) || prepared.plans.is_empty() {
        return Err(ApplyFailure {
            completed: 0,
            detail: "plan refused or empty".into(),
        });
    }
    for plan in &prepared.plans {
        retoque_core::validate_plan(plan).map_err(|e| ApplyFailure {
            completed: 0,
            detail: e.into(),
        })?;
    }
    let total: usize = prepared.plans.iter().map(|p| p.steps().len()).sum();
    let mut offset = 0;
    for (index, original) in prepared.plans.iter().enumerate() {
        // A prior plan updates the receipt or INI. Rebase subsequent plans against the post-step game folder.
        let fresh = replan_intermediate(prepared, index)?;
        let plan = fresh.as_ref().unwrap_or(original);
        plan.clone()
            .into_verified()
            .and_then(|verified| {
                retoque_core::apply(&verified, &mut |mut done| {
                    done.index += offset;
                    done.total = total;
                    progress(done);
                })
            })
            .map_err(|e| ApplyFailure {
                completed: index,
                detail: e.into(),
            })?;
        offset += plan.steps().len();
    }
    Ok(())
}

#[derive(Default)]
pub struct UiState {
    pub dlss: crate::dlss_ui::State,
    pub reshade: crate::reshade_ui::State,
    pub sm86: optimize::UiState,
    pub gpus: Option<Vec<gpu::Gpu>>,
    pub detected_lossless: Option<PathBuf>,
    pub selected_component: Option<String>,
    pub catalog: Option<Result<Vec<components::Component>, AppError>>,
    pub installed: Option<Result<Vec<String>, AppError>>,
    pub readiness: Option<lsfg::LsfgReadiness>,
    pub lossless: Option<PathBuf>,
    pub facts_generation: u64,
    pub request: u64,
    pub review_title: String,
    /// Folders a reviewed tuning is writing to, with the toast that names what it did.
    pub writes: std::collections::HashMap<PathBuf, &'static str>,
    pub write_roots: std::collections::HashMap<PathBuf, Vec<PathBuf>>,
}
pub enum Event {
    Sm86Progress(u64),
    Sm86Release(Result<(sm86::Release, bool), AppError>),
    Sm86Downloaded(Result<(Vec<components::Component>, sm86::Record), AppError>),
    Environment {
        gpus: Vec<gpu::Gpu>,
        lossless: Option<PathBuf>,
        kit_cache: Option<PathBuf>,
    },
    Catalog(Result<Vec<components::Component>, AppError>),
    Facts {
        target: PathBuf,
        generation: u64,
        installed: Result<Vec<String>, AppError>,
        lossless: Option<PathBuf>,
        readiness: lsfg::LsfgReadiness,
    },
    Plan {
        target: PathBuf,
        request: u64,
        result: Result<Prepared, AppError>,
    },
}
pub fn environment() -> Event {
    Event::Environment {
        gpus: gpu::detect(),
        lossless: lsfg::find_lossless_scaling(None),
        kit_cache: std::env::current_exe()
            .ok()
            .and_then(|exe| exe.parent().map(|p| p.join("first-run-cache/sm86")))
            .filter(|p| p.join("release.json").is_file() && p.join("release.zip").is_file()),
    }
}
pub fn catalog(import: Option<PathBuf>, remove: Option<String>) -> Event {
    Event::Catalog(
        (|| {
            let c = components::ComponentCatalog::open(None)?;
            if let Some(path) = import {
                c.import_dir(&path, None)?;
            }
            if let Some(id) = remove {
                c.remove(&id)?;
            }
            let mut items = c.list()?;
            let cache = sm86::Cache::open(&sm86::default_cache()?)?;
            cache.prefer_latest(&mut items)?;
            Ok::<_, std::io::Error>(items)
        })()
        .map_err(AppError::from),
    )
}
pub fn facts(target: PathBuf, generation: u64, selected_lossless: Option<PathBuf>) -> Event {
    let ini = retoque_core::read_ini(&target)
        .ok()
        .flatten()
        .map(|(_, bytes)| retoque_core::ini::Ini::parse(&bytes));
    let configured = ini
        .as_ref()
        .and_then(|i| i.get("LSFG", "DllPath"))
        .filter(|s| !s.eq_ignore_ascii_case("auto"));
    let lossless = selected_lossless
        .or_else(|| {
            configured.map(host_path).map(|p| {
                if p.is_file()
                    && p.file_name().is_some_and(|n| {
                        let n = n.to_string_lossy();
                        n.eq_ignore_ascii_case("Lossless.dll") || n.eq_ignore_ascii_case("lsfg-vk.dll")
                    })
                {
                    p.parent().unwrap_or(&p).to_path_buf()
                } else {
                    p
                }
            })
        })
        .or_else(|| lsfg::find_lossless_scaling(None));
    let shader = ini.as_ref().and_then(|i| i.get("LSFG", "ShaderDir"));
    // Keep ShaderDir as configured. The runtime resolves relative overrides against an unknown CWD;
    // turning one into a target-relative absolute path would falsely qualify it as Ready.
    let readiness = lsfg::lsfg_readiness_for_game(lossless.as_deref(), shader, Some(&target), None);
    Event::Facts {
        installed: installed_components(&target).map_err(AppError::from),
        target,
        generation,
        lossless,
        readiness,
    }
}
fn host_path(path: &str) -> PathBuf {
    #[cfg(not(windows))]
    if path.get(..3).is_some_and(|p| p.eq_ignore_ascii_case("Z:\\")) {
        return PathBuf::from(path[2..].replace('\\', "/"));
    }
    PathBuf::from(path)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn edit(value: &str) -> Edit {
        ("LSFG".into(), "Enabled".into(), value.into())
    }
    #[test]
    fn preparing_is_read_only_and_confirming_applies_reviewed_changes() {
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        let before = "[LSFG]\nEnabled=false\n";
        std::fs::write(&ini, before).unwrap();
        let p = prepare(
            temp.path(),
            Request {
                dlss: None,
                component: None,
                edits: vec![edit("true")],
            },
        )
        .unwrap();
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
        assert_eq!(p.plans.len(), 1);
        apply(&p, &mut |_| {}).unwrap();
        assert!(std::fs::read_to_string(&ini).unwrap().contains("Enabled=true"));
    }
    #[test]
    fn stale_settings_refuse_without_overwriting_external_edit() {
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        std::fs::write(&ini, "[LSFG]\nEnabled=false\n").unwrap();
        let p = prepare(
            temp.path(),
            Request {
                dlss: None,
                component: None,
                edits: vec![edit("true")],
            },
        )
        .unwrap();
        std::fs::write(&ini, "[LSFG]\nEnabled=auto\n").unwrap();
        assert!(apply(&p, &mut |_| {}).is_err());
        assert!(std::fs::read_to_string(&ini).unwrap().contains("Enabled=auto"));
    }
    #[test]
    fn sequential_review_keeps_one_progress_sequence() {
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        std::fs::write(&ini, "[LSFG]\nEnabled=false\nMultiplier=2\n").unwrap();
        let later = vec![("LSFG".into(), "Multiplier".into(), "3".into())];
        let first = retoque_core::plan_settings(temp.path(), &[edit("true")]).unwrap();
        let second = retoque_core::plan_settings(temp.path(), &later).unwrap();
        let p = Prepared {
            target: temp.path().into(),
            request: Request {
                dlss: None,
                component: None,
                edits: later,
            },
            plans: vec![first, second],
            groups: vec![],
        };
        let mut progress = vec![];
        apply(&p, &mut |done| progress.push((done.index, done.total))).unwrap();
        assert_eq!(progress, vec![(0, 2), (1, 2)]);
        let after = std::fs::read_to_string(&ini).unwrap();
        assert!(after.contains("Enabled=true") && after.contains("Multiplier=3"));
    }

    #[test]
    fn all_plans_are_preflighted_before_the_first_write() {
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        let before = "[LSFG]\nEnabled=false\n";
        std::fs::write(&ini, before).unwrap();
        let mut p = prepare(
            temp.path(),
            Request {
                dlss: None,
                component: None,
                edits: vec![edit("true")],
            },
        )
        .unwrap();
        let missing = temp.path().join("missing");
        std::fs::create_dir(&missing).unwrap();
        p.plans
            .push(retoque_core::plan_settings(&missing, &[edit("true")]).unwrap());
        assert!(apply(&p, &mut |_| panic!("refusal in second plan must stop first")).is_err());
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
    }
    #[test]
    fn combined_settings_rebase_preserves_review_and_reports_partial_failure() {
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        std::fs::write(&ini, "[LSFG]\nEnabled=false\nMultiplier=2\n").unwrap();
        let later = vec![("LSFG".into(), "Multiplier".into(), "3".into())];
        let first = retoque_core::plan_settings(temp.path(), &[edit("true")]).unwrap();
        let second = retoque_core::plan_settings(temp.path(), &later).unwrap();
        let p = Prepared {
            target: temp.path().into(),
            request: Request {
                dlss: None,
                component: None,
                edits: later,
            },
            plans: vec![first, second],
            groups: vec![],
        };
        let result = apply(&p, &mut |_| {
            std::fs::write(&ini, "[LSFG]\nEnabled=true\nMultiplier=4\n").unwrap();
        });
        let failure = result.unwrap_err();
        assert_eq!(failure.completed, 1);
        assert!(std::fs::read_to_string(&ini).unwrap().contains("Multiplier=4"));
    }

    #[test]
    fn a_refused_plan_is_never_applied() {
        let temp = tempfile::tempdir().unwrap();
        let p = prepare(
            temp.path(),
            Request {
                dlss: None,
                component: None,
                edits: vec![edit("true")],
            },
        )
        .unwrap();
        assert!(!p.plans[0].refusals().is_empty());
        assert!(apply(&p, &mut |_| panic!("must not write")).is_err());
        assert!(!temp.path().join("OptiScaler.ini").exists());
    }
}
