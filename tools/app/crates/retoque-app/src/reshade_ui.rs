//! ReShade reads, imports and downloads stay on TaskHub. Every game mutation opens the shared review.
use crate::app::*;
use retoque_core::reshade as core;
use retoque_data::reshade as data;
#[derive(Clone, Debug)]
pub enum Change {
    Install { source: PathBuf, exe_dir: PathBuf },
    Uninstall,
    Add(PathBuf),
    Remove(String),
    Enable { file: String, enabled: bool },
    MenuInput,
}
impl Change {
    pub fn plan(&self, target: &Path) -> Result<Plan, retoque_core::Error> {
        match self {
            Self::Install { source, exe_dir } => core::plan_install(target, exe_dir, source),
            Self::Uninstall => core::plan_uninstall(target),
            Self::Add(path) => core::plan_add(target, path),
            Self::Remove(file) => core::plan_remove(target, file),
            Self::Enable { file, enabled } => core::plan_enabled(target, file, *enabled),
            Self::MenuInput => core::plan_menu_input(target),
        }
    }
}
#[derive(Default)]
pub struct State {
    key: Option<(PathBuf, u64)>,
    snapshot: Option<data::Snapshot>,
    cached: Option<data::Cached>,
    rows: Vec<String>,
    cached_label: String,
    language: Option<Lang>,
    consent: bool,
    busy: bool,
    error: Option<AppError>,
}
impl State {
    /// ReShade or its add-ons are in the game, or a ReShade task (consent, download, import, error) is under way:
    /// the Overview then shows the ReShade card open instead of folded away.
    pub(crate) fn in_view(&self) -> bool {
        self.snapshot
            .as_ref()
            .is_some_and(|s| s.build.is_some() || !s.addons.is_empty())
            || self.consent
            || self.busy
            || self.error.is_some()
    }
    pub(crate) fn failed(&mut self) {
        self.busy = false;
    }
    /// An import or the official download is running.
    pub(crate) fn busy(&self) -> bool {
        self.busy
    }
}
pub enum Event {
    Inspected {
        target: PathBuf,
        generation: u64,
        result: Result<(data::Snapshot, Option<data::Cached>), AppError>,
    },
    Cached {
        target: PathBuf,
        request: u64,
        result: Result<data::Cached, AppError>,
    },
    Picked {
        target: PathBuf,
        addon: bool,
        path: Option<PathBuf>,
    },
}
impl RetoqueApp {
    fn reshade_request(&mut self, change: Change) {
        self.autotune_request(
            self.tr("rs.review").into(),
            crate::autotune::Request {
                component: Some(crate::autotune::ComponentChange::ReShade(change)),
                ..Default::default()
            },
        );
    }
    pub(crate) fn reshade_menu_input(&mut self) {
        self.tab = Tab::Install;
        self.reshade_request(Change::MenuInput);
    }
    pub(crate) fn reshade_offer(&mut self) {
        self.tab = Tab::Install;
        if let (Some(target), Some(cached)) = (&self.view.target, &self.tuning.reshade.cached)
            && cached.build == core::Build::Full
        {
            self.reshade_request(Change::Install {
                source: cached.dll(),
                exe_dir: target.clone(),
            });
        } else {
            self.tuning.reshade.consent = true;
        }
    }
    pub(crate) fn reshade_import(&mut self, path: PathBuf, addon: bool) {
        let Some(target) = self.view.target.clone() else {
            self.tuning.reshade.error = Some(self.tr("rs.select_game").into());
            return;
        };
        if addon {
            self.reshade_request(Change::Add(path));
            return;
        }
        self.tuning.reshade.busy = true;
        let request = self.next_request;
        self.jobs.spawn("reshade-import", None, move |_| {
            let result = (|| {
                let root = retoque_data::lsfg_prepare::data_dir().map_err(|e| AppError::from(e.to_string()))?;
                data::import(&root, &path).map_err(AppError::from)
            })();
            Outcome::ReShade(Box::new(Event::Cached {
                target,
                request,
                result,
            }))
        });
    }
    fn reshade_pick(&mut self, addon: bool) {
        let Some(target) = self.view.target.clone() else {
            return;
        };
        self.jobs.spawn("reshade-pick", None, move |_| {
            let extensions = if addon {
                vec!["addon64", "addon"]
            } else {
                vec!["dll", "exe"]
            };
            let path = rfd::FileDialog::new().add_filter("ReShade", &extensions).pick_file();
            Outcome::ReShade(Box::new(Event::Picked { target, addon, path }))
        });
    }
    pub(crate) fn reshade_inspect(&mut self) {
        let Some(target) = self.view.target.as_ref() else {
            return;
        };
        let generation = self.tuning.facts_generation;
        if self
            .tuning
            .reshade
            .key
            .as_ref()
            .is_some_and(|(t, g)| t == target && *g == generation)
        {
            return;
        }
        let target = target.clone();
        self.tuning.reshade.key = Some((target.clone(), generation));
        self.tuning.reshade.snapshot = None;
        self.tuning.reshade.rows.clear();
        self.tuning.reshade.language = None;
        self.tuning.reshade.error = None;
        self.jobs.spawn("reshade-inspect", None, move |_| {
            let result = (|| {
                let snapshot = data::inspect(&target).map_err(AppError::from)?;
                let root = retoque_data::lsfg_prepare::data_dir().map_err(|e| AppError::from(e.to_string()))?;
                Ok((snapshot, data::cached(&root, data::DLL_SHA).map_err(AppError::from)?))
            })();
            Outcome::ReShade(Box::new(Event::Inspected {
                target,
                generation,
                result,
            }))
        });
    }
    pub(crate) fn reshade_event(&mut self, event: Event) {
        match event {
            Event::Inspected {
                target,
                generation,
                result,
            } if self
                .tuning
                .reshade
                .key
                .as_ref()
                .is_some_and(|(t, g)| t == &target && *g == generation) =>
            {
                match result {
                    Ok((snapshot, cached)) => {
                        self.tuning.reshade.snapshot = Some(snapshot);
                        if cached.is_some() {
                            self.tuning.reshade.cached = cached;
                        }
                        self.tuning.reshade.language = None;
                    }
                    Err(e) => self.tuning.reshade.error = Some(e),
                }
            }
            Event::Cached {
                target,
                request,
                result,
            } => {
                self.tuning.reshade.busy = false;
                match result {
                    Ok(cached) => {
                        let change = Change::Install {
                            source: cached.dll(),
                            exe_dir: target.clone(),
                        };
                        self.tuning.reshade.cached = Some(cached);
                        self.tuning.reshade.language = None;
                        if self.view.target.as_ref() == Some(&target) && self.next_request == request {
                            self.reshade_request(change);
                        }
                    }
                    Err(e) => self.tuning.reshade.error = Some(e),
                }
            }
            Event::Picked {
                target,
                addon,
                path: Some(path),
            } if self.view.target.as_ref() == Some(&target) => self.reshade_import(path, addon),
            _ => {}
        }
    }
    pub(crate) fn reshade_card(&mut self, ui: &mut egui::Ui) {
        self.reshade_inspect();
        if self.tuning.reshade.language != Some(self.lang) {
            self.tuning.reshade.rows = self
                .tuning
                .reshade
                .snapshot
                .as_ref()
                .map(|s| {
                    s.addons
                        .iter()
                        .map(|a| {
                            format!(
                                "{} · {} · {} · {}",
                                a.file,
                                crate::format::size_text(self.lang, a.size),
                                a.version.as_deref().unwrap_or(self.tr("mdl.version_unknown")),
                                a.product.as_deref().unwrap_or(&a.name)
                            )
                        })
                        .collect()
                })
                .unwrap_or_default();
            self.tuning.reshade.cached_label = self
                .tuning
                .reshade
                .cached
                .as_ref()
                .map(|cached| {
                    format!(
                        "ReShade {} · {}",
                        cached.component.version.as_deref().unwrap_or("?"),
                        self.tr(match cached.build {
                            core::Build::Full => "rs.full",
                            core::Build::Limited => "rs.limited",
                            core::Build::Unknown => "rs.unknown",
                        })
                    )
                })
                .unwrap_or_default();
            self.tuning.reshade.language = Some(self.lang);
        }
        let mut change = None;
        let mut pick = None;
        let mut policy = None;
        let mut offer = false;
        let mut download = false;
        let enabled = !self.busy() && !self.tuning.reshade.busy;
        let lang = self.lang;
        widgets::card(ui, |ui| {
            widgets::card_title(ui, t(lang, "rs.title"));
            ui.label(t(lang, "rs.description"));
            if let Some(snapshot) = &self.tuning.reshade.snapshot {
                ui.label(t(
                    lang,
                    match snapshot.build {
                        Some(core::Build::Full) => "rs.full",
                        Some(core::Build::Limited) => "rs.limited",
                        Some(core::Build::Unknown) => "rs.unknown",
                        None => "rs.missing",
                    },
                ));
                if !snapshot.addons.is_empty() && snapshot.build != Some(core::Build::Full) {
                    ui.colored_label(theme::WARN, t(lang, "rs.recommend_why"));
                }
                for (index, addon) in snapshot.addons.iter().enumerate() {
                    ui.push_id(&addon.file, |ui| {
                        ui.label(&self.tuning.reshade.rows[index]);
                        ui.horizontal_wrapped(|ui| {
                            let mut on = addon.enabled;
                            if ui
                                .add_enabled(enabled, egui::Checkbox::new(&mut on, t(lang, "rs.enabled")))
                                .changed()
                            {
                                change = Some(Change::Enable {
                                    file: addon.file.clone(),
                                    enabled: on,
                                });
                            }
                            if addon.tracked
                                && ui
                                    .add_enabled(
                                        enabled,
                                        egui::Button::new(t(
                                            lang,
                                            if addon.imported {
                                                "rs.remove_addon"
                                            } else {
                                                "rs.undo_addon"
                                            },
                                        )),
                                    )
                                    .clicked()
                            {
                                change = Some(Change::Remove(addon.file.clone()));
                            }
                        });
                    });
                }
                ui.label(t(
                    lang,
                    match snapshot.owner() {
                        core::NrOwner::Off => "rs.nr_off",
                        core::NrOwner::Addon => "rs.nr_addon",
                        core::NrOwner::Retoque => "rs.nr_retoque",
                    },
                ));
                if snapshot.addons.iter().any(|a| a.enabled && a.nr_candidate) {
                    let ignore = !core::yields_to_addons(snapshot.yield_to_addons.as_deref());
                    ui.add_enabled_ui(enabled, |ui| {
                        if ui.radio(!ignore, t(lang, "rs.yield")).clicked() && ignore {
                            policy = Some("true");
                        }
                        if ui.radio(ignore, t(lang, "rs.ignore")).clicked() && !ignore {
                            policy = Some("false");
                        }
                    });
                }
                if snapshot.menu_input_blocked {
                    ui.label(t(lang, "rs.input_title"));
                    if ui
                        .add_enabled(enabled, egui::Button::new(t(lang, "rs.input_fix")))
                        .on_hover_text(t(lang, "rs.input_why"))
                        .clicked()
                    {
                        change = Some(Change::MenuInput);
                    }
                }
                if snapshot.managed
                    && ui
                        .add_enabled(enabled, egui::Button::new(t(lang, "rs.uninstall")))
                        .clicked()
                {
                    change = Some(Change::Uninstall);
                }
            } else if self.tuning.reshade.error.is_none() {
                widgets::busy(ui, lang, t(lang, "rs.checking"), None, None);
            }
            // The downloaded copy is news only while the game has none: "ready to install", not a second status line.
            let absent = self.tuning.reshade.snapshot.as_ref().is_none_or(|s| s.build.is_none());
            if absent && !self.tuning.reshade.cached_label.is_empty() {
                ui.label(
                    RichText::new(format!("{} {}", t(lang, "rs.ready"), self.tuning.reshade.cached_label))
                        .small()
                        .color(theme::MUTED),
                );
            }
            ui.horizontal_wrapped(|ui| {
                offer = ui
                    .add_enabled(enabled, egui::Button::new(t(lang, "rs.install")))
                    .clicked();
                if ui
                    .add_enabled(enabled, egui::Button::new(t(lang, "rs.import")))
                    .clicked()
                {
                    pick = Some(false);
                }
                if ui.add_enabled(enabled, egui::Button::new(t(lang, "rs.add"))).clicked() {
                    pick = Some(true);
                }
            });
            if self.tuning.reshade.busy {
                widgets::busy(ui, lang, t(lang, "rs.preparing"), None, None);
            }
            if let Some(error) = &self.tuning.reshade.error {
                crate::error::card(ui, error, lang, &self.cmd_tx);
            }
            ui.label(RichText::new(t(lang, "rs.static")).small().color(theme::MUTED));
        });
        if self.tuning.reshade.consent {
            let details = |ui: &mut egui::Ui| {
                ui.add(egui::Label::new(RichText::new(t(lang, "rs.consent_details")).small()).wrap());
                ui.add(egui::Label::new(RichText::new(data::URL).small().monospace()).wrap());
                for (what, sha) in [("setup", data::SETUP_SHA), ("DLL", data::DLL_SHA)] {
                    ui.add(
                        egui::Label::new(RichText::new(format!("SHA-256 ({what}) {sha}")).small().monospace()).wrap(),
                    );
                }
            };
            match widgets::consent_modal_when(
                ui.ctx(),
                lang,
                "reshade-download",
                t(lang, "rs.consent_title"),
                &[t(lang, "rs.consent_body")],
                &download_label(lang, Some(data::SETUP_SIZE)),
                t(lang, "cancel"),
                Some(&details),
                enabled,
            ) {
                Some(widgets::Answer::Confirm) => download = true,
                Some(widgets::Answer::Cancel) => self.tuning.reshade.consent = false,
                None => {}
            }
        }
        ui.add_space(theme::CARD_GAP);
        if let Some(change) = change {
            self.reshade_request(change);
        }
        if let Some(value) = policy {
            self.autotune_request(
                self.tr("rs.review").into(),
                crate::autotune::Request {
                    edits: vec![("DlssNr".into(), "YieldToAddons".into(), value.into())],
                    ..Default::default()
                },
            );
        }
        if let Some(addon) = pick {
            self.reshade_pick(addon);
        }
        if offer {
            self.reshade_offer();
        }
        if download && let Some(target) = self.view.target.clone() {
            self.tuning.reshade.consent = false;
            self.tuning.reshade.busy = true;
            let request = self.next_request;
            self.jobs.spawn("reshade-download", None, move |_| {
                let result = (|| {
                    let root = retoque_data::lsfg_prepare::data_dir().map_err(|e| AppError::from(e.to_string()))?;
                    data::download(&root, true, &data::OfficialFetcher, &mut |_| {}).map_err(AppError::from)
                })();
                Outcome::ReShade(Box::new(Event::Cached {
                    target,
                    request,
                    result,
                }))
            });
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    use std::{
        fs,
        time::{Duration, Instant},
    };
    fn fixture(lang: Lang) -> (tempfile::TempDir, RetoqueApp) {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        fs::create_dir(&game).unwrap();
        fs::write(
            game.join("OptiScaler.ini"),
            b"[DlssNr]\nEnabled=true\n[Plugins]\nLoadReshade=true\n",
        )
        .unwrap();
        fs::write(game.join("ReShade64.dll"), b"MZ ReShade LoadFromDllMain").unwrap();
        fs::write(game.join("renodx-dlss5.addon64"), b"MZ addon").unwrap();
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        app.view.target = Some(game.clone());
        app.tuning.reshade.key = Some((game.clone(), app.tuning.facts_generation));
        app.tuning.reshade.snapshot = Some(data::inspect(&game).unwrap());
        (temp, app)
    }
    #[test]
    fn addon_list_nr_choice_and_review_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let (temp, app) = fixture(lang);
            let mut h = Harness::builder().with_size([1000., 1000.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.reshade_card(ui);
                },
                app,
            );
            h.get_by_label(t(lang, "rs.nr_addon"));
            h.get_by_label(t(lang, "rs.enabled"));
            h.get_by_label(t(lang, "rs.ignore")).click();
            h.run_steps(2);
            let deadline = Instant::now() + Duration::from_secs(8);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert!(review.allowed());
            assert!(review.plans.iter().flat_map(|p| p.steps()).any(
                |s| matches!(s,retoque_core::Step::SetIni{key,value,..} if key=="YieldToAddons" && value=="false")
            ));
            assert!(
                !fs::read_to_string(temp.path().join("game/OptiScaler.ini"))
                    .unwrap()
                    .contains("YieldToAddons")
            );
        }
    }
    #[test]
    fn download_needs_confirmation_and_cancel_does_no_work() {
        for lang in [Lang::En, Lang::Pt] {
            let (_temp, app) = fixture(lang);
            let mut h = Harness::builder()
                .with_size([1000., 1000.])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.reshade_card(ui), app);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "rs.install"))
                .click();
            // The dialog opens on the next frame and is measured once before it is drawn.
            h.run_steps(3);
            h.get_by_label(t(lang, "rs.consent_body"));
            assert!(!h.state().tuning.reshade.busy);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
            h.run_steps(2);
            assert!(!h.state().tuning.reshade.consent);
            assert!(!h.state().jobs.busy());
        }
    }
    #[test]
    fn automatic_optimization_does_not_install_reshade() {
        let (_, mut app) = fixture(Lang::En);
        app.tuning.reshade.snapshot.as_mut().unwrap().build = None;
        let recommendation = data::recommendation(app.tuning.reshade.snapshot.as_ref().unwrap()).unwrap();
        let composed = crate::autotune::optimize::compose(crate::autotune::optimize::Selection {
            recommendations: &[recommendation],
            ..Default::default()
        })
        .unwrap();
        assert!(composed.request.component.is_none());
        assert!(composed.prerequisites.is_empty());
    }
    #[test]
    fn drop_routes_addons_and_setup() {
        let temp = tempfile::tempdir().unwrap();
        for (name, route) in [
            ("x.addon64", crate::drop_import::Route::Addon),
            ("ReShade64.dll", crate::drop_import::Route::ReShade),
            ("ReShade_Setup_6.8.0_Addon.exe", crate::drop_import::Route::ReShade),
        ] {
            let path = temp.path().join(name);
            fs::write(&path, b"fixture").unwrap();
            assert_eq!(crate::drop_import::classify(&path), route);
        }
    }
}
#[cfg(test)]
mod offer_tests {
    use super::*;
    #[test]
    fn doctor_does_not_reinstall_a_cached_limited_build() {
        let temp = tempfile::tempdir().unwrap();
        let mut app = crate::app_tests::shell_tests::app();
        app.view.target = Some(temp.path().to_owned());
        app.tuning.reshade.cached = Some(data::Cached {
            build: core::Build::Limited,
            component: retoque_data::components::Component {
                id: "synthetic".into(),
                kind: retoque_data::components::ComponentKind::ReShade,
                version: None,
                label: "fixture".into(),
                root: temp.path().into(),
                files: vec![core::DLL.into()],
            },
        });
        app.reshade_offer();
        assert!(app.tuning.reshade.consent);
        assert!(!app.jobs.busy());
        assert!(!app.review_open);
    }
}
