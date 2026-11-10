//! The window: the library on the left, the selected game on the right in four tabs. Every write is shown as a plan
//! first and carried out by a background job; the UI thread only reads what the jobs send back.

pub(crate) use std::collections::{BTreeMap, HashMap};
pub(crate) use std::path::{Path, PathBuf};

pub(crate) use egui::{Color32, RichText};
pub(crate) use retoque_core::{InstallOptions, Plan, Refusal, Status, Step, ini::Ini, receipt::ReceiptStatus};
pub(crate) use retoque_data::logs::Summary;
pub(crate) use retoque_data::models::{Catalog, Model};
pub(crate) use retoque_data::settings::{self, Kind};
pub(crate) use retoque_discovery::{Game, Runtime, Store};
pub(crate) use serde::{Deserialize, Serialize};

pub(crate) use crate::commands::{AppCommand, CommandSender};
pub(crate) use crate::error::{AppError, Remedy};
pub(crate) use crate::format::*;
pub(crate) use crate::i18n::{Lang, diagnostic, t};
pub(crate) use crate::jobs::TaskHub;
pub(crate) use crate::modals::review::{ReviewKind, ReviewSession};
pub(crate) use crate::model::{self, Edit, Outcome, PlanKind};
#[cfg(test)]
pub(crate) use crate::tabs::logs::log_verdict;
#[cfg(all(test, not(windows)))]
pub(crate) use crate::tabs::settings::value_widget;
pub(crate) use crate::theme;
pub(crate) use crate::widgets;

pub(crate) use crate::folders_ui::FolderDialog;

const STORAGE_KEY: &str = "retoque";
pub(crate) type Drafts = HashMap<PathBuf, BTreeMap<(String, String), String>>;

