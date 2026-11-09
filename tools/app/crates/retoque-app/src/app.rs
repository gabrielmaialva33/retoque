//! The window: the library on the left, the selected game on the right in four tabs. Every write is shown as a plan
//! first and carried out by a background job; the UI thread only reads what the jobs send back.

use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};

use egui::{Color32, RichText};
use retoque_core::{InstallOptions, Plan, Refusal, Status, Step, ini::Ini, receipt::ReceiptStatus};
use retoque_data::logs::Summary;
use retoque_data::models::{Catalog, Model};
use retoque_data::settings::{self, Kind};
use retoque_discovery::{Artwork, Game, Runtime, Store};
use serde::{Deserialize, Serialize};

use crate::i18n::{Lang, diagnostic, t};
use crate::jobs::Jobs;
use crate::model::{self, Edit, Outcome, PlanKind};
use crate::theme;
use crate::widgets;

#[path = "autotune_ui.rs"]
mod autotune_ui;
#[path = "folders_ui.rs"]
mod folders_ui;
use folders_ui::FolderDialog;

const STORAGE_KEY: &str = "retoque";
type Drafts = HashMap<PathBuf, BTreeMap<(String, String), String>>;

/// What survives a restart.
#[derive(Default, Serialize, Deserialize)]
struct Saved {
    lang: Option<Lang>,
    manual: Vec<PathBuf>,
    #[serde(default)]
    libraries: Vec<PathBuf>,
    payload: Option<PathBuf>,
    /// The executable folder the user picked per game id, when it is not the first candidate.
    targets: BTreeMap<String, PathBuf>,
    #[serde(default)]
    lossless_dir: Option<PathBuf>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Tab {
    Install,
    Settings,
    Model,
    Compare,
    Logs,
}

/// The log files found and the newest one's summary.
type LogsView = (Vec<PathBuf>, Result<Option<Summary>, String>);

#[derive(Clone, Debug, PartialEq)]
enum PlanInputs {
    Install(InstallOptions),
    Uninstall,
    Settings(Vec<Edit>),
}

/// Everything about the selected game's chosen folder. Replaced whole when the selection changes.
#[derive(Default)]
struct GameView {
    target: Option<PathBuf>,
    status: Option<Result<Status, String>>,
    ini: Option<Result<Option<Ini>, String>>,
    edits: BTreeMap<(String, String), String>,
    options: InstallOptions,
    plan: Option<(PlanKind, Result<Plan, String>)>,
    /// The newest preview asked for; an older answer is dropped.
    request: u64,
    /// What the shown plan was made from. An install plan whose options or payload have changed since is stale.
    inputs: Option<PlanInputs>,
    applied: Option<(PlanKind, Result<(), String>)>,
    logs: Option<LogsView>,
    bundle: Option<Result<PathBuf, String>>,
    copied: bool,
}

pub struct RetoqueApp {
    /// The before/after viewer keeps its decoded images and view transform across frames.
    compare: crate::compare::CompareView,
    saved: Saved,
    lang: Lang,
    jobs: Jobs,
    games: Vec<Game>,
    /// Steam's cached cover, banner and logo per game id.
    art: HashMap<String, Artwork>,
    skipped: Vec<String>,
    scanning: bool,
    rescan_pending: bool,
    search: String,
    installed_only: bool,
    selected: Option<String>,
    tab: Tab,
    payload: Option<PathBuf>,
    view: GameView,
    catalog: Option<Result<Vec<Model>, String>>,
    error: Option<String>,
    /// Folders with a write in flight, and its progress. Kept apart from the view so leaving a game and coming back
    /// cannot start a second writer on a folder whose first one is still running.
    writing: HashMap<PathBuf, (usize, usize)>,
    write_kinds: HashMap<PathBuf, PlanKind>,
    next_request: u64,
    /// RETOQUE_SELECT: select the first game whose name contains this once the library arrives (screenshots, demos).
    pending_select: Option<String>,
    drafts: Drafts,
    context: egui::Context,
    shell_tx: std::sync::mpsc::Sender<ShellOutcome>,
    shell_rx: std::sync::mpsc::Receiver<ShellOutcome>,
    payload_build: Option<Result<String, String>>,
    library_status: HashMap<PathBuf, Status>,
    library_generation: u64,
    library_pending: bool,
    review_open: bool,
    review_rendering: bool,
    origin_focus: Option<egui::Id>,
    sidebar_visible: bool,
    focus_search: bool,
    library_has_focus: bool,
    restore_frames: u8,
    toasts: Vec<ActionToast>,
    tuning: crate::autotune::UiState,
    folder_dialog: Option<FolderDialog>,
    folder_focus: Option<egui::Id>,
    folder_restore: u8,
}

impl RetoqueApp {
    pub fn new(cc: &eframe::CreationContext<'_>) -> RetoqueApp {
        theme::apply(&cc.egui_ctx);
        let saved: Saved = cc
            .storage
            .and_then(|s| eframe::get_value(s, STORAGE_KEY))
            .unwrap_or_default();
        let lang = saved.lang.unwrap_or_else(Lang::from_system);
        let payload = model::find_payload().or_else(|| saved.payload.clone().filter(|p| model::is_payload(p)));
        let (shell_tx, shell_rx) = std::sync::mpsc::channel();
        let mut app = RetoqueApp {
            drafts: HashMap::new(),
            context: cc.egui_ctx.clone(),
            shell_tx,
            shell_rx,
            payload_build: None,
            library_status: HashMap::new(),
            library_generation: 0,
            library_pending: false,
            review_open: false,
            review_rendering: false,
            origin_focus: None,
            sidebar_visible: true,
            focus_search: false,
            library_has_focus: false,
            restore_frames: 0,
            toasts: Vec::new(),
            tuning: crate::autotune::UiState::default(),
            folder_dialog: None,
            folder_focus: None,
            folder_restore: 0,
            compare: crate::compare::CompareView::new(),
            saved,
            lang,
            jobs: Jobs::new(cc.egui_ctx.clone()),
            games: Vec::new(),
            art: HashMap::new(),
            skipped: Vec::new(),
            scanning: false,
            rescan_pending: false,
            search: String::new(),
            installed_only: false,
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
        app.hash_payload();
        app.rescan();
        app.jobs.spawn("catalog", None, |_| model::catalog_list());
        app
    }

    fn tr(&self, key: &'static str) -> &'static str {
        t(self.lang, key)
    }

    fn rescan(&mut self) {
        if self.scanning {
            self.rescan_pending = true;
            return;
        }
        self.rescan_pending = false;
        self.scanning = true;
        let manual = self.saved.manual.clone();
        let libraries = self.saved.libraries.clone();
        self.jobs
            .spawn("discover", None, move |_| model::discover(manual, libraries));
    }

    fn selected_game(&self) -> Option<&Game> {
        let id = self.selected.as_ref()?;
        self.games.iter().find(|g| &g.id == id)
    }

    /// The folder Retoque goes into for a game: the user's pick when it is still a candidate, else the best one.
    fn target_for(&self, game: &Game) -> Option<PathBuf> {
        let picked = self.saved.targets.get(&game.id);
        picked
            .filter(|p| game.targets.iter().any(|t| &t.path == *p))
            .cloned()
            .or_else(|| game.targets.first().map(|t| t.path.clone()))
    }

    fn open_target(&mut self, target: Option<PathBuf>) {
        self.tuning.active = false;
        self.tuning.review = None;
        if let Some(old) = self.view.target.clone() {
            self.drafts.insert(old, self.view.edits.clone());
        }
        self.review_open = false;
        let edits = target
            .as_ref()
            .and_then(|t| self.drafts.get(t))
            .cloned()
            .unwrap_or_default();
        let mut options = InstallOptions {
            proxy: "dxgi.dll".into(),
            enable_nr: true,
            copy_nvngx: cfg!(not(windows)),
            ..Default::default()
        };
        if let Some(t) = &target {
            options.game_dir = t.clone();
        }
        self.view = GameView {
            target: target.clone(),
            options,
            edits,
            ..Default::default()
        };
        if let Some(t) = target {
            self.load_target(t);
        }
    }

    fn load_target(&mut self, t: PathBuf) {
        self.autotune_refresh(t.clone());
        let configured = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(|ini| ini.as_ref())
            .and_then(|ini| ini.get("DlssNr", "ModelFile"))
            .unwrap_or("auto");
        self.context
            .data_mut(|data| data.remove::<ModelInspectionTask>(model_inspection_id(&t, configured)));
        let (a, b, c) = (t.clone(), t.clone(), t);
        self.jobs.spawn("status", None, move |_| model::status(a));
        self.jobs.spawn("ini", None, move |_| model::read_ini(b));
        self.jobs.spawn("logs", None, move |_| model::logs(c));
    }

    /// True while the selected folder has a write in flight: every control that writes is disabled then.
    fn busy(&self) -> bool {
        self.view.target.as_ref().is_some_and(|t| self.writing.contains_key(t))
    }

    /// What an install preview would be made from right now.
    fn install_inputs(&self) -> Option<InstallOptions> {
        let target = self.view.target.clone()?;
        let mut options = self.view.options.clone();
        options.game_dir = target;
        options.source_dir = self.payload.clone()?;
        Some(options)
    }

    fn request_plan(&mut self, kind: PlanKind, inputs: PlanInputs) {
        self.tuning.active = false;
        let Some(target) = self.view.target.clone() else { return };
        self.review_open = true;
        self.origin_focus = self.context.memory(|m| m.focused());
        self.next_request += 1;
        let request = self.next_request;
        self.view.request = request;
        self.view.plan = None;
        self.view.applied = None;
        self.view.inputs = Some(inputs.clone());
        let (options, edits) = match inputs {
            PlanInputs::Install(o) => (o, Vec::new()),
            PlanInputs::Uninstall => (
                InstallOptions {
                    game_dir: target.clone(),
                    ..Default::default()
                },
                Vec::new(),
            ),
            PlanInputs::Settings(e) => (InstallOptions::default(), e),
        };
        let ctx = self.context.clone();
        self.jobs.spawn("plan", None, move |_| {
            let outcome = model::plan(kind, request, target, options, edits);
            if let Outcome::Plan { result: Ok(plan), .. } = &outcome {
                // Copy can overwrite our own files without a Backup step. Inspect destinations once,
                // beside plan creation, so the review distinguishes replacement without UI-thread IO.
                let groups: Vec<usize> = plan
                    .steps
                    .iter()
                    .map(|step| {
                        let exists = matches!(step, Step::Copy { to, .. } if plan.game_dir.join(to).exists());
                        step_group(step, &plan.steps, exists)
                    })
                    .collect();
                ctx.data_mut(|data| {
                    data.insert_temp(egui::Id::unique(("review_groups", &plan.game_dir, request)), groups)
                });
            }
            outcome
        });
    }

    fn apply_plan(&mut self, kind: PlanKind, plan: Plan, ctx: egui::Context) {
        let Some(target) = self.view.target.clone() else { return };
        if self.writing.contains_key(&target) {
            return;
        }
        let edits = match &self.view.inputs {
            Some(PlanInputs::Settings(e)) => e.clone(),
            _ => Vec::new(),
        };
        self.review_open = false;
        self.restore_focus();
        self.writing.insert(target.clone(), (0, plan.steps.len()));
        self.write_kinds.insert(target.clone(), kind);
        self.view.plan = None;
        let t = target.clone();
        self.jobs.spawn("apply", Some(target), move |tx| {
            model::apply(kind, plan, t, edits, tx, &ctx)
        });
    }

    fn receive(&mut self) {
        self.receive_shell();
        for outcome in self.jobs.drain() {
            let current = self.view.target.clone();
            let ours = |t: &Path| current.as_deref() == Some(t);
            match outcome {
                Outcome::Auto(event) => self.autotune_event(*event),
                Outcome::Discovered { games, skipped, art } => {
                    if self.rescan_pending {
                        self.scanning = false;
                        self.rescan();
                        continue;
                    }
                    self.games = games;
                    self.art = art;
                    self.skipped = skipped;
                    self.scanning = false;
                    self.inspect_library();
                    if let Some(needle) = self.pending_select.take()
                        && let Some(id) = self
                            .games
                            .iter()
                            .find(|g| g.name.to_lowercase().contains(&needle))
                            .map(|g| g.id.clone())
                    {
                        self.selected = Some(id);
                        let target = self.selected_game().and_then(|g| self.target_for(g));
                        self.open_target(target);
                    }
                    if self
                        .selected
                        .as_ref()
                        .is_some_and(|id| !self.games.iter().any(|g| &g.id == id))
                    {
                        self.selected = None;
                        self.open_target(None);
                    }
                }
                Outcome::Status { target, result } if ours(&target) => {
                    if let Ok(s) = &result {
                        self.library_status.insert(target.clone(), s.clone());
                        if let Some(proxy) = s.receipt.as_ref().and_then(first_proxy) {
                            self.view.options.proxy = proxy;
                        }
                        if let Some(nr) = s.nr_enabled {
                            self.view.options.enable_nr = nr || s.receipt.is_none();
                        }
                    }
                    self.view.status = Some(result);
                }
                Outcome::Ini { target, result } if ours(&target) => {
                    self.view.ini = Some(result.map(|o| o.map(|(_, b)| Ini::parse(&b))));
                }
                Outcome::Plan {
                    target,
                    kind,
                    request,
                    result,
                } if ours(&target) && request == self.view.request => {
                    self.view.plan = Some((kind, result));
                }
                Outcome::Progress { target, done } => {
                    if let Some(p) = self.writing.get_mut(&target) {
                        *p = (done.index + 1, done.total);
                    }
                }
                Outcome::Applied {
                    target,
                    kind,
                    edits,
                    result,
                } => {
                    // The folder is free again whether or not the user is still looking at it.
                    self.writing.remove(&target);
                    self.write_kinds.remove(&target);
                    let game_name = self
                        .games
                        .iter()
                        .find(|g| g.targets.iter().any(|t| t.path == target))
                        .map(|g| g.name.clone())
                        .unwrap_or_else(|| short_path(&target));
                    if self.tuning.writes.remove(&target) {
                        self.toast(
                            format!(
                                "{} · {game_name}",
                                self.tr(if result.is_ok() { "auto.done" } else { "shell.failed" })
                            ),
                            result.as_ref().err().cloned(),
                            result.is_err(),
                        );
                    } else {
                        self.notify_action(kind, &game_name, &result);
                    }
                    if result.is_ok()
                        && let Some(draft) = self.drafts.get_mut(&target)
                    {
                        clear_applied(draft, &edits);
                    }
                    if ours(&target) {
                        if result.is_ok() {
                            // Only what this plan carried leaves the pending list; later edits stay.
                            for (section, key, value) in &edits {
                                let k = (section.clone(), key.clone());
                                if self.view.edits.get(&k) == Some(value) {
                                    self.view.edits.remove(&k);
                                }
                            }
                        }
                        self.view.applied = Some((kind, result));
                        self.load_target(target);
                    }
                    if kind != PlanKind::Settings {
                        self.rescan();
                    }
                }
                Outcome::Logs { target, files, result } if ours(&target) => {
                    self.view.logs = Some((files, result));
                }
                Outcome::Bundle { target, result } if ours(&target) => {
                    self.toast(
                        self.tr(if result.is_ok() {
                            "shell.bundle_done"
                        } else {
                            "shell.failed"
                        })
                        .into(),
                        result.as_ref().err().cloned(),
                        result.is_err(),
                    );
                    self.view.bundle = Some(result);
                }
                Outcome::Catalog { result } => {
                    if self.catalog.is_some()
                        && result.as_ref().is_ok_and(|models| {
                            self.catalog
                                .as_ref()
                                .and_then(|r| r.as_ref().ok())
                                .is_some_and(|old| old != models)
                        })
                    {
                        self.toast(self.tr("shell.catalog_changed").into(), None, false);
                    }
                    self.catalog = Some(result);
                }
                Outcome::Failed { job, target, message } => {
                    self.toast(self.tr("shell.failed").into(), Some(message.clone()), true);
                    self.error = Some(format!("{} ({job}): {message}", self.tr("job.failed")));
                    if job == "autotune-plan" {
                        self.tuning.review = Some((self.tuning.review_title.clone(), Err(message.clone())));
                    }
                    if job == "discover" {
                        self.scanning = false;
                        if self.rescan_pending {
                            self.rescan();
                        }
                    }
                    // A write that died part-way may have changed the folder: release it and read it again.
                    if let Some(t) = target {
                        self.tuning.writes.remove(&t);
                        self.writing.remove(&t);
                        self.write_kinds.remove(&t);
                        if ours(&t) {
                            self.load_target(t);
                        }
                    }
                }
                // A result for a folder the user has left.
                _ => {}
            }
        }
    }
}

fn first_proxy(r: &retoque_core::receipt::Receipt) -> Option<String> {
    r.records.iter().find_map(|rec| match rec {
        retoque_core::receipt::Record::Proxy { name } => Some(name.clone()),
        _ => None,
    })
}

impl eframe::App for RetoqueApp {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.receive();
        self.shell_shortcuts(ui.ctx());
        if ui.ctx().input(|i| i.viewport().close_requested()) && !self.writing.is_empty() {
            ui.ctx().send_viewport_cmd(egui::ViewportCommand::CancelClose);
            self.error = Some(self.tr("app.wait_write").to_string());
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
                self.game(&mut child);
                ui.advance_cursor_after_rect(rect);
            });
        self.review_sheet(ui.ctx());
        self.folders_dialog(ui.ctx());
        self.show_toasts(ui.ctx());
    }

