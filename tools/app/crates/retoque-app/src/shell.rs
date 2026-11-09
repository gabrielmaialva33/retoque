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
                ui.colored_label(theme::BAD, format!("{}: {message}", self.tr("error")));
                crate::error::buttons(ui, &message, self.lang, &self.cmd_tx);
                if ui
                    .small_button(self.tr("dismiss"))
                    .on_hover_text(self.tr("dismiss.help"))
                    .clicked()
                {
                    self.error = None;
                }
                return;
            }
            self.payload_bar(ui);
        });
        self.payload_dialog(ui.ctx());
    }

    pub(crate) fn library(&mut self, ui: &mut egui::Ui) {
        ui.add_space(6.0);
        self.search_field(ui);
        ui.horizontal(|ui| {
            ui.checkbox(
                &mut self.library_state.installed_only,
                t(self.lang, "lib.installed_only"),
            );
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.small_button(self.tr("lib.add_folder")).clicked() {
                    self.folders_open(FolderDialog::Add);
                }
            });
        });
        self.folders_button(ui);
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

    pub(crate) fn game(&mut self, ui: &mut egui::Ui) {
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
            });
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
            ui.checkbox(
                &mut self.library_state.installed_only,
                t(self.lang, "lib.installed_only"),
            );
            if ui.button(self.tr("lib.add_folder")).clicked() {
                self.folders_open(FolderDialog::Add);
            }
            self.folders_button(ui);
        });
        if self.library_state.games.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr(if self.scanning { "lib.scanning" } else { "lib.empty" }));
                ui.label(self.tr("shell.home_help"));
            });
            return;
        }
        self.refresh_library_cache();
        let mut clicked = None;
        egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
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
                    ui.spacing_mut().item_spacing = egui::vec2(20.0, 20.0);
                    for &index in games {
                        let game = &self.library_state.games[index];
                        let state = self.library_state.derived.states[index];
                        let art = self.library_state.art.get(&game.id).and_then(|a| a.capsule.as_deref());
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