/// What survives a restart.
#[derive(Default, Serialize, Deserialize)]
pub(crate) struct Saved {
    pub(crate) lang: Option<Lang>,
    pub(crate) manual: Vec<PathBuf>,
    #[serde(default)]
    pub(crate) libraries: Vec<PathBuf>,
    pub(crate) payload: Option<PathBuf>,
    // The executable folder the user picked per game id, when it is not the first candidate.
    pub(crate) targets: BTreeMap<String, PathBuf>,
    #[serde(default)]
    pub(crate) lossless_dir: Option<PathBuf>,
    #[serde(default)]
    pub(crate) sm86_offer_seen: bool,
    #[serde(default)]
    pub(crate) payload_source: Option<crate::payload_ui::PayloadSource>,
    #[serde(default)]
    pub(crate) nr_model_offer_seen: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum Tab {
    Install,
    Settings,
    Model,
    Compare,
    Logs,
}

/// The log files found and the newest one's summary.
pub(crate) type LogsView = (Vec<PathBuf>, Result<Option<Summary>, AppError>);

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum PlanInputs {
    Install(InstallOptions),
    Uninstall,
    Settings(Vec<Edit>),
}

/// Everything about the selected game's chosen folder. Replaced whole when the selection changes.
#[derive(Default)]
pub(crate) struct GameView {
    pub(crate) target: Option<PathBuf>,
    pub(crate) status: Option<Result<Status, AppError>>,
    pub(crate) ini: Option<Result<Option<Ini>, AppError>>,
    pub(crate) edits: BTreeMap<(String, String), String>,
    pub(crate) options: InstallOptions,
    // The newest preview asked for; an older answer is dropped.
    pub(crate) request: u64,
    // What the shown plan was made from. An install plan whose options or payload have changed since is stale.
    pub(crate) inputs: Option<PlanInputs>,
    pub(crate) applied: Option<(PlanKind, Result<(), AppError>)>,
    pub(crate) logs: Option<LogsView>,
    pub(crate) bundle: Option<Result<PathBuf, AppError>>,
    pub(crate) copied: bool,
    pub(crate) inspection: Option<ModelInspection>,
    pub(crate) doctor: Option<crate::tabs::doctor::DoctorCard>,
    pub(crate) intelligence: crate::intelligence::ViewState,
    pub(crate) ini_values: crate::state::settings::IniValues,
}

pub struct RetoqueApp {
    pub(crate) intuitive: crate::intuitive::State,
    // The before/after viewer keeps its decoded images and view transform across frames.
    pub(crate) doctor_badges: HashMap<PathBuf, usize>,
    pub(crate) compare: crate::compare::CompareView,
    pub(crate) saved: Saved,
    pub(crate) lang: Lang,
    pub(crate) jobs: TaskHub,
    pub(crate) cmd_tx: CommandSender,
    pub(crate) cmd_rx: std::sync::mpsc::Receiver<AppCommand>,
    // Steam's cached cover, banner and logo per game id.
    pub(crate) skipped: Vec<String>,
    pub(crate) scanning: bool,
    pub(crate) library_state: crate::state::library::LibraryState,
    pub(crate) rescan_pending: bool,
    pub(crate) selected: Option<String>,
    pub(crate) tab: Tab,
    pub(crate) payload: Option<PathBuf>,
    pub(crate) view: GameView,
    pub(crate) catalog: Option<Result<Vec<Model>, AppError>>,
    pub(crate) error: Option<AppError>,
    // Folders with a write in flight, and its progress. Kept apart from the view so leaving a game and coming back
    // cannot start a second writer on a folder whose first one is still running.
    pub(crate) writing: HashMap<PathBuf, (usize, usize)>,
    pub(crate) write_kinds: HashMap<PathBuf, PlanKind>,
    pub(crate) next_request: u64,
    // RETOQUE_SELECT: select the first game whose name contains this once the library arrives (screenshots, demos).
    pub(crate) pending_select: Option<String>,
    pub(crate) drafts: Drafts,
    pub(crate) context: egui::Context,
    pub(crate) model_usage: HashMap<PathBuf, usize>,
    pub(crate) usage_refreshed: Option<std::time::Instant>,
    pub(crate) payload_build: Option<Result<String, AppError>>,
    pub(crate) library_generation: u64,
    pub(crate) library_pending: bool,
    pub(crate) review_open: bool,
    pub(crate) review: Option<Result<ReviewSession, AppError>>,
    pub(crate) origin_focus: Option<egui::Id>,
    pub(crate) sidebar_visible: bool,
    pub(crate) focus_search: bool,
    pub(crate) library_has_focus: bool,
    pub(crate) restore_frames: u8,
    pub(crate) toasts: Vec<ActionToast>,
    pub(crate) process_notice: Option<Vec<(u32, String)>>,
    pub(crate) tuning: crate::autotune::UiState,
    pub(crate) lsfg: crate::lsfg_ui::State,
    pub(crate) payload_ui: crate::payload_ui::State,
    pub(crate) nr_fetch: crate::tabs::model::NrFetchState,
    pub(crate) folder_dialog: Option<FolderDialog>,
    pub(crate) folder_focus: Option<egui::Id>,
    pub(crate) folder_restore: u8,
}

impl RetoqueApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> RetoqueApp {
        theme::apply(&cc.egui_ctx);
        let saved: Saved = cc
            .storage
            .and_then(|s| eframe::get_value(s, STORAGE_KEY))
            .unwrap_or_default();
        let lang = saved.lang.unwrap_or_else(Lang::from_system);
        let payload = match saved.payload_source {
            Some(crate::payload_ui::PayloadSource::Folder) => saved
                .payload
                .clone()
                .filter(|p| model::is_payload(p))
                .or_else(model::find_payload),
            _ => model::find_payload().or_else(|| saved.payload.clone().filter(|p| model::is_payload(p))),
        };
        let (cmd_tx, cmd_rx) = CommandSender::channel();
        let mut app = RetoqueApp {
            intuitive: crate::intuitive::State {
                history_root: crate::history::default_root(),
                ..Default::default()
            },
            doctor_badges: HashMap::new(),
            drafts: HashMap::new(),
            context: cc.egui_ctx.clone(),
            cmd_tx,
            cmd_rx,
            model_usage: HashMap::new(),
            usage_refreshed: None,
            payload_build: None,
            library_generation: 0,
            library_pending: false,
            review_open: false,
            review: None,
            origin_focus: None,
            sidebar_visible: true,
            focus_search: false,
            library_has_focus: false,
            restore_frames: 0,
            toasts: Vec::new(),
            process_notice: None,
            tuning: crate::autotune::UiState::default(),
            lsfg: crate::lsfg_ui::State::default(),
            payload_ui: crate::payload_ui::State::default(),
            nr_fetch: crate::tabs::model::NrFetchState::default(),
            folder_dialog: None,
            folder_focus: None,
            folder_restore: 0,
            compare: crate::compare::CompareView::new(),
            saved,
            lang,
            jobs: TaskHub::new(cc.egui_ctx.clone()),
            skipped: Vec::new(),
            scanning: false,
            library_state: Default::default(),
            rescan_pending: false,
            selected: None,
            tab: Tab::Install,
            payload,
            view: GameView::default(),
            catalog: None,
            error: None,
            writing: HashMap::new(),
            write_kinds: HashMap::new(),
            next_request: 0,
            pending_select: std::env::var("RETOQUE_SELECT").ok().map(|s| s.to_lowercase()),
        };
        app.tab = match std::env::var("RETOQUE_TAB").as_deref() {
            Ok("settings") => Tab::Settings,
            Ok("model") => Tab::Model,
            Ok("logs") => Tab::Logs,
            Ok("compare") => Tab::Compare,
            _ => Tab::Install,
        };
        if app.tab == Tab::Compare {
            app.sidebar_visible = false;
        }
        app.autotune_start();
        app.payload_init();
        app.hash_payload();
        app.rescan();
        app.jobs.spawn("catalog", None, |_| model::catalog_list());
        app
    }

