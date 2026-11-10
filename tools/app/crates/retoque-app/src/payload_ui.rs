//! Payload source selection (beside the app, chosen folder, or official GitHub release) in the "Retoque files"
//! dialog, consent modal, background download/verification, and the update check. Updating a game is the game
//! page's own reviewed action.

use crate::app::*;
use retoque_data::payload_fetch;
use std::{cmp::Ordering, sync::Arc};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub enum PayloadSource {
    #[default]
    BesideApp,
    Folder,
    Release,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Consent {
    Download,
    Check,
}

#[derive(Clone, Debug)]
pub struct CheckOutcome {
    pub latest: Option<payload_fetch::Release>,
    pub local_record: Option<payload_fetch::Record>,
    pub active_version: Option<String>,
    pub update_available: bool,
}

pub enum Event {
    Startup(Option<Box<payload_fetch::Record>>),
    Progress {
        request: u64,
        bytes: u64,
    },
    Checked {
        request: u64,
        result: Result<Box<CheckOutcome>, AppError>,
    },
    Downloaded {
        request: u64,
        result: Result<Box<payload_fetch::Record>, AppError>,
    },
}

#[derive(Default)]
pub struct State {
    pub consent: Option<Consent>,
    pub busy: bool,
    pub request: u64,
    pub downloaded: u64,
    pub release: Option<payload_fetch::Release>,
    pub record: Option<payload_fetch::Record>,
    pub active_version: Option<String>,
    pub no_release: bool,
    pub update_available: bool,
    pub error: Option<AppError>,
    pub fetcher: Option<Arc<dyn payload_fetch::Fetcher + Send + Sync>>,
    pub cache_dir: Option<PathBuf>,
    pub beside_dir: Option<PathBuf>,
    /// The "Retoque files" dialog (where the files come from and official releases), opened from the status bar's
    /// one button.
    pub files_open: bool,
}

impl RetoqueApp {
    pub(crate) fn beside_payload_dir(&self) -> Option<PathBuf> {
        if let Some(dir) = &self.payload_ui.beside_dir {
            return model::is_payload(dir).then(|| dir.clone());
        }
        model::find_payload()
    }

    fn payload_cache_dir(&self) -> std::io::Result<PathBuf> {
        match &self.payload_ui.cache_dir {
            Some(dir) => Ok(dir.clone()),
            None => payload_fetch::default_cache(),
        }
    }

    pub(crate) fn payload_init(&mut self) {
        let want_release = self.saved.payload_source == Some(PayloadSource::Release) || self.payload.is_none();
        let cache_dir = self.payload_ui.cache_dir.clone();
        self.jobs.spawn("payload-startup", None, move |_| {
            let record = (|| {
                let root = match cache_dir {
                    Some(dir) => dir,
                    None => payload_fetch::default_cache()?,
                };
                if !root.exists() {
                    return Ok::<_, std::io::Error>(None);
                }
                payload_fetch::Cache::open(&root)?.latest_local()
            })()
            .ok()
            .flatten();
            let _ = want_release;
            Outcome::PayloadFetch(Box::new(Event::Startup(record.map(Box::new))))
        });
    }

    pub(crate) fn payload_event(&mut self, event: Event) {
        match event {
            Event::Startup(Some(rec)) => {
                let rec = *rec;
                let use_it = self.saved.payload_source == Some(PayloadSource::Release) || self.payload.is_none();
                if use_it && model::is_payload(&rec.payload_dir) {
                    self.payload = Some(rec.payload_dir.clone());
                    self.payload_build = Some(Ok(rec.optiscaler_sha256.clone()));
                    self.payload_ui.active_version =
                        rec.file_version.clone().or_else(|| Some(rec.release.version.clone()));
                    self.hash_payload();
                }
                self.payload_ui.release = Some(rec.release.clone());
                self.payload_ui.record = Some(rec);
            }
            Event::Progress { request, bytes } if request == self.payload_ui.request => {
                self.payload_ui.downloaded = bytes;
            }
            Event::Checked { request, result } if request == self.payload_ui.request => {
                self.payload_ui.busy = false;
                match result {
                    Ok(outcome) => {
                        let outcome = *outcome;
                        self.payload_ui.error = None;
                        self.payload_ui.active_version = outcome.active_version;
                        if let Some(rec) = outcome.local_record {
                            self.payload_ui.record = Some(rec);
                        }
                        match outcome.latest {
                            Some(release) => {
                                self.payload_ui.no_release = false;
                                self.payload_ui.update_available = outcome.update_available;
                                self.payload_ui.release = Some(release);
                            }
                            None => {
                                self.payload_ui.no_release = true;
                                self.payload_ui.update_available = false;
                                self.payload_ui.release = None;
                                self.keep_local_payload_fallback();
                            }
                        }
                        self.library_state.invalidate();
                    }
                    Err(err) => {
                        self.payload_ui.error = Some(err);
                    }
                }
            }
            Event::Downloaded { request, result } if request == self.payload_ui.request => {
                self.payload_ui.busy = false;
                match result {
                    Ok(record) => {
                        let record = *record;
                        self.payload_ui.error = None;
                        self.payload_ui.no_release = false;
                        self.payload_ui.update_available = false;
                        self.payload_ui.active_version = record
                            .file_version
                            .clone()
                            .or_else(|| Some(record.release.version.clone()));
                        self.payload_ui.release = Some(record.release.clone());
                        self.saved.payload_source = Some(PayloadSource::Release);
                        self.payload = Some(record.payload_dir.clone());
                        self.payload_build = Some(Ok(record.optiscaler_sha256.clone()));
                        self.payload_ui.record = Some(record);
                        self.error = None;
                        self.hash_payload();
                        self.library_state.invalidate();
                    }
                    Err(err) => {
                        if err.to_string().contains("no release published yet") {
                            self.payload_ui.no_release = true;
                            self.payload_ui.update_available = false;
                            self.payload_ui.error = None;
                            self.keep_local_payload_fallback();
                        } else {
                            self.payload_ui.error = Some(err);
                        }
                    }
                }
            }
            _ => {}
        }
    }

    fn keep_local_payload_fallback(&mut self) {
        if self.payload.as_ref().is_some_and(|p| model::is_payload(p)) {
            return;
        }
        if let Some(beside) = self.beside_payload_dir() {
            self.saved.payload_source = Some(PayloadSource::BesideApp);
            self.payload = Some(beside);
            self.hash_payload();
        } else if let Some(folder) = self.saved.payload.clone().filter(|p| model::is_payload(p)) {
            self.saved.payload_source = Some(PayloadSource::Folder);
            self.payload = Some(folder);
            self.hash_payload();
        }
    }

    fn payload_start(&mut self, consent: Consent) {
        if self.payload_ui.busy {
            return;
        }
        self.payload_ui.busy = true;
        self.payload_ui.error = None;
        self.payload_ui.downloaded = 0;
        self.payload_ui.request += 1;
        let request = self.payload_ui.request;
        let checked = self.payload_ui.release.clone();
        let active_payload = self.payload.clone();
        let active_sha = self.payload_build.as_ref().and_then(|r| r.as_ref().ok()).cloned();
        let fetcher = self.payload_ui.fetcher.clone();
        let cache_dir = self.payload_cache_dir();
        let ctx = self.context.clone();

        self.jobs.spawn("payload-fetch", None, move |tx| {
            let fetcher_ref: &dyn payload_fetch::Fetcher = match &fetcher {
                Some(f) => f.as_ref(),
                None => &payload_fetch::GithubFetcher,
            };
            match consent {
                Consent::Check => {
                    let result = (|| -> Result<Box<CheckOutcome>, AppError> {
                        let root = cache_dir?;
                        let cache = payload_fetch::Cache::open(&root)?;
                        let local_record = cache.latest_local()?;
                        let active_version = active_payload
                            .as_ref()
                            .and_then(|p| retoque_core::pe_version::file_version(&p.join("OptiScaler.dll")))
                            .or_else(|| local_record.as_ref().map(|r| r.release.version.clone()));
                        let latest = payload_fetch::latest(fetcher_ref)?;
                        let update_available = match &latest {
                            Some(rel) => {
                                let already_cached = cache.contains(rel)?;
                                let version_newer = match local_record.as_ref().map(|r| &r.release.version) {
                                    Some(local_ver) => {
                                        payload_fetch::compare_versions(&rel.version, local_ver)
                                            == Some(Ordering::Greater)
                                    }
                                    None => true,
                                };
                                let active_differs = active_sha
                                    .as_ref()
                                    .zip(local_record.as_ref())
                                    .is_none_or(|(act, rec)| *act != rec.optiscaler_sha256 || !already_cached);
                                !already_cached || version_newer || active_differs
                            }
                            None => false,
                        };
                        Ok(Box::new(CheckOutcome {
                            latest,
                            local_record,
                            active_version,
                            update_available,
                        }))
                    })();
                    Outcome::PayloadFetch(Box::new(Event::Checked { request, result }))
                }
                Consent::Download => {
                    let result = (|| -> Result<Box<payload_fetch::Record>, AppError> {
                        let root = cache_dir?;
                        let cache = payload_fetch::Cache::open(&root)?;
                        let release = match checked {
                            Some(r) => Some(r),
                            None => payload_fetch::latest(fetcher_ref)?,
                        };
                        let Some(release) = release else {
                            return Err(
                                std::io::Error::new(std::io::ErrorKind::NotFound, "no release published yet").into(),
                            );
                        };
                        let mut prev = 0u64;
                        let record = cache.download(fetcher_ref, &release, &mut |bytes| {
                            if bytes.saturating_sub(prev) >= 256 * 1024 || bytes == release.size {
                                prev = bytes;
                                let _ = tx.send(Outcome::PayloadFetch(Box::new(Event::Progress { request, bytes })));
                                ctx.request_repaint();
                            }
                        })?;
                        Ok(Box::new(record))
                    })();
                    Outcome::PayloadFetch(Box::new(Event::Downloaded { request, result }))
                }
            }
        });
    }

    /// Which Retoque the app installs: a release says its own version; files from anywhere else are named by their
    /// build, the id the game page's tiles show. OptiScaler's DLL version is not Retoque's.
    fn payload_name(&self) -> Option<String> {
        let p = self.payload.as_ref()?;
        Some(match &self.payload_ui.record {
            Some(rec) if &rec.payload_dir == p => format!("Retoque {}", rec.release.version),
            _ => match self.payload_build.as_ref().and_then(|r| r.as_ref().ok()) {
                Some(hash) => format!("Retoque · {} {}", self.tr("st.build"), &hash[..hash.len().min(8)]),
                None => self.tr("app.payload").to_owned(),
            },
        })
    }

    /// The status bar's button: the screen-reader label names the dialog it opens and what it shows.
    pub(crate) fn payload_button_label(&self) -> String {
        match self.payload_name() {
            Some(name) => format!("{}: {name}", self.tr("app.payload")),
            None => self.tr("app.payload").to_owned(),
        }
    }

    pub(crate) fn payload_bar(&mut self, ui: &mut egui::Ui) {
        // One button, which Retoque the app installs; where its files come from and official releases are in the
        // "Retoque files" dialog it opens. Updating a game is the game page's own action.
        let text = self.payload_name().unwrap_or_else(|| self.tr("app.payload").to_owned());
        let response = ui.button(text);
        let label = self.payload_button_label();
        response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Button, true, &label));
        let response = match &self.payload {
            Some(p) => response.on_hover_text(crate::format::display_path(p)),
            None => response,
        };
        if response.clicked() {
            self.payload_ui.files_open = true;
        }
        if self.payload.is_none() {
            ui.colored_label(theme::WARN, RichText::new(self.tr("payload.missing")).small());
        }
        if self.payload_ui.busy {
            ui.spinner();
        } else if self.payload_ui.update_available {
            // The accent dot never stands alone: the words say what it means.
            let (rect, _) = ui.allocate_exact_size(egui::vec2(8.0, 8.0), egui::Sense::hover());
            ui.painter().circle_filled(rect.center(), 4.0, theme::ACCENT);
            ui.colored_label(theme::ACCENT, RichText::new(self.tr("payload.new_release")).small());
        }
        if let Some(error) = &self.payload_ui.error {
            ui.colored_label(theme::BAD, RichText::new(diagnostic(self.lang, error)).small());
        }
    }

    /// "Retoque files": the current files, where they come from, and official releases. Downloads still ask first
    /// (`payload_dialog`), and no game changes from here.
    fn payload_files_modal(&mut self, ctx: &egui::Context) {
        if !self.payload_ui.files_open {
            return;
        }
        let mut close = false;
        let modal = egui::Modal::new(egui::Id::unique("payload-files"))
            .frame(widgets::modal_frame())
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 560.0));
                ui.heading(self.tr("app.payload"));
                ui.add(egui::Label::new(RichText::new(self.tr("payload.files_note")).color(theme::MUTED)).wrap());
                ui.add_space(8.0);
                match self.payload.clone() {
                    Some(p) => {
                        if let Some(name) = self.payload_name() {
                            ui.label(RichText::new(name).strong());
                        }
                        ui.label(RichText::new(short_path(&p)).small().monospace().color(theme::MUTED))
                            .on_hover_text(crate::format::display_path(&p));
                    }
                    None => {
                        ui.add(
                            egui::Label::new(
                                RichText::new(format!("{} {}", self.tr("app.payload"), self.tr("app.payload.none")))
                                    .color(theme::WARN),
                            )
                            .wrap(),
                        );
                    }
                }

                widgets::section_header(ui, self.tr("payload.files_source"));
                ui.horizontal_wrapped(|ui| self.payload_source_buttons(ui));

                widgets::section_header(ui, self.tr("payload.files_updates"));
                ui.horizontal_wrapped(|ui| self.payload_release_controls(ui));
                if let Some(error) = &self.payload_ui.error {
                    ui.colored_label(theme::BAD, diagnostic(self.lang, error));
                }
                ui.add_space(12.0);
                close = ui.button(self.tr("close")).clicked();
            });
        if close || modal.should_close() {
            self.payload_ui.files_open = false;
        }
    }

    fn payload_source_buttons(&mut self, ui: &mut egui::Ui) {
        if let Some(beside) = self.beside_payload_dir()
            && self.payload.as_ref() != Some(&beside)
            && ui.button(self.tr("payload.source.beside")).clicked()
        {
            self.saved.payload_source = Some(PayloadSource::BesideApp);
            self.payload = Some(beside);
            self.payload_ui.active_version = self
                .payload
                .as_ref()
                .and_then(|p| retoque_core::pe_version::file_version(&p.join("OptiScaler.dll")));
            self.hash_payload();
        }

        if ui.button(self.tr("app.payload.choose")).clicked() {
            self.cmd_tx.send(AppCommand::Remedy(Remedy::ChoosePayloadDir, None));
        }

        if let Some(folder) = self.saved.payload.clone().filter(|p| model::is_payload(p))
            && self.payload.as_ref() != Some(&folder)
            && ui.button(self.tr("payload.source.folder")).clicked()
        {
            self.saved.payload_source = Some(PayloadSource::Folder);
            self.payload = Some(folder);
            self.payload_ui.active_version = self
                .payload
                .as_ref()
                .and_then(|p| retoque_core::pe_version::file_version(&p.join("OptiScaler.dll")));
            self.hash_payload();
        }

        if let Some(rec) = self.payload_ui.record.clone()
            && self.payload.as_ref() != Some(&rec.payload_dir)
            && model::is_payload(&rec.payload_dir)
            && ui.button(self.tr("payload.source.release")).clicked()
        {
            self.saved.payload_source = Some(PayloadSource::Release);
            self.payload = Some(rec.payload_dir.clone());
            self.payload_build = Some(Ok(rec.optiscaler_sha256.clone()));
            self.payload_ui.active_version = rec.file_version.clone().or_else(|| Some(rec.release.version.clone()));
            self.hash_payload();
        }
    }

    fn payload_release_controls(&mut self, ui: &mut egui::Ui) {
        if ui
            .add_enabled(!self.payload_ui.busy, egui::Button::new(self.tr("payload.check")))
            .clicked()
        {
            self.payload_ui.consent = Some(Consent::Check);
        }

        if self.payload_ui.busy {
            ui.spinner();
            ui.label(format!(
                "{} ({:.1} MiB)",
                self.tr("payload.progress"),
                self.payload_ui.downloaded as f64 / 1_048_576.0
            ));
        } else if self.payload_ui.no_release {
            ui.colored_label(theme::WARN, self.tr("payload.no_release"));
        } else if let Some(rel) = self.payload_ui.release.clone() {
            if self.payload_ui.update_available {
                ui.colored_label(
                    theme::ACCENT,
                    format!("{}: v{}", self.tr("payload.new_release"), rel.version),
                );
                if ui.button(self.tr("payload.download_now")).clicked() {
                    self.payload_ui.consent = Some(Consent::Download);
                }
            } else {
                ui.colored_label(
                    theme::GOOD,
                    format!("{}: v{}", self.tr("payload.up_to_date"), rel.version),
                );
            }
        }
        if !self.payload_ui.update_available
            && ui
                .add_enabled(!self.payload_ui.busy, egui::Button::new(self.tr("payload.download")))
                .clicked()
        {
            self.payload_ui.consent = Some(Consent::Download);
        }
    }

    pub(crate) fn payload_dialog(&mut self, ctx: &egui::Context) {
        self.payload_files_modal(ctx);
        let Some(consent) = self.payload_ui.consent else {
            return;
        };
        let mut confirm = false;
        let mut cancel = false;
        egui::Modal::new(egui::Id::unique("payload-consent")).show(ctx, |ui| {
            ui.set_max_width(520.0);
            ui.heading(self.tr("payload.consent_title"));
            ui.label(self.tr(match consent {
                Consent::Check => "payload.check_consent",
                Consent::Download => "payload.download_consent",
            }));
            ui.label(RichText::new(self.tr("payload.hosts_note")).small().color(theme::MUTED));
            ui.horizontal(|ui| {
                confirm = ui.button(self.tr("payload.confirm")).clicked();
                cancel = ui.button(self.tr("payload.later")).clicked();
            });
        });
        if cancel {
            self.payload_ui.consent = None;
        }
        if confirm {
            self.payload_ui.consent = None;
            self.payload_start(consent);
        }
    }
}