    fn save(&mut self, storage: &mut dyn eframe::Storage) {
        eframe::set_value(storage, STORAGE_KEY, &self.saved);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Top bar and library
// ---------------------------------------------------------------------------------------------------------------

impl RetoqueApp {
    fn top_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if ui
                .add(
                    egui::Button::new(RichText::new(self.tr("brand")).size(22.0).strong().color(theme::ACCENT))
                        .frame(false),
                )
                .on_hover_text(self.tr("shell.home"))
                .clicked()
            {
                self.selected = None;
                self.open_target(None);
            }
            if self.selected.is_some() && ui.small_button(self.tr("shell.library")).clicked() {
                self.sidebar_visible = !self.sidebar_visible;
            }
            if ui.available_width() > 1000.0 {
                ui.label(RichText::new(self.tr("app.tagline")).color(theme::MUTED));
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                let label = match self.lang {
                    Lang::En => t(self.lang, "lang.en"),
                    Lang::Pt => t(self.lang, "lang.pt"),
                };
                egui::ComboBox::from_id_salt("lang")
                    .selected_text(label)
                    .show_ui(ui, |ui| {
                        let before = self.lang;
                        ui.selectable_value(&mut self.lang, Lang::En, t(Lang::En, "lang.en"));
                        ui.selectable_value(&mut self.lang, Lang::Pt, t(Lang::Pt, "lang.pt"));
                        if self.lang != before {
                            self.saved.lang = Some(self.lang);
                        }
                    });
                if ui
                    .add_enabled(!self.scanning, egui::Button::new(self.tr("app.refresh")))
                    .clicked()
                {
                    self.rescan();
                    let t = self.view.target.clone();
                    if let Some(t) = t {
                        self.load_target(t);
                    }
                }
                if self.jobs.busy() || self.library_pending {
                    ui.spinner();
                }
            });
        });
        self.autotune_gpu(ui);
    }

    /// The payload in use, and the last error, along the bottom edge.
    fn status_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if let Some(message) = self.error.clone() {
                ui.colored_label(theme::BAD, format!("{}: {message}", self.tr("error")));
                if ui
                    .small_button(self.tr("dismiss"))
                    .on_hover_text(self.tr("dismiss.help"))
                    .clicked()
                {
                    self.error = None;
                }
                return;
            }
            ui.label(RichText::new(self.tr("app.payload")).small().color(theme::MUTED));
            match &self.payload {
                Some(p) => {
                    ui.label(RichText::new(short_path(p)).small().monospace().color(theme::MUTED))
                        .on_hover_text(p.display().to_string());
                }
                None => {
                    ui.colored_label(theme::WARN, RichText::new(self.tr("app.payload.none")).small());
                }
            }
            if ui.small_button(self.tr("app.payload.choose")).clicked()
                && let Some(dir) = rfd::FileDialog::new().pick_folder()
            {
                if model::is_payload(&dir) {
                    self.saved.payload = Some(dir.clone());
                    self.payload = Some(dir);
                    self.hash_payload();
                } else {
                    self.error = Some(format!("{}: {}", dir.display(), self.tr("app.payload.none")));
                }
            }
        });
    }

    fn library(&mut self, ui: &mut egui::Ui) {
        ui.add_space(6.0);
        self.search_field(ui);
        ui.horizontal(|ui| {
            ui.checkbox(&mut self.installed_only, t(self.lang, "lib.installed_only"));
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.small_button(self.tr("lib.add_folder")).clicked() {
                    self.folders_open(FolderDialog::Add);
                }
            });
        });
        self.folders_button(ui);
        ui.separator();
        if self.scanning && self.games.is_empty() {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(self.tr("lib.scanning"));
            });
            return;
        }
        if self.games.is_empty() {
            ui.label(self.tr("lib.empty"));
        }
        self.library_keyboard(ui);
        let needle = self.search.to_lowercase();
        let mut clicked = None;
        let mut list_focus = false;
        let visible: Vec<&Game> = self
            .games
            .iter()
            .filter(|g| needle.is_empty() || g.name.to_lowercase().contains(&needle))
            .collect();
        let (installed, others): (Vec<&Game>, Vec<&Game>) = visible.into_iter().partition(|g| self.has_retoque(g));
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            let groups = [
                (self.tr("lib.group_installed"), &installed),
                (self.tr("lib.group_other"), &others),
            ];
            for (title, games) in groups {
                if games.is_empty() || (self.installed_only && !std::ptr::eq(games, &installed)) {
                    continue;
                }
                widgets::group_label(ui, title, games.len());
                for game in games.iter() {
                    let selected = self.selected.as_ref() == Some(&game.id);
                    let state = self.has_retoque(game).then(|| self.tr("lib.state_installed"));
                    let detail = format!(
                        "{} · {}",
                        store_name(self.lang, game.store),
                        runtime_short(self.lang, &game.runtime)
                    );
                    let capsule = self.art.get(&game.id).and_then(|a| a.capsule.as_deref());
                    let hint = (!game.registered).then(|| self.tr("folders.unregistered"));
                    let response = widgets::game_row(ui, &game.name, &detail, state, capsule, selected, hint);
                    list_focus |= response.has_focus();
                    if response.clicked() {
                        response.request_focus();
                        clicked = Some(game.id.clone());
                    }
                }
            }
            if !self.skipped.is_empty() {
                ui.add_space(8.0);
                ui.label(
                    RichText::new(format!("{}: {}", self.tr("lib.skipped"), self.skipped.join("; ")))
                        .small()
                        .weak(),
                );
            }
        });
        self.library_has_focus = list_focus;
        if let Some(id) = clicked {
            self.select_id(id);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// The selected game
// ---------------------------------------------------------------------------------------------------------------

impl RetoqueApp {
    fn game(&mut self, ui: &mut egui::Ui) {
        let Some(game) = self.selected_game().cloned() else {
            self.home_grid(ui);
            return;
        };
        self.game_banner(ui, &game);
        if game.runtime == Runtime::NativeLinux {
            ui.add_space(8.0);
            ui.colored_label(theme::WARN, self.tr("game.native"));
            return;
        }
        if game.targets.is_empty() {
            ui.add_space(8.0);
            ui.colored_label(theme::WARN, self.tr("game.no_target"));
            return;
        }
        if self.tab != Tab::Compare {
            ui.collapsing(self.tr("shell.details"), |ui| {
                self.target_picker(ui, &game);
                if let Some(target) = game.targets.iter().find(|t| Some(&t.path) == self.view.target.as_ref()) {
                    ui.label(RichText::new(target.dlss.join(" · ")).small().color(theme::MUTED));
                }
            });
            ui.add_space(8.0);
        }
        let tabs = [Tab::Install, Tab::Settings, Tab::Model, Tab::Compare, Tab::Logs];
        let labels = [
            self.tr("tab.install"),
            self.tr("tab.settings"),
            self.tr("tab.model"),
            self.tr("tab.compare"),
            self.tr("tab.logs"),
        ];
        let current = tabs.iter().position(|t| *t == self.tab).unwrap_or(0);
        if let Some(i) = widgets::tabs(ui, &labels, current) {
            if tabs[i] == Tab::Model && self.tab != Tab::Model {
                self.jobs.spawn("catalog", None, |_| model::catalog_list());
                self.autotune_catalog(None, None);
            }
            self.tab = tabs[i];
            if self.tab == Tab::Compare && ui.ctx().content_rect().width() <= 1000.0 {
                self.sidebar_visible = false;
            }
        }
        // The comparison lays out its own viewport (zoom, pan, wipe) and must not sit inside a scroll area.
        if self.tab == Tab::Compare {
            if let Some(target) = self.view.target.clone() {
                self.compare.ui(ui, self.lang, &target);
            }
            return;
        }
        if self.tab == Tab::Settings {
            self.pending_footer(ui);
        }
        egui::ScrollArea::vertical()
            .auto_shrink([false, false])
            .show(ui, |ui| match self.tab {
                Tab::Install => self.install_tab(ui),
                Tab::Settings => self.settings_tab(ui),
                Tab::Model => self.model_tab(ui),
                Tab::Compare => {}
                Tab::Logs => self.logs_tab(ui),
            });
    }

    fn target_picker(&mut self, ui: &mut egui::Ui, game: &Game) {
        let current = self.view.target.clone();
        let root = t(self.lang, "game.root");
        ui.horizontal(|ui| {
            ui.label(self.tr("game.target"));
            if game.targets.len() == 1 {
                ui.add(
                    egui::Label::new(
                        RichText::new(relative_to(&game.targets[0].path, &game.install_dir, root)).monospace(),
                    )
                    .truncate(),
                )
                .on_hover_text(game.targets[0].path.display().to_string());
                return;
            }
            let shown = current
                .as_deref()
                .map(|p| relative_to(p, &game.install_dir, root))
                .unwrap_or_default();
            let mut pick = None;
            egui::ComboBox::from_id_salt("target")
                .selected_text(shown)
                .width(ui.available_width().min(420.0))
                .truncate()
                .show_ui(ui, |ui| {
                    for t in &game.targets {
                        let text = format!(
                            "{}  ({})",
                            relative_to(&t.path, &game.install_dir, root),
                            t.executables.join(", ")
                        );
                        if ui.selectable_label(current.as_ref() == Some(&t.path), text).clicked() {
                            pick = Some(t.path.clone());
                        }
                    }
                });
            if let Some(p) = pick
                && current.as_ref() != Some(&p)
            {
                self.saved.targets.insert(game.id.clone(), p.clone());
                self.open_target(Some(p));
            }
        });
    }

    fn status_chips(&self, ui: &mut egui::Ui) {
        match &self.view.status {
            None => {
                ui.label(self.tr("shell.checking"));
            }
            Some(Err(e)) => {
                ui.colored_label(theme::BAD, e);
            }
            Some(Ok(status)) => {
                let state = install_state(
                    status,
                    self.payload_build
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .map(String::as_str),
                );
                let mut parts = vec![
                    self.tr(if matches!(state, InstallState::Current | InstallState::Update) {
                        "st.installed"
                    } else {
                        state.label()
                    })
                    .to_owned(),
                ];
                if matches!(state, InstallState::Current | InstallState::Update) {
                    parts.push(self.tr(state.label()).into());
                }
                if let Some(hash) = &status.build_sha256 {
                    parts.push(format!("{} {}", self.tr("st.build"), &hash[..hash.len().min(8)]));
                }
                if !status.optiscaler_proxies.is_empty() {
                    // The forwarder only gets a word when its version is known; the mismatch warning covers the rest.
                    let proxy = status.proxy_version.as_deref().unwrap_or(self.tr("ver.unknown"));
                    parts.push(format!("OptiScaler {proxy}"));
                    if let Some(forwarder) = &status.forwarder_version {
                        parts.push(format!("NR forwarder {forwarder}"));
                    }
                }
                if status.version_mismatch() {
                    ui.colored_label(theme::WARN, self.tr("ver.mismatch"));
                }
                if let Some(nr) = status.nr_enabled {
                    parts.push(self.tr(if nr { "st.nr_on" } else { "st.nr_off" }).into());
                }
                let configured = self
                    .view
                    .ini
                    .as_ref()
                    .and_then(|r| r.as_ref().ok())
                    .and_then(|ini| ini.as_ref())
                    .and_then(|ini| ini.get("DlssNr", "ModelFile"))
                    .unwrap_or("auto");
                let inspection = self
                    .view
                    .target
                    .as_ref()
                    .and_then(|target| model_inspection(ui.ctx(), target, configured));
                if let Some(info) = &inspection
                    && info.present
                {
                    parts.push(
                        info.version
                            .as_ref()
                            .map(|version| format!("{} {version}", self.tr("shell.model_version")))
                            .unwrap_or_else(|| self.tr("shell.model_unknown").into()),
                    );
                }
                let response = ui.label(RichText::new(parts.join(" · ")).color(theme::TEXT_STRONG));
                if let Some(path) = inspection.and_then(|info| info.path) {
                    response.on_hover_text(short_path(&path));
                }
            }
        }
    }

    // -- Install --------------------------------------------------------------------------------------------------

    fn install_tab(&mut self, ui: &mut egui::Ui) {
        self.autotune_card(ui);
        self.autotune_lsfg(ui);
        ui.add_space(12.0);
        let Some(target) = self.view.target.clone() else { return };
        let installed = matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_some());
        let busy = self.busy();
        widgets::card(ui, |ui| {
            widgets::card_title(ui, t(self.lang, "card.install"));
            ui.add_enabled_ui(!busy, |ui| self.install_options(ui, installed));
            if let Some(&(done, total)) = self.view.target.as_ref().and_then(|t| self.writing.get(t)) {
                ui.add_space(8.0);
                ui.add(
                    egui::ProgressBar::new(done as f32 / total.max(1) as f32)
                        .text(format!("{} {done}/{total}", t(self.lang, "inst.working"))),
                );
            }
            if let Some((kind, result)) = &self.view.applied {
                ui.add_space(8.0);
                match result {
                    Ok(()) => ui.colored_label(theme::GOOD, t(self.lang, "inst.done")),
                    Err(e) => ui.colored_label(
                        theme::BAD,
                        format!("{} ({}): {e}", t(self.lang, "error"), plan_kind_name(self.lang, *kind)),
                    ),
                };
            }
        });
        let _ = &target;
        if let Some(Ok(s)) = &self.view.status
            && let Some(option) = s.launch_option.clone()
        {
            ui.add_space(12.0);
            widgets::card(ui, |ui| {
                widgets::card_title(ui, t(self.lang, "card.launch"));
                ui.label(RichText::new(t(self.lang, "inst.launch_help")).color(theme::MUTED));
                ui.add_space(4.0);
                ui.horizontal(|ui| {
                    ui.add(egui::Label::new(
                        RichText::new(&option).monospace().color(theme::TEXT_STRONG),
                    ));
                    let label = if self.view.copied {
                        t(self.lang, "inst.copied")
                    } else {
                        t(self.lang, "inst.copy")
                    };
                    if ui.button(label).clicked() {
                        ui.ctx().copy_text(option.clone());
                        self.view.copied = true;
                    }
                });
            });
        }
    }

    fn install_options(&mut self, ui: &mut egui::Ui, installed: bool) {
        {
            egui::Grid::new("install_options")
                .num_columns(2)
                .min_col_width(160.0)
                .spacing([16.0, 8.0])
                .show(ui, |ui| {
                    let label = ui.add(egui::Label::new(self.tr("inst.proxy")).wrap_mode(egui::TextWrapMode::Extend));
                    egui::ComboBox::from_id_salt("proxy")
                        .width(180.0)
                        .selected_text(self.view.options.proxy.clone())
                        .show_ui(ui, |ui| {
                            for name in retoque_core::PROXY_NAMES {
                                ui.selectable_value(&mut self.view.options.proxy, name.to_string(), name);
                            }
                        })
                        .response
                        .labelled_by(label.id);
                    ui.end_row();
                });
            ui.checkbox(&mut self.view.options.enable_nr, t(self.lang, "inst.enable_nr"));
            ui.collapsing(RichText::new(self.tr("inst.advanced")).weak(), |ui| {
                ui.checkbox(&mut self.view.options.keep_reshade, t(self.lang, "inst.keep_reshade"));
                ui.checkbox(
                    &mut self.view.options.replace_foreign,
                    t(self.lang, "inst.replace_foreign"),
                );
                ui.checkbox(
                    &mut self.view.options.allow_second_proxy,
                    t(self.lang, "inst.second_proxy"),
                );
                if cfg!(not(windows)) {
                    ui.checkbox(&mut self.view.options.copy_nvngx, t(self.lang, "inst.copy_nvngx"));
                }
            });
            ui.add_space(8.0);
            ui.horizontal_wrapped(|ui| {
                let inputs = self.install_inputs();
                if ui
                    .add_enabled(inputs.is_some(), egui::Button::new(self.tr("inst.preview")))
                    .clicked()
                    && let Some(options) = inputs
                {
                    self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                }
                // A partial receipt is also "installed" here: its uninstall cleans up what the stopped run left.
                if installed && ui.button(self.tr("inst.preview_uninstall")).clicked() {
                    self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
                }
            });
        }
    }

    fn plan_view(&mut self, ui: &mut egui::Ui, kind: PlanKind, result: &Result<Plan, String>, installed: bool) {
        // Older tab callers are intentionally harmless; the single modal is rendered after the page.
        if !self.review_rendering {
            return;
        }
        let plan = match result {
            Ok(plan) => plan,
            Err(error) => {
                ui.colored_label(theme::BAD, error);
                if ui.button(self.tr("cancel")).clicked() {
                    self.review_open = false;
                    self.restore_focus();
                }
                return;
            }
        };
        let game = self.selected_game().map(|g| g.name.as_str()).unwrap_or("");
        ui.heading(format!(
            "{} · {game}",
            self.tr(match kind {
                PlanKind::Install if installed => "shell.review_update",
                PlanKind::Install => "shell.review_install",
                PlanKind::Uninstall => "shell.review_remove",
                PlanKind::Settings => "set.save",
            })
        ));
        ui.label(self.tr("inst.plan"));
        ui.label(RichText::new(self.tr("shell.review_note")).small().color(theme::MUTED));
        let stale = kind == PlanKind::Install && self.view.inputs != self.install_inputs().map(PlanInputs::Install);
        egui::ScrollArea::vertical()
            .id_salt("review_steps")
            .max_height((ui.ctx().content_rect().height() - 290.0).max(160.0))
            .show(ui, |ui| {
                for refusal in &plan.refusals {
                    let (key, detail) = refusal_text(self.lang, refusal);
                    ui.colored_label(theme::BAD, format!("{} {detail}", self.tr(key)));
                }
                if !plan.refusals.is_empty() {
                    ui.label(self.tr("shell.resolve"));
                }
                if plan.steps.is_empty() {
                    ui.label(self.tr("inst.nothing"));
                }
                for group in 0..5 {
                    let groups = ui
                        .ctx()
                        .data(|data| {
                            data.get_temp::<Vec<usize>>(egui::Id::unique((
                                "review_groups",
                                &plan.game_dir,
                                self.view.request,
                            )))
                        })
                        .unwrap_or_default();
                    let steps: Vec<_> = plan
                        .steps
                        .iter()
                        .enumerate()
                        .filter(|(i, step)| {
                            groups
                                .get(*i)
                                .copied()
                                .unwrap_or_else(|| step_group(step, &plan.steps, false))
                                == group
                        })
                        .map(|(_, step)| step)
                        .collect();
                    if steps.is_empty() {
                        continue;
                    }
                    let title = [
                        "shell.add",
                        "shell.replace",
                        "shell.keep",
                        "shell.remove",
                        "shell.settings",
                    ][group];
                    ui.add_space(10.0);
                    ui.label(RichText::new(format!("{} ({})", self.tr(title), steps.len())).strong());
                    for step in steps {
                        let (_, text, color) = step_text(self.lang, step);
                        ui.add(egui::Label::new(RichText::new(text).color(color)).wrap());
                    }
                }
                if !plan.notes.is_empty() {
                    ui.add_space(12.0);
                    ui.label(RichText::new(self.tr("inst.notes")).strong());
                    for note in &plan.notes {
                        ui.label(RichText::new(diagnostic(self.lang, note)).small().color(theme::MUTED));
                    }
                }
            });
        ui.separator();
        if stale {
            ui.colored_label(theme::WARN, self.tr("inst.stale"));
        }
        let applicable = plan.refusals.is_empty() && (!plan.steps.is_empty() || kind == PlanKind::Uninstall);
        ui.horizontal_wrapped(|ui| {
            if ui.button(self.tr("cancel")).clicked() {
                self.review_open = false;
                self.restore_focus();
            }
            if applicable && !stale {
                let label = match kind {
                    PlanKind::Install if installed => self.tr("inst.update"),
                    PlanKind::Install => self.tr("inst.install"),
                    PlanKind::Uninstall => self.tr("inst.uninstall"),
                    PlanKind::Settings => self.tr("set.apply"),
                };
                let button = if kind == PlanKind::Uninstall {
                    egui::Button::new(label).fill(theme::BAD)
                } else {
                    theme::primary(label)
                };
                if ui.add_enabled(!self.busy(), button).clicked() {
                    self.apply_plan(kind, plan.clone(), ui.ctx().clone());
                }
            }
        });
    }

    // -- Settings -------------------------------------------------------------------------------------------------

    fn settings_tab(&mut self, ui: &mut egui::Ui) {
        if self.view.target.is_none() {
            return;
        }
        let ini = match &self.view.ini {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(self.tr("tab.loading"));
                });
                return;
            }
            Some(Err(e)) => {
                ui.colored_label(theme::BAD, e);
                return;
            }
            Some(Ok(None)) => {
                self.tab_install_prompt(ui);
                return;
            }
            Some(Ok(Some(ini))) => ini.clone(),
        };
        for (section, title) in [
            ("DlssNr", "set.nr"),
            ("Upscalers", "set.upscalers"),
            ("FrameGen", "set.framegen"),
            ("LSFG", "set.lsfg"),
        ] {
            widgets::card(ui, |ui| {
                if section == "LSFG" {
                    egui::CollapsingHeader::new(RichText::new(self.tr(title)).size(16.0).strong())
                        .id_salt("lsfg_settings")
                        .default_open(false)
                        .show(ui, |ui| self.settings_section(ui, &ini, section));
                    ui.label(RichText::new(self.tr("set.lsfg_note")).small().color(theme::MUTED));
                } else {
                    widgets::card_title(ui, self.tr(title));
                    self.settings_section(ui, &ini, section);
                }
            });
            ui.add_space(12.0);
        }
        // Review and Discard live in the pending-changes footer (pending_footer), outside the scroll area.
        self.settings_plan(ui);
    }

    fn settings_section(&mut self, ui: &mut egui::Ui, ini: &Ini, section: &str) {
        self.settings_rows(ui, ini, section, false);
        if section == "DlssNr" {
            ui.add_space(8.0);
            egui::CollapsingHeader::new(self.tr("set.advanced"))
                .id_salt("nr_advanced")
                .show(ui, |ui| self.settings_rows(ui, ini, section, true));
        }
    }

    fn settings_rows(&mut self, ui: &mut egui::Ui, ini: &Ini, section: &str, advanced: bool) {
        let lang = self.lang;
        for s in settings::all().iter().filter(|s| s.section == section) {
            if section == "DlssNr" {
                let common = matches!(s.key, "Enabled" | "Stage" | "WorkingScale" | "ApplyModel" | "ModelFile");
                if common == advanced {
                    continue;
                }
            }
            if s.key == "ModelFile" {
                ui.add_space(8.0);
                if ui.link(t(lang, "set.model_link")).clicked() {
                    self.tab = Tab::Model;
                }
                continue;
            }
            let compare = self
                .view
                .edits
                .get(&("DlssNr".into(), "Compare".into()))
                .map(String::as_str)
                .or_else(|| ini.get("DlssNr", "Compare"))
                .unwrap_or("auto");
            if s.section == "DlssNr"
                && ((s.key == "CompareSplit" && compare != "2")
                    || (s.key == "CompareSwap" && !matches!(compare, "1" | "2")))
            {
                continue;
            }
            let (label, help) = match lang {
                Lang::En => (s.label_en, s.help_en),
                Lang::Pt => (s.label_pt, s.help_pt),
            };
            let key = (s.section.to_owned(), s.key.to_owned());
            let present = ini.get(s.section, s.key);
            let on_disk = present.unwrap_or("auto");
            let mut value = self.view.edits.get(&key).cloned().unwrap_or_else(|| on_disk.into());
            let before = value.clone();
            ui.add_space(8.0);
            let width = ui.available_width();
            let control_width = (width * 0.50).clamp(220.0, 300.0).min(width);
            let label_width = (width - control_width - 20.0).max(40.0);
            ui.horizontal_top(|ui| {
                let label_id = ui
                    .allocate_ui_with_layout(
                        egui::vec2(label_width, 0.0),
                        egui::Layout::top_down(egui::Align::Min),
                        |ui| {
                            ui.set_width(label_width);
                            let response = ui.add(egui::Label::new(RichText::new(label).strong()).wrap());
                            ui.label(RichText::new(help).size(13.0).color(theme::MUTED));
                            if present.is_none() {
                                ui.label(RichText::new(t(lang, "set.game_default")).small().color(theme::MUTED));
                            }
                            response.id
                        },
                    )
                    .inner;
                ui.allocate_ui_with_layout(
                    egui::vec2(control_width, 0.0),
                    egui::Layout::right_to_left(egui::Align::Min),
                    |ui| {
                        ui.set_width(control_width);
                        value_widget(ui, s, &mut value, &format!("{}.{}", s.section, s.key), lang)
                            .labelled_by(label_id);
                    },
                );
            });
            // A value OptiScaler would not accept is kept as typed (so the user can fix it) but blocks the review.
            if self.view.edits.get(&key).is_some_and(|v| !s.accepts(v)) {
                ui.colored_label(theme::BAD, RichText::new(t(lang, "set.invalid")).small());
            }
            if value != before {
                if value == on_disk {
                    self.view.edits.remove(&key);
                } else {
                    self.view.edits.insert(key, value);
                }
            }
            ui.add_space(6.0);
            ui.separator();
        }
    }

    /// The preview of an ini change (Settings or Model tab), with the result of the last one applied.
    fn settings_plan(&mut self, ui: &mut egui::Ui) {
        if let Some((PlanKind::Settings, result)) = self.view.plan.clone() {
            ui.add_space(10.0);
            self.plan_view(ui, PlanKind::Settings, &result, true);
        }
        match &self.view.applied {
            Some((PlanKind::Settings, Ok(()))) => {
                ui.colored_label(theme::GOOD, self.tr("set.saved"));
            }
            Some((PlanKind::Settings, Err(e))) => {
                ui.colored_label(theme::BAD, e);
            }
            _ => {}
        }
    }

    // -- Model ----------------------------------------------------------------------------------------------------

    fn model_tab(&mut self, ui: &mut egui::Ui) {
        self.autotune_components(ui);
        if self.view.target.is_none() {
            return;
        }
        let ini = match &self.view.ini {
            Some(Ok(Some(ini))) => Some(ini.clone()),
            _ => None,
        };
        let current = ini
            .as_ref()
            .and_then(|i| i.get("DlssNr", "ModelFile").map(str::to_string));
        let wine = cfg!(not(windows));
        let busy = self.busy();
        let is_auto = current
            .as_deref()
            .is_none_or(|v| v.trim().is_empty() || v.trim().eq_ignore_ascii_case("auto"));
        let mut action: Option<String> = None;
        let mut remove: Option<String> = None;
        let Some(target) = self.view.target.clone() else { return };
        if ini.is_none() {
            match &self.view.ini {
                None => {
                    ui.spinner();
                    ui.label(self.tr("tab.loading"));
                }
                Some(Err(e)) => {
                    ui.colored_label(theme::BAD, e);
                }
                _ => self.tab_install_prompt(ui),
            }
            return;
        }
        let configured = current.as_deref().unwrap_or("auto");
        let inspection = model_inspection(ui.ctx(), &target, configured);
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("mdl.current"));
            match &inspection {
                None => {
                    ui.spinner();
                    ui.label(self.tr("mdl.loading"));
                }
                Some(ModelInspection {
                    path: Some(path),
                    present: true,
                    version,
                    ..
                }) => {
                    ui.label(
                        RichText::new(version.as_deref().unwrap_or(self.tr("mdl.version_unknown")))
                            .size(22.0)
                            .strong(),
                    );
                    let in_catalog = self
                        .catalog
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .is_some_and(|models| models.iter().any(|m| m.path == *path));
                    let origin = if is_auto {
                        "mdl.beside"
                    } else if in_catalog {
                        "mdl.from_catalog"
                    } else {
                        "mdl.from_path"
                    };
                    ui.label(RichText::new(self.tr(origin)).color(theme::GOOD));
                    ui.add(
                        egui::Label::new(
                            RichText::new(path.file_name().unwrap_or_default().to_string_lossy()).monospace(),
                        )
                        .truncate(),
                    )
                    .on_hover_text(path.display().to_string());
                }
                Some(info) => {
                    let key = if is_auto {
                        "mdl.auto_unresolved"
                    } else if info.path.is_none() {
                        "mdl.unresolved"
                    } else {
                        "mdl.missing_file"
                    };
                    ui.colored_label(
                        if is_auto || info.path.is_none() {
                            theme::WARN
                        } else {
                            theme::BAD
                        },
                        self.tr(key),
                    );
                }
            }
            self.nr_compatibility_notice(ui);
            ui.add_space(6.0);
            ui.label(RichText::new(self.tr("mdl.next_start")).small().color(theme::MUTED));
            ui.horizontal_wrapped(|ui| {
                if !is_auto
                    && ui
                        .add_enabled(!busy, egui::Button::new(self.tr("mdl.use_auto")))
                        .clicked()
                {
                    action = Some("auto".into());
                }
                if ui.small_button(self.tr("mdl.refresh")).clicked() {
                    ui.ctx()
                        .data_mut(|data| data.remove::<ModelInspectionTask>(model_inspection_id(&target, configured)));
                }
            });
        });
        ui.add_space(16.0);
        ui.label(RichText::new(self.tr("mdl.catalog")).size(18.0).strong());
        self.model_import_area(ui);
        let models = match &self.catalog {
            None => {
                ui.spinner();
                return;
            }
            Some(Err(e)) => {
                ui.colored_label(theme::BAD, e);
                return;
            }
            Some(Ok(m)) => m.clone(),
        };
        if models.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr("mdl.catalog_empty"));
                let key = if inspection.as_ref().is_some_and(|i| i.present) {
                    "mdl.optional"
                } else {
                    "mdl.empty"
                };
                ui.label(RichText::new(self.tr(key)).color(theme::MUTED));
            });
        }
        for m in &models {
            let value = Catalog::model_file_value(m, wine);
            let in_use = current.as_deref() == Some(value.as_str());
            let targets: Vec<PathBuf> = self.games.iter().filter_map(|g| self.target_for(g)).collect();
            let used = model_usage(ui.ctx(), &m.path, &targets);
            widgets::card(ui, |ui| {
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new(&m.label).size(16.0).strong());
                    if in_use {
                        widgets::chip(ui, self.tr("mdl.in_use"), theme::GOOD);
                    }
                });
                ui.horizontal_wrapped(|ui| {
                    let version = m.version.as_deref().unwrap_or(self.tr("unknown"));
                    ui.label(RichText::new(format!("{} {version}", self.tr("mdl.version"))).color(theme::MUTED));
                    ui.label(
                        RichText::new(&m.sha256[..m.sha256.len().min(12)])
                            .small()
                            .monospace()
                            .color(theme::MUTED),
                    )
                    .on_hover_text(&m.sha256);
                    ui.label(
                        RichText::new(format!("{:.1} {}", m.size as f64 / 1_048_576.0, self.tr("unit.mib")))
                            .small()
                            .color(theme::MUTED),
                    );
                });
                self.model_compatibility_label(ui, &m.compatibility);
                if let Some(count) = used {
                    ui.label(
                        RichText::new(format!("{count} {}", self.tr("mdl.used_games")))
                            .small()
                            .color(theme::MUTED),
                    );
                }
                ui.add_space(6.0);
                ui.horizontal_wrapped(|ui| {
                    if !in_use
                        && ui
                            .add_enabled(ini.is_some() && !busy, egui::Button::new(self.tr("mdl.use")))
                            .clicked()
                    {
                        action = Some(value.clone());
                    }
                    if ui
                        .add_enabled(!in_use, egui::Button::new(self.tr("mdl.remove")).small())
                        .on_hover_text(self.tr("mdl.remove_help"))
                        .clicked()
                    {
                        remove = Some(m.sha256.clone());
                    }
                });
            });
            ui.add_space(10.0);
        }
        if let Some(v) = action {
            let edit = ("DlssNr".to_string(), "ModelFile".to_string(), v);
            self.request_plan(PlanKind::Settings, PlanInputs::Settings(vec![edit]));
        }
        if let Some(sha) = remove {
            self.jobs.spawn("remove", None, move |_| model::catalog_remove(sha));
        }
        self.settings_plan(ui);
    }

    // -- Logs -----------------------------------------------------------------------------------------------------

    fn logs_tab(&mut self, ui: &mut egui::Ui) {
        let Some(target) = self.view.target.clone() else { return };
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(!self.busy(), theme::primary(self.tr("log.bundle")))
                .clicked()
            {
                let status = format!(
                    "{:#?}\nversion_mismatch={}",
                    self.view.status,
                    self.view
                        .status
                        .as_ref()
                        .and_then(|s| s.as_ref().ok())
                        .is_some_and(Status::version_mismatch)
                );
                let t = target.clone();
                self.view.bundle = None;
                self.jobs
                    .spawn("bundle", None, move |_| model::bundle(t, status, model::bundle_dir()));
            }
            if ui.button(self.tr("log.open_folder")).clicked() {
                model::open_folder(&target);
            }
            if ui
                .add_enabled(!self.busy(), egui::Button::new(self.tr("log.retry")))
                .clicked()
            {
                self.view.logs = None;
                self.jobs
                    .spawn("logs", Some(target.clone()), move |_| model::logs(target));
            }
        });
        ui.add_space(12.0);
        let (files, result) = match &self.view.logs {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(self.tr("log.loading"));
                });
                return;
            }
            Some((files, result)) => (files.clone(), result.clone()),
        };
        match result {
            Err(e) => {
                ui.colored_label(theme::BAD, e);
            }
            Ok(None) => {
                if self.view.ini.as_ref().is_some_and(|i| matches!(i, Ok(None))) {
                    self.tab_install_prompt(ui);
                } else {
                    widgets::card(ui, |ui| {
                        ui.label(self.tr("log.none"));
                    });
                }
                ui.label(RichText::new(self.tr("log.no_log_bundle")).small().color(theme::MUTED));
            }
            Ok(Some(s)) => {
                ui.horizontal_wrapped(|ui| {
                    ui.label(RichText::new(self.tr("log.history")).strong());
                    let mut selected = s.path.clone();
                    egui::ComboBox::from_id_salt("session_log")
                        .selected_text(s.path.file_name().unwrap_or_default().to_string_lossy())
                        .width((ui.available_width() - 16.0).clamp(120.0, 460.0))
                        .truncate()
                        .show_ui(ui, |ui| {
                            for (index, file) in files.iter().enumerate() {
                                let name = file.file_name().unwrap_or_default().to_string_lossy();
                                let title = if index == 0 {
                                    format!("{} · {name}", self.tr("log.latest"))
                                } else {
                                    name.to_string()
                                };
                                ui.selectable_value(&mut selected, file.clone(), title);
                            }
                        })
                        .response
                        .on_hover_text(s.path.display().to_string());
                    if selected != s.path {
                        let target = self.view.target.clone().unwrap();
                        self.view.logs = None;
                        let files = files.clone();
                        self.jobs.spawn("logs", Some(target.clone()), move |_| {
                            let result = retoque_data::logs::summarize(&selected)
                                .map(Some)
                                .map_err(|e| e.to_string());
                            Outcome::Logs { target, files, result }
                        });
                    }
                });
                ui.add_space(12.0);
                let errors = s.error_count.max(s.errors.len());
                widgets::card(ui, |ui| {
                    widgets::card_title(ui, &log_verdict(self.lang, &s));
                    ui.label(
                        RichText::new(self.tr("log.evidence_note"))
                            .size(13.0)
                            .color(theme::MUTED),
                    );
                    if errors > 0 {
                        ui.add_space(8.0);
                        ui.colored_label(theme::WARN, self.tr("log.hint_errors"));
                    } else if !s.nr_initialized {
                        ui.add_space(8.0);
                        ui.label(self.tr("log.hint_inactive"));
                    }
                });
                ui.add_space(12.0);
                ui.horizontal_wrapped(|ui| {
                    widgets::stat_tile(
                        ui,
                        self.tr("log.dlss"),
                        s.dlss_version.as_deref().unwrap_or(self.tr("unknown")),
                        theme::TEXT_STRONG,
                    );
                    widgets::stat_tile(
                        ui,
                        self.tr("log.api"),
                        s.api.as_deref().unwrap_or(self.tr("unknown")),
                        theme::TEXT_STRONG,
                    );
                    widgets::stat_tile(
                        ui,
                        self.tr("log.errors"),
                        &errors.to_string(),
                        if errors > 0 { theme::BAD } else { theme::GOOD },
                    );
                    widgets::stat_tile(
                        ui,
                        self.tr("log.warnings"),
                        &s.warnings.to_string(),
                        if s.warnings > 0 { theme::WARN } else { theme::MUTED },
                    );
                    if let Some(ms) = s.nr_gpu_ms {
                        widgets::stat_tile(
                            ui,
                            self.tr("log.gpu"),
                            &format!("{ms:.2} {}", self.tr("unit.ms")),
                            theme::ACCENT,
                        );
                    }
                });
                if s.nr_gpu_ms.is_none() {
                    ui.add_space(12.0);
                    ui.label(RichText::new(format!("{} · {}", self.tr("log.gpu"), self.tr("log.no_sample"))).strong());
                    ui.label(RichText::new(self.tr("log.hint_timing")).size(13.0).color(theme::MUTED));
                }
                ui.add_space(16.0);
                if !s.errors.is_empty() {
                    widgets::tinted_card(ui, theme::ERROR_CARD, |ui| {
                        egui::CollapsingHeader::new(format!("{} ({errors})", self.tr("log.errors")))
                            .id_salt("session_errors")
                            .show(ui, |ui| {
                                let mut groups = BTreeMap::<&str, usize>::new();
                                for line in &s.errors {
                                    *groups.entry(log_message(line)).or_default() += 1;
                                }
                                for (line, count) in groups {
                                    ui.add(
                                        egui::Label::new(RichText::new(line).monospace().small().color(theme::BAD))
                                            .wrap(),
                                    );
                                    if count > 1 {
                                        ui.label(format!("{count} {}", self.tr("log.repeat")));
                                    }
                                }
                                if errors > s.errors.len() {
                                    ui.label(format!("{} {} / {errors}", self.tr("log.showing"), s.errors.len()));
                                }
                            });
                    });
                    ui.add_space(12.0);
                }
                widgets::card(ui, |ui| {
                    egui::CollapsingHeader::new(self.tr("log.details"))
                        .id_salt("session_details")
                        .show(ui, |ui| {
                            ui.label(s.nr_route.as_deref().unwrap_or(self.tr("log.route_unknown")));
                            ui.add_space(8.0);
                            ui.label(RichText::new(self.tr("log.lines")).strong());
                            egui::ScrollArea::vertical()
                                .id_salt("log_lines")
                                .max_height(260.0)
                                .show(ui, |ui| {
                                    for line in &s.highlights {
                                        ui.add(egui::Label::new(RichText::new(line).monospace().small()).wrap());
                                    }
                                });
                            if s.highlights_omitted > 0 || s.truncated_lines > 0 {
                                ui.label(RichText::new(self.tr("log.limited")).small().color(theme::MUTED));
                            }
                        });
                });
            }
        }
        match &self.view.bundle {
            Some(Ok(p)) => {
                ui.horizontal_wrapped(|ui| {
                    ui.colored_label(theme::GOOD, self.tr("log.bundle_done"));
                    ui.add(egui::Label::new(RichText::new(short_path(p)).monospace()).wrap());
                    if ui.small_button(self.tr("log.open_folder")).clicked()
                        && let Some(dir) = p.parent()
                    {
                        model::open_folder(dir);
                    }
                });
            }
            Some(Err(e)) => {
                ui.colored_label(theme::BAD, e);
            }
            None => {}
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------------------

fn choice_label<'a>(setting: &settings::Setting, value: &'a str, lang: Lang) -> &'a str {
    match setting.choice_labels(value) {
        Some((en, pt)) => match lang {
            Lang::En => en,
            Lang::Pt => pt,
        },
        None => t(lang, "set.custom"),
    }
}

