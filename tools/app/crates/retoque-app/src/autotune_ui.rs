//! GPU recommendations and reviewed frame-generation operations in the existing shell.
use crate::app::*;
use crate::autotune::{
    self, ComponentChange, Event, Request,
    components::ComponentKind,
    gpu::Arch,
    recommend::{self, Action, GameFacts, Need},
};

#[path = "sm86_ui.rs"]
mod sm86_ui;

impl RetoqueApp {
    pub(crate) fn autotune_start(&mut self) {
        self.jobs.spawn("autotune-environment", None, |_| {
            Outcome::Auto(Box::new(autotune::environment()))
        });
        self.autotune_catalog(None, None);
    }
    pub(crate) fn autotune_catalog(&mut self, import: Option<PathBuf>, remove: Option<String>) {
        self.tuning.catalog = None;
        self.jobs.spawn("autotune-catalog", None, move |_| {
            Outcome::Auto(Box::new(autotune::catalog(import, remove)))
        });
    }
    pub(crate) fn autotune_refresh(&mut self, target: PathBuf) {
        self.tuning.facts_generation += 1;
        let generation = self.tuning.facts_generation;
        self.tuning.installed = None;
        self.tuning.readiness = None;
        let dir = self.saved.lossless_dir.clone();
        self.jobs.spawn("autotune-facts", None, move |_| {
            Outcome::Auto(Box::new(autotune::facts(target, generation, dir)))
        });
    }
    pub(crate) fn autotune_event(&mut self, event: Event) {
        match event {
            Event::Sm86Progress(bytes) => self.tuning.sm86.downloaded = bytes,
            Event::Sm86Release(result) => {
                self.tuning.sm86.busy = false;
                match result {
                    Ok((release, available)) => {
                        self.tuning.sm86.release = Some(release);
                        self.tuning.sm86.available = available;
                    }
                    Err(e) => self.tuning.sm86.error = Some(e),
                }
            }
            Event::Sm86Downloaded(result) => self.sm86_downloaded(result),
            Event::Environment {
                gpus,
                lossless,
                kit_cache,
            } => {
                self.tuning.sm86.kit_cache = kit_cache;
                self.tuning.gpus = Some(gpus);
                self.tuning.detected_lossless = lossless;
            }
            Event::Catalog(result) => self.tuning.catalog = Some(result),
            Event::Facts {
                target,
                generation,
                installed,
                lossless,
                readiness,
            } if self.view.target.as_ref() == Some(&target) && generation == self.tuning.facts_generation => {
                self.tuning.installed = Some(installed);
                self.tuning.lossless = lossless;
                self.tuning.readiness = Some(readiness);
            }
            Event::Plan {
                target,
                request,
                result,
            } if self.view.target.as_ref() == Some(&target) && request == self.tuning.request => {
                self.review = Some(result.map(|p| ReviewSession {
                    title: self.tuning.review_title.clone(),
                    kind: ReviewKind::Autotune(Box::new(p.request)),
                    plans: p.plans,
                    groups: p.groups,
                    request_id: request,
                    target: Some(p.target),
                }));
            }
            _ => {}
        }
    }
    pub(crate) fn autotune_gpu(&mut self, ui: &mut egui::Ui) {
        self.sm86_dialog(ui.ctx());
        let Some(gpus) = &self.tuning.gpus else {
            ui.add(egui::Label::new(self.tr("auto.gpu_reading")).wrap());
            return;
        };
        let Some(gpu) = retoque_data::gpu::primary(gpus) else {
            ui.label(RichText::new(self.tr("auto.gpu_unknown")).small().color(theme::MUTED));
            return;
        };
        let arch = self.tr(match gpu.arch {
            Arch::Turing => "auto.turing",
            Arch::Ampere => "auto.ampere",
            Arch::Ada => "auto.ada",
            Arch::Blackwell => "auto.blackwell",
            Arch::OlderNvidia => "auto.older",
            Arch::NotNvidia => "auto.other",
            Arch::Unknown => "auto.unknown",
        });
        let memory = gpu
            .vram_mib
            .map(|m| format!(" · {:.0} {}", m as f64 / 1024., self.tr("auto.gb")))
            .unwrap_or_default();
        let response = ui.label(
            RichText::new(format!("{} · {arch}{memory}", gpu.name))
                .small()
                .color(theme::MUTED),
        );
        if let Some(driver) = &gpu.driver {
            response.on_hover_text(format!("{} {driver}", self.tr("auto.driver")));
        }
    }
    pub(crate) fn autotune_game_facts(&self) -> GameFacts {
        // One fact builder for the CLI and the window (retoque-data::recommend::facts_for_with_ini), fed with what the
        // view already holds: no disk reads on the UI thread.
        let status = self.view.status.as_ref().and_then(|r| r.as_ref().ok());
        let ini = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(|i| i.as_ref());
        let components: Vec<String> = self
            .tuning
            .installed
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .cloned()
            .unwrap_or_default();
        if let Some(game) = self.selected_game()
            && let Some(target) = game.targets.iter().find(|t| Some(&t.path) == self.view.target.as_ref())
        {
            let mut facts = recommend::facts_for_with_ini(game, target, status, &components, ini);
            facts.last_session_api = self.view.intelligence.latest_log.as_ref().and_then(|l| l.api.clone());
            return facts;
        }
        // A folder the library does not list (added by path, or a stale selection): what the status and the ini say.
        let tracked = status.is_some_and(|s| s.receipt.is_some());
        let proxies = status.is_some_and(|s| !s.optiscaler_proxies.is_empty());
        let lsfg_enabled =
            ini.and_then(|i| i.get("LSFG", "Enabled"))
                .and_then(|v| match v.to_ascii_lowercase().as_str() {
                    "true" => Some(true),
                    "false" => Some(false),
                    _ => None,
                });
        GameFacts {
            installed: tracked || proxies,
            untracked: !tracked && proxies,
            sm86_installed: !components.is_empty(),
            lsfg_enabled,
            last_session_api: self.view.intelligence.latest_log.as_ref().and_then(|l| l.api.clone()),
            ..GameFacts::default()
        }
    }
    pub(crate) fn autotune_known_components(&self) -> bool {
        matches!(self.tuning.installed, Some(Ok(_)))
    }
    pub(crate) fn autotune_card(&mut self, ui: &mut egui::Ui) {
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("auto.title"));
            self.nr_compatibility_notice(ui);
            if self.tuning.gpus.is_none() || self.view.status.is_none() || !self.autotune_known_components() {
                ui.add(egui::Label::new(self.tr("auto.checking")).wrap());
                if let Some(Err(e)) = &self.tuning.installed {
                    crate::error::card(ui, e, self.lang, &self.cmd_tx);
                }
                return;
            }
            // Before Retoque is installed (or while an old script's install waits to be registered) the banner's
            // "Install and optimize" is the one action; a second button and per-item "install first" here would
            // repeat it under other names.
            let facts = &self.view.intelligence.facts;
            if !facts.installed || facts.untracked {
                ui.add(egui::Label::new(RichText::new(self.tr("auto.use_banner")).color(theme::MUTED)).wrap());
                if let Some(error) = &self.tuning.sm86.error {
                    ui.colored_label(theme::BAD, crate::i18n::diagnostic(self.lang, error));
                }
                return;
            }
            self.optimize_controls(ui);
            let recs = self.view.intelligence.recommendations.clone();
            if recs.is_empty() {
                ui.add(egui::Label::new(self.tr("auto.none")).wrap());
            }
            for rec in recs.iter() {
                ui.push_id(rec.id, |ui| {
                    ui.label(RichText::new(t(self.lang, rec.title_key)).strong());
                    ui.add(egui::Label::new(t(self.lang, rec.why_key)).wrap());
                    recommendation_evidence(ui, self.lang, rec);
                    let mut ready = !self.busy();
                    match &rec.needs {
                        // Games without (tracked) Retoque returned above: the banner's action installs it.
                        Some(Need::RetoqueInstalled) => return,
                        Some(Need::Component(ComponentKind::ReShade)) => {
                            if ui.button(self.tr("rs.install")).clicked() {
                                self.reshade_offer();
                            }
                            return;
                        }
                        Some(Need::Component(_)) => {
                            ready &= self.autotune_component_picker(ui);
                        }
                        Some(Need::LosslessScaling) => {
                            self.autotune_readiness(ui);
                            ready &= self.autotune_ready();
                        }
                        None => {}
                    }
                    // One recommendation alone: a plain button, the banner keeps the page's primary.
                    let apply = ui.add_enabled(ready, egui::Button::new(self.tr("auto.apply")));
                    apply.widget_info(|| {
                        egui::WidgetInfo::labeled(
                            egui::Role::Button,
                            ready,
                            format!("{}: {}", self.tr("auto.apply"), t(self.lang, rec.title_key)),
                        )
                    });
                    if apply.clicked() {
                        let mut request = Request::default();
                        for action in &rec.actions {
                            match action {
                                Action::SetIni { section, key, value } => {
                                    request.edits.push((section.clone(), key.clone(), value.clone()))
                                }
                                Action::InstallComponent(ComponentKind::DlssgSm86) => {
                                    if let Some(c) = self.autotune_selected_component() {
                                        request.component = Some(ComponentChange::Install(c));
                                    }
                                }
                                Action::InstallComponent(ComponentKind::ReShade)
                                | Action::RemoveComponent(ComponentKind::ReShade) => {}
                                Action::RemoveComponent(ComponentKind::DlssgSm86) => {
                                    if let Some(id) = self
                                        .tuning
                                        .installed
                                        .as_ref()
                                        .and_then(|r| r.as_ref().ok())
                                        .and_then(|ids| ids.first())
                                    {
                                        request.component = Some(ComponentChange::Remove(id.clone()));
                                    }
                                }
                            }
                        }
                        if rec.id == "lsfg_x2" {
                            request.edits = self.autotune_lsfg_edits(2, &self.view.intelligence.facts);
                        }
                        self.autotune_request(t(self.lang, rec.title_key).into(), request);
                    }
                    ui.add_space(8.0);
                });
            }
        });
    }
    pub(crate) fn autotune_lsfg(&mut self, ui: &mut egui::Ui) {
        self.lsfg_card(ui);
    }
    fn autotune_selected_component(&self) -> Option<autotune::components::Component> {
        let items = self.tuning.catalog.as_ref()?.as_ref().ok()?;
        items
            .iter()
            .find(|c| Some(&c.id) == self.tuning.selected_component.as_ref())
            .or_else(|| items.first())
            .cloned()
    }
    fn autotune_component_picker(&mut self, ui: &mut egui::Ui) -> bool {
        let selected = self.autotune_selected_component();
        if let Some(items) = self.tuning.catalog.as_ref().and_then(|r| r.as_ref().ok()) {
            egui::ComboBox::from_id_salt("component_pick")
                .selected_text(
                    selected
                        .as_ref()
                        .map(|c| c.label.as_str())
                        .unwrap_or(self.tr("auto.no_component")),
                )
                .show_ui(ui, |ui| {
                    for c in items {
                        ui.selectable_value(&mut self.tuning.selected_component, Some(c.id.clone()), &c.label);
                    }
                });
        }
        if selected.is_none() {
            self.sm86_download_controls(ui);
        }
        selected.is_some()
    }
    pub(crate) fn autotune_components(&mut self, ui: &mut egui::Ui) {
        self.dlss_card(ui);
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("auto.components"));
            self.sm86_download_controls(ui);
            match self.tuning.catalog.clone() {
                None => {
                    ui.spinner();
                }
                Some(Err(e)) => {
                    crate::error::card(ui, e, self.lang, &self.cmd_tx);
                }
                Some(Ok(items)) => {
                    if items.is_empty() {
                        ui.add(egui::Label::new(self.tr("auto.no_component")).wrap());
                    }
                    for c in items {
                        ui.push_id(&c.id, |ui| {
                            ui.label(
                                RichText::new(format!(
                                    "{} · {}",
                                    c.label,
                                    c.version.as_deref().unwrap_or(self.tr("mdl.version_unknown"))
                                ))
                                .strong(),
                            );
                            ui.label(RichText::new(short_path(&c.root)).small());
                            let installed = self
                                .tuning
                                .installed
                                .as_ref()
                                .and_then(|r| r.as_ref().ok())
                                .is_some_and(|ids| ids.contains(&c.id));
                            let allowed = self.autotune_game_facts().installed
                                && !self.busy()
                                && self.autotune_known_components();
                            ui.horizontal_wrapped(|ui| {
                                if installed {
                                    ui.colored_label(theme::GOOD, self.tr("auto.component_installed"));
                                } else if ui
                                    .add_enabled(allowed, egui::Button::new(self.tr("auto.component_install")))
                                    .clicked()
                                {
                                    self.autotune_request(
                                        self.tr("auto.component_install").into(),
                                        Request {
                                            dlss: None,
                                            component: Some(ComponentChange::Install(c.clone())),
                                            edits: recommend::lsfg_off_edits(),
                                        },
                                    );
                                }
                                if ui
                                    .add_enabled(!self.jobs.busy(), egui::Button::new(self.tr("auto.catalog_remove")))
                                    .clicked()
                                {
                                    self.autotune_catalog(None, Some(c.id.clone()));
                                }
                            });
                        });
                    }
                }
            }
            match self.tuning.installed.clone() {
                Some(Ok(ids)) => {
                    if ids.is_empty() {
                        ui.add(egui::Label::new(self.tr("auto.none_installed")).wrap());
                    }
                    for id in ids {
                        ui.horizontal_wrapped(|ui| {
                            ui.label(format!(
                                "{} · {}",
                                self.tr("auto.component_installed"),
                                &id[..id.len().min(12)]
                            ));
                            if ui
                                .add_enabled(!self.busy(), egui::Button::new(self.tr("auto.component_remove")))
                                .clicked()
                            {
                                self.autotune_request(
                                    self.tr("auto.component_remove").into(),
                                    Request {
                                        dlss: None,
                                        component: Some(ComponentChange::Remove(id)),
                                        edits: vec![],
                                    },
                                );
                            }
                        });
                    }
                }
                Some(Err(e)) => {
                    crate::error::card(ui, e, self.lang, &self.cmd_tx);
                }
                None => {
                    ui.add(egui::Label::new(self.tr("auto.checking")).wrap());
                }
            }
        });
        ui.add_space(12.0);
    }
    pub(crate) fn autotune_request(&mut self, title: String, request: Request) {
        let Some(target) = self.view.target.clone() else { return };
        if self.busy() {
            return;
        }
        self.next_request += 1;
        let serial = self.next_request;
        self.tuning.request = serial;
        self.review = None;
        self.tuning.review_title = title;
        self.review_open = true;
        self.origin_focus = self.context.memory(|m| m.focused());
        self.jobs.spawn("autotune-plan", None, move |_| {
            Outcome::Auto(Box::new(Event::Plan {
                result: autotune::prepare(&target, request),
                target,
                request: serial,
            }))
        });
    }
    /// What the success toast says, repeating the action: Retoque installed, registered or updated with the
    /// optimization, the optimization alone, or other reviewed changes.
    pub(crate) fn tuning_done_key(&self, request: &Request) -> &'static str {
        let status = self.view.status.as_ref().and_then(|r| r.as_ref().ok());
        if request.retoque_install().is_some() {
            match status {
                Some(s) if s.receipt.is_some() => "auto.done_update",
                Some(s) if !s.optiscaler_proxies.is_empty() => "auto.done_register",
                _ => "auto.done_install",
            }
        } else if matches!(&self.review, Some(Ok(r)) if self.optimize_verb(&r.title).is_some()) {
            "auto.done_optimize"
        } else {
            "auto.done"
        }
    }
    pub(crate) fn autotune_apply(&mut self, p: autotune::Prepared) {
        let target = p.target.clone();
        let mut roots = vec![target.clone()];
        for plan in &p.plans {
            if plan.game_dir().starts_with(&target) || target.starts_with(plan.game_dir()) {
                roots.push(plan.game_dir().to_owned());
            }
        }
        if let Some(change) = &p.request.dlss {
            roots.push(match change {
                crate::dlss_ui::Change::Update(u) => u.install_dir.clone(),
                crate::dlss_ui::Change::Restore { install_dir } => install_dir.clone(),
            });
        }
        roots.sort();
        roots.dedup();
        if roots
            .iter()
            .any(|root| self.writing.keys().any(|p| p.starts_with(root) || root.starts_with(p)))
            || self.view.target.as_ref() != Some(&target)
        {
            return;
        }
        let kind = if matches!(p.request.dlss, Some(crate::dlss_ui::Change::Restore { .. })) {
            PlanKind::Uninstall
        } else if p.request.retoque_install().is_some() {
            PlanKind::Install
        } else {
            PlanKind::Settings
        };
        let total = p.plans.iter().map(|p| p.steps().len()).sum();
        self.jobs.begin_write(&target);
        self.writing.insert(target.clone(), (0, total));
        self.write_kinds.insert(target.clone(), kind);
        let done = self.tuning_done_key(&p.request);
        self.tuning.writes.insert(target.clone(), done);
        for root in &roots {
            self.writing.insert(root.clone(), (0, total));
        }
        self.tuning.write_roots.insert(target.clone(), roots);
        self.review_open = false;
        self.restore_focus();
        let ctx = self.context.clone();
        let lang = self.lang;
        self.jobs.spawn("autotune-apply", Some(target.clone()), move |tx| {
            let result = autotune::apply(&p, &mut |done| {
                let _ = tx.send(Outcome::Progress {
                    target: target.clone(),
                    done,
                });
                ctx.request_repaint();
            })
            .map_err(|failure| {
                failure.detail.for_target(target.clone()).with_context(format!(
                    "{} {}/{}",
                    t(lang, "auto.partial"),
                    failure.completed,
                    p.plans.len()
                ))
            });
            Outcome::Applied {
                target,
                kind,
                edits: p.request.edits,
                result,
            }
        });
    }
}

pub(crate) fn recommendation_evidence(ui: &mut egui::Ui, lang: Lang, rec: &recommend::Recommendation) {
    use crate::tabs::doctor::{evidence, text};
    ui.label(text(lang, rec.confidence.key()));
    if let Some(key) = rec.caveat_key {
        ui.label(text(lang, key));
    }
    ui.collapsing(t(lang, "doctor.evidence"), |ui| evidence(ui, lang, &rec.evidence));
}