#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    use std::{
        fs,
        time::{Duration, Instant},
    };

    fn write_payload_dir(dir: &Path, ver: (u16, u16, u16, u16)) {
        fs::create_dir_all(dir).unwrap();
        let opti = payload_fetch::versioned_pe_fixture(ver, b"OptiScaler working as synthetic");
        let fwd = payload_fetch::versioned_pe_fixture(
            ver,
            b"dlssnr_call_create dlssnr_call_evaluate dlssnr_call_set_extras dlssnr_abi_version",
        );
        fs::write(dir.join("OptiScaler.dll"), &opti).unwrap();
        fs::write(dir.join("nvngx.dll_dlssnr.dll"), &fwd).unwrap();
        fs::write(
            dir.join("OptiScaler.ini"),
            b"[Log]\nLogToFile=true\nLogLevel=2\n[DlssNr]\nEnabled=auto\n",
        )
        .unwrap();
    }

    fn build_release_fixture(tag: &str, ver: (u16, u16, u16, u16)) -> Arc<MockFetcher> {
        let (_, zip_bytes, sums_bytes, api_json) = payload_fetch::synthetic_release_fixture(tag, ver, None);
        Arc::new(MockFetcher {
            api_json: Some(api_json),
            zip_bytes,
            sums_bytes,
        })
    }

    struct MockFetcher {
        api_json: Option<Vec<u8>>,
        zip_bytes: Vec<u8>,
        sums_bytes: Vec<u8>,
    }

    impl payload_fetch::Fetcher for MockFetcher {
        fn get(&self, url: &str, limit: u64, progress: &mut dyn FnMut(u64)) -> std::io::Result<Vec<u8>> {
            let bytes = if url == payload_fetch::RELEASES_URL || url == payload_fetch::LATEST_URL {
                match &self.api_json {
                    Some(b) => b,
                    None => {
                        return Err(std::io::Error::new(
                            std::io::ErrorKind::NotFound,
                            "no release published yet",
                        ));
                    }
                }
            } else if url.ends_with("/SHA256SUMS") {
                &self.sums_bytes
            } else {
                &self.zip_bytes
            };
            assert!(bytes.len() as u64 <= limit);
            progress(bytes.len() as u64);
            Ok(bytes.clone())
        }
    }

    fn wait_idle(h: &mut Harness<'_, RetoqueApp>) {
        h.run_steps(2);
        let deadline = Instant::now() + Duration::from_secs(5);
        while h.state().payload_ui.busy || h.state().jobs.busy() {
            h.state_mut().receive();
            h.run_steps(1);
            assert!(Instant::now() < deadline, "timed out waiting for worker");
            std::thread::yield_now();
        }
        h.state_mut().receive();
        h.run_steps(2);
    }

    #[test]
    fn no_release_case_states_plainly_and_keeps_beside_and_folder_sources_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let beside = temp.path().join("beside");
            let folder = temp.path().join("chosen");
            write_payload_dir(&beside, (10, 0, 0, 1));
            write_payload_dir(&folder, (10, 0, 0, 1));

            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.payload = Some(beside.clone());
            app.saved.payload = Some(folder.clone());
            app.payload_ui.beside_dir = Some(beside.clone());
            app.payload_ui.cache_dir = Some(temp.path().join("cache"));
            app.payload_ui.fetcher = Some(Arc::new(MockFetcher {
                api_json: Some(b"[]".to_vec()),
                zip_bytes: Vec::new(),
                sums_bytes: Vec::new(),
            }));

            let mut h = Harness::builder().with_size([1200., 400.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.status_bar(ui);
                },
                app,
            );
            h.run_steps(2);

            // The status bar is one button; the source and release controls are in the dialog it opens.
            assert!(h.query_by_label(t(lang, "payload.check")).is_none());
            assert!(h.query_by_label(t(lang, "payload.source.folder")).is_none());
            let files = h.state().payload_button_label();
            h.get_by_role_and_label(egui::Role::Button, &files).click();
            h.run_steps(2);
            h.get_by_label(t(lang, "payload.files_note"));

            // Click Check for updates -> opens confirmation modal; cancelling does not start network work.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.check"))
                .click();
            h.run_steps(2);
            h.get_by_label(t(lang, "payload.consent_title"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.later"))
                .click();
            h.run_steps(2);
            assert!(!h.state().payload_ui.no_release);

            // Confirm check when no release exists yet.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.check"))
                .click();
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.confirm"))
                .click();
            wait_idle(&mut h);

            assert!(h.state().payload_ui.no_release);
            h.get_by_label(t(lang, "payload.no_release"));
            assert_eq!(h.state().payload.as_ref(), Some(&beside));

            // Source (b) chosen folder and (a) beside app remain usable in the same dialog.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.source.folder"))
                .click();
            wait_idle(&mut h);
            assert_eq!(h.state().payload.as_ref(), Some(&folder));
            assert_eq!(h.state().saved.payload_source, Some(PayloadSource::Folder));

            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.source.beside"))
                .click();
            wait_idle(&mut h);
            assert_eq!(h.state().payload.as_ref(), Some(&beside));
            assert_eq!(h.state().saved.payload_source, Some(PayloadSource::BesideApp));

            // "Close" closes the dialog.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "close")).click();
            h.run_steps(2);
            assert!(!h.state().payload_ui.files_open);
            assert!(h.query_by_label(t(lang, "payload.files_note")).is_none());
        }
    }

    #[test]
    fn check_and_download_release_offers_reviewed_game_update_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let old_payload = temp.path().join("old-payload");
            write_payload_dir(&old_payload, (10, 0, 0, 1));

            let game_dir = temp.path().join("game");
            fs::create_dir_all(&game_dir).unwrap();
            fs::write(game_dir.join("Game.exe"), b"MZ game").unwrap();

            // Install old payload (10.0.0.1) into the temp game first.
            let initial_plan = retoque_core::plan_install(&retoque_core::InstallOptions {
                game_dir: game_dir.clone(),
                source_dir: old_payload.clone(),
                proxy: "dxgi.dll".into(),
                enable_nr: true,
                copy_nvngx: false,
                ..Default::default()
            })
            .unwrap()
            .into_verified()
            .unwrap();
            retoque_core::apply(&initial_plan, &mut |_| {}).unwrap();
            let initial_status = retoque_core::status(&game_dir).unwrap();
            assert_eq!(initial_status.proxy_version.as_deref(), Some("10.0.0.1"));
            let before_update_bytes = fs::read(game_dir.join("dxgi.dll")).unwrap();

            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.payload = Some(old_payload.clone());
            app.payload_ui.beside_dir = Some(old_payload.clone());
            app.payload_ui.cache_dir = Some(temp.path().join("cache"));
            app.payload_ui.fetcher = Some(build_release_fixture("retoque-v0.3.1", (10, 0, 0, 2)));
            app.view.target = Some(game_dir.clone());
            app.view.status = Some(Ok(initial_status));
            app.hash_payload();

            let mut h = Harness::builder().with_size([1300., 600.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.status_bar(ui);
                    app.handle_commands();
                },
                app,
            );
            wait_idle(&mut h);
            let files = h.state().payload_button_label();
            h.get_by_role_and_label(egui::Role::Button, &files).click();
            h.run_steps(2);

            // 1. Check for updates -> finds v0.3.1 (newer than local).
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.check"))
                .click();
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.confirm"))
                .click();
            wait_idle(&mut h);

            assert!(h.state().payload_ui.update_available);
            assert_eq!(h.state().payload_ui.release.as_ref().unwrap().version, "0.3.1");
            // The status bar says so in words beside its accent dot.
            assert!(h.query_all_by_label(t(lang, "payload.new_release")).count() >= 1);

            // 2. Click Download update -> confirm -> unpacks into app data and becomes active payload.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.download_now"))
                .click();
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "payload.confirm"))
                .click();
            wait_idle(&mut h);

            assert_eq!(h.state().saved.payload_source, Some(PayloadSource::Release));
            let rec = h.state().payload_ui.record.clone().unwrap();
            assert_eq!(h.state().payload.as_ref(), Some(&rec.payload_dir));
            assert_eq!(rec.file_version.as_deref(), Some("10.0.0.2"));

            // 3. The game page now offers "Update and optimize"; the status bar has no game action of its own.
            assert!(h.query_by_label_contains(t(lang, "shell.update_optimize")).is_none());
            assert_eq!(h.state().optimize_action_key(), "shell.update_optimize");
            // Its update goes through the normal reviewed install plan.
            let options = h.state().install_inputs().unwrap();
            h.state_mut()
                .start_plan(PlanKind::Install, PlanInputs::Install(options));
            wait_idle(&mut h);

            assert!(h.state().review_open);
            let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert!(matches!(review.kind, ReviewKind::Standard(PlanKind::Install)));
            assert!(review.allowed());
            // Game on disk is untouched before review confirmation!
            assert_eq!(fs::read(game_dir.join("dxgi.dll")).unwrap(), before_update_bytes);
        }
    }
}
