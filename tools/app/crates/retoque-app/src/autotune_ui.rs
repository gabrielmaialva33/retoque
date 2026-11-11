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

/// Where the component waiting for "Apagar" in the delete confirm is kept: (id, label), in egui's temporary data.
fn component_delete_id() -> egui::Id {
    egui::Id::unique("component-delete-pending")
}

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
                // Like the install reviews, the title names the game it is about.
                let game = self.selected_game().map(|g| g.name.as_str()).unwrap_or("");
                let title = match game {
                    "" => self.tuning.review_title.clone(),
                    game => format!("{} · {game}", self.tuning.review_title),
                };
                self.review = Some(result.map(|p| ReviewSession {
                    title,
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
                    crate::error::card(ui, error, self.lang, &self.cmd_tx);
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
        ui.add_space(theme::CARD_GAP);
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
                        .map(|c| component_name(self.lang, c))
                        .unwrap_or_else(|| self.tr("auto.no_component").to_owned()),
                )
                .show_ui(ui, |ui| {
                    for c in items {
                        ui.selectable_value(
                            &mut self.tuning.selected_component,
                            Some(c.id.clone()),
                            component_name(self.lang, c),
                        );
                    }
                });
        }
        if selected.is_none() {
            self.sm86_download_controls(ui);
        }
        selected.is_some()
    }
    pub(crate) fn autotune_components(&mut self, ui: &mut egui::Ui) {
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("auto.components"));
            self.sm86_download_controls(ui);
            match self.tuning.catalog.clone() {
                None => {
                    widgets::busy(ui, self.lang, self.tr("auto.reading_components"), None, None);
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
                            // A plain name; where the app keeps it (a folder named by its hash) is in the hover.
                            ui.label(RichText::new(component_name(self.lang, &c)).strong())
                                .on_hover_text(crate::format::display_path(&c.root));
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
                                } else {
                                    let install =
                                        ui.add_enabled(allowed, egui::Button::new(self.tr("auto.component_install")));
                                    widgets::name_after(
                                        &install,
                                        self.tr("auto.component_install"),
                                        &component_name(self.lang, &c),
                                    );
                                    if install.clicked() {
                                        self.autotune_request(
                                            self.tr("auto.component_install").into(),
                                            Request {
                                                dlss: None,
                                                component: Some(ComponentChange::Install(c.clone())),
                                                edits: recommend::lsfg_off_edits(),
                                            },
                                        );
                                    }
                                }
                                let delete = ui
                                    .add_enabled(!self.jobs.busy(), egui::Button::new(self.tr("auto.catalog_remove")))
                                    .on_hover_text(self.tr("auto.catalog_remove_help"));
                                widgets::name_after(
                                    &delete,
                                    self.tr("auto.catalog_remove"),
                                    &component_name(self.lang, &c),
                                );
                                if delete.clicked() {
                                    let pending = (c.id.clone(), component_name(self.lang, &c));
                                    ui.ctx().data_mut(|d| d.insert_temp(component_delete_id(), pending));
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
                            let remove =
                                ui.add_enabled(!self.busy(), egui::Button::new(self.tr("auto.component_remove")));
                            widgets::name_after(&remove, self.tr("auto.component_remove"), &id[..id.len().min(12)]);
                            if remove.clicked() {
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
        self.component_delete_confirm(ui.ctx());
    }
    /// Deleting a component the app keeps cannot be undone, so it asks first; the catalog job runs on "Apagar".
    fn component_delete_confirm(&mut self, ctx: &egui::Context) {
        let Some((id, label)) = ctx.data(|d| d.get_temp::<(String, String)>(component_delete_id())) else {
            return;
        };
        let answer = widgets::delete_modal(
            ctx,
            self.lang,
            "component",
            self.tr("auto.catalog_remove_title"),
            &label,
            self.tr("auto.catalog_remove_help"),
        );
        if answer.is_some() {
            ctx.data_mut(|d| d.remove::<(String, String)>(component_delete_id()));
        }
        if answer == Some(widgets::Answer::Confirm) {
            self.autotune_catalog(None, Some(id));
        }
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
        // A review is titled after the action without the "…" that announced it.
        self.tuning.review_title = title.trim_end_matches('…').trim_end().to_owned();
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
    // How sure the advice is: one short muted line; the facts behind it stay folded.
    ui.label(
        RichText::new(text(lang, rec.confidence.key()))
            .small()
            .color(theme::MUTED),
    );
    if let Some(key) = rec.caveat_key {
        ui.add(egui::Label::new(RichText::new(text(lang, key)).small().color(theme::MUTED)).wrap());
    }
    let title = t(lang, "doctor.evidence");
    widgets::collapsible(ui, ("evidence", rec.id), title, widgets::Fold::Details, |ui| {
        evidence(ui, lang, &rec.evidence)
    });
}

/// A component as people read it. The official sm86 download is stored as "DLSS-G for sm86 {version} · {RFC 3339
/// time}"; show what it is for, its version once, and the date in the user's format. Anything else keeps its label.
pub(crate) fn component_name(lang: Lang, c: &autotune::components::Component) -> String {
    if let Some(rest) = c.label.strip_prefix("DLSS-G for sm86 ") {
        let (version, published) = rest.split_once(" · ").unwrap_or((rest, ""));
        let mut name = t(lang, "comp.sm86_name").replace("{version}", version);
        if let Some(date) = crate::format::date_text(lang, published) {
            name = format!("{name} · {date}");
        }
        return name;
    }
    match c.version.as_deref() {
        Some(v) if !c.label.contains(v) => format!("{} · {v}", c.label),
        _ => c.label.clone(),
    }
}

#[cfg(test)]
mod component_name_tests {
    use super::*;
    use autotune::components::{Component, ComponentKind};
    fn component(label: &str, version: Option<&str>) -> Component {
        Component {
            id: "id".into(),
            kind: ComponentKind::DlssgSm86,
            version: version.map(Into::into),
            label: label.into(),
            root: std::path::PathBuf::from("components/id"),
            files: vec![],
        }
    }
    #[test]
    fn the_official_sm86_download_reads_as_what_it_is_for() {
        let sm86 = component("DLSS-G for sm86 0.4.1 · 2026-10-27T06:27:00Z", Some("0.4.1"));
        assert_eq!(
            component_name(Lang::Pt, &sm86),
            "Geração de quadros para RTX 20/30 · versão 0.4.1 · 27/10/2026"
        );
        assert_eq!(
            component_name(Lang::En, &sm86),
            "Frame generation for RTX 20/30 · version 0.4.1 · 2026-10-27"
        );
        // Other components keep their label, with the version once.
        assert_eq!(
            component_name(Lang::Pt, &component("My kit", Some("2.0"))),
            "My kit · 2.0"
        );
        assert_eq!(
            component_name(Lang::Pt, &component("My kit 2.0", Some("2.0"))),
            "My kit 2.0"
        );
    }
}
