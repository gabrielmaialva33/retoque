//! Windows-only Preferences controls. Network and writes begin only after the matching consent dialog.
use crate::{
    app::RetoqueApp,
    app_install::{self, InstallPlan, windows},
    i18n::{Lang, t},
    widgets,
};
use retoque_data::{
    app_release,
    payload_fetch::{GithubFetcher, Release},
};
use std::{io, sync::mpsc};

#[derive(Default)]
pub(crate) struct State {
    review: Option<Review>,
    result: Option<ResultState>,
    rx: Option<mpsc::Receiver<io::Result<ResultState>>>,
    error: Option<crate::error::AppError>,
}
enum Review {
    Install(InstallPlan),
    Check,
    Download(Release),
    Restart,
}
enum ResultState {
    Installed,
    Checked(Option<Release>),
    Staged(app_release::Staged),
}

impl State {
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
                None => {}
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
        let Some(review) = self.app_installer.review.as_ref() else {
            return;
        };
        let lang = self.lang;
        let (title, body, confirm) = review_text(lang, review, self.app_installer.result.as_ref());
        let mut accepted = false;
        let mut cancelled = false;
        let modal = egui::Modal::new(egui::Id::unique("app-install-review"))
            .frame(widgets::modal_frame())
            .show(ctx, |ui| {
                ui.set_width(520.0);
                ui.heading(title);
                egui::ScrollArea::vertical().max_height(360.0).show(ui, |ui| {
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
                    ui.label(t(lang, "app_install.preserve"));
                });
                ui.horizontal(|ui| {
                    // Restart cannot interrupt a game-install write already in flight.
                    accepted = ui.add_enabled(!self.jobs.busy(), egui::Button::new(confirm)).clicked();
                    cancelled = ui.button(t(lang, "cancel")).clicked();
                });
            });
        if cancelled || modal.should_close() {
            self.app_installer.review = None;
            return;
        }
        if !accepted {
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
    };
    let confirm = if let Review::Download(release) = review {
        crate::format::download_label(lang, Some(release.size))
    } else {
        t(lang, confirm).into()
    };
    (t(lang, title).into(), t(lang, body).into(), confirm)
}
