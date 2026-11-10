//! Global drops are classified on the worker, then explicitly confirmed before importing/registering.
use crate::app::*;
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Route {
    Model,
    Addon,
    ReShade,
    Component,
    Game,
    Unsupported,
}
#[derive(Default)]
pub(crate) struct DropState {
    request: u64,
    pub pending: Vec<(PathBuf, Route)>,
    loading: usize,
}
impl DropState {
    pub fn open(&self) -> bool {
        self.loading > 0 || !self.pending.is_empty()
    }
    pub fn receive(&mut self, request: u64, paths: Vec<(PathBuf, Route)>) {
        if request == self.request {
            self.pending.extend(paths);
            self.loading = self.loading.saturating_sub(1);
        }
    }
}
pub(crate) fn classify(path: &Path) -> Route {
    let name = path
        .file_name()
        .unwrap_or_default()
        .to_string_lossy()
        .to_ascii_lowercase();
    if path.is_file() && retoque_core::reshade::addon_filename(&name) {
        Route::Addon
    } else if path.is_file()
        && (name == "reshade64.dll" || (name.starts_with("reshade_setup_") && name.ends_with(".exe")))
    {
        Route::ReShade
    } else if path.is_file() && name.starts_with("nvngx_dlssnr") && name.ends_with(".dll") {
        Route::Model
    } else if path.is_dir() && path.join("dlssg_sm86.ini").is_file() {
        Route::Component
    } else if path.is_dir() {
        Route::Game
    } else {
        Route::Unsupported
    }
}
impl RetoqueApp {
    pub(crate) fn global_drop_input(&mut self, ctx: &egui::Context) {
        // Consume once before the per-tab UI so its legacy drop handler cannot also queue an import.
        let files = ctx.input_mut(|i| std::mem::take(&mut i.raw.dropped_files));
        if files.is_empty() {
            return;
        }
        let request = self.intuitive.drop.request;
        self.intuitive.drop.loading += 1;
        self.jobs.spawn("drop-classify", None, move |_| {
            let paths = files
                .into_iter()
                .map(|f| {
                    let p = f.path().to_path_buf();
                    let r = classify(&p);
                    (p, r)
                })
                .collect();
            Outcome::DropClassified { request, paths }
        });
    }
    pub(crate) fn drop_sheet(&mut self, ctx: &egui::Context) {
        if ctx.input(|i| !i.raw.hovered_files.is_empty()) {
            let painter = ctx.layer_painter(egui::LayerId::new(egui::Order::Tooltip, egui::Id::unique("drop_hover")));
            painter.rect_filled(ctx.content_rect(), 0., Color32::from_black_alpha(210));
            painter.text(
                ctx.content_rect().center(),
                egui::Align2::CENTER_CENTER,
                self.tr("drop.hover"),
                egui::FontId::proportional(24.),
                Color32::WHITE,
            );
        }
        if self.review_open
            || self.folder_dialog.is_some()
            || self.intuitive.palette.open
            || !self.intuitive.drop.open()
        {
            return;
        }
        let lang = self.lang;
        let first = self.intuitive.drop.pending.first().cloned();
        let answer = match &first {
            Some((path, route)) if *route != Route::Unsupported => {
                let (body, confirm) = match route {
                    Route::Addon => ("rs.drop_addon", "drop.import"),
                    Route::ReShade => ("rs.drop_reshade", "drop.import"),
                    Route::Model => ("drop.model", "drop.import"),
                    Route::Component => ("drop.component", "drop.import"),
                    _ => ("drop.game", "drop.add_game"),
                };
                let name = path
                    .file_name()
                    .map(|n| n.to_string_lossy().into_owned())
                    .unwrap_or_else(|| path.display().to_string());
                let details = |ui: &mut egui::Ui| {
                    ui.add(egui::Label::new(RichText::new(path.display().to_string()).small().monospace()).wrap());
                };
                // The user dropped it, so the dialog is dismissed with "Cancel"; a write in flight waits.
                widgets::consent_modal_when(
                    ctx,
                    lang,
                    "drop-review",
                    t(lang, "drop.title"),
                    &[&name, t(lang, body)],
                    t(lang, confirm),
                    t(lang, "cancel"),
                    Some(&details),
                    !self.busy(),
                )
            }
            other => {
                // Still checking, or nothing the app can import: information, not a choice.
                let mut close = false;
                let modal = egui::Modal::new(egui::Id::unique("drop_review"))
                    .frame(widgets::modal_frame())
                    .show(ctx, |ui| {
                        ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 560.0));
                        ui.label(RichText::new(t(lang, "drop.title")).size(theme::TITLE).strong());
                        ui.add_space(6.0);
                        match other {
                            Some((path, _)) => {
                                ui.add(egui::Label::new(path.display().to_string()).wrap());
                                ui.add(egui::Label::new(t(lang, "drop.unsupported")).wrap());
                            }
                            None => widgets::busy(ui, lang, t(lang, "drop.checking"), None, None),
                        }
                        ui.add_space(12.0);
                        close = ui
                            .button(t(lang, if other.is_some() { "close" } else { "cancel" }))
                            .clicked();
                    });
                (close || modal.should_close()).then_some(widgets::Answer::Cancel)
            }
        };
        let cancel = answer == Some(widgets::Answer::Cancel);
        let confirm = answer == Some(widgets::Answer::Confirm);
        if cancel {
            self.intuitive.drop.request += 1;
            self.intuitive.drop.pending.clear();
            self.intuitive.drop.loading = 0;
        } else if confirm {
            let (path, route) = self.intuitive.drop.pending.remove(0);
            match route {
                Route::Addon => self.reshade_import(path, true),
                Route::ReShade => self.reshade_import(path, false),
                Route::Model => self.cmd_tx.send(AppCommand::ImportModel(path)),
                Route::Component => self.autotune_catalog(Some(path), None),
                Route::Game => {
                    if self.saved.register_folder(crate::folders_ui::FolderKind::Game, path) {
                        self.rescan();
                    }
                }
                Route::Unsupported => {}
            }
        }
    }
}
