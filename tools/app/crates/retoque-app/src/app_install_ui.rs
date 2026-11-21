//! Windows-only Preferences controls. Network and writes begin only after the matching consent dialog.
//! The Windows setup (`retoque-app.exe --setup <dir>`) opens the same reviews at startup; see [`State::start_setup`].
use crate::{
    app::RetoqueApp,
    app_install::{self, InstallPlan, SetupAction, windows},
    i18n::{Lang, t},
    theme, widgets,
};
use retoque_data::{
    app_release,
    payload_fetch::{GithubFetcher, Release},
};
use std::{io, path::PathBuf, sync::mpsc};

#[derive(Default)]
pub(crate) struct State {
    review: Option<Review>,
    waiting_host: bool,
    result: Option<ResultState>,
    rx: Option<mpsc::Receiver<io::Result<ResultState>>>,
    error: Option<crate::error::AppError>,
    /// The setup's private folder when this app was started by the Windows setup.
    setup: Option<PathBuf>,
}
enum Review {
    Install(InstallPlan),
    Check,
    Download(Release),
    Restart,
    /// The setup carries a newer app than the installed one.
    SetupUpdate {
        installed: String,
    },
    /// The installed app is the setup's version or newer.
    SetupCurrent {
        installed: String,
    },
    /// A setup step failed; the dialog shows why and offers to try again.
    SetupFailed,
}
enum ResultState {
    Installed,
    Checked(Option<Release>),
    Staged(app_release::Staged),
    /// The setup handed its update to the helper, which waits for this app to close.
    UpdateScheduled,
}

impl State {
    pub(crate) fn busy(&self) -> bool {
        self.rx.is_some() || self.waiting_host
    }
    pub(crate) fn open(&self) -> bool {
        self.review.is_some()
    }
    fn run(&mut self, ctx: &egui::Context, job: impl FnOnce() -> io::Result<ResultState> + Send + 'static) {
        let (tx, rx) = mpsc::channel();
        let ctx = ctx.clone();
        self.rx = Some(rx);
        self.error = None;
        std::thread::spawn(move || {
            let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(job))
                .unwrap_or_else(|_| Err(io::Error::other("app management worker panicked")));
            let _ = tx.send(result);
            ctx.request_repaint();
        });
    }
    fn failure(&mut self, error: io::Error) {
        self.error = Some(crate::error::AppError::io(std::path::PathBuf::from("Retoque"), error));
    }
    /// Started by the Windows setup: open the install review, the update review or the already-installed notice,
    /// by the Apps & features registration (`app_install::setup_action`). Nothing is written before a confirmation.
    pub(crate) fn start_setup(&mut self, dir: PathBuf) {
        self.setup = Some(dir);
        self.error = None;
        let action = windows::installed_version()
            .map(|registered| app_install::setup_action(registered.as_deref(), app_install::version()));
        match action {
            Ok(SetupAction::Install) => match windows::prepare() {
                Ok(plan) => self.review = Some(Review::Install(plan)),
                Err(e) => self.failure(e),
            },
            Ok(SetupAction::Update { installed }) => self.review = Some(Review::SetupUpdate { installed }),
            Ok(SetupAction::Current { installed }) => self.review = Some(Review::SetupCurrent { installed }),
            Err(e) => self.failure(e),
        }
    }
}

impl RetoqueApp {
    pub(crate) fn app_install_controls(&mut self, ui: &mut egui::Ui) {
        let lang = self.lang;
        widgets::section_header(ui, t(lang, "app_install.heading"));
        let installed = windows::paths()
            .ok()
            .zip(std::env::current_exe().ok())
            .is_some_and(|(p, e)| p.is_installed(&e));
        ui.add_enabled_ui(self.app_installer.rx.is_none(), |ui| {
            if !installed && ui.button(t(lang, "app_install.install")).clicked() {
                match windows::prepare() {
                    Ok(plan) => {
                        self.app_installer.review = Some(Review::Install(plan));
                        self.app_installer.error = None;
                    }
                    Err(e) => self.app_installer.failure(e),
                }
            }
            if installed && ui.button(t(lang, "app_install.check")).clicked() {
                self.app_installer.review = Some(Review::Check);
            }
            match &self.app_installer.result {
                Some(ResultState::Installed) => {
                    ui.label(t(lang, "app_install.installed"));
                    if ui.button(t(lang, "app_install.restart_offer")).clicked() {
                        self.app_installer.review = Some(Review::Restart);
                    }
                }
                Some(ResultState::Checked(Some(release)))
                    if app_release::newer(&release.version, app_install::version()) =>
                {
                    ui.label(t(lang, "app_install.available").replace("{version}", &release.version));
                    if ui.button(t(lang, "app_install.update")).clicked() {
                        self.app_installer.review = Some(Review::Download(release.clone()));
                    }
                }
                Some(ResultState::Checked(Some(_))) => {
                    ui.label(t(lang, "app_install.current"));
                }
                Some(ResultState::Checked(None)) => {
                    ui.label(t(lang, "app_install.no_release"));
                }
                Some(ResultState::Staged(_)) => {
                    ui.label(t(lang, "app_install.ready"));
                    if ui.button(t(lang, "app_install.restart_offer")).clicked() {
                        self.app_installer.review = Some(Review::Restart);
                    }
                }
                // Never kept: the app closes as soon as the setup's update is handed to the helper.
                Some(ResultState::UpdateScheduled) | None => {}
            }
        });
        if self.app_installer.rx.is_some() {
            widgets::busy(ui, lang, t(lang, "app_install.busy"), None, None);
        }
        if let Some(error) = &self.app_installer.error {
            crate::error::body(ui, error, lang, &self.cmd_tx);
        }
    }

