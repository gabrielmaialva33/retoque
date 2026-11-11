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
                    widgets::collapsible(ui, "lsfg_settings", self.tr(title), widgets::Fold::Section, |ui| {
                        self.settings_section(ui, section, edits, commands)
                    });
                    ui.label(RichText::new(self.tr("set.lsfg_note")).small().color(theme::MUTED));
                } else {
                    widgets::card_title(ui, self.tr(title));
                    self.settings_section(ui, section, edits, commands);
                }
            });
            ui.add_space(theme::CARD_GAP);
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
                widgets::collapsible(
                    ui,
                    "nr_advanced",
                    self.tr("set.advanced"),
                    widgets::Fold::Section,
                    |ui| self.settings_rows(ui, section, true, edits, commands),
                );
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
            let (label, help) = crate::i18n::setting_text(lang, s);
            let key = (s.section.to_owned(), s.key.to_owned());
            let present = self.view.ini_values.get(s.section, s.key);
            let on_disk = present.unwrap_or("auto");
            let mut value = edits.get(&key).cloned().unwrap_or_else(|| on_disk.into());
            let before = value.clone();
            ui.add_space(8.0);
            let width = ui.available_width();
            let control_width = (width * 0.50).clamp(220.0, 300.0).min(width);
            let label_width = (width - control_width - 20.0).max(40.0);
            let row = ui.horizontal_top(|ui| {
                let label_id = ui
                    .allocate_ui_with_layout(
                        egui::vec2(label_width, 0.0),
                        egui::Layout::top_down(egui::Align::Min),
                        |ui| {
                            ui.set_width(label_width);
                            let response = ui.add(egui::Label::new(RichText::new(label).strong()).wrap());
                            ui.label(RichText::new(help).small().color(theme::MUTED));
                            response.id
                        },
                    )
                    .inner;
                ui.allocate_ui_with_layout(
                    egui::vec2(control_width, 0.0),
                    egui::Layout::right_to_left(egui::Align::Min),
                    |ui| {
                        ui.set_width(control_width);
                        // The reset sits at the far right and keeps its room when hidden, so the controls line up.
                        if value.eq_ignore_ascii_case("auto") {
                            ui.add_space(widgets::RESET_SIZE + ui.spacing().item_spacing.x);
                        } else if ui
                            .push_id((s.section, s.key, "reset"), |ui| {
                                widgets::reset_button(ui, &t(lang, "set.reset").replace("{label}", label))
                            })
                            .inner
                            .clicked()
                        {
                            value = "auto".into();
                        }
                        ui.label_inputs_by(label_id, |ui| {
                            value_widget(ui, s, &mut value, &format!("{}.{}", s.section, s.key), lang)
                                .labelled_by(label_id);
                        });
                    },
                );
            });
            // A draft not yet reviewed: an accent bar in the card's margin, with the explanation on hover.
            if edits.contains_key(&key) {
                widgets::change_bar(ui, (s.section, s.key), row.response.rect, t(lang, "set.source_draft"));
            }
            // A value OptiScaler would not accept is kept as typed (so the user can fix it) but blocks the review.
            if edits.get(&key).is_some_and(|v| !s.accepts(v)) {
                ui.colored_label(theme::BAD, RichText::new(t(lang, "set.invalid")).small());
            }
            if (s.section, s.key) == ("DlssNr", "Passes") {
                self.passes_notes(ui, &value);
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

    /// Under the pass count: what the chosen count costs (as the in-game menu says it, from two passes up), and on an
    /// RTX 30 card that its video memory budget may refuse the later passes (bounded-chain.md, "Ten passes").
    fn passes_notes(&self, ui: &mut egui::Ui, value: &str) {
        if let Some(cost) = passes_cost(self.lang, value) {
            ui.add(egui::Label::new(RichText::new(cost).small().color(theme::MUTED)).wrap());
        }
        let gpu = self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
        if gpu.is_some_and(|g| g.arch == retoque_data::gpu::Arch::Ampere) {
            ui.add(egui::Label::new(RichText::new(self.tr("set.passes_ampere")).small().color(theme::MUTED)).wrap());
        }
    }

    pub(crate) fn settings_plan(&self, ui: &mut egui::Ui) {
        // A success is told by the toast alone; only a failure stays on the page.
        if let Some((PlanKind::Settings, Err(e))) = &self.view.applied {
            crate::error::card(ui, e, self.lang, &self.cmd_tx);
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
                            egui::Button::selectable(value.eq_ignore_ascii_case(option), RichText::new(label).small())
                                .wrap_mode(egui::TextWrapMode::Extend),
                        );
                        if response.clicked() {
                            *value = option.into();
                        }
                        response.on_hover_text(format!("{}: {label}", crate::i18n::setting_label(lang, setting)))
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
        // As Float: Automatic is a toggle beside the number, never "auto" typed into it.
        Kind::Int { min, max } => {
            ui.horizontal(|ui| {
                let automatic = value.eq_ignore_ascii_case("auto");
                let toggle = ui.add(egui::Button::selectable(automatic, t(lang, "set.automatic")));
                if toggle.clicked() {
                    *value = if automatic {
                        1_i64.clamp(*min, *max).to_string()
                    } else {
                        "auto".into()
                    };
                }
                if automatic {
                    return toggle;
                }
                match value.trim().parse::<i64>() {
                    Ok(mut i) => {
                        let response = ui.add(egui::DragValue::new(&mut i).range(*min..=*max));
                        if response.changed() {
                            *value = i.to_string();
                        }
                        toggle.union(response)
                    }
                    Err(_) => toggle.union(ui.add(egui::TextEdit::singleline(value).desired_width(70.0))),
                }
            })
            .inner
        }
        Kind::Text => ui.add(egui::TextEdit::singleline(value).desired_width(ui.available_width())),
    };
    response.on_hover_text(raw)
}

