use crate::app::*;

impl RetoqueApp {
    pub(crate) fn top_bar(&mut self, ui: &mut egui::Ui) {
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
            if self.selected.is_some() && ui.button(self.tr("shell.library")).clicked() {
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
                    })
                    .response
                    .widget_info(|| {
                        egui::WidgetInfo::labeled(egui::Role::ComboBox, true, t(self.lang, "palette.language"))
                    });
                if ui
                    .add_enabled(!self.scanning, egui::Button::new(self.tr("app.refresh")))
                    .clicked()
                {
                    self.cmd_tx.send(AppCommand::RescanLibrary);
                    let t = self.view.target.clone();
                    if let Some(t) = t {
                        self.load_target(t);
                    }
                }
                if ui
                    .button(self.tr("palette.title"))
                    .on_hover_text("Ctrl+K · Ctrl+P")
                    .clicked()
                {
                    self.intuitive.palette.open(ui.ctx());
                }
                if self.jobs.busy() || self.library_pending {
                    ui.spinner();
                }
            });
        });
        self.autotune_gpu(ui);
    }

    pub(crate) fn status_bar(&mut self, ui: &mut egui::Ui) {
        ui.horizontal(|ui| {
            if let Some(message) = self.error.clone() {
                crate::applog::shown_error(&message.to_string());
                ui.colored_label(theme::BAD, format!("{}: {message}", self.tr("error")));
                crate::error::buttons(ui, &message, self.lang, &self.cmd_tx);
                if widgets::close_button(ui, self.tr("dismiss.help")).clicked() {
                    self.error = None;
                }
                return;
            }
            self.payload_bar(ui);
        });
        self.payload_dialog(ui.ctx());
    }

    pub(crate) fn library(&mut self, ui: &mut egui::Ui) {
        self.migrate_saved_folders();
        ui.add_space(6.0);
        self.search_field(ui);
        ui.horizontal(|ui| {
            ui.checkbox(
                &mut self.library_state.installed_only,
                t(self.lang, "lib.installed_only"),
            );
            // One row in the narrow sidebar; adding a folder is one step inside "Your folders".
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                self.folders_button(ui);
            });
        });
        self.migration_notice_ui(ui);
        ui.separator();
        if self.scanning && self.library_state.games.is_empty() {
            ui.horizontal(|ui| {
                ui.spinner();
                ui.label(self.tr("lib.scanning"));
            });
            return;
        }
        if self.library_state.games.is_empty() {
            ui.label(self.tr("lib.empty"));
        }
        self.library_keyboard(ui);
        self.refresh_library_cache();
        let mut clicked = None;
        let mut list_focus = false;
        let installed = &self.library_state.derived.installed;
        let others = &self.library_state.derived.others;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
            let groups = [
                (self.tr("lib.group_installed"), installed),
                (self.tr("lib.group_other"), others),
            ];
            for (title, games) in groups {
                if games.is_empty() {
                    continue;
                }
                widgets::group_label(ui, title, games.len());
                for &index in games.iter() {
                    let game = &self.library_state.games[index];
                    let selected = self.selected.as_ref() == Some(&game.id);
                    let state = (self.library_state.derived.states[index] != InstallState::Missing)
                        .then(|| self.tr("lib.state_installed"));
                    let detail = &self.library_state.derived.details[index];
                    let capsule = self.library_state.art.get(&game.id).and_then(|a| a.capsule.as_deref());
                    let hint = (!game.registered).then(|| self.tr("folders.unregistered"));
                    let response = widgets::game_row(ui, &game.name, detail, state, capsule, selected, hint);
                    // Cached Doctor findings for any of the game's folders; looked up by reference, no per-frame work.
                    let findings = game
                        .targets
                        .iter()
                        .filter_map(|t| self.doctor_badges.get(&t.path))
                        .copied()
                        .max()
                        .unwrap_or(0);
                    let response = crate::tabs::doctor::row_badge(ui, response, findings, self.lang);
                    list_focus |= response.has_focus();
                    if response.clicked() {
                        response.request_focus();
                        clicked = Some(game.id.clone());
                    }
                }
            }
            self.skipped_note(ui);
        });
        self.library_has_focus = list_focus;
        if let Some(id) = clicked {
            self.select_id(id);
        }
    }

    pub(crate) fn skipped_note(&self, ui: &mut egui::Ui) {
        if self.skipped.is_empty() {
            return;
        }
        ui.add_space(8.0);
        let header = format!("{} ({})", self.tr("lib.skipped"), self.skipped.len());
        egui::CollapsingHeader::new(RichText::new(header).small().color(theme::MUTED))
            .id_salt("skipped_entries")
            .default_open(false)
            .show(ui, |ui| {
                for entry in &self.skipped {
                    ui.label(RichText::new(entry).small().color(theme::MUTED));
                }
            });
    }

    pub(crate) fn game(&mut self, ui: &mut egui::Ui) {
        let Some(game) = self.selected_game().cloned() else {
            self.home_grid(ui);
            return;
        };
        self.game_banner(ui, &game);
        if self.tab != Tab::Compare {
            self.feature_tiles(ui);
        }
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
        // Each tab keeps its own scroll state, and its content never grows past the page column: one wide line on
        // one tab used to widen the shared content and push every later card past the others' right edge.
        let column = ui.available_width();
        egui::ScrollArea::vertical()
            .id_salt(("tab_scroll", self.tab))
            .auto_shrink([false, false])
            .show(ui, |ui| {
                ui.set_max_width(column - ui.spacing().scroll.allocated_width());
                self.tab_body(ui);
            });
    }

    fn tab_body(&mut self, ui: &mut egui::Ui) {
        match self.tab {
            Tab::Install => self.install_tab(ui),
            Tab::Settings => {
                let mut edits = std::mem::take(&mut self.view.edits);
                let commands = self.cmd_tx.clone();
                self.settings_tab(ui, &mut edits, &commands);
                self.view.edits = edits;
            }
            Tab::Model => {
                self.autotune_components(ui);
                self.model_tab(ui, &self.cmd_tx);
            }
            Tab::Compare => {}
            Tab::Logs => self.logs_tab(ui, &self.cmd_tx),
        }
    }

    pub(crate) fn target_picker(&mut self, ui: &mut egui::Ui, game: &Game) {
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
                .on_hover_text(crate::format::display_path(&game.targets[0].path));
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

    pub(crate) fn search_field(&mut self, ui: &mut egui::Ui) {
        let response = ui.add(
            egui::TextEdit::singleline(&mut self.library_state.search)
                .id(egui::Id::unique("library_search"))
                .hint_text(t(self.lang, "lib.search"))
                .desired_width(f32::INFINITY),
        );
        response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::TextInput, true, t(self.lang, "lib.search")));
        response.clone().on_hover_text("/ · Ctrl+F");
        if self.focus_search {
            response.request_focus();
            self.focus_search = false;
        }
    }

    pub(crate) fn library_keyboard(&mut self, ui: &egui::Ui) {
        if self.review_open || self.folder_dialog.is_some() || self.intuitive.palette.open || self.intuitive.drop.open()
        {
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
        self.refresh_library_cache();
        let games = &self.library_state.derived;
        let count = games.installed.len() + games.others.len();
        if count == 0 {
            return;
        }
        let index = self.selected.as_ref().and_then(|id| {
            games
                .installed
                .iter()
                .chain(&games.others)
                .position(|&i| &self.library_state.games[i].id == id)
        });
        let next = match index {
            Some(index) => (index as isize + direction).clamp(0, count as isize - 1) as usize,
            None => {
                if direction > 0 {
                    0
                } else {
                    count - 1
                }
            }
        };
        let index = *games.installed.iter().chain(&games.others).nth(next).unwrap();
        self.select_id(self.library_state.games[index].id.clone());
    }

    pub(crate) fn refresh_library_cache(&mut self) {
        let payload = self
            .payload_build
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .map(String::as_str);
        self.library_state.refresh(&self.saved.targets, payload, self.lang);
    }

    pub(crate) fn home_grid(&mut self, ui: &mut egui::Ui) {
        self.migrate_saved_folders();
        ui.heading(self.tr("welcome.title"));
        self.refresh_library_cache();
        let installed = self.library_state.derived.installed_count;
        let updates = self.library_state.derived.updates;
        ui.label(
            RichText::new(format!(
                "{} {} · {} {} · {} {}",
                self.library_state.games.len(),
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
        ui.add_space(12.0);
        self.first_run_nr_card(ui);
        // Search and its filters on one line: the field takes what the controls on its right leave.
        ui.horizontal(|ui| {
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                self.folders_button(ui);
                if ui.button(self.tr("lib.add_folder")).clicked() {
                    self.folders_open(FolderDialog::Add);
                }
                ui.checkbox(
                    &mut self.library_state.installed_only,
                    t(self.lang, "lib.installed_only"),
                );
                ui.add_space(6.0);
                self.search_field(ui);
            });
        });
        self.library_keyboard(ui);
        ui.add_space(6.0);
        self.migration_notice_ui(ui);
        if self.library_state.games.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr(if self.scanning { "lib.scanning" } else { "lib.empty" }));
                ui.label(self.tr("shell.home_help"));
            });
            self.skipped_note(ui);
            return;
        }
        self.refresh_library_cache();
        let mut clicked = None;
        // A new scan reorders and resizes the grid; an offset kept from before would open it cut off at the top.
        let mut scroll = egui::ScrollArea::vertical().auto_shrink([false, false]);
        if std::mem::take(&mut self.library_state.scroll_to_top) {
            scroll = scroll.vertical_scroll_offset(0.0);
        }
        scroll.show(ui, |ui| {
            for installed_group in [true, false] {
                if !installed_group && self.library_state.installed_only {
                    continue;
                }
                let games = if installed_group {
                    &self.library_state.derived.installed
                } else {
                    &self.library_state.derived.others
                };
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
                    ui.spacing_mut().item_spacing = egui::vec2(18.0, 22.0);
                    // As many covers as fit at 150 points, widened together so each row ends at the column's edge.
                    let cover_w = widgets::row_width(usize::MAX, ui.available_width(), 150.0, 18.0).min(210.0);
                    for &index in games {
                        let game = &self.library_state.games[index];
                        let state = self.library_state.derived.states[index];
                        let art = self.library_state.art.get(&game.id).and_then(|a| a.capsule.as_deref());
                        if widgets::cover_card(
                            ui,
                            cover_w,
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
            self.leftovers_card(ui);
            self.skipped_note(ui);
        });
        self.leftovers_modal(ui.ctx());
        if let Some(id) = clicked {
            self.select_id(id);
        }
    }

    pub(crate) fn game_banner(&mut self, ui: &mut egui::Ui, game: &Game) {
        let art = self.library_state.art.get(&game.id).cloned().unwrap_or_default();
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
        if compact {
            ui.painter().rect_filled(rect, 12, Color32::from_black_alpha(200));
        } else {
            widgets::hero_shade(ui.painter(), rect, 12.0);
        }
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
                    .fit_to_exact_size(egui::vec2(300.0, 96.0))
                    .maintain_aspect_ratio(true),
            );
        } else {
            child.label(RichText::new(&game.name).size(28.0).strong());
            child.add_space(22.0);
        }
        self.status_chips(&mut child, !compact);
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
            let ready = self.optimize_ready();
            child.horizontal_wrapped(|ui| {
                // One big action, like a GPU panel: a game without Retoque (or with an old script's install) gets
                // "Install and optimize", which opens one review for Retoque plus everything this card and GPU need.
                match state {
                    Some(InstallState::Partial) => {
                        if ui
                            .add(theme::primary(self.tr("shell.resume")))
                            .on_hover_text(self.tr("shell.review_shortcut"))
                            .clicked()
                        {
                            self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
                        }
                    }
                    Some(InstallState::Missing | InstallState::Untracked) | None => {
                        let label = if state == Some(InstallState::Untracked) {
                            "shell.register_optimize"
                        } else {
                            "shell.install_optimize"
                        };
                        let response = ui
                            .add_enabled(
                                state.is_some() && self.payload.is_some() && ready,
                                theme::primary(self.tr(label)).min_size(egui::vec2(190.0, 38.0)),
                            )
                            .on_hover_text(self.tr("shell.install_optimize_hint"))
                            .on_disabled_hover_text(self.tr(if self.payload.is_none() {
                                "shell.no_payload"
                            } else {
                                "shell.checking"
                            }));
                        if response.clicked() {
                            self.optimize_automatically();
                        }
                        if ui
                            .add_enabled(
                                state.is_some() && self.payload.is_some() && !self.busy(),
                                egui::Button::new(self.tr("shell.install_only")),
                            )
                            .clicked()
                            && let Some(options) = self.install_inputs()
                        {
                            self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                        }
                    }
                    Some(InstallState::Update) => {
                        if ui
                            .add_enabled(self.payload.is_some(), theme::primary(self.tr("shell.update")))
                            .on_hover_text(self.tr("shell.review_shortcut"))
                            .clicked()
                            && let Some(options) = self.install_inputs()
                        {
                            self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                        }
                        if ui
                            .add_enabled(ready, egui::Button::new(self.tr("shell.optimize")))
                            .clicked()
                        {
                            self.optimize_automatically();
                        }
                    }
                    Some(InstallState::Current | InstallState::Installed) => {
                        if ui
                            .add_enabled(ready, theme::primary(self.tr("shell.optimize")))
                            .on_hover_text(self.tr("shell.optimize_hint"))
                            .clicked()
                        {
                            self.optimize_automatically();
                        }
                        if ui.button(self.tr("shell.reinstall")).clicked()
                            && let Some(options) = self.install_inputs()
                        {
                            self.request_plan(PlanKind::Install, PlanInputs::Install(options));
                        }
                    }
                }
                let download = if self.nr_fetch.busy {
                    Some(("nr_fetch.downloading", self.nr_fetch.downloaded))
                } else if self.tuning.sm86.busy {
                    Some(("sm86.progress", self.tuning.sm86.downloaded))
                } else {
                    self.tuning
                        .dlss
                        .download_progress()
                        .map(|bytes| ("dlss.downloading", bytes))
                };
                if let Some((label, bytes)) = download {
                    ui.spinner();
                    let text = if bytes > 0 {
                        format!("{} · {:.0} MiB", self.tr(label), bytes as f64 / 1048576.)
                    } else {
                        self.tr(label).to_string()
                    };
                    ui.label(RichText::new(text).small());
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

    pub(crate) fn pending_footer(&mut self, ui: &mut egui::Ui) {
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
                    self.review = None;
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

    pub(crate) fn show_toasts(&mut self, ctx: &egui::Context) {
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
                        // The message and its close control share the first line; Details, when there are any,
                        // is an ordinary button under it.
                        ui.horizontal(|ui| {
                            let color = if toast.failed { theme::BAD } else { theme::GOOD };
                            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                                if widgets::close_button(ui, t(lang, "dismiss.help")).clicked() {
                                    remove = Some(i);
                                }
                                ui.with_layout(egui::Layout::left_to_right(egui::Align::Center), |ui| {
                                    ui.add(egui::Label::new(RichText::new(&toast.message).color(color)).wrap());
                                });
                            });
                        });
                        if toast.detail.is_some() && ui.button(t(lang, "shell.details")).clicked() {
                            toast.details_open = !toast.details_open;
                        }
                        if let Some(detail) = &toast.detail {
                            crate::error::buttons(ui, detail, lang, &self.cmd_tx);
                        }
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