fn value_widget(
    ui: &mut egui::Ui,
    setting: &settings::Setting,
    value: &mut String,
    id: &str,
    lang: Lang,
) -> egui::Response {
    let raw = value.clone();
    let response = match &setting.kind {
        &Kind::Bool | &Kind::Choice(&["auto", "true", "false"]) => {
            ui.push_id(id, |ui| {
                ui.with_layout(egui::Layout::left_to_right(egui::Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 2.0;
                    let mut responses = ["auto", "true", "false"].into_iter().map(|option| {
                        let label = choice_label(setting, option, lang);
                        let response = ui.add(
                            egui::Button::selectable(
                                value.eq_ignore_ascii_case(option),
                                RichText::new(label).size(13.0),
                            )
                            .wrap_mode(egui::TextWrapMode::Extend),
                        );
                        if response.clicked() {
                            *value = option.into();
                        }
                        response.on_hover_text(format!(
                            "{}: {}",
                            match lang {
                                Lang::En => setting.label_en,
                                Lang::Pt => setting.label_pt,
                            },
                            option
                        ))
                    });
                    let first = responses.next().unwrap();
                    responses.fold(first, |all, response| all.union(response))
                })
                .inner
            })
            .inner
        }
        Kind::Choice(options) => {
            let label = choice_label(setting, value, lang).to_owned();
            egui::ComboBox::from_id_salt(id)
                .selected_text(&label)
                .width(ui.available_width())
                .truncate()
                .show_ui(ui, |ui| {
                    for o in options.iter() {
                        ui.selectable_value(value, o.to_string(), choice_label(setting, o, lang))
                            .on_hover_text(*o);
                    }
                })
                .response
                .on_hover_text(label)
        }
        Kind::Float { min, max } => {
            ui.horizontal(|ui| {
                let automatic = value.eq_ignore_ascii_case("auto");
                let toggle = ui.add(egui::Button::selectable(automatic, t(lang, "set.automatic")));
                if toggle.clicked() {
                    *value = if automatic {
                        1.0_f32.clamp(*min, *max).to_string()
                    } else {
                        "auto".into()
                    };
                }
                if automatic {
                    return toggle;
                }
                match value.trim().parse::<f32>() {
                    Ok(mut f) => {
                        let response = ui.add(egui::DragValue::new(&mut f).range(*min..=*max).speed(0.01));
                        if response.changed() {
                            *value = format!("{f:.2}");
                        }
                        toggle.union(response)
                    }
                    Err(_) => toggle.union(ui.add(egui::TextEdit::singleline(value).desired_width(70.0))),
                }
            })
            .inner
        }
        Kind::Int { min, max } => match value.trim().parse::<i64>() {
            Ok(mut i) => {
                let response = ui.add(egui::DragValue::new(&mut i).range(*min..=*max));
                if response.changed() {
                    *value = i.to_string();
                }
                response
            }
            Err(_) => ui.add(egui::TextEdit::singleline(value).desired_width(ui.available_width().min(120.0))),
        },
        Kind::Text => ui.add(egui::TextEdit::singleline(value).desired_width(ui.available_width())),
    };
    response.on_hover_text(raw)
}

