use crate::app::*;

impl RetoqueApp {
    pub(crate) fn settings_tab(
        &mut self,
        ui: &mut egui::Ui,
        edits: &mut BTreeMap<(String, String), String>,
        commands: &CommandSender,
    ) {
        if self.view.target.is_none() {
            return;
        }
        match &self.view.ini {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(self.tr("tab.loading"));
                });
                return;
            }
            Some(Err(e)) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
                return;
            }
            Some(Ok(None)) => {
                self.tab_install_prompt(ui);
                return;
            }
            Some(Ok(Some(_))) => (),
        };
        ui.horizontal_wrapped(|ui| {
            let label = ui.label(t(self.lang, "set.filter"));
            let response = ui
                .add(
                    egui::TextEdit::singleline(&mut self.intuitive.settings.query)
                        .id(egui::Id::unique("settings_filter")),
                )
                .labelled_by(label.id)
                .on_hover_text("/");
            if self.intuitive.settings.focus {
                response.request_focus();
                self.intuitive.settings.focus = false;
            }
            ui.checkbox(
                &mut self.intuitive.settings.only_changed,
                t(self.lang, "set.only_changed"),
            );
            if ui
                .add_enabled(!self.busy(), egui::Button::new(t(self.lang, "undo.title")))
                .clicked()
                && let Some(target) = &self.view.target
            {
                commands.send(AppCommand::UndoSettings(target.clone()));
            }
        });
        ui.label(
            RichText::new(t(self.lang, "undo.auto_note"))
                .small()
                .color(theme::MUTED),
        );
        self.intuitive.settings.refresh(self.lang);
        let mut any = false;
        for (section, title) in [
            ("DlssNr", "set.nr"),
            ("Upscalers", "set.upscalers"),
            ("FrameGen", "set.framegen"),
            ("LSFG", "set.lsfg"),
        ] {
            // While filtering, a section with nothing to show is left out instead of drawn as an empty card.
            if self.intuitive.settings.active() && !self.settings_section_matches(section, edits) {
                continue;
            }
            any = true;
            widgets::card(ui, |ui| {
                if section == "LSFG" && !self.intuitive.settings.active() {
                    egui::CollapsingHeader::new(RichText::new(self.tr(title)).size(16.0).strong())
                        .id_salt("lsfg_settings")
                        .default_open(false)
                        .show(ui, |ui| self.settings_section(ui, section, edits, commands));
                    ui.label(RichText::new(self.tr("set.lsfg_note")).small().color(theme::MUTED));
                } else {
                    widgets::card_title(ui, self.tr(title));
                    self.settings_section(ui, section, edits, commands);
                }
            });
            ui.add_space(12.0);
        }
        if !any {
            ui.label(RichText::new(self.tr("set.no_match")).color(theme::MUTED));
        }
        // Review and Discard live in the pending-changes footer (pending_footer), outside the scroll area.
        self.settings_plan(ui);
    }

    fn settings_section_matches(&self, section: &str, edits: &BTreeMap<(String, String), String>) -> bool {
        settings::all()
            .iter()
            .enumerate()
            .filter(|(_, s)| s.section == section)
            .any(|(index, s)| {
                let key = (s.section.to_owned(), s.key.to_owned());
                let on_disk = self.view.ini_values.get(s.section, s.key).unwrap_or("auto");
                self.intuitive
                    .settings
                    .includes(index, on_disk, edits.get(&key).map(String::as_str))
            })
    }

    pub(crate) fn settings_section(
        &self,
        ui: &mut egui::Ui,
        section: &str,
        edits: &mut BTreeMap<(String, String), String>,
        commands: &CommandSender,
    ) {
        self.settings_rows(ui, section, false, edits, commands);
        if section == "DlssNr" {
            ui.add_space(8.0);
            if self.intuitive.settings.active() {
                self.settings_rows(ui, section, true, edits, commands);
            } else {
                egui::CollapsingHeader::new(self.tr("set.advanced"))
                    .id_salt("nr_advanced")
                    .show(ui, |ui| self.settings_rows(ui, section, true, edits, commands));
            }
        }
    }

    pub(crate) fn settings_rows(
        &self,
        ui: &mut egui::Ui,
        section: &str,
        advanced: bool,
        edits: &mut BTreeMap<(String, String), String>,
        commands: &CommandSender,
    ) {
        let lang = self.lang;
        let mut shown = 0;
        for (index, s) in settings::all().iter().enumerate().filter(|(_, s)| s.section == section) {
            let key = (s.section.to_owned(), s.key.to_owned());
            let on_disk = self.view.ini_values.get(s.section, s.key).unwrap_or("auto");
            if !self
                .intuitive
                .settings
                .includes(index, on_disk, edits.get(&key).map(String::as_str))
            {
                continue;
            }
            if section == "DlssNr" {
                let common = matches!(s.key, "Enabled" | "Stage" | "WorkingScale" | "ApplyModel" | "ModelFile");
                if common == advanced {
                    continue;
                }
            }
            let compare = edits
                .get(&("DlssNr".into(), "Compare".into()))
                .map(String::as_str)
                .or_else(|| self.view.ini_values.get("DlssNr", "Compare"))
                .unwrap_or("auto");
            if s.section == "DlssNr"
                && ((s.key == "CompareSplit" && compare != "2")
                    || (s.key == "CompareSwap" && !matches!(compare, "1" | "2")))
            {
                continue;
            }
            // A rule between rows, never under the last one.
            if shown > 0 {
                ui.separator();
            }
            shown += 1;
            if s.key == "ModelFile" {
                ui.add_space(8.0);
                if ui.button(t(lang, "set.model_link")).clicked() {
                    commands.send(AppCommand::SwitchTab(Tab::Model));
                }
            }
            let (label, help) = match lang {
                Lang::En => (s.label_en, s.help_en),
                Lang::Pt => (s.label_pt, s.help_pt),
            };
            let key = (s.section.to_owned(), s.key.to_owned());
            let present = self.view.ini_values.get(s.section, s.key);
            let on_disk = present.unwrap_or("auto");
            let mut value = edits.get(&key).cloned().unwrap_or_else(|| on_disk.into());
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
                            ui.label(
                                RichText::new(t(
                                    lang,
                                    if on_disk.eq_ignore_ascii_case("auto") {
                                        "set.source_auto"
                                    } else {
                                        "set.source_ini"
                                    },
                                ))
                                .small()
                                .color(theme::MUTED),
                            );
                            if edits.contains_key(&key) {
                                ui.label(RichText::new(t(lang, "set.source_draft")).small().color(theme::WARN));
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
                        ui.label_inputs_by(label_id, |ui| {
                            value_widget(ui, s, &mut value, &format!("{}.{}", s.section, s.key), lang)
                                .labelled_by(label_id);
                        });
                    },
                );
            });
            ui.push_id((s.section, s.key, "reset"), |ui| {
                if ui
                    .add_enabled(
                        !value.eq_ignore_ascii_case("auto"),
                        egui::Button::new(t(lang, "set.reset")),
                    )
                    .on_hover_text(format!("[{}] {}", s.section, s.key))
                    .clicked()
                {
                    value = "auto".into();
                }
            });
            // A value OptiScaler would not accept is kept as typed (so the user can fix it) but blocks the review.
            if edits.get(&key).is_some_and(|v| !s.accepts(v)) {
                ui.colored_label(theme::BAD, RichText::new(t(lang, "set.invalid")).small());
            }
            if value != before {
                if value == on_disk {
                    edits.remove(&key);
                } else {
                    edits.insert(key, value);
                }
            }
            ui.add_space(6.0);
        }
    }

    pub(crate) fn settings_plan(&self, ui: &mut egui::Ui) {
        match &self.view.applied {
            Some((PlanKind::Settings, Ok(()))) => {
                ui.colored_label(theme::GOOD, self.tr("set.saved"));
            }
            Some((PlanKind::Settings, Err(e))) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
            }
            _ => {}
        }
    }
}
pub(crate) fn value_widget(
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

pub(crate) fn choice_label<'a>(setting: &settings::Setting, value: &'a str, lang: Lang) -> &'a str {
    match setting.choice_labels(value) {
        Some((en, pt)) => match lang {
            Lang::En => en,
            Lang::Pt => pt,
        },
        None => t(lang, "set.custom"),
    }
}
