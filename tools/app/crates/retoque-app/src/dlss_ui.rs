//! DLSS inspection/downloads use the heavy worker. Every game mutation enters the existing review.
use crate::app::*;
use retoque_data::dlss;
#[derive(Clone, Debug)]
pub enum Change {
    Update(dlss::Update),
    Restore { install_dir: PathBuf },
}
#[derive(Default)]
pub struct State {
    pub selected_for: Option<PathBuf>,
    key: Option<(PathBuf, u64)>,
    files: Vec<dlss::GameDll>,
    rows: Vec<String>,
    rows_lang: Option<Lang>,
    ready: bool,
    restorable: bool,
    busy: bool,
    consent: bool,
    error: Option<AppError>,
}
pub enum Event {
    Inspected {
        root: PathBuf,
        generation: u64,
        result: Result<(Vec<dlss::GameDll>, bool, bool), AppError>,
    },
    Downloaded(Result<(), AppError>),
}
impl RetoqueApp {
    pub(crate) fn dlss_install_dir(&self) -> Option<PathBuf> {
        self.selected_game()
            .map(|g| g.install_dir.clone())
            .or_else(|| self.view.target.clone())
    }
    pub(crate) fn dlss_update_request(&self) -> Option<Change> {
        self.tuning.dlss.ready.then(|| {
            Some(Change::Update(dlss::Update {
                install_dir: self.dlss_install_dir()?,
                data_dir: retoque_data::lsfg_prepare::data_dir().ok()?,
            }))
        })?
    }
    pub(crate) fn dlss_request_update(&mut self) {
        if let Some(change) = self.dlss_update_request() {
            self.autotune_request(
                self.tr("dlss.update").into(),
                crate::autotune::Request {
                    dlss: Some(change),
                    ..Default::default()
                },
            );
        } else {
            self.tab = Tab::Model;
            self.tuning.dlss.error = Some(self.tr("dlss.download_first").into());
        }
    }
    /// The game folder holds an app receipt for Retoque (as opposed to only a root DLSS receipt).
    fn dlss_target_has_receipt(&self) -> bool {
        matches!(&self.view.status, Some(Ok(status)) if status.receipt.is_some())
    }
    /// Name the review and the button after what will really be undone.
    pub(crate) fn dlss_restore_label(&self) -> &'static str {
        match (self.dlss_target_has_receipt(), self.tuning.dlss.restorable) {
            (true, true) => "dlss.restore",
            (false, true) => "dlss.restore_only",
            _ => "shell.review_remove",
        }
    }
    pub(crate) fn dlss_restore(&mut self) {
        let Some(install_dir) = self.dlss_install_dir() else {
            return;
        };
        self.autotune_request(
            self.tr(self.dlss_restore_label()).into(),
            crate::autotune::Request {
                dlss: Some(Change::Restore { install_dir }),
                ..Default::default()
            },
        );
    }
    pub(crate) fn dlss_event(&mut self, event: Event) {
        match event {
            Event::Inspected {
                root,
                generation,
                result,
            } if self
                .tuning
                .dlss
                .key
                .as_ref()
                .is_some_and(|(r, g)| r == &root && *g == generation) =>
            {
                match result {
                    Ok((files, ready, restorable)) => {
                        self.tuning.dlss.restorable = restorable;
                        self.tuning.dlss.files = files;
                        self.tuning.dlss.rows_lang = None;
                        self.tuning.dlss.ready = ready;
                    }
                    Err(e) => self.tuning.dlss.error = Some(e),
                }
            }
            Event::Downloaded(result) => {
                self.tuning.dlss.busy = false;
                self.tuning.dlss.key = None;
                match result {
                    Ok(()) => self.tuning.dlss.ready = true,
                    Err(e) => self.tuning.dlss.error = Some(e),
                }
                self.refresh_intelligence();
            }
            _ => {}
        }
    }
    fn dlss_inspect(&mut self) {
        let Some(root) = self
            .selected_game()
            .map(|g| g.install_dir.as_path())
            .or(self.view.target.as_deref())
        else {
            return;
        };
        let generation = self.tuning.facts_generation;
        if self
            .tuning
            .dlss
            .key
            .as_ref()
            .is_some_and(|(r, g)| r == root && *g == generation)
        {
            return;
        }
        let root = root.to_owned();
        self.tuning.dlss.key = Some((root.clone(), generation));
        self.tuning.dlss.files.clear();
        self.tuning.dlss.rows.clear();
        self.tuning.dlss.rows_lang = None;
        let target = self.view.target.clone().unwrap_or_else(|| root.clone());
        self.tuning.dlss.error = None;
        self.jobs.spawn("dlss-inspect", None, move |_| {
            let result = (|| {
                let files = dlss::inspect(&root)?;
                let data = retoque_data::lsfg_prepare::data_dir().map_err(|e| std::io::Error::other(e.to_string()))?;
                let restorable = retoque_core::dlss_root_state(&root).ok().flatten().is_some()
                    || retoque_core::status(&target).ok().is_some_and(|s| s.receipt.is_some());
                Ok::<_, std::io::Error>((files, dlss::verified_catalog(&data)?.is_some(), restorable))
            })()
            .map_err(AppError::from);
            Outcome::Dlss(Event::Inspected {
                root,
                generation,
                result,
            })
        });
    }
    pub(crate) fn dlss_optimize_checkbox(&mut self, ui: &mut egui::Ui) {
        let target = self.view.target.as_ref();
        let mut selected = target.is_some() && self.tuning.dlss.selected_for.as_ref() == target;
        if ui.checkbox(&mut selected, self.tr("dlss.optimize")).changed() {
            self.tuning.dlss.selected_for = if selected { target.cloned() } else { None };
        }
    }
    pub(crate) fn dlss_card(&mut self, ui: &mut egui::Ui) {
        self.dlss_inspect();
        if self.tuning.dlss.rows_lang != Some(self.lang) {
            self.tuning.dlss.rows = self
                .tuning
                .dlss
                .files
                .iter()
                .map(|file| {
                    // Only an older known version is offered an update; equal or newer reads as current.
                    let offer = match file.version.as_deref() {
                        Some(version) if dlss::current(version) => self.tr("dlss.current").to_string(),
                        _ => format!("{} {}", dlss::VERSION, self.tr("dlss.available")),
                    };
                    format!(
                        "{} · {} {} · {}",
                        file.path,
                        self.tr("dlss.installed"),
                        file.version.as_deref().unwrap_or(self.tr("mdl.version_unknown")),
                        offer
                    )
                })
                .collect();
            self.tuning.dlss.rows_lang = Some(self.lang);
        }
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("dlss.title"));
            for row in &self.tuning.dlss.rows {
                ui.label(row);
            }
            ui.label(self.tr("dlss.caution"));
            if self.tuning.dlss.busy {
                ui.spinner();
                ui.label(self.tr("dlss.downloading"));
            } else if !self.tuning.dlss.ready {
                if ui.button(self.tr("dlss.download")).clicked() {
                    self.tuning.dlss.consent = true;
                }
            } else if ui
                .add_enabled(
                    !self.busy() && !self.tuning.dlss.files.is_empty(),
                    egui::Button::new(self.tr("dlss.update")),
                )
                .clicked()
            {
                self.dlss_request_update();
            }
            if self.tuning.dlss.restorable
                && ui
                    .add_enabled(!self.busy(), egui::Button::new(self.tr(self.dlss_restore_label())))
                    .clicked()
            {
                self.dlss_restore();
            }
            if self.tuning.dlss.consent {
                ui.label(self.tr("dlss.consent"));
                ui.hyperlink_to(
                    self.tr("dlss.license"),
                    "https://github.com/NVIDIA/DLSS/blob/v310.9.16/LICENSE.txt",
                );
                if ui.button(self.tr("dlss.confirm_download")).clicked() {
                    self.tuning.dlss.consent = false;
                    self.tuning.dlss.busy = true;
                    self.jobs.spawn("dlss-download", None, move |_| {
                        let result = (|| {
                            let data = retoque_data::lsfg_prepare::data_dir()
                                .map_err(|e| std::io::Error::other(e.to_string()))?;
                            dlss::download(&data, true, &dlss::NvidiaFetcher, &mut |_| {}).map(|_| ())
                        })()
                        .map_err(AppError::from);
                        Outcome::Dlss(Event::Downloaded(result))
                    });
                }
                if ui.button(self.tr("cancel")).clicked() {
                    self.tuning.dlss.consent = false;
                }
            }
            if let Some(error) = &self.tuning.dlss.error {
                ui.colored_label(theme::BAD, diagnostic(self.lang, error));
            }
        });
    }
}

