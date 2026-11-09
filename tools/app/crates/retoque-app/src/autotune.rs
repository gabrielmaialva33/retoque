//! GUI orchestration: one reviewed component operation followed by settings, one writer lock.
use crate::error::AppError;
use crate::model::Edit;
use retoque_core::{Plan, Step, StepDone};
use std::path::{Path, PathBuf};
// Integration: replace these two imports with retoque_data / retoque_core exports, then delete stubs.
use retoque_core::{ComponentFiles, installed_components, plan_component_install, plan_component_uninstall};
pub use retoque_data::{components, gpu, lsfg, recommend};

#[derive(Clone, Debug)]
pub enum ComponentChange {
    Install(components::Component),
    Remove(String),
}
#[derive(Clone, Debug, Default)]
pub struct Request {
    pub component: Option<ComponentChange>,
    pub edits: Vec<Edit>,
}
#[derive(Clone)]
pub struct Prepared {
    pub target: PathBuf,
    pub request: Request,
    pub plans: Vec<Plan>,
    pub groups: Vec<Vec<usize>>,
}

pub fn prepare(target: &Path, request: Request) -> Result<Prepared, AppError> {
    let mut plans = Vec::new();
    if let Some(change) = &request.component {
        let p = match change {
            ComponentChange::Install(c) => plan_component_install(
                target,
                &ComponentFiles {
                    id: c.id.clone(),
                    source_root: c.root.clone(),
                    files: c.files.clone(),
                },
            ),
            ComponentChange::Remove(id) => plan_component_uninstall(target, id),
        }
        .map_err(AppError::from)?;
        plans.push(p);
    }
    if !request.edits.is_empty() {
        plans.push(retoque_core::plan_settings(target, &request.edits).map_err(AppError::from)?);
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
pub fn apply(prepared: &Prepared, progress: &mut dyn FnMut(StepDone)) -> Result<(), ApplyFailure> {
    if prepared.plans.iter().any(|p| !p.refusals().is_empty()) || prepared.plans.is_empty() {
        return Err(ApplyFailure {
            completed: 0,
            detail: "plan refused or empty".into(),
        });
    }
    let total: usize = prepared.plans.iter().map(|p| p.steps().len()).sum();
    let mut offset = 0;
    for (index, original) in prepared.plans.iter().enumerate() {
        // A component updates the receipt. Rebase only the subsequent INI edit, never a component source.
        let fresh;
        let plan = if index > 0 {
            fresh =
                retoque_core::plan_settings(&prepared.target, &prepared.request.edits).map_err(|e| ApplyFailure {
                    completed: index,
                    detail: e.into(),
                })?;
            if fresh.steps() != original.steps() || !fresh.refusals().is_empty() {
                return Err(ApplyFailure {
                    completed: index,
                    detail: "settings changed since review".into(),
                });
            }
            &fresh
        } else {
            original
        };
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
    pub writes: std::collections::HashSet<PathBuf>,
}
pub enum Event {
    Environment {
        gpus: Vec<gpu::Gpu>,
        lossless: Option<PathBuf>,
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
            c.list()
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
