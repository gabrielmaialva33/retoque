//! Consent, background download/import, and a single review for automatic optimization.
use crate::app::*;
use crate::autotune::{
    self, ComponentChange, Event,
    optimize::{self, Consent, Pending, Prerequisite},
    sm86,
};
use std::sync::{Arc, Mutex};

type PendingRecs = (PathBuf, u64, Arc<Vec<autotune::recommend::Recommendation>>);
static PENDING_RECS: Mutex<Option<PendingRecs>> = Mutex::new(None);

impl RetoqueApp {
    pub(crate) fn sm86_dialog(&mut self, ctx: &egui::Context) {
        let eligible = self.tuning.gpus.as_deref().is_some_and(optimize::first_run_eligible);
        if eligible && !self.saved.sm86_offer_seen && matches!(&self.tuning.catalog, Some(Ok(c)) if c.is_empty()) {
            self.saved.sm86_offer_seen = true;
            self.tuning.sm86.consent = Some(Consent::Download);
        }
        let Some(consent) = self.tuning.sm86.consent else {
            return;
        };
        let mut confirm = false;
        let mut cancel = false;
        egui::Modal::new(egui::Id::unique("sm86-consent")).show(ctx, |ui| {
            ui.set_max_width(520.);
            ui.heading(self.tr("sm86.consent_title"));
            ui.label(self.tr(if matches!(consent, Consent::Check) {
                "sm86.check_consent"
            } else {
                "sm86.download_consent"
            }));
            if self.tuning.sm86.kit_cache.is_some() && matches!(consent, Consent::Download) {
                ui.label(self.tr("sm86.kit_available"));
            }
            ui.label(self.tr("sm86.license"));
            ui.horizontal(|ui| {
                confirm = ui.button(self.tr("sm86.confirm")).clicked();
                cancel = ui.button(self.tr("sm86.later")).clicked();
            });
        });
        if cancel {
            self.tuning.sm86.consent = None;
            self.tuning.sm86.pending = None;
        }
        if confirm {
            self.tuning.sm86.consent = None;
            self.sm86_start(consent);
        }
    }
    fn sm86_start(&mut self, consent: Consent) {
        if self.tuning.sm86.busy {
            return;
        }
        self.tuning.sm86.busy = true;
        self.tuning.sm86.error = None;
        self.tuning.sm86.downloaded = 0;
        let kit = self.tuning.sm86.kit_cache.clone();
        // "Check for update" always queries latest. Subsequent download uses that exact reviewed metadata.
        let checked = self.tuning.sm86.release.clone();
        let ctx = self.context.clone();
        self.jobs.spawn("sm86-download", None, move |tx| {
            let result = (|| -> Result<_, AppError> {
                let cache = sm86::Cache::open(&sm86::default_cache()?)?;
                let catalog = autotune::components::ComponentCatalog::open(None)?;
                if matches!(consent, Consent::Check) {
                    let release = sm86::latest(&sm86::GithubFetcher)?;
                    let available = cache.contains(&release, &catalog.list()?)?;
                    return Ok(Event::Sm86Release(Ok((release, available))));
                }
                let (_, record) = if let Some(kit) = kit.filter(|_| checked.is_none()) {
                    sm86::import_first_run_cache(&kit, &cache, &catalog)?
                } else {
                    let release = match checked {
                        Some(r) => r,
                        None => sm86::latest(&sm86::GithubFetcher)?,
                    };
                    let mut previous = 0;
                    cache.download(&sm86::GithubFetcher, &release, &catalog, &mut |bytes| {
                        if bytes.saturating_sub(previous) >= 256 * 1024 || bytes == release.size {
                            previous = bytes;
                            let _ = tx.send(Outcome::Auto(Box::new(Event::Sm86Progress(bytes))));
                            ctx.request_repaint();
                        }
                    })?
                };
                let mut items = catalog.list()?;
                cache.prefer_latest(&mut items)?;
                Ok(Event::Sm86Downloaded(Ok((items, record))))
            })();
            Outcome::Auto(Box::new(match result {
                Ok(event) => event,
                Err(error) => match consent {
                    Consent::Check => Event::Sm86Release(Err(error)),
                    Consent::Download => Event::Sm86Downloaded(Err(error)),
                },
            }))
        });
    }
    pub(crate) fn sm86_downloaded(
        &mut self,
        result: Result<(Vec<autotune::components::Component>, sm86::Record), AppError>,
    ) {
        self.tuning.sm86.busy = false;
        match result {
            Ok((items, record)) => {
                let component = items.iter().find(|c| c.id == record.component_id).cloned();
                self.tuning.selected_component = Some(record.component_id.clone());
                self.tuning.sm86.release = Some(record.release.clone());
                self.tuning.sm86.record = Some(record);
                self.tuning.sm86.available = true;
                self.tuning.catalog = Some(Ok(items));
                if let Some(mut pending) = self.tuning.sm86.pending.take()
                    && self.view.target.as_ref() == Some(&pending.target)
                    && self.tuning.facts_generation == pending.generation
                    && self.next_request == pending.review_request
                    && let Some(component) = component
                {
                    pending.request.component = Some(ComponentChange::Install(component));
                    self.autotune_request(self.tr("optimize.title").into(), pending.request);
                }
            }
            Err(error) => {
                self.tuning.sm86.error = Some(error);
                self.tuning.sm86.pending = None;
            }
        }
    }
    pub(crate) fn sm86_download_controls(&mut self, ui: &mut egui::Ui) {
        ui.label(self.tr("sm86.download_help"));
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(!self.tuning.sm86.busy, egui::Button::new(self.tr("sm86.download")))
                .clicked()
            {
                self.tuning.sm86.consent = Some(Consent::Download);
            }
            if ui
                .add_enabled(!self.tuning.sm86.busy, egui::Button::new(self.tr("sm86.check")))
                .clicked()
            {
                self.tuning.sm86.consent = Some(Consent::Check);
            }
        });
        if self.tuning.sm86.busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(format!(
                    "{} · {:.1} MiB",
                    self.tr("sm86.progress"),
                    self.tuning.sm86.downloaded as f64 / 1048576.
                ));
            });
        }
        if let Some(release) = &self.tuning.sm86.release {
            ui.label(format!("{} · {}", release.version, release.published_at));
            ui.label(self.tr(if self.tuning.sm86.available {
                "sm86.current"
            } else {
                "sm86.new"
            }));
        }
        if let Some(record) = &self.tuning.sm86.record {
            ui.label(format!("SHA-256: {}", record.sha256));
            ui.collapsing(self.tr("sm86.ignored"), |ui| {
                for name in &record.ignored {
                    ui.label(name);
                }
            });
        }
        if let Some(error) = &self.tuning.sm86.error {
            ui.colored_label(theme::BAD, crate::i18n::diagnostic(self.lang, error));
        }
    }
    pub(crate) fn optimize_controls(&mut self, ui: &mut egui::Ui) {
        self.dlss_optimize_checkbox(ui);
        let recs = self.view.intelligence.recommendations.clone();
        let target = self.view.target.as_ref();
        let mut experimental = target.is_some() && self.tuning.sm86.experimental_for.as_ref() == target;
        if recs
            .iter()
            .any(|r| r.confidence == autotune::recommend::Confidence::ExperimentalUnderProton)
            && ui
                .checkbox(&mut experimental, self.tr("optimize.experimental"))
                .changed()
        {
            self.tuning.sm86.experimental_for = if experimental { target.cloned() } else { None };
        }
        let mut lsfg = target.is_some() && self.tuning.sm86.lsfg_for.as_ref() == target;
        if recs
            .iter()
            .any(|r| r.needs == Some(autotune::recommend::Need::LosslessScaling))
            && ui.checkbox(&mut lsfg, self.tr("optimize.choose_lsfg")).changed()
        {
            self.tuning.sm86.lsfg_for = if lsfg { target.cloned() } else { None };
        }
        if ui
            .add_enabled(
                target.is_some()
                    && !self.busy()
                    && !self.tuning.sm86.busy
                    && !self.nr_fetch.busy
                    && self.tuning.sm86.pending.is_none(),
                theme::primary(self.tr("optimize.title")),
            )
            .clicked()
        {
            self.optimize_automatically();
        }
        if self.tuning.sm86.busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(self.tr("sm86.progress"));
            });
        }
        if self.nr_fetch.busy {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(self.tr("nr_fetch.downloading"));
            });
        }
        if let Some(error) = &self.tuning.sm86.error {
            ui.colored_label(theme::BAD, crate::i18n::diagnostic(self.lang, error));
        }
    }
    pub(crate) fn optimize_automatically(&mut self) {
        let Some(target) = self.view.target.clone() else { return };
        if self.view.intelligence.recommendations.is_empty()
            && let Ok(mut guard) = PENDING_RECS.lock()
            && let Some((saved_target, saved_gen, saved_recs)) = guard.take()
            && saved_target == target
            && saved_gen == self.tuning.facts_generation
        {
            self.view.intelligence.recommendations = saved_recs;
        }
        let component = self.autotune_selected_component();
        let models = self
            .catalog
            .as_ref()
            .and_then(|c| c.as_ref().ok())
            .map(Vec::as_slice)
            .unwrap_or(&[]);
        let gpu = self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
        let nr_enabled = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(|i| i.as_ref())
            .and_then(|i| i.get("DlssNr", "Enabled"))
            .map(|s| !matches!(s.trim().to_ascii_lowercase().as_str(), "false" | "0"))
            .or_else(|| {
                self.view
                    .status
                    .as_ref()
                    .and_then(|r| r.as_ref().ok())
                    .and_then(|s| s.nr_enabled)
            })
            .unwrap_or(true);
        let model_incompatible =
            self.view.inspection.as_ref().is_some_and(|m| {
                m.compatibility.for_gpu(gpu) == retoque_data::model_compat::Compatibility::Incompatible
            });
        let model_missing = nr_enabled
            && self.view.inspection.as_ref().is_some_and(|m| {
                !m.present
                    && (m.path.is_some()
                        || self
                            .view
                            .ini
                            .as_ref()
                            .and_then(|r| r.as_ref().ok())
                            .and_then(|i| i.as_ref())
                            .and_then(|i| i.get("DlssNr", "ModelFile"))
                            .is_none_or(|v| v.trim().is_empty() || v.trim().eq_ignore_ascii_case("auto")))
            });
        let (model, need_nr_model) = if model_incompatible || model_missing {
            let local = models
                .iter()
                .find(|m| m.compatibility.for_gpu(gpu) == retoque_data::model_compat::Compatibility::Compatible);
            if local.is_some() {
                (local, false)
            } else {
                (
                    None,
                    retoque_data::model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu).is_some(),
                )
            }
        } else {
            (None, false)
        };
        let wine = self.view.intelligence.facts.runtime.is_wine();
        let model_edit = model.map(|m| ("DlssNr".into(), "ModelFile".into(), Catalog::model_file_value(m, wine)));
        let mut composed = optimize::compose(optimize::Selection {
            recommendations: &self.view.intelligence.recommendations,
            component: component.as_ref(),
            installed_ids: self
                .tuning
                .installed
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .map(Vec::as_slice)
                .unwrap_or(&[]),
            model_edit,
            need_nr_model,
            include_experimental: self.tuning.sm86.experimental_for.as_ref() == Some(&target),
            choose_lsfg: self.tuning.sm86.lsfg_for.as_ref() == Some(&target),
            lsfg_ready: self.autotune_ready(),
            lsfg_edits: self.autotune_lsfg_edits(2, &self.view.intelligence.facts),
        });
        if self.tuning.dlss.selected_for.as_ref() == Some(&target)
            && let Ok(c) = &mut composed
        {
            c.request.dlss = self.dlss_update_request();
            if c.request.dlss.is_none() {
                self.tuning.sm86.error = Some(self.tr("dlss.download_first").into());
                return;
            }
        }
        self.tuning.sm86.error = None;
        match composed {
            Err(e) => self.tuning.sm86.error = Some(self.tr(e).into()),
            Ok(c) if c.prerequisites.contains(&Prerequisite::NrModel) => {
                if let Ok(mut guard) = PENDING_RECS.lock() {
                    *guard = Some((
                        target.clone(),
                        self.tuning.facts_generation,
                        self.view.intelligence.recommendations.clone(),
                    ));
                }
                if !self.nr_fetch_prepare_for_optimize(target) {
                    self.tuning.sm86.error = Some(self.tr(Prerequisite::NrModel.unavailable_key()).into());
                }
            }
            Ok(c) if c.prerequisites.contains(&Prerequisite::LsfgShaders) => {
                // Shaders are prepared in app data first; the optimization resumes when they are ready
                // (lsfg_ui::lsfg_event). Nothing is applied to the game before that single review.
                if let Ok(mut guard) = PENDING_RECS.lock() {
                    *guard = Some((
                        target.clone(),
                        self.tuning.facts_generation,
                        self.view.intelligence.recommendations.clone(),
                    ));
                }
                if self.lsfg_prepare_for_optimize(target) {
                    self.tuning.sm86.error = Some(self.tr("optimize.lsfg_preparing").into());
                } else {
                    self.tuning.sm86.error = Some(self.tr(Prerequisite::LsfgShaders.unavailable_key()).into());
                }
            }
            Ok(c) if c.prerequisites.contains(&Prerequisite::Sm86) => {
                self.tuning.sm86.pending = Some(Pending {
                    target,
                    generation: self.tuning.facts_generation,
                    review_request: self.next_request,
                    request: c.request,
                });
                self.tuning.sm86.consent = Some(Consent::Download);
            }
            Ok(c) if c.request.component.is_some() || c.request.dlss.is_some() || !c.request.edits.is_empty() => {
                self.autotune_request(self.tr("optimize.title").into(), c.request)
            }
            Ok(c) => {
                self.tuning.sm86.error = Some(
                    self.tr(if c.experimental_omitted {
                        "optimize.experimental_omitted"
                    } else {
                        "optimize.nothing"
                    })
                    .into(),
                )
            }
        }
    }
}
