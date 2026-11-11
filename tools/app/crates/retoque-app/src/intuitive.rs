//! Small shell integration for navigation and reviewed undo. No I/O runs in draw callbacks.
use crate::{
    app::*,
    commands::PaletteAction,
    modals::palette::{Destination, Entry, Palette, TABS},
};

#[derive(Default)]
pub(crate) struct State {
    pub palette: Palette,
    pub settings: crate::state::settings::Filter,
    pub drop: crate::drop_import::DropState,
    // None in isolated GUI fixtures. The real app sets this to its own data directory.
    pub history_root: Option<PathBuf>,
    pub undo_request: Option<u64>,
    pub compound_history: HashMap<PathBuf, Vec<crate::history::Applied>>,
}
impl RetoqueApp {
    pub(crate) fn intuitive_shortcuts(&mut self, ctx: &egui::Context) -> bool {
        if self.review_open || self.folder_dialog.is_some() || self.intuitive.drop.open() {
            return true;
        }
        if ctx.input_mut(|i| {
            i.consume_key(egui::Modifiers::COMMAND, egui::Key::K)
                || i.consume_key(egui::Modifiers::COMMAND, egui::Key::P)
        }) {
            self.intuitive.palette.open(ctx);
        }
        if self.intuitive.palette.open {
            return true;
        }
        for (key, (tab, _, _)) in [
            egui::Key::Num1,
            egui::Key::Num2,
            egui::Key::Num3,
            egui::Key::Num4,
            egui::Key::Num5,
        ]
        .into_iter()
        .zip(TABS)
        {
            if ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, key)) {
                self.cmd_tx.send(AppCommand::SwitchTab(tab));
            }
        }
        if !ctx.egui_wants_keyboard_input() && ctx.input_mut(|i| i.consume_key(egui::Modifiers::NONE, egui::Key::Slash))
        {
            self.focus_current_search();
        }
        false
    }
    fn focus_current_search(&mut self) {
        if self.selected.is_some() && self.tab == Tab::Settings {
            self.intuitive.settings.focus = true;
        } else {
            self.focus_search = true;
            self.sidebar_visible = true;
        }
    }
    pub(crate) fn palette_sheet(&mut self, ctx: &egui::Context) {
        self.intuitive.palette.restore_focus(ctx);
        if !self.intuitive.palette.open {
            return;
        }
        let optimize = self.optimize_action_key();
        let key = (self.library_state.generation(), self.lang, optimize);
        if self.intuitive.palette.catalog_key != Some(key) {
            let mut entries: Vec<_> = self
                .library_state
                .games
                .iter()
                .map(|g| Entry {
                    label: format!("{} · {}", g.name, t(self.lang, "palette.game")),
                    search: String::new(),
                    destination: Destination::Game(g.id.clone()),
                })
                .collect();
            entries.extend(TABS.into_iter().map(|(tab, label, shortcut)| Entry {
                label: format!("{} · {shortcut}", t(self.lang, label)),
                search: String::new(),
                destination: Destination::Tab(tab),
            }));
            // A setting reads "{name} · Settings"; its INI name stays searchable without being shown.
            entries.extend(settings::all().iter().map(|s| Entry {
                label: format!(
                    "{} · {}",
                    crate::i18n::setting_label(self.lang, s),
                    t(self.lang, "tab.settings")
                ),
                search: format!("[{}] {}", s.section, s.key),
                destination: Destination::Setting(s.key),
            }));
            entries.extend(PaletteAction::ALL.into_iter().map(|action| {
                let (label, search) = if action == PaletteAction::Optimize {
                    // Found by any of the main action's names, whichever one the open game shows.
                    (t(self.lang, optimize), self.optimize_names())
                } else {
                    (t(self.lang, action.label_key()), String::new())
                };
                Entry {
                    label: label.into(),
                    search,
                    destination: Destination::Action(action),
                }
            }));
            entries.push(Entry {
                label: format!("{} · /", t(self.lang, "palette.focus")),
                search: String::new(),
                destination: Destination::FocusSearch,
            });
            self.intuitive.palette.entries = entries;
            self.intuitive.palette.catalog_key = Some(key);
            self.intuitive.palette.invalidate();
        }
        if let Some(destination) = self.intuitive.palette.show(ctx, self.lang) {
            match destination {
                Destination::Game(id) => self.select_id(id),
                Destination::Tab(tab) => self.cmd_tx.send(AppCommand::SwitchTab(tab)),
                Destination::Setting(key) => {
                    self.intuitive.settings.query = key.into();
                    self.intuitive.settings.only_changed = false;
                    self.intuitive.settings.focus = true;
                    self.cmd_tx.send(AppCommand::SwitchTab(Tab::Settings));
                }
                Destination::FocusSearch => self.focus_current_search(),
                Destination::Action(action) => match action {
                    PaletteAction::RescanLibrary => self.cmd_tx.send(AppCommand::RescanLibrary),
                    // The banner's action, with the same readiness: nothing opens before the facts are known.
                    PaletteAction::Optimize => {
                        if self.optimize_ready() {
                            self.optimize_automatically();
                        }
                    }
                    PaletteAction::ReviewUninstall => self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall),
                    PaletteAction::ReviewSettings => {
                        self.tab = Tab::Settings;
                        if !self.view.edits.is_empty()
                            && self.view.edits.iter().all(|((section, key), value)| {
                                settings::all()
                                    .iter()
                                    .find(|s| s.section == section && s.key == key)
                                    .is_some_and(|s| s.accepts(value))
                            })
                        {
                            self.request_plan(
                                PlanKind::Settings,
                                PlanInputs::Settings(
                                    self.view
                                        .edits
                                        .iter()
                                        .map(|((s, k), v)| (s.clone(), k.clone(), v.clone()))
                                        .collect(),
                                ),
                            );
                        }
                    }
                    PaletteAction::UndoSettings => {
                        if let Some(target) = self.view.target.clone() {
                            self.cmd_tx.send(AppCommand::UndoSettings(target));
                        }
                    }
                },
            }
        }
    }
    pub(crate) fn capture_compound_history(&mut self, target: &Path, plans: &[Plan]) {
        let entries: Vec<_> = plans
            .iter()
            .map(crate::history::capture)
            .filter(|p| !p.changes.is_empty())
            .collect();
        if !entries.is_empty() {
            self.intuitive.compound_history.insert(target.to_owned(), entries);
        }
    }
    pub(crate) fn finish_compound_history(&mut self, target: &Path, success: bool) {
        let Some(entries) = self.intuitive.compound_history.remove(target) else {
            return;
        };
        if !success {
            return;
        }
        let root = self.intuitive.history_root.clone();
        let target = target.to_owned();
        self.jobs.spawn("settings-history", None, move |_| {
            let result = root
                .ok_or_else(|| AppError::from("application data directory unavailable"))
                .and_then(|root| {
                    entries
                        .into_iter()
                        .try_for_each(|entry| crate::history::record(&root, &target, entry).map_err(AppError::from))
                });
            Outcome::HistoryWritten(result)
        });
    }
    pub(crate) fn start_undo(&mut self, target: PathBuf) {
        if self.view.target.as_ref() != Some(&target) || self.busy() {
            return;
        }
        self.review_open = true;
        self.origin_focus = self.context.memory(|m| m.focused());
        self.next_request += 1;
        let request = self.next_request;
        self.view.request = request;
        self.intuitive.undo_request = Some(request);
        self.view.inputs = Some(PlanInputs::Settings(Vec::new()));
        self.view.applied = None;
        self.review = None;
        let root = self.intuitive.history_root.clone();
        let lang = self.lang;
        self.jobs.spawn("plan", None, move |_| {
            let result = root
                .as_deref()
                .ok_or_else(|| AppError::info(t(lang, "undo.empty")))
                .and_then(|root| crate::history::undo_plan(root, &target, lang));
            let groups = result
                .as_ref()
                .map(|p| p.steps().iter().map(|_| 4).collect())
                .unwrap_or_default();
            Outcome::Plan {
                target,
                kind: PlanKind::Settings,
                request,
                result,
                groups,
            }
        });
    }
}
