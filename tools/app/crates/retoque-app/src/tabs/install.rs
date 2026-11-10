use crate::app::*;
use retoque_data::gpu::Arch;

fn gpu_arch_key(arch: Arch) -> &'static str {
    match arch {
        Arch::Turing => "auto.turing",
        Arch::Ampere => "auto.ampere",
        Arch::Ada => "auto.ada",
        Arch::Blackwell => "auto.blackwell",
        Arch::OlderNvidia => "auto.older",
        Arch::NotNvidia => "auto.other",
        Arch::Unknown => "auto.unknown",
    }
}

/// The title over a change that did not finish.
fn failed_key(kind: PlanKind) -> &'static str {
    match kind {
        PlanKind::Install => "inst.failed_install",
        PlanKind::Uninstall => "inst.failed_uninstall",
        PlanKind::Settings => "inst.failed_settings",
    }
}

impl RetoqueApp {
    pub(crate) fn install_tab(&mut self, ui: &mut egui::Ui) {
        let Some(target) = self.view.target.clone() else { return };
        let installed = matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_some());
        let busy = self.busy();
        // The banner's button is the way to install; progress and the outcome of the last change still show here,
        // first, and only while there is one. The proxy name and the other options sit closed at the end.
        let writing = self.view.target.as_ref().and_then(|t| self.writing.get(t)).copied();
        let failed = match &self.view.applied {
            Some((kind, Err(e))) => Some((*kind, e)),
            _ => None,
        };
        if writing.is_some() || matches!(self.view.applied, Some((_, Ok(())))) {
            widgets::card(ui, |ui| {
                if let Some((done, total)) = writing {
                    ui.add(egui::ProgressBar::new(done as f32 / total.max(1) as f32).text(format!(
                        "{} · {done} {} {total}",
                        t(self.lang, "inst.working"),
                        t(self.lang, "progress.of")
                    )));
                }
                if matches!(self.view.applied, Some((_, Ok(())))) {
                    ui.colored_label(theme::GOOD, t(self.lang, "inst.done"));
                }
            });
            ui.add_space(12.0);
        }
        // A failed change: what did not finish, why in plain words, what to do, and the details folded.
        if let Some((kind, error)) = failed {
            widgets::tinted_card(ui, theme::ERROR_CARD, |ui| {
                widgets::card_title(ui, t(self.lang, failed_key(kind)));
                crate::error::body(ui, error, self.lang, &self.cmd_tx);
            });
            ui.add_space(12.0);
        }
        self.session_card(ui);
        if let (Some(card), Some(target)) = (&self.view.doctor, &self.view.target) {
            ui.add_enabled_ui(!self.busy(), |ui| {
                crate::tabs::doctor::show(ui, card, target, self.lang, &self.cmd_tx, self.payload.is_some())
            });
        }
        self.reshade_card(ui);
        self.autotune_card(ui);
        self.autotune_lsfg(ui);
        ui.add_space(12.0);
        let _ = &target;
        widgets::card(ui, |ui| {
            egui::CollapsingHeader::new(RichText::new(t(self.lang, "card.install_options")).strong())
                .id_salt("install_options_section")
                .show(ui, |ui| {
                    ui.add_enabled_ui(!busy, |ui| self.install_options(ui, installed));
                });
        });
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