/// The cost line for a pass count: None for one pass (the plain model), Automatic, or a value OptiScaler would not
/// accept (that row already says so).
pub(crate) fn passes_cost(lang: Lang, value: &str) -> Option<String> {
    let setting = settings::all()
        .iter()
        .find(|s| (s.section, s.key) == ("DlssNr", "Passes"))?;
    let count = value.trim().parse::<i64>().ok()?;
    (setting.accepts(value) && count > 1).then(|| t(lang, "set.passes_cost").replace("{n}", &count.to_string()))
}

pub(crate) fn choice_label<'a>(setting: &settings::Setting, value: &'a str, lang: Lang) -> &'a str {
    match setting.choice_labels(value) {
        Some((en, pt)) => crate::i18n::choice_text(lang, en, pt),
        None => t(lang, "set.custom"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};

    #[test]
    fn the_pass_cost_follows_the_value() {
        for lang in [Lang::En, Lang::Pt] {
            for n in 2..=10 {
                let cost = passes_cost(lang, &n.to_string()).unwrap();
                assert_eq!(cost, t(lang, "set.passes_cost").replace("{n}", &n.to_string()));
                assert_eq!(cost.matches(&format!("{n}")).count(), 2, "{cost}");
            }
            // One pass is the plain model; invalid values already say so on their row.
            for value in ["auto", "1", "0", "11", "abc", ""] {
                assert_eq!(passes_cost(lang, value), None, "{value}");
            }
        }
    }

    #[test]
    fn passes_round_trip_through_the_reviewed_settings_plan() {
        let temp = tempfile::tempdir().unwrap();
        std::fs::write(temp.path().join("OptiScaler.ini"), "[DlssNr]\nPasses=auto\n").unwrap();
        for n in (1..=10).map(|n| n.to_string()).chain(["auto".to_owned()]) {
            let edits = vec![("DlssNr".to_owned(), "Passes".to_owned(), n.clone())];
            let plan = retoque_core::plan_settings(temp.path(), &edits).unwrap();
            retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| ()).unwrap();
            let (_, bytes) = retoque_core::read_ini(temp.path()).unwrap().unwrap();
            let values = crate::state::settings::IniValues::read(&Ini::parse(&bytes));
            assert_eq!(values.get("DlssNr", "Passes"), Some(n.as_str()));
        }
    }

    fn settings_with(passes: &str, ampere: bool) -> Harness<'static, RetoqueApp> {
        let text = format!("[DlssNr]\nPasses={passes}\n");
        let ini = Ini::parse(text.as_bytes());
        let mut app = crate::app_tests::shell_tests::app();
        app.view.target = Some("/synthetic/game".into());
        app.view.ini_values = crate::state::settings::IniValues::read(&ini);
        app.view.ini = Some(Ok(Some(ini)));
        // While filtering, the advanced NR rows are shown unfolded.
        app.intuitive.settings.query = "Passes".into();
        if ampere {
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
        }
        Harness::builder().with_size([1100., 900.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                let mut edits = std::mem::take(&mut app.view.edits);
                app.settings_tab(ui, &mut edits, &app.cmd_tx.clone());
                app.view.edits = edits;
            },
            app,
        )
    }

    #[test]
    fn passes_row_shows_its_cost_and_the_rtx30_note() {
        let lang = Lang::En;
        let label = all_label();
        let h = settings_with("4", true);
        assert!(h.query_all_by_label(label).next().is_some());
        h.get_by_label(&passes_cost(lang, "4").unwrap());
        h.get_by_label(t(lang, "set.passes_ampere"));
        // One pass costs nothing extra; another card gets no RTX 30 note.
        let h = settings_with("1", false);
        assert!(h.query_all_by_label(label).next().is_some());
        assert!(h.query_by_label_contains("model instances").is_none());
        assert!(h.query_by_label(t(lang, "set.passes_ampere")).is_none());
    }

    fn all_label() -> &'static str {
        settings::all()
            .iter()
            .find(|s| (s.section, s.key) == ("DlssNr", "Passes"))
            .unwrap()
            .label_en
    }
}