    // The folder Retoque goes into for a game: the user's pick when it is still a candidate, else the best one.

    // True while the selected folder has a write in flight: every control that writes is disabled then.

    // What an install preview would be made from right now.
}

impl eframe::App for RetoqueApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.receive();
        self.global_drop_input(ui.ctx());
        self.shell_shortcuts(ui.ctx());
        if ui.ctx().input(|i| i.viewport().close_requested()) && !self.writing.is_empty() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::CancelClose);
            // Information, not a failure: the window stays open until the write ends.
            self.toast(self.tr("app.wait_write").into(), None, false);
        }
        egui::Panel::top("top")
            .frame(
                egui::Frame::new()
                    .fill(theme::SIDE)
                    .inner_margin(egui::Margin::symmetric(16, 10)),
            )
            .show(ui, |ui| self.top_bar(ui));
        egui::Panel::bottom("status")
            .frame(
                egui::Frame::new()
                    .fill(theme::SIDE)
                    .inner_margin(egui::Margin::symmetric(16, 6)),
            )
            .show(ui, |ui| self.status_bar(ui));
        if self.selected.is_some() && self.sidebar_visible {
            egui::Panel::left("library")
                .resizable(true)
                .default_size(320.0)
                .frame(
                    egui::Frame::new()
                        .fill(theme::SIDE)
                        .inner_margin(egui::Margin::symmetric(10, 10)),
                )
                .show(ui, |ui| self.library(ui));
        }
        egui::CentralPanel::default()
            .frame(
                egui::Frame::new()
                    .fill(theme::BG)
                    .inner_margin(egui::Margin::symmetric(22, 16)),
            )
            .show(ui, |ui| {
                let mut rect = ui.available_rect_before_wrap();
                let width = rect.width().min(1100.0);
                rect.min.x += (rect.width() - width) / 2.0;
                rect.max.x = rect.min.x + width;
                let mut child = ui.new_child(
                    egui::UiBuilder::new()
                        .max_rect(rect)
                        .layout(egui::Layout::top_down(egui::Align::Min)),
                );
                self.watcher_notice(&mut child);
                self.game(&mut child);
                ui.advance_cursor_after_rect(rect);
            });
        self.review_sheet(ui.ctx());
        self.folders_dialog(ui.ctx());
        self.show_toasts(ui.ctx());
        self.show_process_notice(ui.ctx());
        self.palette_sheet(ui.ctx());
        self.drop_sheet(ui.ctx());
        self.nr_fetch_dialog(ui.ctx());
        self.handle_commands();
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        eframe::set_value(storage, STORAGE_KEY, &self.saved);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Top bar and library
// ---------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------
// The selected game
// ---------------------------------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

pub(crate) struct ActionToast {
    pub(crate) message: String,
    pub(crate) detail: Option<AppError>,
    pub(crate) failed: bool,
    pub(crate) created: std::time::Instant,
    pub(crate) details_open: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InstallState {
    Missing,
    Installed,
    Update,
    Current,
    Partial,
    Untracked,
}
impl InstallState {
    pub(crate) fn label(self) -> &'static str {
        match self {
            Self::Missing => "st.not_installed",
            Self::Installed => "st.installed",
            Self::Update => "shell.update_available",
            Self::Current => "shell.up_to_date",
            Self::Partial => "shell.partial",
            Self::Untracked => "st.untracked",
        }
    }
    pub(crate) fn color(self) -> Color32 {
        match self {
            Self::Update => theme::ACCENT,
            Self::Partial => theme::BAD,
            Self::Current | Self::Installed => theme::GOOD,
            _ => theme::MUTED,
        }
    }
}

#[derive(Clone, Default)]
pub struct ModelInspection {
    pub(crate) path: Option<PathBuf>,
    pub(crate) present: bool,
    pub(crate) version: Option<String>,
    pub(crate) compatibility: retoque_data::model_compat::ModelCompatibility,
}