fn refusal_text(lang: Lang, r: &Refusal) -> (&'static str, String) {
    match r {
        Refusal::GameRunning { processes } => (
            "ref.running",
            processes
                .iter()
                .map(|(pid, n)| format!("{n} ({pid})"))
                .collect::<Vec<_>>()
                .join(", "),
        ),
        Refusal::Foreign { path, what } => {
            let kind = match what {
                retoque_core::Foreign::ReShade => t(lang, "ref.reshade"),
                retoque_core::Foreign::Unknown => t(lang, "ref.unknown_file"),
            };
            ("ref.foreign", format!("{path} ({kind})"))
        }
        Refusal::SecondProxy { name } => ("ref.second", name.clone()),
        Refusal::Receipt(s) => ("ref.receipt", diagnostic(lang, s)),
        Refusal::Usage(s) => ("ref.usage", diagnostic(lang, s)),
    }
}

fn step_text(lang: Lang, step: &Step) -> (&'static str, String, Color32) {
    match step {
        Step::Copy { from, to } => {
            let name = from
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            (
                t(lang, "step.copy"),
                if name == *to {
                    to.clone()
                } else {
                    format!("{to}  <- {name}")
                },
                theme::GOOD,
            )
        }
        Step::Backup { path, copy } => (t(lang, "step.backup"), format!("{path}  -> {copy}"), theme::MUTED),
        Step::Rename { from, to } => (t(lang, "step.rename"), format!("{from}  -> {to}"), theme::WARN),
        Step::SetIni {
            file,
            section,
            key,
            value,
            old,
        } => {
            let before = old.as_deref().unwrap_or(t(lang, "value.unset"));
            (
                t(lang, "step.ini"),
                format!("{file}: [{section}] {key}: {before} -> {value}"),
                theme::ACCENT,
            )
        }
        Step::Remove { path } => (t(lang, "step.remove"), path.clone(), theme::BAD),
        Step::Restore { copy, to } => (t(lang, "step.restore"), format!("{to}  <- {copy}"), theme::WARN),
        Step::Keep { path, why } => (
            t(lang, "step.keep"),
            format!("{path} ({})", diagnostic(lang, why)),
            theme::MUTED,
        ),
    }
}

