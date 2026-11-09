//! Automatic, cancellable preparation in app data; game settings always use the reviewed Plan flow.
use crate::app::*;
use crate::autotune::{
    Request,
    lsfg::LsfgReadiness,
    recommend::{self, GameFacts},
};
use retoque_data::lsfg_prepare as prepare;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
#[derive(Default)]
pub(crate) struct State {
    source: Option<PathBuf>,
    request: u64,
    running: bool,
    download_consent: bool,
    cancel: Arc<AtomicBool>,
    progress: Option<prepare::Progress>,
    result: Option<prepare::Result<prepare::PreparedCache>>,
    /// "Optimize automatically" waiting for these shaders: the game and the facts generation it was asked for.
    pub(crate) then_optimize: Option<(PathBuf, u64)>,
}
impl Drop for State {
    fn drop(&mut self) {
        self.cancel.store(true, Ordering::Relaxed);
    }
}
pub enum Event {
    Progress {
        request: u64,
        progress: prepare::Progress,
    },
    Finished {
        request: u64,
        result: prepare::Result<prepare::PreparedCache>,
    },
}
impl RetoqueApp {
    fn lsfg_source(&self) -> Option<&Path> {
        self.saved
            .lossless_dir
            .as_deref()
            .or(self.tuning.lossless.as_deref())
            .or(self.tuning.detected_lossless.as_deref())
    }
    /// Starts (or joins) shader preparation for "Optimize automatically". App data only; false when there is no
    /// Lossless Scaling folder to prepare from.
    pub(crate) fn lsfg_prepare_for_optimize(&mut self, target: PathBuf) -> bool {
        self.lsfg_ensure_started();
        if self.lsfg.source.is_none() {
            return false;
        }
        if !self.lsfg.running {
            self.lsfg_start(false);
        }
        self.lsfg.then_optimize = Some((target, self.tuning.facts_generation));
        true
    }
    fn lsfg_ensure_started(&mut self) {
        if self.lsfg_source() != self.lsfg.source.as_deref() {
            let source = self.lsfg_source().map(Path::to_path_buf);
            self.lsfg.cancel.store(true, Ordering::Relaxed);
            let mut next = State::default();
            next.request = self.lsfg.request + 1;
            next.source = source;
            self.lsfg = next;
        }
        if self.lsfg.source.is_some() && !self.lsfg.running && self.lsfg.result.is_none() {
            self.lsfg_start(false);
        }
    }
    pub(crate) fn lsfg_recheck(&mut self) {
        // A fresh game inspection may follow a Lossless Scaling update in the same folder.
        // Reverify on the heavy lane next time the card is shown; never hash on a UI frame.
        if !self.lsfg.running && matches!(self.lsfg.result, Some(Ok(_))) {
            self.lsfg.result = None;
        }
    }
    fn lsfg_start(&mut self, consent: bool) {
        let Some(source) = self.lsfg.source.clone() else { return };
        self.lsfg.request += 1;
        let request = self.lsfg.request;
        self.lsfg.running = true;
        self.lsfg.download_consent |= consent;
        let consent = self.lsfg.download_consent;
        self.lsfg.result = None;
        self.lsfg.progress = Some(prepare::Progress::Inspecting);
        self.lsfg.cancel = Arc::new(AtomicBool::new(false));
        let cancel = self.lsfg.cancel.clone();
        let ctx = self.context.clone();
        self.jobs.spawn("lsfg-prepare", None, move |tx| {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                let data = prepare::data_dir()?;
                let exe = std::env::current_exe().map_err(prepare::Error::from)?;
                let directory = exe
                    .parent()
                    .ok_or_else(|| prepare::Error::Failed("Application directory unavailable".into()))?;
                prepare::ensure_lsfg_shaders(&source, &data, directory, consent, &cancel, &mut |progress| {
                    let _ = tx.send(Outcome::Lsfg(Event::Progress { request, progress }));
                    ctx.request_repaint();
                })
            }))
            .unwrap_or_else(|_| {
                Err(prepare::Error::Failed(
                    "Shader preparation worker stopped unexpectedly".into(),
                ))
            });
            Outcome::Lsfg(Event::Finished { request, result })
        });
    }
    pub(crate) fn lsfg_event(&mut self, event: Event) {
        match event {
            Event::Progress { request, progress } if request == self.lsfg.request => {
                self.lsfg.progress = Some(progress)
            }
            Event::Finished { request, result } if request == self.lsfg.request => {
                self.lsfg.running = false;
                let failed = result.is_err();
                self.lsfg.result = Some(result);
                // Resume a one-click optimization only for the same game and facts it was started for.
                if let Some((target, generation)) = self.lsfg.then_optimize.take() {
                    let same = self.view.target.as_ref() == Some(&target) && self.tuning.facts_generation == generation;
                    if failed {
                        if same {
                            self.tuning.sm86.error = Some(self.tr("optimize.lsfg_failed").into());
                        }
                    } else if same {
                        self.optimize_automatically();
                    }
                }
            }
            _ => {}
        }
    }
    pub(crate) fn autotune_ready(&self) -> bool {
        matches!(self.tuning.readiness, Some(LsfgReadiness::Ready))
            || (self.lsfg.source.as_deref() == self.lsfg_source() && matches!(self.lsfg.result, Some(Ok(_))))
    }
    pub(crate) fn autotune_readiness(&mut self, ui: &mut egui::Ui) {
        self.lsfg_ensure_started();
        if self.lsfg.running {
            ui.spinner();
            let key = if self.lsfg.progress == Some(prepare::Progress::Downloading) {
                "lsfg.downloading"
            } else {
                "lsfg.preparing"
            };
            ui.label(self.tr(key));
            if let Some(prepare::Progress::Compiling { done, total }) = self.lsfg.progress {
                ui.add(egui::ProgressBar::new(done as f32 / total as f32).text(format!("{done} / {total}")));
            }
            if ui.button(self.tr("lsfg.cancel")).clicked() {
                self.lsfg.cancel.store(true, Ordering::Relaxed);
            }
        } else if let Some(Err(error)) = &self.lsfg.result {
            match error {
                prepare::Error::MissingCompiler => {
                    ui.label(self.tr("lsfg.found"));
                    ui.add(egui::Label::new(self.tr("lsfg.download_prompt")).wrap());
                    if ui.button(self.tr("lsfg.download_confirm")).clicked() {
                        self.lsfg_start(true);
                    }
                }
                prepare::Error::Cancelled => {
                    ui.label(self.tr("lsfg.cancelled"));
                    if ui.button(self.tr("lsfg.retry")).clicked() {
                        self.lsfg_start(false);
                    }
                }
                prepare::Error::Failed(reason) => {
                    ui.colored_label(theme::WARN, self.tr("lsfg.failed"));
                    ui.add(egui::Label::new(reason).wrap());
                    if ui.button(self.tr("lsfg.retry")).clicked() {
                        self.lsfg_start(false);
                    }
                }
            }
        } else if self.autotune_ready() {
            ui.colored_label(theme::GOOD, self.tr("auto.ls_ready"));
        } else if self.lsfg.source.is_none() {
            ui.colored_label(theme::WARN, self.tr("auto.ls_missing"));
        } else {
            ui.label(self.tr("lsfg.found"));
        }
        if let Some(dir) = &self.lsfg.source {
            ui.label(RichText::new(short_path(dir)).small())
                .on_hover_text(dir.display().to_string());
        }
        if ui.button(self.tr("auto.choose_lossless")).clicked()
            && let Some(path) = rfd::FileDialog::new().pick_folder()
        {
            self.saved.lossless_dir = Some(path);
            if let Some(target) = self.view.target.clone() {
                self.autotune_refresh(target);
            }
            self.lsfg_ensure_started();
        }
    }
    pub(crate) fn autotune_lsfg_edits(&self, multiplier: u8, facts: &GameFacts) -> Vec<Edit> {
        let wine = matches!(
            self.selected_game().map(|g| &g.runtime),
            Some(Runtime::Wine { .. } | Runtime::Proton { .. })
        );
        let mut edits = recommend::lsfg_edits(multiplier, self.lsfg_source(), wine);
        edits.extend(recommend::lsfg_input_edits(facts));
        if let Some(Ok(cache)) = &self.lsfg.result
            && self.lsfg.source.as_deref() == self.lsfg_source()
        {
            edits.push(prepare::shader_dir_edit(cache, wine));
        }
        edits
    }
    pub(crate) fn lsfg_card(&mut self, ui: &mut egui::Ui) {
        // Scroll contents can inherit a wider minimum from another tab; constrain these cards to the viewport.
        ui.set_width((ui.clip_rect().right() - ui.next_widget_position().x - 4.0).max(100.0));
        ui.add_space(12.0);
        widgets::card(ui, |ui| {
            self.lsfg_ensure_started();
            let facts = self.view.intelligence.facts.clone();
            widgets::card_title(ui, self.tr("auto.lsfg"));
            ui.add(egui::Label::new(self.tr("auto.ls_help")).wrap());
            let ini = self
                .view
                .ini
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .and_then(|i| i.as_ref());
            let current = if facts.lsfg_enabled == Some(true) {
                ini.and_then(|i| i.get("LSFG", "Multiplier"))
                    .and_then(|s| s.parse::<u8>().ok())
                    .unwrap_or(2)
            } else {
                0
            };
            let can_on = self.autotune_ready() && self.autotune_known_components() && !facts.sm86_installed;
            ui.horizontal_wrapped(|ui| {
                for (value, key) in [(0, "auto.off"), (2, "auto.x2"), (3, "auto.x3"), (4, "auto.x4")] {
                    if ui
                        .add_enabled(
                            facts.installed && !self.busy() && (value == 0 || can_on),
                            egui::Button::new(self.tr(key)).selected(current == value),
                        )
                        .clicked()
                    {
                        let edits = if value == 0 {
                            recommend::lsfg_off_edits()
                        } else {
                            self.autotune_lsfg_edits(value, &facts)
                        };
                        self.autotune_request(
                            format!("{} · {}", self.tr("auto.lsfg"), self.tr(key)),
                            Request {
                                component: None,
                                edits,
                                ..Default::default()
                            },
                        );
                    }
                }
            });
            if !facts.installed {
                ui.add(egui::Label::new(self.tr("auto.need_install")).wrap());
            }
            if facts.sm86_installed {
                ui.colored_label(theme::WARN, self.tr("auto.ls_conflict"));
            }
            self.autotune_readiness(ui);
        });
    }
}

