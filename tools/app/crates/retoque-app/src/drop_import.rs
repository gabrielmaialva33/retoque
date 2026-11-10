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
        let mut confirm = false;
        let mut cancel = false;
        let modal = egui::Modal::new(egui::Id::unique("drop_review")).show(ctx, |ui| {
            ui.set_max_width(600.);
            ui.heading(self.tr("drop.title"));
            if let Some((path, route)) = self.intuitive.drop.pending.first() {
                ui.label(path.display().to_string());
                ui.label(self.tr(match route {
                    Route::Addon => "rs.drop_addon",
                    Route::ReShade => "rs.drop_reshade",
                    Route::Model => "drop.model",
                    Route::Component => "drop.component",
                    Route::Game => "drop.game",
                    Route::Unsupported => "drop.unsupported",
                }));
                if *route != Route::Unsupported {
                    confirm = ui
                        .add_enabled(!self.busy(), theme::primary(self.tr("drop.confirm")))
                        .clicked();
                }
            } else {
                ui.spinner();
            }
            cancel = ui.button(self.tr("cancel")).clicked();
        });
        if cancel || modal.should_close() {
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
