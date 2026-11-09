use crate::app::*;

impl RetoqueApp {
    pub(crate) fn logs_tab(&self, ui: &mut egui::Ui, commands: &CommandSender) {
        let Some(target) = self.view.target.clone() else { return };
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(!self.busy(), theme::primary(self.tr("log.bundle")))
                .clicked()
            {
                commands.send(AppCommand::Bundle);
            }
            if ui.button(self.tr("log.open_folder")).clicked() {
                commands.send(AppCommand::OpenFolder(target.clone()));
            }
            if ui
                .add_enabled(!self.busy(), egui::Button::new(self.tr("log.retry")))
                .clicked()
            {
                commands.send(AppCommand::ReadLogs(None));
            }
        });
        ui.add_space(12.0);
        if !self.view.intelligence.log_diagnoses.is_empty() {
            widgets::card(ui, |ui| {
                widgets::card_title(ui, self.tr("log.diagnoses"));
                for diagnosis in &self.view.intelligence.log_diagnoses {
                    ui.push_id(diagnosis.id, |ui| {
                        ui.strong(crate::tabs::doctor::text(self.lang, diagnosis.title_key));
                        ui.label(crate::tabs::doctor::text(self.lang, diagnosis.why_key));
                        ui.monospace(&diagnosis.evidence_line);
                        ui.add_enabled_ui(!self.busy(), |ui| {
                            crate::tabs::doctor::repair_button(
                                ui,
                                diagnosis.repair.as_ref(),
                                &target,
                                self.lang,
                                commands,
                                self.payload.is_some(),
                            )
                        });
                        ui.add_space(8.0);
                    });
                }
            });
        }
        let (files, result) = match &self.view.logs {
            None => {
                ui.horizontal(|ui| {
                    ui.spinner();
                    ui.label(self.tr("log.loading"));
                });
                return;
            }
            Some((files, result)) => (files, result),
        };
        match result {
            Err(e) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
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
                        .on_hover_text(crate::format::display_path(&s.path));
                    if selected != s.path {
                        commands.send(AppCommand::ReadLogs(Some(selected)));
                    }
                });
                ui.add_space(12.0);
                let errors = s.error_count.max(s.errors.len());
                widgets::card(ui, |ui| {
                    widgets::card_title(ui, &log_verdict(self.lang, s));
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
                        commands.send(AppCommand::OpenFolder(dir.to_owned()));
                    }
                });
            }
            Some(Err(e)) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
            }
            None => {}
        }
    }
}
pub(crate) fn log_verdict(lang: Lang, summary: &Summary) -> String {
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

// Group repeated problem text while preserving original lines in Technical details.
pub(crate) fn log_message(line: &str) -> &str {
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