fn plan_kind_name(lang: Lang, kind: PlanKind) -> &'static str {
    match kind {
        PlanKind::Install => t(lang, "kind.install"),
        PlanKind::Uninstall => t(lang, "kind.uninstall"),
        PlanKind::Settings => t(lang, "kind.settings"),
    }
}

fn store_name(lang: Lang, store: Store) -> &'static str {
    match store {
        Store::Steam => t(lang, "store.steam"),
        Store::Heroic => t(lang, "store.heroic"),
        Store::Lutris => t(lang, "store.lutris"),
        Store::Epic => t(lang, "store.epic"),
        Store::Gog => t(lang, "store.gog"),
        Store::Manual => t(lang, "store.manual"),
        Store::Xbox => t(lang, "store.xbox"),
    }
}

fn runtime_short(lang: Lang, runtime: &Runtime) -> &'static str {
    match runtime {
        Runtime::Windows => t(lang, "runtime.windows"),
        Runtime::Proton { .. } => t(lang, "runtime.proton"),
        Runtime::Wine { .. } => t(lang, "runtime.wine"),
        Runtime::NativeLinux => t(lang, "runtime.linux"),
        Runtime::Unknown => t(lang, "unknown"),
    }
}

fn runtime_text(lang: Lang, runtime: &Runtime) -> String {
    match runtime {
        Runtime::Windows => t(lang, "runtime.windows").into(),
        Runtime::Proton { app_id, .. } => format!("{} · {app_id}", t(lang, "runtime.proton")),
        Runtime::Wine { .. } => t(lang, "runtime.wine").into(),
        Runtime::NativeLinux => t(lang, "runtime.linux").into(),
        Runtime::Unknown => t(lang, "unknown").into(),
    }
}

/// `~` for the home folder, so the window shows less noise and screenshots carry no user name.
fn short_path(p: &Path) -> String {
    if let Some(home) = dirs::home_dir()
        && let Ok(rest) = p.strip_prefix(&home)
    {
        return format!("~/{}", rest.display());
    }
    p.display().to_string()
}

fn relative_to(p: &Path, base: &Path, root: &str) -> String {
    match p.strip_prefix(base) {
        Ok(rest) if rest.as_os_str().is_empty() => root.into(),
        Ok(rest) => rest.display().to_string(),
        Err(_) => short_path(p),
    }
}

/// A ModelFile value as a path on this machine: `Z:\a\b` is `/a/b` under Wine; other drive letters cannot be
/// checked from Linux.
fn host_path(value: &str) -> Option<PathBuf> {
    let v = value.trim();
    if cfg!(windows) {
        // Only an absolute path can be checked from here; a relative one is resolved by OptiScaler, not us.
        let p = PathBuf::from(v);
        return p.is_absolute().then_some(p);
    }
    if let Some(rest) = v.strip_prefix("Z:").or_else(|| v.strip_prefix("z:")) {
        return Some(PathBuf::from(rest.replace('\\', "/")));
    }
    v.starts_with('/').then(|| PathBuf::from(v))
}

#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;

    #[test]
    fn setting_control_is_named_and_interactive_for_accessibility() {
        use egui_kittest::{Harness, kittest::Queryable};
        let setting = settings::all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Stage")
            .unwrap();
        let mut harness = Harness::new_ui_state(
            |ui, value| {
                let label = ui.label(setting.label_en);
                value_widget(ui, setting, value, "stage-test", Lang::En).labelled_by(label.id);
            },
            "auto".to_owned(),
        );
        harness
            .get_by_role_and_label(egui::Role::ComboBox, setting.label_en)
            .click();
        harness.run();
        harness.get_by_label("Before the upscaler (experimental)").click();
        harness.run();
        assert_eq!(harness.state(), "1");
    }

    #[test]
    fn model_file_values_map_back_to_host_paths() {
        assert_eq!(host_path("Z:\\home\\u\\m.dll"), Some(PathBuf::from("/home/u/m.dll")));
        assert_eq!(host_path("/abs/m.dll"), Some(PathBuf::from("/abs/m.dll")));
        assert_eq!(host_path("C:\\x.dll"), None);
        assert_eq!(host_path("nvngx_dlssnr.dll"), None);
    }
}

// Shell-only state and helpers. Tab content workers do not need to change this contract.
enum ShellOutcome {
    Payload(PathBuf, Result<String, String>),
    Library(u64, PathBuf, Status),
    LibraryDone(u64),
}

struct ActionToast {
    message: String,
    detail: Option<String>,
    failed: bool,
    created: std::time::Instant,
    details_open: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum InstallState {
    Missing,
    Installed,
    Update,
    Current,
    Partial,
    Untracked,
}
impl InstallState {
    fn label(self) -> &'static str {
        match self {
            Self::Missing => "st.not_installed",
            Self::Installed => "st.installed",
            Self::Update => "shell.update_available",
            Self::Current => "shell.up_to_date",
            Self::Partial => "shell.partial",
            Self::Untracked => "st.untracked",
        }
    }
    fn color(self) -> Color32 {
        match self {
            Self::Update => theme::ACCENT,
            Self::Partial => theme::BAD,
            Self::Current | Self::Installed => theme::GOOD,
            _ => theme::MUTED,
        }
    }
}

fn install_state(status: &Status, payload: Option<&str>) -> InstallState {
    match status.receipt.as_ref().map(|r| r.status) {
        Some(ReceiptStatus::Partial | ReceiptStatus::UninstallIncomplete) => InstallState::Partial,
        Some(ReceiptStatus::Complete) => match (status.build_sha256.as_deref(), payload) {
            (Some(installed), Some(available)) if installed == available => InstallState::Current,
            (Some(_), Some(_)) => InstallState::Update,
            _ => InstallState::Installed,
        },
        None if !status.optiscaler_proxies.is_empty() => InstallState::Untracked,
        None => InstallState::Missing,
    }
}

fn clear_applied(draft: &mut BTreeMap<(String, String), String>, edits: &[Edit]) {
    for (section, key, value) in edits {
        let key = (section.clone(), key.clone());
        if draft.get(&key) == Some(value) {
            draft.remove(&key);
        }
    }
}

pub(crate) fn step_group(step: &Step, steps: &[Step], destination_exists: bool) -> usize {
    match step {
        Step::Keep { .. } => 2,
        Step::Remove { .. } => 3,
        Step::SetIni { .. } => 4,
        Step::Backup { .. } | Step::Rename { .. } | Step::Restore { .. } => 1,
        Step::Copy { to, .. }
            if destination_exists
                || steps
                    .iter()
                    .any(|s| matches!(s, Step::Backup { path, .. } | Step::Remove { path } if path == to)) =>
        {
            1
        }
        Step::Copy { .. } => 0,
    }
}

