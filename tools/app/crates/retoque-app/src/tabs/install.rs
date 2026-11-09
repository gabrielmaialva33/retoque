use crate::app::*;

impl RetoqueApp {
    pub(crate) fn install_tab(&mut self, ui: &mut egui::Ui) {
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
        ui.add_space(12.0);
        if let (Some(card), Some(target)) = (&self.view.doctor, &self.view.target) {
            ui.add_enabled_ui(!self.busy(), |ui| {
                crate::tabs::doctor::show(ui, card, target, self.lang, &self.cmd_tx, self.payload.is_some())
            });
        }
        self.autotune_card(ui);
        self.autotune_lsfg(ui);
        ui.add_space(12.0);
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

    pub(crate) fn install_options(&mut self, ui: &mut egui::Ui, installed: bool) {
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

    pub(crate) fn status_chips(&self, ui: &mut egui::Ui) {
        match &self.view.status {
            None => {
                ui.label(self.tr("shell.checking"));
            }
            Some(Err(e)) => {
                crate::error::card(ui, e, self.lang, &self.cmd_tx);
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
                let inspection = &self.view.inspection;
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
                if let Some(path) = inspection.as_ref().and_then(|info| info.path.as_ref()) {
                    response.on_hover_text(short_path(path));
                }
            }
        }
    }

    pub(crate) fn tab_install_prompt(&self, ui: &mut egui::Ui) {
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("tab.install_first"));
            if ui.button(self.tr("tab.review_install")).clicked() {
                self.cmd_tx.send(AppCommand::SwitchTab(Tab::Install));
                if let Some(inputs) = self.install_inputs() {
                    self.request_plan(PlanKind::Install, PlanInputs::Install(inputs));
                }
            }
        });
    }
}