    pub(crate) fn app_install_dialog(&mut self, ctx: &egui::Context) {
        if let Some(rx) = &self.app_installer.rx {
            match rx.try_recv() {
                Ok(result) => {
                    self.app_installer.rx = None;
                    match result {
                        Ok(ResultState::UpdateScheduled) => {
                            // The helper waits for this process; closing lets it replace the installed files.
                            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                        }
                        Ok(result) => {
                            if matches!(result, ResultState::Installed | ResultState::Staged(_)) {
                                self.app_installer.review = Some(Review::Restart);
                            }
                            self.app_installer.result = Some(result);
                        }
                        Err(e) => self.app_installer.failure(e),
                    }
                }
                Err(mpsc::TryRecvError::Disconnected) => {
                    self.app_installer.rx = None;
                    self.app_installer
                        .failure(io::Error::other("app management worker disconnected"));
                }
                Err(mpsc::TryRecvError::Empty) => {}
            }
        }
        let lang = self.lang;
        if self.app_installer.setup.is_some() && self.app_installer.review.is_none() {
            if self.app_installer.rx.is_some() {
                // The setup has no Preferences open to show progress in.
                egui::Modal::new(egui::Id::unique("app-install-setup-busy"))
                    .frame(widgets::modal_frame(ctx))
                    .show(ctx, |ui| {
                        ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 520.0));
                        widgets::busy(ui, lang, t(lang, "app_install.busy"), None, None);
                    });
                return;
            }
            if self.app_installer.error.is_some() {
                self.app_installer.review = Some(Review::SetupFailed);
            }
        }
        let Some(review) = self.app_installer.review.as_ref() else {
            return;
        };
        let (title, body, confirm) = review_text(lang, review, self.app_installer.result.as_ref());
        let mut accepted = false;
        let mut cancelled = false;
        let modal = egui::Modal::new(egui::Id::unique("app-install-review"))
            .frame(widgets::modal_frame(ctx))
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 520.0));
                ui.heading(title);
                egui::ScrollArea::vertical()
                    .max_height((ctx.content_rect().height() - 190.0).clamp(160.0, 360.0))
                    .show(ui, |ui| {
                        ui.label(body);
                        if let Review::Install(plan) = review {
                            ui.label(plan.paths.install.display().to_string());
                            ui.label(t(lang, "app_install.files"));
                            for file in &plan.files {
                                ui.label(&file.name);
                            }
                            ui.label(t(lang, "app_install.shortcuts"));
                            for shortcut in &plan.paths.shortcuts {
                                ui.label(shortcut.display().to_string());
                            }
                            ui.label(t(lang, "app_install.registration"));
                            crate::widgets::collapsible(
                                ui,
                                "app-install-registry",
                                t(lang, "technical.details"),
                                crate::widgets::Fold::Details,
                                |ui| {
                                    ui.label(format!("HKCU\\{}", app_install::UNINSTALL_KEY));
                                    for (name, value) in
                                        app_install::registry_values(&plan.paths, app_install::version(), plan.bytes())
                                    {
                                        let value = match value {
                                            app_install::RegistryValue::Text(v) => v,
                                            app_install::RegistryValue::Dword(v) => v.to_string(),
                                        };
                                        ui.label(format!("{name}: {value}"));
                                    }
                                },
                            );
                        }
                        if matches!(review, Review::Check | Review::Download(_)) {
                            ui.label(t(lang, "payload.hosts_note"));
                        }
                        if let (Review::SetupFailed, Some(error)) = (review, &self.app_installer.error) {
                            crate::error::body(ui, error, lang, &self.cmd_tx);
                        }
                        ui.label(t(lang, "app_install.preserve"));
                    });
                if self.app_installer.waiting_host {
                    widgets::busy(ui, lang, t(lang, "any.stopping"), None, None);
                }
                ui.horizontal(|ui| {
                    cancelled = ui.button(t(lang, "cancel")).clicked();
                    // Restart cannot interrupt a game-install write already in flight.
                    accepted = ui
                        .add_enabled(
                            !self.jobs.busy() && !self.app_installer.waiting_host,
                            theme::primary(ui, &confirm),
                        )
                        .clicked();
                });
            });
        // A setup's dialog is the whole setup: only Cancel ends it, and it closes the app it started.
        let setup = self.app_installer.setup.is_some();
        if cancelled || (modal.should_close() && !setup) {
            self.app_installer.review = None;
            self.app_installer.waiting_host = false;
            if setup {
                ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            }
            return;
        }
        if self.app_installer.waiting_host {
            if self.any_window.active() || self.jobs.busy() {
                ctx.request_repaint_after(std::time::Duration::from_millis(50));
                return;
            }
            self.app_installer.waiting_host = false;
            accepted = true;
        }
        if !accepted {
            return;
        }
        let replaces_files = matches!(
            self.app_installer.review.as_ref(),
            Some(Review::Install(_) | Review::Restart | Review::SetupUpdate { .. })
        );
        if replaces_files && self.any_window.active() {
            self.any_window.stop();
            self.app_installer.waiting_host = true;
            return;
        }
        match self.app_installer.review.take().unwrap() {
            Review::Install(plan) => self.app_installer.run(ctx, move || {
                windows::install(&plan)?;
                Ok(ResultState::Installed)
            }),
            Review::Check => self
                .app_installer
                .run(ctx, || app_release::latest(&GithubFetcher).map(ResultState::Checked)),
            Review::Download(release) => self.app_installer.run(ctx, move || {
                let p = windows::paths()?;
                if !p.is_installed(&std::env::current_exe()?) {
                    return Err(io::Error::other("app is no longer installed"));
                }
                app_install::plain_path(&p.install)?;
                app_release::download(&GithubFetcher, &release, &p.install).map(ResultState::Staged)
            }),
            Review::SetupUpdate { .. } => {
                let Some(dir) = self.app_installer.setup.clone() else {
                    return;
                };
                self.app_installer.run(ctx, move || {
                    windows::setup_update(&dir)?;
                    Ok(ResultState::UpdateScheduled)
                })
            }
            Review::SetupCurrent { .. } => match windows::relaunch() {
                Ok(()) => ctx.send_viewport_cmd(egui::ViewportCommand::Close),
                Err(e) => self.app_installer.failure(e),
            },
            Review::SetupFailed => {
                if let Some(dir) = self.app_installer.setup.clone() {
                    self.app_installer.start_setup(dir);
                }
            }
            Review::Restart => {
                let result = match self.app_installer.result.take() {
                    Some(ResultState::Staged(stage)) => windows::schedule_update(stage),
                    Some(ResultState::Installed) => windows::relaunch(),
                    _ => Err(io::Error::other("no prepared app restart")),
                };
                match result {
                    Ok(()) => ctx.send_viewport_cmd(egui::ViewportCommand::Close),
                    Err(e) => self.app_installer.failure(e),
                }
            }
        }
    }
}