impl RetoqueApp {
    fn shell_shortcuts(&mut self, ctx: &egui::Context) {
        if self.review_open || self.folder_dialog.is_some() {
            return;
        }
        if ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::F)) {
            self.focus_search = true;
            self.sidebar_visible = true;
        }
        if !self.busy() && ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::Enter)) {
            let partial = matches!(&self.view.status,Some(Ok(s)) if install_state(s,None)==InstallState::Partial);
            if partial {
                self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
            } else if let Some(options) = self.install_inputs() {
                self.request_plan(PlanKind::Install, PlanInputs::Install(options));
            }
        }
    }

    fn hash_payload(&mut self) {
        self.payload_build = None;
        let Some(path) = self.payload.clone() else { return };
        let tx = self.shell_tx.clone();
        let ctx = self.context.clone();
        let spawn = std::thread::Builder::new()
            .name("retoque-payload".into())
            .spawn(move || {
                use sha2::{Digest, Sha256};
                use std::io::Read;
                let result = (|| {
                    let mut file = std::fs::File::open(path.join("OptiScaler.dll"))?;
                    let mut hash = Sha256::new();
                    let mut bytes = [0; 65536];
                    loop {
                        let count = file.read(&mut bytes)?;
                        if count == 0 {
                            break;
                        }
                        hash.update(&bytes[..count]);
                    }
                    Ok::<_, std::io::Error>(format!("{:x}", hash.finalize()))
                })()
                .map_err(|e| e.to_string());
                let _ = tx.send(ShellOutcome::Payload(path, result));
                ctx.request_repaint();
            });
        if let Err(error) = spawn {
            self.payload_build = Some(Err(error.to_string()));
        }
    }

    fn inspect_library(&mut self) {
        self.library_generation += 1;
        self.library_pending = true;
        self.library_status.clear();
        let generation = self.library_generation;
        let paths: Vec<_> = self.games.iter().filter_map(|g| self.target_for(g)).collect();
        let tx = self.shell_tx.clone();
        let ctx = self.context.clone();
        if let Err(error) = std::thread::Builder::new()
            .name("retoque-library-status".into())
            .spawn(move || {
                for path in paths {
                    if let Ok(status) = retoque_core::status(&path) {
                        let _ = tx.send(ShellOutcome::Library(generation, path, status));
                        ctx.request_repaint();
                    }
                }
                let _ = tx.send(ShellOutcome::LibraryDone(generation));
                ctx.request_repaint();
            })
        {
            self.library_pending = false;
            self.error = Some(error.to_string());
        }
    }

    fn receive_shell(&mut self) {
        while let Ok(outcome) = self.shell_rx.try_recv() {
            match outcome {
                ShellOutcome::Payload(path, result) if self.payload.as_ref() == Some(&path) => {
                    self.payload_build = Some(result)
                }
                ShellOutcome::Library(generation, path, status) if generation == self.library_generation => {
                    if self.view.target.as_ref() != Some(&path) || self.view.status.is_none() {
                        self.library_status.insert(path, status);
                    }
                }
                ShellOutcome::LibraryDone(generation) if generation == self.library_generation => {
                    self.library_pending = false;
                }
                _ => {}
            }
        }
    }

    fn select_id(&mut self, id: String) {
        if self.selected.as_ref() != Some(&id) {
            self.selected = Some(id);
            let target = self.selected_game().and_then(|g| self.target_for(g));
            self.open_target(target);
        }
    }

    fn search_field(&mut self, ui: &mut egui::Ui) {
        let response = ui.add(
            egui::TextEdit::singleline(&mut self.search)
                .id(egui::Id::unique("library_search"))
                .hint_text(t(self.lang, "lib.search"))
                .desired_width(f32::INFINITY),
        );
        response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::TextInput, true, t(self.lang, "lib.search")));
        if self.focus_search {
            response.request_focus();
            self.focus_search = false;
        }
    }

    fn library_keyboard(&mut self, ui: &egui::Ui) {
        if self.review_open || self.folder_dialog.is_some() {
            return;
        }
        let search_focus = ui.memory(|m| m.has_focus(egui::Id::unique("library_search")));
        if ui.ctx().egui_wants_keyboard_input() && !search_focus && !self.library_has_focus {
            return;
        }
        let direction = ui.input_mut(|i| {
            if i.consume_key(egui::Modifiers::NONE, egui::Key::ArrowDown) {
                1
            } else if i.consume_key(egui::Modifiers::NONE, egui::Key::ArrowUp) {
                -1
            } else {
                0
            }
        });
        if direction == 0 {
            return;
        }
        let needle = self.search.to_lowercase();
        let mut games: Vec<_> = self
            .games
            .iter()
            .filter(|g| {
                (needle.is_empty() || g.name.to_lowercase().contains(&needle))
                    && (!self.installed_only || self.has_retoque(g))
            })
            .collect();
        games.sort_by_key(|g| !self.has_retoque(g));
        if games.is_empty() {
            return;
        }
        let index = self
            .selected
            .as_ref()
            .and_then(|id| games.iter().position(|g| &g.id == id));
        let next = match index {
            Some(index) => (index as isize + direction).clamp(0, games.len() as isize - 1) as usize,
            None => {
                if direction > 0 {
                    0
                } else {
                    games.len() - 1
                }
            }
        };
        self.select_id(games[next].id.clone());
    }

    /// Retoque (or an OptiScaler build) is in this game's folder, with or without a receipt: installs made by
    /// the old scripts have none, and they still belong in "With Retoque".
    fn has_retoque(&self, game: &Game) -> bool {
        !matches!(self.state_for(game), InstallState::Missing)
    }

    fn state_for(&self, game: &Game) -> InstallState {
        self.target_for(game)
            .and_then(|t| self.library_status.get(&t))
            .map(|s| {
                install_state(
                    s,
                    self.payload_build
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .map(String::as_str),
                )
            })
            .unwrap_or(if game.targets.iter().any(|t| t.has_receipt) {
                InstallState::Installed
            } else {
                InstallState::Missing
            })
    }

    fn home_grid(&mut self, ui: &mut egui::Ui) {
        ui.heading(self.tr("welcome.title"));
        let installed = self.games.iter().filter(|g| self.has_retoque(g)).count();
        let updates = self
            .games
            .iter()
            .filter(|g| self.state_for(g) == InstallState::Update)
            .count();
        ui.label(
            RichText::new(format!(
                "{} {} · {} {} · {} {}",
                self.games.len(),
                self.tr("shell.games"),
                installed,
                self.tr("welcome.installed"),
                updates,
                self.tr("shell.updates")
            ))
            .color(theme::MUTED),
        );
        if self.library_pending {
            ui.label(
                RichText::new(self.tr("shell.library_checking"))
                    .small()
                    .color(theme::MUTED),
            );
        }
        if let Some(Ok(hash)) = &self.payload_build {
            ui.label(
                RichText::new(format!("{} {}", self.tr("app.payload"), &hash[..8]))
                    .small()
                    .color(theme::MUTED),
            );
        }
        ui.add_space(12.0);
        self.search_field(ui);
        self.library_keyboard(ui);
        ui.horizontal(|ui| {
            ui.checkbox(&mut self.installed_only, t(self.lang, "lib.installed_only"));
            if ui.button(self.tr("lib.add_folder")).clicked() {
                self.folders_open(FolderDialog::Add);
            }
            self.folders_button(ui);
        });
        if self.games.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr(if self.scanning { "lib.scanning" } else { "lib.empty" }));
                ui.label(self.tr("shell.home_help"));
            });
            return;
        }
        let needle = self.search.to_lowercase();
        let mut clicked = None;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            for installed_group in [true, false] {
                if !installed_group && self.installed_only {
                    continue;
                }
                let games: Vec<_> = self
                    .games
                    .iter()
                    .filter(|g| {
                        self.has_retoque(g) == installed_group
                            && (needle.is_empty() || g.name.to_lowercase().contains(&needle))
                    })
                    .collect();
                if games.is_empty() {
                    continue;
                }
                widgets::group_label(
                    ui,
                    self.tr(if installed_group {
                        "lib.group_installed"
                    } else {
                        "lib.group_other"
                    }),
                    games.len(),
                );
                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing = egui::vec2(20.0, 20.0);
                    for game in games {
                        let state = self.state_for(game);
                        let art = self.art.get(&game.id).and_then(|a| a.capsule.as_deref());
                        if widgets::cover_card(
                            ui,
                            &game.name,
                            art,
                            self.tr(state.label()),
                            state.color(),
                            (!game.registered).then(|| self.tr("folders.unregistered")),
                        )
                        .clicked()
                        {
                            clicked = Some(game.id.clone());
                        }
                    }
                });
                ui.add_space(20.0);
            }
        });
        if let Some(id) = clicked {
            self.select_id(id);
        }
    }

    fn game_banner(&mut self, ui: &mut egui::Ui, game: &Game) {
        let art = self.art.get(&game.id).cloned().unwrap_or_default();
        let compact = self.tab == Tab::Compare;
        let height = if compact { 100.0 } else { 240.0 };
        let (rect, _) = ui.allocate_exact_size(egui::vec2(ui.available_width(), height), egui::Sense::hover());
        if let Some(hero) = art.hero.as_deref() {
            let ratio = rect.width() / rect.height();
            let source_ratio = 3840.0 / 1240.0;
            let uv = if ratio < source_ratio {
                let fraction = ratio / source_ratio;
                egui::Rect::from_min_max(
                    egui::pos2((1.0 - fraction) / 2.0, 0.0),
                    egui::pos2((1.0 + fraction) / 2.0, 1.0),
                )
            } else {
                let fraction = source_ratio / ratio;
                egui::Rect::from_min_max(
                    egui::pos2(0.0, (1.0 - fraction) / 2.0),
                    egui::pos2(1.0, (1.0 + fraction) / 2.0),
                )
            };
            egui::Image::new(widgets::file_uri(hero))
                .uv(uv)
                .corner_radius(12)
                .paint_at(ui, rect);
        } else {
            ui.painter().rect_filled(rect, 12, theme::CARD);
        }
        ui.painter()
            .rect_filled(rect, 12, Color32::from_black_alpha(if compact { 200 } else { 155 }));
        let mut child = ui.new_child(
            egui::UiBuilder::new()
                .max_rect(rect.shrink(16.0))
                .layout(egui::Layout::top_down(egui::Align::Min)),
        );
        if compact {
            child.label(RichText::new(&game.name).size(22.0).strong());
        } else if let Some(logo) = art.logo.as_deref() {
            child.add(
                egui::Image::new(widgets::file_uri(logo))
                    .fit_to_exact_size(egui::vec2(220.0, 70.0))
                    .maintain_aspect_ratio(true),
            );
        } else {
            child.label(RichText::new(&game.name).size(28.0).strong());
            child.add_space(22.0);
        }
        self.status_chips(&mut child);
        if compact {
            return;
        }
        child.add_space(8.0);
        if let Some((done, total)) = self.view.target.as_ref().and_then(|t| self.writing.get(t)) {
            let kind = self
                .view
                .target
                .as_ref()
                .and_then(|target| self.write_kinds.get(target));
            let key = match kind {
                Some(PlanKind::Uninstall) => "shell.progress_remove",
                Some(PlanKind::Settings) => "shell.progress_save",
                Some(PlanKind::Install) if matches!(&self.view.status,Some(Ok(s)) if s.receipt.is_some()) => {
                    "shell.progress_update"
                }
                _ => "shell.progress_install",
            };
            child.add_enabled(false, theme::primary(&format!("{} {done}/{total}", self.tr(key))));
            child.ctx().request_repaint_after(std::time::Duration::from_millis(100));
        } else {
            let state = self.view.status.as_ref().and_then(|r| r.as_ref().ok()).map(|s| {
                install_state(
                    s,
                    self.payload_build
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .map(String::as_str),
                )
            });
            child.horizontal_wrapped(|ui| {
                if state == Some(InstallState::Current) {
                    ui.add_enabled(false, theme::primary(self.tr("shell.up_to_date")));
                    if ui.button(self.tr("shell.reinstall")).clicked()
                        && let Some(options) = self.install_inputs()
                    {
                        self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                    }
                } else {
                    let label = match state {
                        Some(InstallState::Partial) => "shell.resume",
                        Some(InstallState::Update) => "shell.update",
                        Some(InstallState::Installed) => "shell.reinstall",
                        Some(InstallState::Untracked) => "shell.install",
                        _ => "shell.install",
                    };
                    if ui
                        .add_enabled(
                            state.is_some() && (state == Some(InstallState::Partial) || self.payload.is_some()),
                            theme::primary(self.tr(label)),
                        )
                        .on_hover_text(self.tr("shell.review_shortcut"))
                        .clicked()
                    {
                        if state == Some(InstallState::Partial) {
                            self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
                        } else if let Some(options) = self.install_inputs() {
                            self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                        }
                    }
                }
                if matches!(
                    state,
                    Some(InstallState::Current | InstallState::Update | InstallState::Installed)
                ) && ui.button(self.tr("shell.uninstall_open")).clicked()
                {
                    self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
                }
                if ui.button(self.tr("shell.open_folder")).clicked() {
                    model::open_folder(&game.install_dir);
                }
            });
        }
        child.label(
            RichText::new(format!(
                "{} · {}",
                store_name(self.lang, game.store),
                runtime_text(self.lang, &game.runtime)
            ))
            .small()
            .color(theme::MUTED),
        );
        ui.add_space(12.0);
    }

    /// Sticky, shared by the shell and settings tab integration. It reserves space before the tab scroll area.
    fn pending_footer(&mut self, ui: &mut egui::Ui) {
        let count = self.view.edits.len();
        if count == 0 {
            return;
        }
        let invalid = self.view.edits.iter().any(|((section, key), value)| {
            settings::all()
                .iter()
                .find(|s| s.section == section && s.key == key)
                .is_some_and(|s| !s.accepts(value))
        });
        egui::Panel::bottom("pending_changes").show(ui, |ui| {
            ui.add_space(8.0);
            ui.horizontal_wrapped(|ui| {
                ui.label(format!("{count} {}", self.tr("shell.pending")));
                let review = self.tr("set.save").to_owned();
                if ui
                    .add_enabled(!invalid && !self.busy(), theme::primary(&review))
                    .clicked()
                {
                    let edits = self
                        .view
                        .edits
                        .iter()
                        .map(|((s, k), v)| (s.clone(), k.clone(), v.clone()))
                        .collect();
                    self.request_plan(PlanKind::Settings, PlanInputs::Settings(edits));
                }
                if ui
                    .add_enabled(!self.busy(), egui::Button::new(self.tr("shell.discard")))
                    .clicked()
                {
                    self.view.edits.clear();
                    self.view.plan = None;
                    if let Some(target) = &self.view.target {
                        self.drafts.remove(target);
                    }
                }
            });
            if invalid {
                ui.colored_label(theme::BAD, self.tr("set.invalid"));
            }
            ui.add_space(4.0);
        });
    }

    fn restore_focus(&mut self) {
        self.restore_frames = 2;
        self.context.request_repaint();
    }

    fn review_sheet(&mut self, ctx: &egui::Context) {
        if !self.review_open {
            if self.restore_frames > 0 {
                if let Some(id) = self.origin_focus {
                    ctx.memory_mut(|m| m.request_focus(id));
                }
                self.restore_frames -= 1;
                ctx.request_repaint();
            }
            return;
        }
        self.review_rendering = true;
        let modal = egui::Modal::new(egui::Id::unique("plan_review"))
            .frame(
                egui::Frame::new()
                    .fill(theme::CARD)
                    .stroke(egui::Stroke::new(1.0, theme::CARD_EDGE))
                    .corner_radius(12)
                    .inner_margin(20),
            )
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 740.0));
                if self.tuning.active {
                    self.autotune_review(ui);
                } else if let Some((kind, result)) = self.view.plan.clone() {
                    let installed = matches!(&self.view.status,Some(Ok(s)) if s.receipt.is_some());
                    self.plan_view(ui, kind, &result, installed);
                } else {
                    ui.horizontal(|ui| {
                        ui.spinner();
                        ui.label(self.tr("shell.preparing"));
                    });
                    if ui.button(self.tr("cancel")).clicked() {
                        self.review_open = false;
                        self.restore_focus();
                    }
                }
            });
        self.review_rendering = false;
        if modal.should_close() {
            self.review_open = false;
            self.restore_focus();
        }
    }

    fn notify_action(&mut self, kind: PlanKind, game: &str, result: &Result<(), String>) {
        let key = match (kind, result.is_ok()) {
            (_, false) => "shell.failed",
            (PlanKind::Install, true) => "shell.installed_done",
            (PlanKind::Uninstall, true) => "shell.removed_done",
            (PlanKind::Settings, true) => "shell.settings_done",
        };
        self.toast(
            format!("{} · {game}", self.tr(key)),
            result
                .as_ref()
                .err()
                .cloned()
                .or_else(|| Some(self.tr("shell.done_details").into())),
            result.is_err(),
        );
    }
    fn toast(&mut self, message: String, detail: Option<String>, failed: bool) {
        self.toasts.push(ActionToast {
            message,
            detail,
            failed,
            created: std::time::Instant::now(),
            details_open: false,
        });
        if self.toasts.len() > 3 {
            self.toasts.remove(0);
        }
    }
    fn show_toasts(&mut self, ctx: &egui::Context) {
        self.toasts
            .retain(|toast| toast.failed || toast.details_open || toast.created.elapsed().as_secs() < 8);
        if self.toasts.is_empty() {
            return;
        }
        let lang = self.lang;
        egui::Area::new(egui::Id::unique("action_toasts"))
            .order(egui::Order::Foreground)
            .anchor(egui::Align2::RIGHT_BOTTOM, egui::vec2(-16.0, -48.0))
            .show(ctx, |ui| {
                ui.set_max_width(420.0_f32.min(ctx.content_rect().width() - 32.0));
                let mut remove = None;
                for (i, toast) in self.toasts.iter_mut().enumerate() {
                    widgets::card(ui, |ui| {
                        ui.label(RichText::new(&toast.message).color(if toast.failed {
                            theme::BAD
                        } else {
                            theme::GOOD
                        }));
                        ui.horizontal(|ui| {
                            if toast.detail.is_some() && ui.small_button(t(lang, "shell.details")).clicked() {
                                toast.details_open = !toast.details_open;
                            }
                            if ui.small_button(t(lang, "dismiss")).clicked() {
                                remove = Some(i);
                            }
                        });
                        if toast.details_open
                            && let Some(detail) = &toast.detail
                        {
                            ui.label(detail);
                        }
                    });
                }
                if let Some(i) = remove {
                    self.toasts.remove(i);
                }
            });
        ctx.request_repaint_after(std::time::Duration::from_millis(500));
    }
}

