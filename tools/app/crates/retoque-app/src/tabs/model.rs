use crate::app::*;
use retoque_data::model_fetch::{self, RemoteModel};
use std::sync::Arc;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum NrConsent {
    Download(RemoteModel),
    RefreshCatalog,
}

pub enum NrFetchEvent {
    Progress(u64),
    CatalogRefreshed(Result<Vec<RemoteModel>, AppError>),
    Downloaded(Result<Model, AppError>),
}

pub type SharedModelFetcher = Arc<dyn model_fetch::Fetcher + Send + Sync>;

pub struct NrFetchState {
    pub entries: Vec<RemoteModel>,
    pub consent: Option<NrConsent>,
    pub busy: bool,
    pub downloaded: u64,
    pub error: Option<AppError>,
    pub select_for_target: Option<PathBuf>,
    pub then_optimize: Option<(PathBuf, u64, u64)>,
    pub catalog_root: Option<PathBuf>,
    pub fetcher: Option<SharedModelFetcher>,
}

impl Default for NrFetchState {
    fn default() -> Self {
        Self {
            entries: model_fetch::pinned_entries(),
            consent: None,
            busy: false,
            downloaded: 0,
            error: None,
            select_for_target: None,
            then_optimize: None,
            catalog_root: None,
            fetcher: None,
        }
    }
}