    /// The game at a glance, as a GPU panel shows it: graphics card, Retoque, Neural Rendering, frame generation
    /// and DLSS. Every value comes from facts already inspected; nothing here reads the disk or decides anything.
    pub(crate) fn feature_tiles(&self, ui: &mut egui::Ui) {
        let Some(Ok(status)) = &self.view.status else { return };
        let facts = &self.view.intelligence.facts;
        let gpu = self.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
        let ini = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(|i| i.as_ref());
        let state = install_state(
            status,
            self.payload_build
                .as_ref()
                .and_then(|r| r.as_ref().ok())
                .map(String::as_str),
        );
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing = egui::vec2(10.0, 10.0);
            // Five tiles share the column's width: one even row when they fit, equal columns when they wrap.
            let tile_w = widgets::row_width(5, ui.available_width(), 138.0, 10.0);
            // Graphics card.
            let (value, detail) = match gpu {
                Some(g) => (
                    g.name
                        .trim_start_matches("NVIDIA ")
                        .trim_start_matches("GeForce ")
                        .to_owned(),
                    format!(
                        "{}{}",
                        self.tr(gpu_arch_key(g.arch)),
                        g.vram_mib
                            .map(|m| format!(" · {:.0} GB", m as f64 / 1024.0))
                            .unwrap_or_default()
                    ),
                ),
                None => (self.tr("tile.unknown").to_owned(), String::new()),
            };
            widgets::feature_tile(ui, tile_w, self.tr("tile.gpu"), &value, &detail, theme::ACCENT);
            // Retoque itself.
            let color = match state {
                InstallState::Current | InstallState::Installed => theme::GOOD,
                InstallState::Update | InstallState::Untracked => theme::WARN,
                InstallState::Partial => theme::BAD,
                InstallState::Missing => theme::MUTED,
            };
            let detail = match (&status.build_sha256, state) {
                (_, InstallState::Untracked) => self.tr("tile.old_install").to_owned(),
                (Some(hash), _) => format!("{} {}", self.tr("st.build"), &hash[..hash.len().min(8)]),
                (None, _) => String::new(),
            };
            let value = self.tr(match state {
                InstallState::Current => "tile.st_current",
                InstallState::Update => "tile.st_update",
                InstallState::Installed => "tile.st_installed",
                InstallState::Partial => "tile.st_partial",
                InstallState::Untracked => "tile.st_untracked",
                InstallState::Missing => "tile.st_missing",
            });
            widgets::feature_tile(ui, tile_w, "Retoque", value, &detail, color);
            // Neural Rendering.
            let model = self.view.inspection.as_ref();
            let (value, detail, color) = match status.nr_enabled {
                _ if state == InstallState::Missing => (self.tr("tile.off"), String::new(), theme::MUTED),
                Some(true) => match model {
                    Some(m) if m.present => (
                        self.tr("tile.on"),
                        m.version
                            .as_ref()
                            .map(|v| format!("{} {v}", self.tr("shell.model_version")))
                            .unwrap_or_else(|| self.tr("shell.model_unknown").into()),
                        theme::GOOD,
                    ),
                    Some(_) => (
                        self.tr("tile.on"),
                        self.tr("tile.model_missing").to_owned(),
                        theme::WARN,
                    ),
                    None => (self.tr("tile.on"), String::new(), theme::GOOD),
                },
                Some(false) => (self.tr("tile.off"), String::new(), theme::MUTED),
                None => (self.tr("tile.unknown"), String::new(), theme::MUTED),
            };
            widgets::feature_tile(ui, tile_w, "Neural Rendering", value, &detail, color);
            // Frame generation: ours (LSFG, sm86) first, then the game's own DLSS-G where this card can run it.
            let lsfg_multiplier = ini.and_then(|i| i.get("LSFG", "Multiplier")).map(str::trim);
            let older = gpu.is_some_and(|g| matches!(g.arch, Arch::Turing | Arch::Ampere));
            let (value, detail, color) = if facts.sm86_installed {
                ("DLSS-G".to_owned(), self.tr("tile.fg_sm86").to_owned(), theme::GOOD)
            } else if facts.lsfg_enabled == Some(true) {
                (
                    format!("LSFG x{}", lsfg_multiplier.unwrap_or("2")),
                    "Lossless Scaling".to_owned(),
                    theme::GOOD,
                )
            } else if facts.has_dlssg && older {
                (
                    self.tr("tile.fg_available").to_owned(),
                    self.tr("tile.fg_needs_sm86").to_owned(),
                    theme::WARN,
                )
            } else if facts.has_dlssg {
                ("DLSS-G".to_owned(), self.tr("tile.fg_game").to_owned(), theme::GOOD)
            } else {
                (self.tr("tile.off").to_owned(), String::new(), theme::MUTED)
            };
            widgets::feature_tile(ui, tile_w, self.tr("tile.fg"), &value, &detail, color);
            // The game's DLSS files.
            let names = self
                .selected_game()
                .and_then(|g| g.targets.iter().find(|t| Some(&t.path) == self.view.target.as_ref()))
                .map(|t| t.dlss.clone())
                .unwrap_or_default();
            let mut parts = Vec::new();
            for (file, part) in [
                ("nvngx_dlss.dll", "SR"),
                ("nvngx_dlssd.dll", "RR"),
                ("nvngx_dlssg.dll", "FG"),
            ] {
                if names.iter().any(|n| n.eq_ignore_ascii_case(file))
                    || (part == "SR" && facts.has_dlss)
                    || (part == "FG" && facts.has_dlssg)
                {
                    parts.push(part);
                }
            }
            let (value, color) = if parts.is_empty() {
                (self.tr("tile.dlss_none").to_owned(), theme::MUTED)
            } else {
                (parts.join(" · "), theme::GOOD)
            };
            widgets::feature_tile(ui, tile_w, "DLSS", &value, self.tr("tile.dlss_detail"), color);
        });
        ui.add_space(12.0);
    }

    /// The banner's status line. Under the full banner the tiles already say installed, update, NR and model, so the
    /// line keeps only what they do not: the OptiScaler and forwarder versions. The compact banner keeps it all.
    pub(crate) fn status_chips(&self, ui: &mut egui::Ui, technical_only: bool) {
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
                if technical_only {
                    if status.version_mismatch() {
                        ui.colored_label(theme::WARN, self.tr("ver.mismatch"));
                    }
                    if !status.optiscaler_proxies.is_empty() {
                        let proxy = status.proxy_version.as_deref().unwrap_or(self.tr("ver.unknown"));
                        let mut line = format!("OptiScaler {proxy}");
                        if let Some(forwarder) = &status.forwarder_version {
                            line.push_str(&format!(" · NR forwarder {forwarder}"));
                        }
                        ui.label(RichText::new(line).small().color(theme::TEXT));
                    }
                    return;
                }
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