#[cfg(test)]
mod shell_tests {
    use super::*;
    use egui_kittest::{
        Harness,
        kittest::{NodeT as _, Queryable},
    };

    #[test]
    fn review_groups_existing_copy_as_replace_without_backup() {
        let copy = Step::Copy {
            from: "payload/OptiScaler.dll".into(),
            to: "winmm.dll".into(),
        };
        assert_eq!(step_group(&copy, std::slice::from_ref(&copy), true), 1);
        assert_eq!(step_group(&copy, std::slice::from_ref(&copy), false), 0);
    }

    fn app() -> RetoqueApp {
        let context = egui::Context::default();
        let (shell_tx, shell_rx) = std::sync::mpsc::channel();
        RetoqueApp {
            tuning: crate::autotune::UiState::default(),
            folder_dialog: None,
            folder_focus: None,
            folder_restore: 0,
            compare: crate::compare::CompareView::new(),
            saved: Saved::default(),
            lang: Lang::En,
            jobs: Jobs::new(context.clone()),
            games: vec![],
            art: HashMap::new(),
            skipped: vec![],
            scanning: false,
            rescan_pending: false,
            search: String::new(),
            installed_only: false,
            selected: None,
            tab: Tab::Install,
            payload: None,
            view: GameView::default(),
            catalog: None,
            error: None,
            writing: HashMap::new(),
            write_kinds: HashMap::new(),
            next_request: 0,
            pending_select: None,
            drafts: HashMap::new(),
            context,
            shell_tx,
            shell_rx,
            payload_build: None,
            library_status: HashMap::new(),
            library_generation: 0,
            library_pending: false,
            review_open: false,
            review_rendering: false,
            origin_focus: None,
            sidebar_visible: true,
            focus_search: false,
            library_has_focus: false,
            restore_frames: 0,
            toasts: vec![],
        }
    }

    #[test]
    fn nr_warning_suggestion_and_unknown_are_visible_in_both_languages() {
        use retoque_data::model_compat::ModelCompatibility;
        for lang in [Lang::En, Lang::Pt] {
            let mut app = app();
            app.lang = lang;
            app.view.target = Some("/synthetic/nr-game".into());
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.catalog = Some(Ok(vec![retoque_data::models::Model {
                sha256: "synthetic".into(),
                version: None,
                size: 64,
                label: "Compatible fixture".into(),
                path: "/synthetic/model.dll".into(),
                compatibility: ModelCompatibility {
                    archs: vec![86],
                    known: true,
                },
            }]));
            let mut h = Harness::builder().with_size([900., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    let target = app.view.target.as_ref().unwrap();
                    let task: ModelInspectionTask = std::sync::Arc::new(std::sync::Mutex::new(Some(ModelInspection {
                        present: true,
                        compatibility: ModelCompatibility {
                            archs: vec![120],
                            known: true,
                        },
                        ..Default::default()
                    })));
                    ui.ctx()
                        .data_mut(|d| d.insert_temp(model_inspection_id(target, "auto"), task));
                    app.nr_compatibility_notice(ui);
                    app.model_compatibility_label(ui, &ModelCompatibility::default());
                },
                app,
            );
            h.run_steps(2);
            h.get_by_label(t(lang, "mdl.incompatible"));
            h.get_by_label(t(lang, "mdl.compat_unknown"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "mdl.use_suggested"));
            assert!(h.query_by_label_contains("Compatible fixture").is_some());
        }
    }

    #[test]
    fn version_mismatch_warns_but_missing_resource_is_unknown() {
        let mut app = app();
        app.view.status = Some(Ok(Status {
            optiscaler_proxies: vec!["dxgi.dll".into()],
            proxy_version: Some("1.2.3.4".into()),
            forwarder_version: Some("1.2.3.5".into()),
            ..Default::default()
        }));
        let mut h = Harness::builder()
            .with_size([1100., 400.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.status_chips(ui), app);
        h.run_steps(2);
        h.get_by_label(t(Lang::En, "ver.mismatch"));
        h.state_mut()
            .view
            .status
            .as_mut()
            .unwrap()
            .as_mut()
            .unwrap()
            .forwarder_version = None;
        h.run_steps(2);
        assert!(h.query_by_label(t(Lang::En, "ver.mismatch")).is_none());
        assert!(h.query_by_label_contains("NR forwarder").is_none());
        assert!(h.query_by_label_contains("OptiScaler 1.2.3.4").is_some());
    }

    #[test]
    fn root_changes_queue_a_fresh_scan_instead_of_racing_in_flight_discovery() {
        let mut app = app();
        app.scanning = true;
        app.rescan();
        assert!(app.rescan_pending);
        assert!(!app.jobs.busy());
        app.rescan();
        assert!(app.rescan_pending);
        assert!(!app.jobs.busy());
    }

    #[test]
    fn add_folder_asks_kind_and_manager_lists_both_kinds() {
        let mut app = app();
        app.saved.manual.push("/synthetic/Single".into());
        app.saved.libraries.push("/synthetic/Several".into());
        let mut h = Harness::builder().with_size([930., 900.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                if ui.button(t(app.lang, "lib.add_folder")).clicked() {
                    app.folders_open(FolderDialog::Add);
                }
                app.folders_button(ui);
                app.folders_dialog(ui.ctx());
            },
            app,
        );
        h.get_by_role_and_label(egui::Role::Button, "Add folder…")
            .click_accesskit();
        h.run_steps(3);
        h.get_by_role_and_label(egui::Role::Button, "One game");
        h.get_by_role_and_label(egui::Role::Button, "A folder with several games");
        h.key_press(egui::Key::Escape);
        h.run_steps(3);
        assert!(h.state().folder_dialog.is_none());
        h.get_by_role_and_label(egui::Role::Button, "Your folders")
            .click_accesskit();
        h.run_steps(3);
        h.get_by_label("Individual games and emulators");
        h.get_by_label("Folders with several games");
        h.get_by_label("/synthetic/Single");
        h.get_by_label("/synthetic/Several");
        h.get_by_role_and_label(egui::Role::Button, "Close").click_accesskit();
        h.run_steps(3);
        assert!(h.state().folder_dialog.is_none());
    }

    #[test]
    fn unregistered_library_row_keeps_name_and_shows_hint() {
        let mut app = app();
        app.games.push(Game {
            id: "orphan".into(),
            name: "Unlisted game".into(),
            store: Store::Epic,
            install_dir: "/synthetic/game".into(),
            runtime: Runtime::Unknown,
            targets: vec![],
            dlss_anywhere: vec![],
            registered: false,
        });
        let h = Harness::new_ui_state(|ui, app: &mut RetoqueApp| app.library(ui), app);
        h.get_by_role_and_label(egui::Role::Button, "Unlisted game");
        h.get_by_label(t(Lang::En, "folders.unregistered"));
    }

    #[test]
    fn gpu_header_names_architecture_memory_and_driver() {
        use crate::autotune::gpu::{Arch, Gpu, Vendor};
        let mut app = app();
        app.tuning.gpus = Some(vec![Gpu {
            name: "RTX 3060".into(),
            vendor: Vendor::Nvidia,
            arch: Arch::Ampere,
            compute_cap: Some((8, 6)),
            driver: Some("617.14".into()),
            vram_mib: Some(12288),
        }]);
        let harness = Harness::new_ui_state(|ui, app: &mut RetoqueApp| app.autotune_gpu(ui), app);
        harness.get_by_label("RTX 3060 · Ampere · 12 GB");
    }

    #[test]
    fn lsfg_is_gated_by_readiness_and_review_does_not_write() {
        use crate::autotune::lsfg::LsfgReadiness;
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        let before = "[LSFG]\nEnabled=false\nMultiplier=2\n[FrameGen]\nEnabled=false\n";
        std::fs::write(&ini, before).unwrap();
        let mut app = app();
        app.view.target = Some(temp.path().into());
        app.view.ini = Some(Ok(Some(Ini::parse(before.as_bytes()))));
        app.view.status = Some(Ok(Status {
            receipt: Some(retoque_core::receipt::Receipt {
                status: ReceiptStatus::Complete,
                updated: String::new(),
                records: vec![],
            }),
            ..Default::default()
        }));
        app.tuning.installed = Some(Ok(vec![]));
        app.tuning.readiness = Some(LsfgReadiness::NoLosslessScaling);
        let mut harness = Harness::builder().with_size([930., 1200.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.receive();
                app.autotune_lsfg(ui);
                app.review_sheet(ui.ctx());
            },
            app,
        );
        assert!(
            harness
                .get_by_role_and_label(egui::Role::Button, "x2")
                .accesskit_node()
                .is_disabled()
        );
        harness.state_mut().tuning.readiness = Some(LsfgReadiness::Ready);
        harness.run_steps(3);
        harness
            .get_by_role_and_label(egui::Role::Button, "x2")
            .click_accesskit();
        for _ in 0..100 {
            harness.step();
            if harness.state().tuning.review.is_some() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(harness.state().tuning.review.is_some());
        assert!(harness.state().review_open);
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
        harness.key_press(egui::Key::Escape);
        harness.run_steps(3);
        assert!(!harness.state().review_open);
        harness.state_mut().tuning.installed = Some(Ok(vec!["component".into()]));
        harness.run_steps(3);
        assert!(
            harness
                .get_by_role_and_label(egui::Role::Button, "x2")
                .accesskit_node()
                .is_disabled()
        );
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
    }

    #[test]
    fn states_distinguish_updates_current_and_partial_cleanup() {
        let mut status = Status::default();
        assert_eq!(install_state(&status, Some("new")), InstallState::Missing);
        status.receipt = Some(retoque_core::receipt::Receipt {
            status: ReceiptStatus::Complete,
            updated: String::new(),
            records: vec![],
        });
        status.build_sha256 = Some("old".into());
        assert_eq!(install_state(&status, None), InstallState::Installed);
        assert_eq!(install_state(&status, Some("new")), InstallState::Update);
        assert_eq!(install_state(&status, Some("old")), InstallState::Current);
        status.receipt.as_mut().unwrap().status = ReceiptStatus::Partial;
        assert_eq!(install_state(&status, Some("old")), InstallState::Partial);
    }

    #[test]
    fn drafts_survive_target_switches_and_only_applied_values_are_cleared() {
        let temp = tempfile::tempdir().unwrap();
        let first = temp.path().join("first");
        let second = temp.path().join("second");
        std::fs::create_dir_all(&first).unwrap();
        std::fs::create_dir_all(&second).unwrap();
        let mut app = app();
        app.open_target(Some(first.clone()));
        let key = ("DlssNr".into(), "Stage".into());
        app.view.edits.insert(key.clone(), "1".into());
        app.open_target(Some(second.clone()));
        assert!(app.view.edits.is_empty());
        app.view.edits.insert(key.clone(), "0".into());
        app.open_target(Some(first));
        assert_eq!(app.view.edits.get(&key).map(String::as_str), Some("1"));
        let edits = vec![("DlssNr".into(), "Stage".into(), "0".into())];
        clear_applied(&mut app.view.edits, &edits);
        assert_eq!(app.view.edits.len(), 1);
        clear_applied(app.drafts.get_mut(&second).unwrap(), &edits);
        assert!(app.drafts[&second].is_empty());
    }

    #[test]
    fn search_focus_arrow_navigation_and_modal_escape_restore_focus() {
        let mut app = app();
        app.focus_search = false;
        for name in ["First", "Second"] {
            app.games.push(Game {
                dlss_anywhere: Vec::new(),
                registered: true,
                id: name.into(),
                name: name.into(),
                store: Store::Manual,
                install_dir: PathBuf::new(),
                runtime: Runtime::Unknown,
                targets: vec![],
            });
        }
        let mut harness = Harness::new_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.shell_shortcuts(ui.ctx());
                app.search_field(ui);
                app.library_keyboard(ui);
                app.review_sheet(ui.ctx());
            },
            app,
        );
        harness.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::F);
        harness.run_steps(3);
        assert!(
            harness
                .state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
        harness.get_by_role_and_label(egui::Role::TextInput, "Search games");
        harness.key_press(egui::Key::ArrowDown);
        harness.run_steps(3);
        assert_eq!(harness.state().selected.as_deref(), Some("First"));
        harness.key_press(egui::Key::ArrowDown);
        harness.run_steps(3);
        assert_eq!(harness.state().selected.as_deref(), Some("Second"));
        harness.state_mut().review_open = true;
        harness.state_mut().origin_focus = Some(egui::Id::unique("library_search"));
        harness.run_steps(3);
        harness.key_press(egui::Key::Escape);
        harness.run_steps(3);
        assert!(!harness.state().review_open);
        assert!(
            harness
                .state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
    }
}

impl RetoqueApp {
    fn tab_install_prompt(&mut self, ui: &mut egui::Ui) {
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("tab.install_first"));
            if ui.button(self.tr("tab.review_install")).clicked() {
                self.tab = Tab::Install;
                if let Some(inputs) = self.install_inputs() {
                    self.request_plan(PlanKind::Install, PlanInputs::Install(inputs));
                }
            }
        });
    }
}