impl State {
    pub(crate) fn download_failed(&mut self) {
        self.busy = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    #[test]
    fn standalone_root_restore_opens_review_in_both_languages() {
        use std::{
            fs,
            time::{Duration, Instant},
        };
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let game = temp.path().join("game");
            let target = game.join("Binaries");
            let source = temp.path().join("catalog");
            fs::create_dir_all(&target).unwrap();
            fs::create_dir_all(&source).unwrap();
            fs::write(game.join("nvngx_dlss.dll"), b"original").unwrap();
            fs::write(source.join("nvngx_dlss.dll"), b"updated").unwrap();
            fs::write(source.join("receipt-marker.txt"), dlss::MARKER).unwrap();
            let plan = retoque_core::plan_dlss_update(&game, &target, &source, &["nvngx_dlss.dll".into()]).unwrap();
            retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some(target.clone());
            app.selected = Some("synthetic".into());
            app.library_state.games.push(Game {
                id: "synthetic".into(),
                name: "Synthetic".into(),
                store: Store::Manual,
                install_dir: game.clone(),
                runtime: Runtime::Unknown,
                targets: vec![],
                dlss_anywhere: vec!["nvngx_dlss.dll".into()],
                registered: true,
            });
            app.tuning.dlss.key = Some((game.clone(), app.tuning.facts_generation));
            app.tuning.dlss.restorable = true;
            let mut h = Harness::builder().with_size([1000., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.dlss_card(ui);
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "dlss.restore_only"))
                .click();
            h.run_steps(2);
            let deadline = Instant::now() + Duration::from_secs(8);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert!(review.allowed());
            assert_eq!(review.plans.len(), 1);
            assert_eq!(review.plans[0].game_dir(), game.canonicalize().unwrap());
            assert_eq!(fs::read(game.join("nvngx_dlss.dll")).unwrap(), b"updated");
            let ReviewKind::Autotune(request) = &review.kind else {
                panic!("expected compound review")
            };
            let prepared = crate::autotune::Prepared {
                target: target.clone(),
                request: (**request).clone(),
                plans: review.plans.clone(),
                groups: review.groups.clone(),
            };
            crate::autotune::apply(&prepared, &mut |_| {}).unwrap();
            assert_eq!(fs::read(game.join("nvngx_dlss.dll")).unwrap(), b"original");
        }
    }
    #[test]
    fn consent_cancel_and_default_off_optimize_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some(temp.path().into());
            app.tuning.dlss.key = Some((temp.path().into(), app.tuning.facts_generation));
            app.tuning.dlss.files = vec![dlss::GameDll {
                path: "nvngx_dlss.dll".into(),
                version: Some("310.1.0".into()),
            }];
            let mut h = Harness::builder().with_size([1000., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.dlss_card(ui);
                    app.dlss_optimize_checkbox(ui);
                },
                app,
            );
            assert!(h.state().tuning.dlss.selected_for.is_none());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "dlss.download"))
                .click();
            h.run_steps(2);
            h.get_by_label(t(lang, "dlss.consent"));
            assert!(!h.state().jobs.busy());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
            h.run_steps(2);
            assert!(!h.state().tuning.dlss.consent);
            assert!(!h.state().jobs.busy());
            h.get_by_label(t(lang, "dlss.optimize")).click();
            h.run_steps(2);
            assert_eq!(h.state().tuning.dlss.selected_for.as_deref(), Some(temp.path()));
            assert!(!temp.path().join(".retoque").exists());
        }
    }
}