fn review_text(lang: Lang, review: &Review, result: Option<&ResultState>) -> (String, String, String) {
    let (title, body, confirm) = match review {
        Review::Install(_) => (
            "app_install.install_title",
            "app_install.install_body",
            "app_install.install_confirm",
        ),
        Review::Check => ("app_install.check_title", "app_install.check_body", "check_now"),
        Review::Download(_) => ("app_install.update_title", "app_install.download_body", "download"),
        Review::Restart => (
            "app_install.restart_title",
            if matches!(result, Some(ResultState::Installed)) {
                "app_install.installed"
            } else {
                "app_install.restart_body"
            },
            "app_install.restart_confirm",
        ),
        Review::SetupUpdate { .. } => (
            "app_install.update_title",
            "app_install.setup_update_body",
            "app_install.setup_update_confirm",
        ),
        Review::SetupCurrent { .. } => (
            "app_install.setup_current_title",
            "app_install.setup_current_body",
            "app_install.setup_open",
        ),
        Review::SetupFailed => (
            "app_install.setup_failed_title",
            "app_install.failed",
            "app_install.setup_retry",
        ),
    };
    let confirm = if let Review::Download(release) = review {
        crate::format::download_label(lang, Some(release.size))
    } else {
        t(lang, confirm).into()
    };
    let body = match review {
        Review::SetupUpdate { installed } | Review::SetupCurrent { installed } => t(lang, body)
            .replace("{installed}", if installed.is_empty() { "?" } else { installed })
            .replace("{version}", app_install::version()),
        _ => t(lang, body).into(),
    };
    (t(lang, title).into(), body, confirm)
}
