//! Generation-scoped diagnostic snapshots. Rendering performs no diagnosis or filesystem I/O.
use crate::app::*;
use retoque_data::{
    doctor::{self, Inputs, Repair},
    logs,
    recommend::{self, Action, GameFacts},
};
use std::{
    sync::Arc,
    time::{Duration, Instant},
};

#[derive(Default)]
pub(crate) struct ViewState {
    pub latest_log: Option<Summary>,
    pub log_diagnoses: Vec<logs::LogDiagnosis>,
    pub recommendations: Arc<Vec<recommend::Recommendation>>,
    pub facts: Arc<GameFacts>,
    pub diagnose_time: Option<Duration>,
    pub watching: bool,
    pub watch_error: Option<String>,
}
pub struct Assessment {
    pub(crate) target: PathBuf,
    pub(crate) findings: Vec<doctor::Finding>,
    pub(crate) log_diagnoses: Vec<logs::LogDiagnosis>,
    pub(crate) recommendations: Arc<Vec<recommend::Recommendation>>,
    pub(crate) elapsed: Duration,
}
struct Snapshot {
    install_dir: PathBuf,
    target: PathBuf,
    status: Option<Status>,
    ini: Option<Ini>,
    model: Option<doctor::ModelInspection>,
    model_path: Option<PathBuf>,
    catalog: Vec<Model>,
    gpu: Option<retoque_data::gpu::Gpu>,
    latest: Option<Summary>,
    displayed: Option<Summary>,
    readiness: Option<retoque_data::lsfg::LsfgReadiness>,
    components: Vec<String>,
    facts: GameFacts,
    components_known: bool,
}
// The UI's legacy `present` boolean cannot distinguish permission errors from absence.
// Recheck metadata on the worker before turning that boolean into actionable diagnostic evidence.
fn inspect_presence(path: &Path) -> Option<bool> {
    match std::fs::symlink_metadata(path) {
        Ok(metadata) if metadata.is_file() => Some(true),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Some(false),
        _ => None,
    }
}
impl Snapshot {
    fn assess(mut self) -> Assessment {
        if let Some(model) = self.model.as_mut() {
            model.present = self.model_path.as_deref().and_then(inspect_presence);
        }
        let pending = std::fs::symlink_metadata(self.target.join(".retoque/pending-receipt.txt")).is_ok();
        let driver = inspect_presence(&self.target.join("_nvngx.dll"));
        let start = Instant::now();
        let fallback = Status::default();
        let mut findings = if self.status.is_some() || pending {
            let mut inputs = Inputs::new(self.status.as_ref().unwrap_or(&fallback));
            inputs.ini = self.ini.as_ref();
            inputs.pending_journal = pending;
            inputs.model = self.model.as_ref();
            inputs.catalog = &self.catalog;
            inputs.gpu = self.gpu.as_ref();
            inputs.log = self.latest.as_ref();
            inputs.lsfg = self.readiness.as_ref();
            inputs.components = &self.components;
            inputs.sm86_present = self.facts.sm86_installed;
            inputs.runtime = self.facts.runtime;
            // Discovery has no actual launcher configuration evidence. A suggested launch_option in
            // Status is not proof of the user's launcher setting; leave override_present unknown.
            inputs.proxy_loaded_last_session = self
                .latest
                .as_ref()
                .filter(|log| log.nr_initialized || log.dlss_version.is_some())
                .map(|_| true);
            inputs.driver_nvngx_present = driver;
            inputs.allow_model_fetch = true;
            if inputs.runtime == recommend::Runtime::Unknown {
                inputs.runtime = if cfg!(windows) {
                    recommend::Runtime::Windows
                } else {
                    recommend::Runtime::Proton
                };
            }
            doctor::diagnose(&inputs)
        } else {
            Vec::new()
        };
        if self.status.is_none() && pending {
            for f in &mut findings {
                if f.id == "unfinished_install" {
                    f.why_key = "doctor.journal_unreadable.why";
                }
            }
        }
        let dlss_files = retoque_data::dlss::inspect(&self.install_dir).unwrap_or_default();
        let root_incomplete = self.install_dir != self.target
            && (std::fs::symlink_metadata(self.install_dir.join(".retoque/pending-receipt.txt")).is_ok()
                || retoque_core::status(&self.install_dir)
                    .ok()
                    .and_then(|s| s.receipt)
                    .is_some_and(|r| r.status != ReceiptStatus::Complete));
        findings.extend(doctor::diagnose_dlss(&dlss_files, root_incomplete));
        findings.sort_by_key(|f| match f.severity {
            doctor::Severity::Error => 0,
            doctor::Severity::Warning => 1,
            doctor::Severity::Info => 2,
        });
        let compatibility = self
            .model
            .as_ref()
            .map_or(retoque_data::model_compat::Compatibility::Unknown, |m| {
                m.compatibility.for_gpu(self.gpu.as_ref())
            });
        let log_diagnoses = self
            .displayed
            .as_ref()
            .map(|log| logs::diagnose(log, self.facts.runtime, compatibility, driver))
            .unwrap_or_default();
        let recommendations = if self.status.is_some() && self.components_known {
            recommend::recommend(self.gpu.as_ref(), &self.facts)
        } else {
            Vec::new()
        };
        Assessment {
            target: self.target,
            findings,
            log_diagnoses,
            recommendations: Arc::new(recommendations),
            elapsed: start.elapsed(),
        }
    }
}
impl RetoqueApp {
    pub(crate) fn refresh_intelligence(&mut self) {
        let Some(target) = self.view.target.clone() else { return };
        let facts = self.autotune_game_facts();
        self.view.intelligence.facts = Arc::new(facts.clone());
        let snapshot = Snapshot {
            install_dir: self.dlss_install_dir().unwrap_or_else(|| target.clone()),
            target: target.clone(),
            status: self.view.status.as_ref().and_then(|s| s.as_ref().ok()).cloned(),
            ini: self
                .view
                .ini
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .and_then(|i| i.as_ref())
                .cloned(),
            // Pending inspection is unknown, not the adjacent-model fallback from Status.
            model: Some(
                self.view
                    .inspection
                    .as_ref()
                    .map(|m| doctor::ModelInspection {
                        present: m.path.as_ref().map(|_| m.present),
                        compatibility: m.compatibility.clone(),
                    })
                    .unwrap_or_default(),
            ),
            model_path: self.view.inspection.as_ref().and_then(|m| m.path.clone()),
            catalog: self
                .catalog
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .cloned()
                .unwrap_or_default(),
            // More than one adapter is not proof of the GPU this game uses.
            gpu: self
                .tuning
                .gpus
                .as_ref()
                .filter(|g| g.len() == 1)
                .and_then(|g| g.first())
                .cloned(),
            latest: self.view.intelligence.latest_log.clone(),
            displayed: self
                .view
                .logs
                .as_ref()
                .and_then(|(_, r)| r.as_ref().ok())
                .and_then(|l| l.as_ref())
                .cloned(),
            readiness: self.tuning.readiness.clone(),
            components: self
                .tuning
                .installed
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .cloned()
                .unwrap_or_default(),
            facts,
            components_known: matches!(self.tuning.installed, Some(Ok(_))),
        };
        self.jobs
            .spawn("doctor", None, move |_| Outcome::Doctor(Box::new(snapshot.assess())));
    }
    pub(crate) fn receive_assessment(&mut self, assessment: Assessment) {
        if self.view.target.as_ref() != Some(&assessment.target) {
            return;
        }
        self.doctor_badges.insert(assessment.target, assessment.findings.len());
        self.view.doctor = Some(crate::tabs::doctor::DoctorCard {
            findings: assessment.findings,
        });
        self.view.intelligence.log_diagnoses = assessment.log_diagnoses;
        self.view.intelligence.recommendations = assessment.recommendations;
        self.view.intelligence.diagnose_time = Some(assessment.elapsed);
    }
    pub(crate) fn prepare_repair(&mut self, target: PathBuf, repair: Repair) {
        if self.view.target.as_ref() != Some(&target) || self.busy() {
            return;
        }
        match repair {
            Repair::UpdateDlss => self.dlss_request_update(),
            Repair::RestoreDlss => self.dlss_restore(),
            Repair::Settings(actions) => {
                let edits: Option<Vec<_>> = actions
                    .into_iter()
                    .map(|a| match a {
                        Action::SetIni { section, key, value } => Some((section, key, value)),
                        _ => None,
                    })
                    .collect();
                if let Some(edits) = edits {
                    self.start_plan(PlanKind::Settings, PlanInputs::Settings(edits));
                }
            }
            Repair::Reinstall => {
                if let Some(options) = self.install_inputs() {
                    self.start_plan(PlanKind::Install, PlanInputs::Install(options));
                }
            }
            Repair::ReconcileInstallation => self.start_plan(PlanKind::Uninstall, PlanInputs::Uninstall),
            Repair::RemoveComponent { id } => self.autotune_request(
                self.tr("doctor.prepare").into(),
                crate::autotune::Request {
                    dlss: None,
                    component: Some(crate::autotune::ComponentChange::Remove(id)),
                    edits: vec![],
                },
            ),
            Repair::FetchCompatibleModel { sha256 } => {
                self.nr_fetch_repair(target, &sha256);
            }
        }
    }
    pub(crate) fn receive_watch(&mut self, target: PathBuf, result: Result<crate::watcher::Changes, String>) {
        if self.view.target.as_ref() != Some(&target) {
            return;
        }
        match result {
            Err(error) => {
                self.view.intelligence.watching = false;
                self.view.intelligence.watch_error = Some(error);
            }
            Ok(changes) => {
                self.view.intelligence.watching = true;
                self.view.intelligence.watch_error = None;
                if self.busy() {
                    return;
                } // apply always explicitly reloads its completed writes
                if changes.status {
                    let t = target.clone();
                    self.jobs.spawn("status", None, move |_| model::status(t));
                    self.refresh_model_inspection();
                    self.autotune_refresh(target.clone());
                }
                if changes.ini {
                    let t = target.clone();
                    self.jobs.spawn("ini", None, move |_| model::read_ini(t));
                    self.autotune_refresh(target.clone());
                }
                if changes.logs || changes.ini {
                    let t = target.clone();
                    self.jobs.spawn("logs", None, move |_| model::logs(t));
                }
                if changes.captures {
                    self.compare.invalidate_captures(&target);
                }
            }
        }
    }
    pub(crate) fn watcher_notice(&self, ui: &mut egui::Ui) {
        if let Some(error) = &self.view.intelligence.watch_error {
            ui.horizontal_wrapped(|ui| {
                ui.colored_label(theme::WARN, self.tr("watch.manual"))
                    .on_hover_text(error);
                if ui.button(self.tr("watch.refresh")).clicked() {
                    self.cmd_tx.send(AppCommand::RefreshSelected);
                }
            });
        }
    }
}

#[cfg(test)]
mod tests;