impl RetoqueApp {
    pub(crate) fn model_tab(&self, ui: &mut egui::Ui, commands: &CommandSender) {
        if self.view.target.is_none() {
            return;
        }
        let ini = match &self.view.ini {
            Some(Ok(Some(ini))) => Some(ini.clone()),
            _ => None,
        };
        let current = ini
            .as_ref()
            .and_then(|i| i.get("DlssNr", "ModelFile").map(str::to_string));
        let wine = cfg!(not(windows));
        let busy = self.busy();
        let is_auto = current
            .as_deref()
            .is_none_or(|v| v.trim().is_empty() || v.trim().eq_ignore_ascii_case("auto"));
        let mut action: Option<String> = None;
        // The model waiting for "Apagar" in the delete confirm: (sha256, label).
        let pending_delete = egui::Id::unique("model-delete-pending");
        if ini.is_none() {
            match &self.view.ini {
                None => {
                    ui.spinner();
                    ui.label(self.tr("tab.loading"));
                }
                Some(Err(e)) => {
                    crate::error::card(ui, e, self.lang, &self.cmd_tx);
                }
                _ => self.tab_install_prompt(ui),
            }
            ui.add_space(12.0);
            self.nr_fetch_card(ui);
            return;
        }
        let inspection = self.view.inspection.clone();
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("mdl.current"));
            match &inspection {
                None => {
                    ui.spinner();
                    ui.label(self.tr("mdl.loading"));
                }
                Some(ModelInspection {
                    path: Some(path),
                    present: true,
                    version,
                    ..
                }) => {
                    ui.label(
                        RichText::new(version.as_deref().unwrap_or(self.tr("mdl.version_unknown")))
                            .size(22.0)
                            .strong(),
                    );
                    let in_catalog = self
                        .catalog
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .is_some_and(|models| {
                            let key = crate::format::path_key(path);
                            models.iter().any(|m| crate::format::path_key(&m.path) == key)
                        });
                    let origin = if is_auto {
                        "mdl.beside"
                    } else if in_catalog {
                        "mdl.from_catalog"
                    } else {
                        "mdl.from_path"
                    };
                    ui.label(RichText::new(self.tr(origin)).color(theme::GOOD));
                    ui.add(
                        egui::Label::new(
                            RichText::new(path.file_name().unwrap_or_default().to_string_lossy()).monospace(),
                        )
                        .truncate(),
                    )
                    .on_hover_text(crate::format::display_path(path));
                }
                Some(info) => {
                    let key = if is_auto {
                        "mdl.auto_unresolved"
                    } else if info.path.is_none() {
                        "mdl.unresolved"
                    } else {
                        "mdl.missing_file"
                    };
                    ui.colored_label(
                        if is_auto || info.path.is_none() {
                            theme::WARN
                        } else {
                            theme::BAD
                        },
                        self.tr(key),
                    );
                }
            }
            self.nr_compatibility_notice(ui);
            ui.add_space(6.0);
            ui.label(RichText::new(self.tr("mdl.next_start")).small().color(theme::MUTED));
            ui.horizontal_wrapped(|ui| {
                if !is_auto
                    && ui
                        .add_enabled(!busy, egui::Button::new(self.tr("mdl.use_auto")))
                        .clicked()
                {
                    action = Some("auto".into());
                }
                if ui.button(self.tr("mdl.refresh")).clicked() {
                    commands.send(AppCommand::RefreshModel);
                }
            });
        });
        ui.add_space(16.0);
        ui.label(RichText::new(self.tr("mdl.catalog")).size(18.0).strong());
        self.nr_fetch_card(ui);
        self.model_import_area(ui);
        let models = match &self.catalog {
            None => {
                ui.spinner();
                return;
            }
            Some(Err(e)) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
                return;
            }
            Some(Ok(m)) => m.clone(),
        };
        if models.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr("mdl.catalog_empty"));
                let key = if inspection.as_ref().is_some_and(|i| i.present) {
                    "mdl.optional"
                } else {
                    "mdl.empty"
                };
                ui.label(RichText::new(self.tr(key)).color(theme::MUTED));
            });
        }
        for m in &models {
            let value = Catalog::model_file_value(m, wine);
            // The file this game will load, however its ModelFile is spelled (plain, extended-length or from an
            // older build): a model in use can never be offered for removal.
            let in_use = current.as_deref() == Some(value.as_str())
                || inspection.as_ref().and_then(|i| i.path.as_ref()).is_some_and(|p| {
                    !current.as_deref().unwrap_or("auto").eq_ignore_ascii_case("auto")
                        && crate::format::path_key(p) == crate::format::path_key(&m.path)
                });
            let used = self.model_usage.get(&m.path).copied();
            widgets::card(ui, |ui| {
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new(&m.label).size(16.0).strong());
                    if in_use {
                        widgets::chip(ui, self.tr("mdl.in_use"), theme::GOOD);
                    }
                });
                ui.horizontal_wrapped(|ui| {
                    let version = m.version.as_deref().unwrap_or(self.tr("unknown"));
                    ui.label(RichText::new(format!("{} {version}", self.tr("mdl.version"))).color(theme::MUTED));
                    ui.label(
                        RichText::new(&m.sha256[..m.sha256.len().min(12)])
                            .small()
                            .monospace()
                            .color(theme::MUTED),
                    )
                    .on_hover_text(&m.sha256);
                    ui.label(
                        RichText::new(format!("{:.1} {}", m.size as f64 / 1_048_576.0, self.tr("unit.mib")))
                            .small()
                            .color(theme::MUTED),
                    );
                });
                self.model_compatibility_label(ui, &m.compatibility);
                if let Some(count) = used {
                    ui.label(
                        RichText::new(format!("{count} {}", self.tr("mdl.used_games")))
                            .small()
                            .color(theme::MUTED),
                    );
                }
                ui.add_space(6.0);
                ui.horizontal_wrapped(|ui| {
                    if !in_use {
                        let use_here = ui.add_enabled(ini.is_some() && !busy, egui::Button::new(self.tr("mdl.use")));
                        widgets::name_after(&use_here, self.tr("mdl.use"), &m.label);
                        if use_here.clicked() {
                            action = Some(value.clone());
                        }
                    }
                    let delete = ui
                        .add_enabled(!in_use, egui::Button::new(self.tr("mdl.remove")))
                        .on_hover_text(self.tr("mdl.remove_help"));
                    widgets::name_after(&delete, self.tr("mdl.remove"), &m.label);
                    if delete.clicked() {
                        let pending = (m.sha256.clone(), m.label.clone());
                        ui.ctx().data_mut(|d| d.insert_temp(pending_delete, pending));
                    }
                });
            });
            ui.add_space(10.0);
        }
        if let Some(v) = action {
            let edit = ("DlssNr".to_string(), "ModelFile".to_string(), v);
            self.request_plan(PlanKind::Settings, PlanInputs::Settings(vec![edit]));
        }
        // Deleting the app's copy cannot be undone, so it asks first.
        if let Some((sha, label)) = ui.ctx().data(|d| d.get_temp::<(String, String)>(pending_delete)) {
            let answer = widgets::delete_modal(
                ui.ctx(),
                self.lang,
                "model",
                self.tr("mdl.remove_title"),
                &label,
                self.tr("mdl.remove_help"),
            );
            if answer.is_some() {
                ui.ctx().data_mut(|d| d.remove::<(String, String)>(pending_delete));
            }
            if answer == Some(widgets::Answer::Confirm) {
                commands.send(AppCommand::RemoveModel(sha));
            }
        }
        self.settings_plan(ui);
    }

    pub(crate) fn model_import_area(&self, ui: &mut egui::Ui) {
        let pending_id = egui::Id::unique("model-import-pending");
        let frame = egui::Frame::new()
            .inner_margin(16.0)
            .corner_radius(12)
            .fill(theme::CARD);
        let area = frame.show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(RichText::new(self.tr("mdl.drop")).color(theme::MUTED));
            if ui.button(self.tr("mdl.import")).clicked()
                && let Some(path) = rfd::FileDialog::new()
                    .add_filter(self.tr("mdl.dll_filter"), &["dll"])
                    .pick_file()
            {
                ui.ctx().data_mut(|data| data.insert_temp(pending_id, path));
            }
        });
        let border = area.response.rect.shrink(1.0);
        let stroke = egui::Stroke::new(1.0, theme::MUTED);
        for (from, to) in [
            (border.left_top(), border.right_top()),
            (border.right_top(), border.right_bottom()),
            (border.right_bottom(), border.left_bottom()),
            (border.left_bottom(), border.left_top()),
        ] {
            let delta = to - from;
            let length = delta.length();
            let direction = delta / length.max(1.0);
            let mut offset = 8.0;
            while offset < length - 8.0 {
                ui.painter().line_segment(
                    [
                        from + direction * offset,
                        from + direction * (offset + 5.0).min(length - 8.0),
                    ],
                    stroke,
                );
                offset += 10.0;
            }
        }
        // Dropping previews an import, just like selecting a file; it does not silently copy anything.
        if ui
            .ctx()
            .pointer_latest_pos()
            .is_some_and(|p| area.response.rect.contains(p))
        {
            let dropped = ui
                .ctx()
                .input(|i| i.raw.dropped_files.first().map(|file| file.path().to_path_buf()));
            if let Some(path) = dropped {
                ui.ctx().data_mut(|data| data.insert_temp(pending_id, path));
            }
        }
        if let Some(path) = ui.ctx().data(|data| data.get_temp::<PathBuf>(pending_id)) {
            ui.label(self.tr("mdl.import_review"));
            ui.add(egui::Label::new(path.file_name().unwrap_or_default().to_string_lossy()).truncate());
            ui.horizontal(|ui| {
                if ui
                    .add_enabled(!self.busy(), theme::primary(self.tr("mdl.confirm_import")))
                    .clicked()
                {
                    ui.ctx().data_mut(|data| data.remove::<PathBuf>(pending_id));
                    self.cmd_tx.send(AppCommand::ImportModel(path));
                }
                if ui.button(self.tr("cancel")).clicked() {
                    ui.ctx().data_mut(|data| data.remove::<PathBuf>(pending_id));
                }
            });
        }
        ui.add_space(12.0);
    }

    pub(crate) fn model_compatibility_label(
        &self,
        ui: &mut egui::Ui,
        info: &retoque_data::model_compat::ModelCompatibility,
    ) {
        use retoque_data::model_compat::Compatibility;
        let gpu = self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
        let (key, color) = match info.for_gpu(gpu) {
            Compatibility::Incompatible => ("mdl.incompatible", theme::WARN),
            Compatibility::Compatible => ("mdl.arch_match", theme::MUTED),
            Compatibility::Unknown => ("mdl.compat_unknown", theme::WARN),
        };
        ui.add(egui::Label::new(RichText::new(self.tr(key)).color(color)).wrap());
        if !info.archs.is_empty() {
            let archs = info
                .archs
                .iter()
                .map(|sm| format!("sm_{sm}"))
                .collect::<Vec<_>>()
                .join(", ");
            ui.label(format!("{} {archs}", self.tr("mdl.archs")));
        }
    }

    pub(crate) fn nr_compatibility_notice(&self, ui: &mut egui::Ui) {
        let Some(info) = self.view.inspection.as_ref() else {
            return;
        };
        self.model_compatibility_label(ui, &info.compatibility);
        let models = self
            .catalog
            .as_ref()
            .and_then(|c| c.as_ref().ok())
            .map(Vec::as_slice)
            .unwrap_or(&[]);
        let (_, alternative) = retoque_data::recommend::nr_model(
            &info.compatibility,
            self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary),
            models,
        );
        if let Some(model) = alternative {
            ui.label(format!("{} {}", self.tr("mdl.suggest"), model.label));
            let value = Catalog::model_file_value(model, cfg!(not(windows)));
            if ui
                .add_enabled(!self.busy(), egui::Button::new(self.tr("mdl.use_suggested")))
                .clicked()
            {
                self.request_plan(
                    PlanKind::Settings,
                    PlanInputs::Settings(vec![("DlssNr".into(), "ModelFile".into(), value)]),
                );
            }
        } else if self.tab != Tab::Model && !self.doctor_has_model_fetch() {
            let gpu = self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
            let missing_or_incompatible = (!info.present && info.path.is_some())
                || info.compatibility.for_gpu(gpu) == retoque_data::model_compat::Compatibility::Incompatible;
            if missing_or_incompatible && let Some(remote) = model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu) {
                ui.label(format!(
                    "{}: {} ({:.1} {})",
                    self.tr("nr_fetch.title"),
                    remote.label,
                    remote.size as f64 / 1_048_576.0,
                    self.tr("unit.mib")
                ));
                if ui
                    .add_enabled(
                        !self.busy() && !self.nr_fetch.busy,
                        egui::Button::new(self.tr("nr_fetch.get_compatible")),
                    )
                    .clicked()
                {
                    self.cmd_tx
                        .send(AppCommand::PromptFetchCompatibleModel(self.view.target.clone()));
                }
            }
        }
    }

    pub(crate) fn doctor_has_model_fetch(&self) -> bool {
        self.view.doctor.as_ref().is_some_and(|card| {
            card.findings.iter().any(|f| {
                matches!(
                    f.repair,
                    Some(retoque_data::doctor::Repair::FetchCompatibleModel { .. })
                )
            })
        })
    }

    pub(crate) fn nr_fetch_single_gpu(&self) -> Option<&retoque_data::gpu::Gpu> {
        self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary)
    }

    pub(crate) fn nr_fetch_card(&self, ui: &mut egui::Ui) {
        let gpu = self.nr_fetch_single_gpu();
        let compatible = model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu);
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("nr_fetch.title"));
            ui.add(egui::Label::new(RichText::new(self.tr("nr_fetch.source_note")).color(theme::MUTED)).wrap());
            ui.add_space(6.0);
            if let Some(entry) = compatible {
                ui.label(
                    RichText::new(format!(
                        "{} · {:.1} {} · SHA-256 {}",
                        entry.label,
                        entry.size as f64 / 1_048_576.0,
                        self.tr("unit.mib"),
                        &entry.sha256[..entry.sha256.len().min(12)]
                    ))
                    .strong(),
                );
                self.model_compatibility_label(ui, &entry.compatibility);
            } else {
                ui.colored_label(theme::WARN, self.tr("nr_fetch.no_compatible_gpu"));
            }
            ui.add_space(6.0);
            ui.horizontal_wrapped(|ui| {
                if ui
                    .add_enabled(
                        compatible.is_some() && !self.busy() && !self.nr_fetch.busy,
                        theme::primary(self.tr("nr_fetch.get_compatible")),
                    )
                    .clicked()
                {
                    self.cmd_tx
                        .send(AppCommand::PromptFetchCompatibleModel(self.view.target.clone()));
                }
                if ui
                    .add_enabled(
                        !self.nr_fetch.busy,
                        egui::Button::new(self.tr("nr_fetch.refresh_catalog")),
                    )
                    .clicked()
                {
                    self.cmd_tx.send(AppCommand::PromptRefreshModelCatalog);
                }
            });
            if self.nr_fetch.busy {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(format!(
                        "{} · {:.1} {}",
                        self.tr("nr_fetch.downloading"),
                        self.nr_fetch.downloaded as f64 / 1_048_576.0,
                        self.tr("unit.mib")
                    ));
                });
            }
            if let Some(error) = &self.nr_fetch.error {
                ui.colored_label(theme::BAD, crate::i18n::diagnostic(self.lang, error));
            }
            ui.collapsing(self.tr("nr_fetch.available_variants"), |ui| {
                for entry in &self.nr_fetch.entries {
                    ui.push_id(&entry.sha256, |ui| {
                        ui.horizontal_wrapped(|ui| {
                            ui.strong(&entry.label);
                            ui.label(format!(
                                "{:.1} {} · {}",
                                entry.size as f64 / 1_048_576.0,
                                self.tr("unit.mib"),
                                &entry.sha256[..entry.sha256.len().min(12)]
                            ));
                            if ui
                                .add_enabled(
                                    !self.busy() && !self.nr_fetch.busy,
                                    egui::Button::new(self.tr("nr_fetch.download_variant")),
                                )
                                .clicked()
                            {
                                self.cmd_tx
                                    .send(AppCommand::PromptFetchModel(entry.clone(), self.view.target.clone()));
                            }
                        });
                        self.model_compatibility_label(ui, &entry.compatibility);
                        ui.add_space(4.0);
                    });
                }
            });
        });
        ui.add_space(12.0);
    }

    pub(crate) fn nr_fetch_prompt_compatible(&mut self, select_for_target: Option<PathBuf>) {
        let gpu = self.nr_fetch_single_gpu();
        if let Some(entry) = model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu).cloned() {
            self.nr_fetch_open_consent(entry, select_for_target);
        } else {
            self.nr_fetch.error = Some(self.tr("nr_fetch.unavailable").into());
        }
    }

    pub(crate) fn nr_fetch_open_consent(&mut self, entry: RemoteModel, select_for_target: Option<PathBuf>) {
        self.saved.nr_model_offer_seen = true;
        self.nr_fetch.error = None;
        self.nr_fetch.select_for_target = select_for_target;
        self.nr_fetch.consent = Some(NrConsent::Download(entry));
    }

    pub(crate) fn nr_fetch_prompt_refresh(&mut self) {
        self.nr_fetch.error = None;
        self.nr_fetch.consent = Some(NrConsent::RefreshCatalog);
    }

    pub(crate) fn nr_fetch_prepare_for_optimize(&mut self, target: PathBuf) -> bool {
        let gpu = self.nr_fetch_single_gpu();
        let Some(entry) = model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu).cloned() else {
            return false;
        };
        self.saved.nr_model_offer_seen = true;
        self.nr_fetch.error = None;
        self.nr_fetch.select_for_target = None;
        self.nr_fetch.then_optimize = Some((target, self.tuning.facts_generation, self.next_request));
        self.nr_fetch.consent = Some(NrConsent::Download(entry));
        true
    }

    pub(crate) fn nr_fetch_repair(&mut self, target: PathBuf, sha256: &str) {
        if let Some(Ok(models)) = &self.catalog
            && let Some(existing) = models.iter().find(|m| m.sha256 == sha256)
        {
            let value = Catalog::model_file_value(existing, cfg!(not(windows)));
            self.start_plan(
                PlanKind::Settings,
                PlanInputs::Settings(vec![("DlssNr".into(), "ModelFile".into(), value)]),
            );
            return;
        }
        if let Some(entry) = self
            .nr_fetch
            .entries
            .iter()
            .find(|e| e.sha256 == sha256)
            .cloned()
            .or_else(|| {
                let gpu = self.nr_fetch_single_gpu();
                model_fetch::pick_for_gpu(&self.nr_fetch.entries, gpu).cloned()
            })
        {
            self.nr_fetch_open_consent(entry, Some(target));
        }
    }

    pub(crate) fn nr_fetch_dialog(&mut self, ctx: &egui::Context) {
        let Some(consent) = self.nr_fetch.consent.clone() else {
            return;
        };
        let mut confirm = false;
        let mut cancel = false;
        egui::Modal::new(egui::Id::unique("nr-fetch-consent")).show(ctx, |ui| {
            ui.set_max_width(540.0);
            match &consent {
                NrConsent::Download(entry) => {
                    ui.heading(self.tr("nr_fetch.consent_title"));
                    ui.label(self.tr("nr_fetch.source_note"));
                    ui.label(
                        RichText::new(format!(
                            "{}: {} · {:.1} {} ({} B) · SHA-256 {}",
                            self.tr("model"),
                            entry.label,
                            entry.size as f64 / 1_048_576.0,
                            self.tr("unit.mib"),
                            entry.size,
                            &entry.sha256[..entry.sha256.len().min(12)]
                        ))
                        .strong(),
                    );
                    self.model_compatibility_label(ui, &entry.compatibility);
                    ui.label(self.tr("nr_fetch.consent_body"));
                    ui.horizontal(|ui| {
                        confirm = ui.button(self.tr("nr_fetch.confirm")).clicked();
                        cancel = ui.button(self.tr("cancel")).clicked();
                    });
                }
                NrConsent::RefreshCatalog => {
                    ui.heading(self.tr("nr_fetch.refresh_title"));
                    ui.label(self.tr("nr_fetch.refresh_body"));
                    ui.horizontal(|ui| {
                        confirm = ui.button(self.tr("nr_fetch.confirm_refresh")).clicked();
                        cancel = ui.button(self.tr("cancel")).clicked();
                    });
                }
            }
        });
        if cancel {
            self.nr_fetch.consent = None;
            self.nr_fetch.select_for_target = None;
            self.nr_fetch.then_optimize = None;
        }
        if confirm {
            self.nr_fetch.consent = None;
            self.nr_fetch_start(consent);
        }
    }

    pub(crate) fn nr_fetch_start(&mut self, consent: NrConsent) {
        if self.nr_fetch.busy {
            return;
        }
        self.nr_fetch.busy = true;
        self.nr_fetch.error = None;
        self.nr_fetch.downloaded = 0;
        let custom_fetcher = self.nr_fetch.fetcher.clone();
        let catalog_root = self.nr_fetch.catalog_root.clone();
        let publications = self.jobs.catalog_publications.clone();
        let ctx = self.context.clone();
        self.jobs.spawn("nr-model-fetch", None, move |tx| {
            let active_fetcher: &dyn model_fetch::Fetcher = match custom_fetcher.as_deref() {
                Some(f) => f,
                None => &model_fetch::CommunityFetcher,
            };
            match consent {
                NrConsent::RefreshCatalog => {
                    let result = model_fetch::refresh_catalog(active_fetcher).map_err(AppError::from);
                    Outcome::NrFetch(NrFetchEvent::CatalogRefreshed(result))
                }
                NrConsent::Download(entry) => {
                    let result = (|| -> Result<Model, AppError> {
                        let catalog = Catalog::open(catalog_root.as_deref())?;
                        let mut previous = 0u64;
                        let mut imported =
                            model_fetch::download_and_import(active_fetcher, &entry, &catalog, &mut |bytes| {
                                if bytes.saturating_sub(previous) >= 256 * 1024 || bytes >= entry.size {
                                    previous = bytes;
                                    let _ = tx.send(Outcome::NrFetch(NrFetchEvent::Progress(bytes)));
                                    ctx.request_repaint();
                                }
                            })?;
                        if entry.compatibility.known {
                            imported.compatibility = entry.compatibility.clone();
                        }
                        let snapshot = publications.snapshot(|| {
                            let result = catalog
                                .list()
                                .map(|mut items| {
                                    if entry.compatibility.known {
                                        for item in &mut items {
                                            if item.sha256 == imported.sha256 {
                                                item.compatibility = entry.compatibility.clone();
                                            }
                                        }
                                    }
                                    items
                                })
                                .map_err(AppError::from);
                            Outcome::Catalog { result }
                        });
                        if let Outcome::CatalogSnapshot { result: Err(error), .. } = snapshot {
                            return Err(error);
                        }
                        // Completion stays unscoped: even if a newer list wins, release busy and resume the caller.
                        let _ = tx.send(snapshot);
                        Ok(imported)
                    })();
                    Outcome::NrFetch(NrFetchEvent::Downloaded(result))
                }
            }
        });
    }

    pub(crate) fn nr_fetch_event(&mut self, event: NrFetchEvent) {
        match event {
            NrFetchEvent::Progress(bytes) => self.nr_fetch.downloaded = bytes,
            NrFetchEvent::CatalogRefreshed(result) => {
                self.nr_fetch.busy = false;
                match result {
                    Ok(entries) => self.nr_fetch.entries = entries,
                    Err(e) => self.nr_fetch.error = Some(e),
                }
            }
            NrFetchEvent::Downloaded(result) => {
                self.nr_fetch.busy = false;
                match result {
                    Ok(imported) => {
                        if let Some((target, generation, review_request)) = self.nr_fetch.then_optimize.take() {
                            self.nr_fetch.select_for_target = None;
                            if self.view.target.as_ref() == Some(&target)
                                && self.tuning.facts_generation == generation
                                && self.next_request == review_request
                            {
                                self.optimize_automatically();
                            }
                        } else if let Some(target) = self.nr_fetch.select_for_target.take()
                            && self.view.target.as_ref() == Some(&target)
                            && matches!(&self.view.ini, Some(Ok(Some(_))))
                        {
                            let value = Catalog::model_file_value(&imported, cfg!(not(windows)));
                            self.start_plan(
                                PlanKind::Settings,
                                PlanInputs::Settings(vec![("DlssNr".into(), "ModelFile".into(), value)]),
                            );
                        }
                    }
                    Err(error) => {
                        self.nr_fetch.error = Some(error.clone());
                        self.nr_fetch.select_for_target = None;
                        if let Some((target, generation, _)) = self.nr_fetch.then_optimize.take()
                            && self.view.target.as_ref() == Some(&target)
                            && self.tuning.facts_generation == generation
                        {
                            self.tuning.sm86.error = Some(error);
                        }
                    }
                }
            }
        }
    }
}