#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    #[test]
    fn download_requires_visible_consent_and_both_languages_work() {
        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.saved.lossless_dir = Some("/synthetic/LS".into());
            app.lsfg.source = app.saved.lossless_dir.clone();
            app.lsfg.result = Some(Err(prepare::Error::MissingCompiler));
            let mut harness = Harness::new_ui_state(|ui, app| app.autotune_readiness(ui), app);
            harness.run();
            harness.get_by_label(t(lang, "lsfg.download_confirm"));
            harness.get_by_label(t(lang, "lsfg.download_prompt"));
            assert!(!harness.state().lsfg.running);
            assert!(!harness.state().jobs.busy());
        }
    }
    #[test]
    fn one_click_resumes_only_for_the_same_game_after_shaders_are_ready() {
        let lang = Lang::En;
        let ready = || {
            Ok(prepare::PreparedCache {
                directory: "/synthetic/cache".into(),
                dll_sha256: "hash".into(),
            })
        };
        let setup = || {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.saved.lossless_dir = Some("/synthetic/LS".into());
            app.lsfg.source = app.saved.lossless_dir.clone();
            // A preparation already in flight: joining it must not spawn another worker.
            app.lsfg.running = true;
            app.lsfg.request = 7;
            app.view.target = Some("/synthetic/game".into());
            app.tuning.facts_generation = 3;
            assert!(app.lsfg_prepare_for_optimize("/synthetic/game".into()));
            assert!(!app.jobs.busy());
            app
        };
        // Same game and facts: the optimization runs again (here it finds nothing else to do).
        let mut app = setup();
        app.lsfg_event(Event::Finished {
            request: 7,
            result: ready(),
        });
        assert!(app.lsfg.then_optimize.is_none());
        assert!(app.autotune_ready());
        let message = app
            .tuning
            .sm86
            .error
            .as_ref()
            .map(|e| e.to_string())
            .unwrap_or_default();
        assert_ne!(message, t(lang, "optimize.lsfg_preparing"));
        assert!(app.review.is_none());
        // The user moved to another game meanwhile: nothing resumes.
        let mut app = setup();
        app.view.target = Some("/synthetic/other".into());
        app.tuning.sm86.error = None;
        app.lsfg_event(Event::Finished {
            request: 7,
            result: ready(),
        });
        assert!(app.lsfg.then_optimize.is_none());
        assert!(app.tuning.sm86.error.is_none());
        assert!(app.review.is_none());
        // Preparation failed: say so, apply nothing.
        let mut app = setup();
        app.lsfg_event(Event::Finished {
            request: 7,
            result: Err(prepare::Error::Failed("synthetic".into())),
        });
        let message = app
            .tuning
            .sm86
            .error
            .as_ref()
            .map(|e| e.to_string())
            .unwrap_or_default();
        assert_eq!(message, t(lang, "optimize.lsfg_failed"));
        assert!(app.review.is_none());
        // No Lossless Scaling folder at all: refuse up front.
        let mut app = crate::app_tests::shell_tests::app();
        app.view.target = Some("/synthetic/game".into());
        assert!(!app.lsfg_prepare_for_optimize("/synthetic/game".into()));
        assert!(app.lsfg.then_optimize.is_none());
    }
    #[test]
    fn cancel_and_stale_results_do_not_publish_or_enable() {
        let mut app = crate::app_tests::shell_tests::app();
        app.saved.lossless_dir = Some("/synthetic/LS".into());
        app.lsfg.source = app.saved.lossless_dir.clone();
        app.lsfg.running = true;
        app.lsfg.request = 4;
        app.lsfg.progress = Some(prepare::Progress::Compiling { done: 24, total: 99 });
        let mut harness = Harness::new_ui_state(|ui, app| app.autotune_readiness(ui), app);
        harness.get_by_label(t(Lang::En, "lsfg.cancel")).click();
        harness.run_steps(2);
        assert!(harness.state().lsfg.cancel.load(Ordering::Relaxed));
        harness.state_mut().lsfg_event(Event::Finished {
            request: 3,
            result: Ok(prepare::PreparedCache {
                directory: "/synthetic/cache".into(),
                dll_sha256: "hash".into(),
            }),
        });
        assert!(!harness.state().autotune_ready());
    }
    #[test]
    fn ready_cache_is_only_a_settings_proposal_and_bound_to_source() {
        let mut app = crate::app_tests::shell_tests::app();
        app.saved.lossless_dir = Some("/synthetic/LS".into());
        app.tuning.lossless = app.saved.lossless_dir.clone();
        app.lsfg.source = app.saved.lossless_dir.clone();
        app.lsfg.result = Some(Ok(prepare::PreparedCache {
            directory: "/synthetic/cache".into(),
            dll_sha256: "hash".into(),
        }));
        assert!(app.autotune_ready());
        assert!(
            app.autotune_lsfg_edits(2, &GameFacts::default())
                .iter()
                .any(|(s, k, v)| s == "LSFG" && k == "ShaderDir" && v == "/synthetic/cache")
        );
        assert!(app.review.is_none());
        assert!(!app.jobs.busy());
        app.saved.lossless_dir = Some("/synthetic/other-LS".into());
        assert!(!app.autotune_ready());
    }
}
