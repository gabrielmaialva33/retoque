use crate::app::*;

impl RetoqueApp {
    pub(crate) fn model_tab(&self, ui: &mut egui::Ui, commands: &CommandSender) {
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
        if ini.is_none() {
            match &self.view.ini {
                None => {
                    ui.spinner();
                    ui.label(self.tr("tab.loading"));
                }
                Some(Err(e)) => {
                    crate::error::card(ui, e, self.lang, &self.cmd_tx);
                }
                _ => self.tab_install_prompt(ui),
            }
            return;
        }
        let inspection = self.view.inspection.clone();
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
                    commands.send(AppCommand::RefreshModel);
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
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
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
            let used = self.model_usage.get(&m.path).copied();
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
            commands.send(AppCommand::RemoveModel(sha));
        }
        self.settings_plan(ui);
    }

    pub(crate) fn model_import_area(&self, ui: &mut egui::Ui) {
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
                    self.cmd_tx.send(AppCommand::ImportModel(path));
                }
                if ui.button(self.tr("cancel")).clicked() {
                    ui.ctx().data_mut(|data| data.remove::<PathBuf>(pending_id));
                }
            });
        }
        ui.add_space(12.0);
    }

    pub(crate) fn model_compatibility_label(
        &self,
        ui: &mut egui::Ui,
        info: &retoque_data::model_compat::ModelCompatibility,
    ) {
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

    pub(crate) fn nr_compatibility_notice(&self, ui: &mut egui::Ui) {
        let Some(info) = self.view.inspection.as_ref() else {
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