#[cfg(all(test, not(windows)))]
pub(crate) fn synthetic_model_fixture_for_optimize() -> (RemoteModel, SharedModelFetcher) {
    let (entry, archive) = tests::synthetic_model_fixture();
    (
        entry,
        Arc::new(tests::SyntheticNrFetcher {
            archive,
            catalog_json: Vec::new(),
        }),
    )
}

#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    use sha2::{Digest, Sha256};
    use std::{
        fs,
        time::{Duration, Instant},
    };

    pub(crate) struct SyntheticNrFetcher {
        pub archive: Vec<u8>,
        pub catalog_json: Vec<u8>,
    }

    impl model_fetch::Fetcher for SyntheticNrFetcher {
        fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> std::io::Result<Vec<u8>> {
            let data = if url == model_fetch::CATALOG_URL {
                &self.catalog_json
            } else {
                assert!(model_fetch::allowed_asset_url(url));
                &self.archive
            };
            assert!(data.len() as u64 <= limit);
            progress(data.len() as u64);
            Ok(data.clone())
        }
    }

    pub(crate) fn synthetic_model_fixture() -> (RemoteModel, Vec<u8>) {
        let dll_bytes = b"MZ synthetic NR compat-fp16 fixture".to_vec();
        let sha256 = format!("{:x}", Sha256::digest(&dll_bytes));
        let archive = model_fetch::build_single_dll_zip(&dll_bytes).unwrap();
        let entry = RemoteModel {
            version: "310.9.16".into(),
            variant: "compat-fp16".into(),
            label: "DLSS NR 310.9.16 (compat-fp16)".into(),
            url: "https://github.com/RankFTW/rhi-repo/releases/download/dlssnr-310.9.16/nvngx_dlssnr_310.9.16.zip"
                .into(),
            sha256,
            size: dll_bytes.len() as u64,
            compatibility: retoque_data::model_compat::ModelCompatibility {
                archs: vec![75, 86, 89, 120],
                known: true,
            },
        };
        (entry, archive)
    }

    fn settle(app: &mut RetoqueApp, ready: impl Fn(&RetoqueApp) -> bool) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while !ready(app) {
            app.receive();
            assert!(Instant::now() < deadline, "timed out waiting for worker");
            std::thread::yield_now();
        }
    }

    #[test]
    fn delayed_catalog_snapshot_cannot_hide_a_downloaded_model() {
        let temp = tempfile::tempdir().unwrap();
        let mut app = crate::app_tests::shell_tests::app();
        let (entry, archive) = synthetic_model_fixture();
        app.nr_fetch.catalog_root = Some(temp.path().join("catalog"));
        app.nr_fetch.fetcher = Some(Arc::new(SyntheticNrFetcher {
            archive,
            catalog_json: Vec::new(),
        }));
        // Pause after the old snapshot was taken, but before its worker delivers it.
        let old = app
            .jobs
            .catalog_publications
            .snapshot(|| Outcome::Catalog { result: Ok(vec![]) });
        let (started, ready) = std::sync::mpsc::channel();
        let (release, blocked) = std::sync::mpsc::channel();
        app.jobs.spawn("hash", None, move |_| {
            started.send(()).unwrap();
            blocked.recv_timeout(Duration::from_secs(5)).unwrap();
            old
        });
        ready.recv_timeout(Duration::from_secs(5)).unwrap();
        app.nr_fetch_start(NrConsent::Download(entry.clone()));
        settle(&mut app, |app| !app.nr_fetch.busy);
        assert!(app.nr_fetch.error.is_none());
        release.send(()).unwrap();
        settle(&mut app, |app| !app.jobs.busy());
        let models = app.catalog.as_ref().unwrap().as_ref().unwrap();
        assert_eq!(models.len(), 1, "a late Heavy snapshot must not erase the Net import");
        assert_eq!(models[0].sha256, entry.sha256);
        assert!(models[0].compatibility.known);
    }

    #[test]
    fn model_tab_get_compatible_shows_community_source_downloads_imports_and_reviews_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let game = temp.path().join("game");
            fs::create_dir(&game).unwrap();
            let before = b"[DlssNr]\nEnabled=true\nModelFile=auto\n";
            fs::write(game.join("OptiScaler.ini"), before).unwrap();
            let (entry, archive) = synthetic_model_fixture();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.tab = Tab::Model;
            app.view.target = Some(game.clone());
            app.view.ini = Some(Ok(Some(Ini::parse(before))));
            app.catalog = Some(Ok(vec![]));
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.nr_fetch.entries = vec![entry.clone()];
            app.nr_fetch.catalog_root = Some(temp.path().join("catalog"));
            app.nr_fetch.fetcher = Some(Arc::new(SyntheticNrFetcher {
                archive,
                catalog_json: Vec::new(),
            }));

            let mut h = Harness::builder().with_size([1000., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    let tx = app.cmd_tx.clone();
                    app.model_tab(ui, &tx);
                    app.nr_fetch_dialog(ui.ctx());
                    app.review_sheet(ui.ctx());
                    app.handle_commands();
                },
                app,
            );
            h.get_by_label(t(lang, "nr_fetch.source_note"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "nr_fetch.get_compatible"))
                .click();
            h.run_steps(3);
            assert!(h.state().nr_fetch.consent.is_some());
            assert!(!h.state().jobs.busy());
            h.get_by_label(t(lang, "nr_fetch.consent_title"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "nr_fetch.confirm"))
                .click();
            h.run_steps(3);
            settle(h.state_mut(), |app| app.review.is_some());
            assert!(h.state().review_open);
            let models = h.state().catalog.as_ref().unwrap().as_ref().unwrap();
            assert_eq!(models.len(), 1);
            assert_eq!(models[0].label, entry.label);
            let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert!(
                review
                    .plans
                    .iter()
                    .flat_map(|p| p.steps())
                    .any(|s| matches!(s, Step::SetIni { key, .. } if key == "ModelFile"))
            );
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before);
        }
    }

    #[test]
    fn doctor_repair_fetches_compatible_model_and_opens_settings_review_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let game = temp.path().join("game");
            fs::create_dir(&game).unwrap();
            let before = b"[DlssNr]\nEnabled=true\nModelFile=missing.dll\n";
            fs::write(game.join("OptiScaler.ini"), before).unwrap();
            let (entry, archive) = synthetic_model_fixture();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some(game.clone());
            app.view.status = Some(Ok(Status {
                nr_enabled: Some(true),
                model_present: false,
                ..Default::default()
            }));
            app.view.ini = Some(Ok(Some(Ini::parse(before))));
            app.view.inspection = Some(ModelInspection {
                path: Some(game.join("missing.dll")),
                present: false,
                ..Default::default()
            });
            app.catalog = Some(Ok(vec![]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.nr_fetch.entries = vec![entry.clone()];
            app.nr_fetch.catalog_root = Some(temp.path().join("catalog"));
            app.nr_fetch.fetcher = Some(Arc::new(SyntheticNrFetcher {
                archive,
                catalog_json: Vec::new(),
            }));
            app.refresh_intelligence();
            settle(&mut app, |app| app.view.doctor.is_some());

            let mut h = Harness::builder().with_size([1000., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    if let (Some(card), Some(target)) = (&app.view.doctor, &app.view.target) {
                        crate::tabs::doctor::show(ui, card, target, app.lang, &app.cmd_tx, false);
                    }
                    app.nr_fetch_dialog(ui.ctx());
                    app.review_sheet(ui.ctx());
                    app.handle_commands();
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "nr_fetch.get_compatible"))
                .click();
            h.run_steps(3);
            assert!(h.state().nr_fetch.consent.is_some());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "nr_fetch.confirm"))
                .click();
            h.run_steps(3);
            settle(h.state_mut(), |app| app.review.is_some());
            assert!(h.state().review_open);
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before);
        }
    }

    #[test]
    fn deleting_a_stored_model_asks_first_and_names_the_model_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some("/synthetic/nr-game".into());
            app.view.ini = Some(Ok(Some(Ini::parse(b"[DlssNr]\nModelFile=auto\n"))));
            app.catalog = Some(Ok(vec![Model {
                sha256: "synthetic".into(),
                version: None,
                size: 64,
                label: "Stored fixture".into(),
                path: "/synthetic/model.dll".into(),
                compatibility: Default::default(),
            }]));
            let mut h = Harness::builder().with_size([1000., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    let tx = app.cmd_tx.clone();
                    app.model_tab(ui, &tx);
                },
                app,
            );
            h.run_steps(2);
            let delete = format!("{}: Stored fixture", t(lang, "mdl.remove").trim_end_matches('…'));
            let use_here = format!("{}: Stored fixture", t(lang, "mdl.use"));
            h.get_by_role_and_label(egui::Role::Button, &use_here);
            // Cancel closes the confirm and deletes nothing.
            h.get_by_role_and_label(egui::Role::Button, &delete).click();
            h.run_steps(3);
            h.get_by_label(t(lang, "mdl.remove_title"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
            h.run_steps(3);
            assert!(h.query_by_label(t(lang, "mdl.remove_title")).is_none());
            assert!(h.state().cmd_rx.try_recv().is_err());
            // "Apagar" sends the removal for that model only.
            h.get_by_role_and_label(egui::Role::Button, &delete).click();
            h.run_steps(3);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "delete")).click();
            h.run_steps(3);
            assert!(matches!(
                h.state().cmd_rx.try_recv(),
                Ok(AppCommand::RemoveModel(sha)) if sha == "synthetic"
            ));
            assert!(h.query_by_label(t(lang, "mdl.remove_title")).is_none());
        }
    }
}