#[derive(Clone, Default)]
struct ModelInspection {
    path: Option<PathBuf>,
    present: bool,
    version: Option<String>,
    compatibility: retoque_data::model_compat::ModelCompatibility,
}

type ModelInspectionTask = std::sync::Arc<std::sync::Mutex<Option<ModelInspection>>>;

fn model_inspection_id(target: &Path, configured: &str) -> egui::Id {
    egui::Id::unique(("model-inspection", target, configured))
}

// A relative ModelFile belongs to the proxy/executable folder, never to the app's working directory.
fn model_host_path(target: &Path, configured: &str) -> Option<PathBuf> {
    let value = configured.trim();
    if value.is_empty() || value.eq_ignore_ascii_case("auto") {
        Some(target.join("nvngx_dlssnr.dll"))
    } else if value.contains(':') || value.starts_with(['/', '\\']) {
        if value.contains(':') && !value.as_bytes().get(2).is_some_and(|b| matches!(b, b'/' | b'\\')) {
            return None;
        }
        host_path(value)
    } else {
        Some(target.join(value.replace('\\', "/")))
    }
}

fn inspect_model(target: &Path, configured: &str) -> ModelInspection {
    use std::io::Read;
    let path = model_host_path(target, configured);
    let present = path.as_ref().is_some_and(|p| p.is_file());
    let version = path.as_ref().filter(|_| present).and_then(|path| {
        let mut file = std::fs::File::open(path)
            .ok()?
            .take(retoque_data::model_compat::MAX_MODEL_BYTES);
        // The model may be hundreds of MB. Keep only a chunk and enough overlap for version records.
        let mut block = [0; 65536];
        let mut tail = Vec::new();
        let mut version = None;
        loop {
            let count = file.read(&mut block).ok()?;
            if count == 0 {
                break;
            }
            tail.extend_from_slice(&block[..count]);
            if tail.windows(8).any(|s| s == [0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0]) {
                version = retoque_data::models::pe_file_version(&tail);
            }
            if tail.len() > 15 {
                tail.drain(..tail.len() - 15);
            }
        }
        version
    });
    let compatibility = path
        .as_ref()
        .filter(|_| present)
        .and_then(|p| {
            let mut file = std::fs::File::open(p).ok()?;
            retoque_data::model_compat::inspect(&mut file).ok()
        })
        .unwrap_or_default();
    ModelInspection {
        path,
        present,
        version,
        compatibility,
    }
}

fn model_inspection(ctx: &egui::Context, target: &Path, configured: &str) -> Option<ModelInspection> {
    let id = model_inspection_id(target, configured);
    let target = target.to_owned();
    let configured = configured.to_owned();
    cached_tab_read(ctx, id, move || inspect_model(&target, &configured))
}

// Scope read-only work to the tab cache rather than adding cross-worker messages to the shared app shell.
fn cached_tab_read<T: Clone + Default + Send + Sync + 'static>(
    ctx: &egui::Context,
    id: egui::Id,
    read: impl FnOnce() -> T + Send + 'static,
) -> Option<T> {
    type Task<T> = std::sync::Arc<std::sync::Mutex<Option<T>>>;
    let existing = ctx.data(|data| data.get_temp::<Task<T>>(id));
    let task = existing.unwrap_or_else(|| {
        let task: Task<T> = Default::default();
        ctx.data_mut(|data| data.insert_temp(id, task.clone()));
        let result = task.clone();
        let repaint = ctx.clone();
        if std::thread::Builder::new()
            .name("retoque-tab-read".into())
            .spawn(move || {
                let value = std::panic::catch_unwind(std::panic::AssertUnwindSafe(read)).unwrap_or_default();
                *result.lock().unwrap_or_else(|e| e.into_inner()) = Some(value);
                repaint.request_repaint();
            })
            .is_err()
        {
            *task.lock().unwrap_or_else(|e| e.into_inner()) = Some(T::default());
        }
        task
    });
    task.lock().unwrap_or_else(|e| e.into_inner()).clone()
}

fn model_usage(ctx: &egui::Context, model: &Path, targets: &[PathBuf]) -> Option<usize> {
    // A short-lived snapshot also picks up edits made outside this window without scanning every frame.
    let epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
        / 30;
    let id = egui::Id::unique(("model-usage", model, targets, epoch));
    let model = model.to_owned();
    let targets = targets.to_vec();
    cached_tab_read(ctx, id, move || {
        let unique: std::collections::HashSet<_> = targets.into_iter().collect();
        Some(
            unique
                .iter()
                .filter(|target| {
                    let Ok(bytes) = std::fs::read(target.join("OptiScaler.ini")) else {
                        return false;
                    };
                    let ini = Ini::parse(&bytes);
                    model_host_path(target, ini.get("DlssNr", "ModelFile").unwrap_or("auto")).as_ref() == Some(&model)
                })
                .count(),
        )
    })
    .flatten()
}

impl RetoqueApp {
    fn model_import_area(&mut self, ui: &mut egui::Ui) {
        let pending_id = egui::Id::unique("model-import-pending");
        let frame = egui::Frame::new()
            .inner_margin(16.0)
            .corner_radius(12)
            .fill(theme::CARD);
        let area = frame.show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.label(RichText::new(self.tr("mdl.drop")).color(theme::MUTED));
            if ui.button(self.tr("mdl.import")).clicked()
                && let Some(path) = rfd::FileDialog::new()
                    .add_filter(self.tr("mdl.dll_filter"), &["dll"])
                    .pick_file()
            {
                ui.ctx().data_mut(|data| data.insert_temp(pending_id, path));
            }
        });
        let border = area.response.rect.shrink(1.0);
        let stroke = egui::Stroke::new(1.0, theme::MUTED);
        for (from, to) in [
            (border.left_top(), border.right_top()),
            (border.right_top(), border.right_bottom()),
            (border.right_bottom(), border.left_bottom()),
            (border.left_bottom(), border.left_top()),
        ] {
            let delta = to - from;
            let length = delta.length();
            let direction = delta / length.max(1.0);
            let mut offset = 8.0;
            while offset < length - 8.0 {
                ui.painter().line_segment(
                    [
                        from + direction * offset,
                        from + direction * (offset + 5.0).min(length - 8.0),
                    ],
                    stroke,
                );
                offset += 10.0;
            }
        }
        // Dropping previews an import, just like selecting a file; it does not silently copy anything.
        if ui
            .ctx()
            .pointer_latest_pos()
            .is_some_and(|p| area.response.rect.contains(p))
        {
            let dropped = ui
                .ctx()
                .input(|i| i.raw.dropped_files.first().map(|file| file.path().to_path_buf()));
            if let Some(path) = dropped {
                ui.ctx().data_mut(|data| data.insert_temp(pending_id, path));
            }
        }
        if let Some(path) = ui.ctx().data(|data| data.get_temp::<PathBuf>(pending_id)) {
            ui.label(self.tr("mdl.import_review"));
            ui.add(egui::Label::new(path.file_name().unwrap_or_default().to_string_lossy()).truncate());
            ui.horizontal(|ui| {
                if ui
                    .add_enabled(!self.busy(), theme::primary(self.tr("mdl.confirm_import")))
                    .clicked()
                {
                    ui.ctx().data_mut(|data| data.remove::<PathBuf>(pending_id));
                    self.jobs.spawn("import", None, move |_| model::catalog_import(path));
                }
                if ui.button(self.tr("cancel")).clicked() {
                    ui.ctx().data_mut(|data| data.remove::<PathBuf>(pending_id));
                }
            });
        }
        ui.add_space(12.0);
    }
}

fn log_verdict(lang: Lang, summary: &Summary) -> String {
    // Initialization and CPU coverage do not prove completed GPU execution or present activity.
    let key = if summary.coverage.as_ref().is_some_and(|c| c.model_ok > 0) || summary.timing_samples > 0 {
        "log.ran"
    } else if summary.nr_initialized {
        "log.initialized_only"
    } else {
        "log.no_evidence"
    };
    let mut verdict = t(lang, key).to_string();
    if let Some(api) = &summary.api {
        verdict.push_str(&format!(" {} {api}", t(lang, "log.on_api")));
    }
    if let Some(coverage) = &summary.coverage {
        let stage = match coverage.stage.as_str() {
            "pre" => Some("log.stage_pre"),
            "post" => Some("log.stage_post"),
            "present" => Some("log.stage_present"),
            _ => None,
        };
        if let Some(stage) = stage {
            verdict.push_str(&format!(" {}", t(lang, stage)));
        }
    }
    verdict.push_str(&format!(
        " · {} {}",
        summary.error_count.max(summary.errors.len()),
        t(lang, "log.error_count")
    ));
    verdict
}

// Group repeated problem text while preserving the original lines in Technical details.
fn log_message(line: &str) -> &str {
    if let Some((stamp, message)) = line.strip_prefix('[').and_then(|s| s.split_once(']'))
        && stamp.contains(':')
        && stamp
            .chars()
            .all(|c| c.is_ascii_digit() || matches!(c, ':' | '.' | '-' | ' '))
    {
        message.trim_start()
    } else {
        line
    }
}

#[cfg(test)]
mod tab_content_tests {
    use super::*;

    #[test]
    fn model_inspection_uses_current_file_and_never_falls_back_for_a_named_missing_file() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("nvngx_dlssnr.dll");
        // Cross a streaming chunk boundary with the version signature.
        let mut bytes = vec![0; 65532];
        bytes.extend([0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0]);
        bytes.extend(((310_u32 << 16) | 9).to_le_bytes());
        bytes.extend((16_u32 << 16).to_le_bytes());
        std::fs::write(&path, bytes).unwrap();
        let found = inspect_model(temp.path(), "auto");
        assert!(found.present);
        assert_eq!(found.version.as_deref(), Some("310.9.16.0"));
        assert!(!inspect_model(temp.path(), "missing.dll").present);
        std::fs::rename(&path, temp.path().join("renamed.dll")).unwrap();
        assert_eq!(inspect_model(temp.path(), "renamed.dll").version, found.version);
        assert!(!inspect_model(temp.path(), "auto").present);
        assert_eq!(model_host_path(temp.path(), "Z:relative.dll"), None);
    }

    #[test]
    fn log_verdict_does_not_promote_initialization_to_evaluation() {
        let mut summary = Summary {
            api: Some("Vulkan".into()),
            nr_initialized: true,
            ..Default::default()
        };
        assert!(log_verdict(Lang::En, &summary).starts_with("NR initialized; evaluation is not confirmed"));
        summary.coverage = Some(retoque_data::logs::Coverage {
            stage: "post".into(),
            model_ok: 3,
            ..Default::default()
        });
        assert_eq!(
            log_verdict(Lang::En, &summary),
            "NR evaluation recorded on Vulkan after the upscaler · 0 errors"
        );
        assert!(log_verdict(Lang::Pt, &summary).contains("depois da ampliação"));
    }

    #[cfg(not(windows))]
    #[test]
    fn boolean_control_can_restore_automatic_without_writing_an_ini() {
        use egui_kittest::{Harness, kittest::Queryable};
        let setting = settings::all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Enabled")
            .unwrap();
        let mut harness = Harness::new_ui_state(
            |ui, value| {
                value_widget(ui, setting, value, "bool-test", Lang::En);
            },
            "true".to_owned(),
        );
        // Native egui buttons stay in the tab order and activate with Space.
        harness.key_down(egui::Key::Tab);
        harness.run_steps(1);
        harness.key_up(egui::Key::Tab);
        harness.run_steps(1);
        harness.key_down(egui::Key::Space);
        harness.run_steps(1);
        harness.key_up(egui::Key::Space);
        harness.run_steps(1);
        assert_eq!(harness.state(), "auto");
        harness.get_by_label("Off").click_accesskit();
        harness.run();
        assert_eq!(harness.state(), "false");
        harness.get_by_label("Automatic").click_accesskit();
        harness.run();
        assert_eq!(harness.state(), "auto");
    }
}

impl RetoqueApp {
    fn model_compatibility_label(&self, ui: &mut egui::Ui, info: &retoque_data::model_compat::ModelCompatibility) {
        use retoque_data::model_compat::Compatibility;
        let gpu = self.tuning.gpus.as_ref().and_then(|g| g.first());
        let (key, color) = match info.for_gpu(gpu) {
            Compatibility::Incompatible => ("mdl.incompatible", theme::WARN),
            Compatibility::Compatible => ("mdl.arch_match", theme::MUTED),
            Compatibility::Unknown => ("mdl.compat_unknown", theme::WARN),
        };
        ui.add(egui::Label::new(RichText::new(self.tr(key)).color(color)).wrap());
        if !info.archs.is_empty() {
            let archs = info
                .archs
                .iter()
                .map(|sm| format!("sm_{sm}"))
                .collect::<Vec<_>>()
                .join(", ");
            ui.label(format!("{} {archs}", self.tr("mdl.archs")));
        }
    }
    pub(super) fn nr_compatibility_notice(&mut self, ui: &mut egui::Ui) {
        let Some(target) = &self.view.target else {
            return;
        };
        let configured = self
            .view
            .ini
            .as_ref()
            .and_then(|i| i.as_ref().ok())
            .and_then(|i| i.as_ref())
            .and_then(|i| i.get("DlssNr", "ModelFile"))
            .unwrap_or("auto");
        // While the model is still being read the caller already shows its own progress.
        let Some(info) = model_inspection(ui.ctx(), target, configured) else {
            return;
        };
        self.model_compatibility_label(ui, &info.compatibility);
        let models = self
            .catalog
            .as_ref()
            .and_then(|c| c.as_ref().ok())
            .map(Vec::as_slice)
            .unwrap_or(&[]);
        let (_, alternative) = retoque_data::recommend::nr_model(
            &info.compatibility,
            self.tuning.gpus.as_ref().and_then(|g| g.first()),
            models,
        );
        if let Some(model) = alternative {
            ui.label(format!("{} {}", self.tr("mdl.suggest"), model.label));
            let value = Catalog::model_file_value(model, cfg!(not(windows)));
            if ui
                .add_enabled(!self.busy(), egui::Button::new(self.tr("mdl.use_suggested")))
                .clicked()
            {
                self.request_plan(
                    PlanKind::Settings,
                    PlanInputs::Settings(vec![("DlssNr".into(), "ModelFile".into(), value)]),
                );
            }
        }
    }
}
