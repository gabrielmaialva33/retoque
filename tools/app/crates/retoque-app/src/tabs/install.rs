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

/// The launch card's title and help for a store.
fn launch_keys(store: Store) -> (&'static str, &'static str) {
    match store {
        Store::Steam => ("card.launch", "inst.launch_help"),
        Store::Heroic => ("card.launch_heroic", "inst.launch_help_heroic"),
        Store::Lutris => ("card.launch_lutris", "inst.launch_help_lutris"),
        _ => ("card.launch_other", "inst.launch_help_other"),
    }
}

/// `dxgi=n,b` out of `WINEDLLOVERRIDES="dxgi=n,b" %command%`: what Heroic's and Lutris's variable fields take.
fn launch_env_value(option: &str) -> Option<&str> {
    let value = option
        .trim()
        .strip_suffix("%command%")?
        .trim_end()
        .strip_prefix("WINEDLLOVERRIDES=")?
        .trim_matches('"');
    (!value.is_empty()).then_some(value)
}

impl RetoqueApp {
    pub(crate) fn install_tab(&mut self, ui: &mut egui::Ui) {
        let Some(target) = self.view.target.clone() else { return };
        let installed = matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_some());
        let busy = self.busy();
        // The banner's button is the way to install; progress and a failed last change still show here, first, and
        // only while there is one (a success is the toast's). The loader file name and the other options sit closed at the
        // end.
        let writing = self.view.target.as_ref().and_then(|t| self.writing.get(t)).copied();
        let failed = matches!(&self.view.applied, Some((_, Err(_))));
        if writing.is_some() || failed {
            widgets::card(ui, |ui| {
                if let Some((done, total)) = writing {
                    ui.add(
                        egui::ProgressBar::new(done as f32 / total.max(1) as f32)
                            .text(format!("{} {done}/{total}", t(self.lang, "inst.working"))),
                    );
                }
                if let Some((kind, Err(e))) = &self.view.applied {
                    ui.colored_label(
                        theme::BAD,
                        format!("{} ({}): {e}", t(self.lang, "error"), plan_kind_name(self.lang, *kind)),
                    );
                }
            });
            ui.add_space(theme::CARD_GAP);
        }
        let launch = self.launch_step();
        if let Some((option, store)) = &launch
            && installed
            && !self.launch_confirmed()
        {
            self.launch_card(ui, option, *store);
            ui.add_space(theme::CARD_GAP);
        }
        // Before install the page stays short: what "Install and optimize" will do, and the Diagnóstico only when it
        // found something. ReShade shows up when the game already has it.
        if !installed {
            if let (Some(card), Some(target)) = (&self.view.doctor, &self.view.target)
                && !card.findings.is_empty()
            {
                ui.add_enabled_ui(!self.busy(), |ui| {
                    crate::tabs::doctor::show(ui, card, target, self.lang, &self.cmd_tx, self.payload.is_some())
                });
            }
            self.reshade_inspect();
            if self.tuning.reshade.in_view() {
                self.reshade_card(ui);
                ui.add_space(theme::CARD_GAP);
            }
            self.plan_card(ui, busy);
            return;
        }
        self.session_card(ui);
        if let (Some(card), Some(target)) = (&self.view.doctor, &self.view.target) {
            ui.add_enabled_ui(!self.busy(), |ui| {
                crate::tabs::doctor::show(ui, card, target, self.lang, &self.cmd_tx, self.payload.is_some())
            });
        }
        // ReShade is open when the game has it; otherwise it stays one click away, folded.
        self.reshade_inspect();
        if self.tuning.reshade.in_view() {
            self.reshade_card(ui);
        } else {
            widgets::collapsible(
                ui,
                "reshade_section",
                self.tr("rs.title"),
                widgets::Fold::Section,
                |ui| self.reshade_card(ui),
            );
        }
        self.autotune_card(ui);
        ui.add_space(theme::CARD_GAP);
        let _ = &target;
        widgets::card(ui, |ui| {
            widgets::collapsible(
                ui,
                "install_options_section",
                self.tr("card.reinstall_options"),
                widgets::Fold::Section,
                |ui| {
                    ui.add_enabled_ui(!busy, |ui| self.install_options(ui, installed));
                    // Once the game has loaded Retoque the launch option leaves the top of the page; it stays here.
                    if let Some((option, store)) = &launch
                        && self.launch_confirmed()
                    {
                        ui.add_space(8.0);
                        ui.label(RichText::new(self.tr("inst.launch_label")).strong());
                        self.launch_value(ui, option, *store);
                    }
                },
            );
        });
    }

    /// Before install: what "Install and optimize" will do in this game, as read-only bullets (the recommendations
    /// that one-click review would include), and the install options closed at the end. Nothing here applies.
    fn plan_card(&mut self, ui: &mut egui::Ui, busy: bool) {
        use crate::autotune::recommend::{Confidence, Need};
        let checking = self.tuning.gpus.is_none() || self.view.status.is_none() || !self.autotune_known_components();
        let recs = if checking {
            Default::default()
        } else {
            self.optimize_recommendations()
        };
        let target = self.view.target.as_ref();
        let experimental = target.is_some() && self.tuning.sm86.experimental_for.as_ref() == target;
        let lsfg = target.is_some() && self.tuning.sm86.lsfg_for.as_ref() == target;
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("inst.plan_title"));
            if checking {
                ui.add(egui::Label::new(RichText::new(self.tr("auto.checking")).color(theme::MUTED)).wrap());
            }
            for rec in recs.iter() {
                // The same filter the one-click review uses: ReShade is always the user's own step, and experimental
                // or LSFG items need a choice that only exists once Retoque is installed.
                let skipped = matches!(
                    rec.needs,
                    Some(Need::Component(retoque_data::components::ComponentKind::ReShade))
                ) || (rec.confidence == Confidence::ExperimentalUnderProton && !experimental)
                    || (rec.needs == Some(Need::LosslessScaling) && !lsfg);
                if skipped {
                    continue;
                }
                let title = if rec.id == "install_retoque" {
                    self.tr("inst.plan_install")
                } else {
                    t(self.lang, rec.title_key)
                };
                ui.add(egui::Label::new(format!("•  {title}")).wrap())
                    .on_hover_text(t(self.lang, rec.why_key));
            }
            ui.add_space(4.0);
            ui.add(egui::Label::new(RichText::new(self.tr("auto.use_banner")).color(theme::MUTED)).wrap());
            if let Some(error) = &self.tuning.sm86.error {
                ui.colored_label(theme::BAD, crate::i18n::diagnostic(self.lang, error));
            }
            ui.add_space(4.0);
            widgets::collapsible(
                ui,
                "install_options_section",
                self.tr("card.install_options"),
                widgets::Fold::Section,
                |ui| {
                    ui.add_enabled_ui(!busy, |ui| self.install_options(ui, false));
                },
            );
        });
    }

    /// The launch option the game needs under Proton or Wine, and the store whose words explain where it goes.
    /// None on Windows, where the game loads Retoque by itself.
    fn launch_step(&self) -> Option<(String, Store)> {
        let option = self.view.status.as_ref()?.as_ref().ok()?.launch_option.clone()?;
        let store = self.selected_game().map_or(Store::Manual, |g| g.store);
        Some((option, store))
    }

    /// The Diagnóstico's evidence that the launch option is in place: the game's newest log shows Retoque loaded.
    fn launch_confirmed(&self) -> bool {
        self.view
            .intelligence
            .latest_log
            .as_ref()
            .is_some_and(crate::intelligence::proxy_loaded)
    }

    /// The last step after an install under Proton or Wine, first on the page in the accent tint until the game has
    /// loaded Retoque once. Its words follow the store, because each one keeps the setting somewhere else.
    fn launch_card(&mut self, ui: &mut egui::Ui, option: &str, store: Store) {
        let (title, help) = launch_keys(store);
        widgets::tinted_card(ui, theme::ACCENT.gamma_multiply(0.12), |ui| {
            widgets::card_title(ui, self.tr(title));
            ui.add(egui::Label::new(RichText::new(self.tr(help)).color(theme::TEXT)).wrap());
            ui.add_space(4.0);
            self.launch_value(ui, option, store);
        });
    }

    /// The text to paste and its copy button. Heroic and Lutris take a variable and a value, not a Steam command.
    fn launch_value(&mut self, ui: &mut egui::Ui, option: &str, store: Store) {
        let value = match store {
            Store::Heroic | Store::Lutris => launch_env_value(option).unwrap_or(option),
            _ => option,
        }
        .to_owned();
        ui.horizontal_wrapped(|ui| {
            ui.add(egui::Label::new(
                RichText::new(&value).monospace().color(theme::TEXT_STRONG),
            ));
            let label = if self.view.copied {
                t(self.lang, "inst.copied")
            } else {
                t(self.lang, "inst.copy")
            };
            if ui.button(label).clicked() {
                ui.ctx().copy_text(value.clone());
                self.view.copied = true;
            }
        });
    }

    pub(crate) fn install_options(&mut self, ui: &mut egui::Ui, installed: bool) {
        {
            ui.checkbox(&mut self.view.options.enable_nr, t(self.lang, "inst.enable_nr"));
            let advanced = self.tr("inst.advanced");
            widgets::collapsible(ui, "install_advanced", advanced, widgets::Fold::Section, |ui| {
                // The loader file name only matters when a game does not start with the default one.
                egui::Grid::new("install_options")
                    .num_columns(2)
                    .min_col_width(160.0)
                    .spacing([16.0, 8.0])
                    .show(ui, |ui| {
                        let label =
                            ui.add(egui::Label::new(self.tr("inst.proxy")).wrap_mode(egui::TextWrapMode::Extend));
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
                ui.label(RichText::new(self.tr("inst.proxy_help")).small().color(theme::MUTED));
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
            let detail = match &status.build_sha256 {
                Some(hash) => self.build_text(hash),
                None => String::new(),
            };
            let value = self.tr(state.short_label());
            let tile = widgets::feature_tile(ui, tile_w, self.tr("brand"), value, &detail, state.color());
            if let Some(hash) = &status.build_sha256 {
                tile.on_hover_text(self.build_hover(hash));
            }
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
            widgets::feature_tile(ui, tile_w, self.tr("tile.nr"), value, &detail, color);
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
            widgets::feature_tile(
                ui,
                tile_w,
                self.tr("tile.dlss"),
                &value,
                self.tr("tile.dlss_detail"),
                color,
            );
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
                            line.push_str(&format!(" · {}", self.tr("ver.forwarder").replace("{v}", forwarder)));
                        }
                        ui.label(RichText::new(line).small().color(theme::TEXT));
                    }
                    return;
                }
                let mut parts = vec![self.tr(state.label()).to_owned()];
                if let Some(hash) = &status.build_sha256 {
                    parts.push(self.build_text(hash));
                }
                if !status.optiscaler_proxies.is_empty() {
                    // The forwarder only gets a word when its version is known; the mismatch warning covers the rest.
                    let proxy = status.proxy_version.as_deref().unwrap_or(self.tr("ver.unknown"));
                    parts.push(format!("OptiScaler {proxy}"));
                    if let Some(forwarder) = &status.forwarder_version {
                        parts.push(self.tr("ver.forwarder").replace("{v}", forwarder));
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
                // The build's hash and the model's path are technical detail: hover only.
                let hover: Vec<String> = status
                    .build_sha256
                    .iter()
                    .map(|hash| self.build_hover(hash))
                    .chain(
                        inspection
                            .as_ref()
                            .and_then(|info| info.path.as_ref())
                            .map(|p| short_path(p)),
                    )
                    .collect();
                if !hover.is_empty() {
                    response.on_hover_text(hover.join("\n"));
                }
            }
        }
    }

    /// A Retoque build in words: its release version when a known release has this OptiScaler hash, otherwise
    /// "Local build". The hash itself goes in a hover (`build_hover`), never in the main text.
    pub(crate) fn build_text(&self, hash: &str) -> String {
        match &self.payload_ui.record {
            Some(rec) if rec.optiscaler_sha256.eq_ignore_ascii_case(hash) => {
                self.tr("st.release").replace("{v}", &rec.release.version)
            }
            _ => self.tr("st.build").to_owned(),
        }
    }

    pub(crate) fn build_hover(&self, hash: &str) -> String {
        self.tr("st.build_hash").replace("{hash}", &hash[..hash.len().min(12)])
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

#[cfg(test)]
mod tests {
    use super::*;
    use crate::autotune::recommend::{self, GameFacts};
    use egui_kittest::{Harness, kittest::Queryable};
    use retoque_discovery::TargetDir;
    use std::sync::Arc;

    const OPTION: &str = "WINEDLLOVERRIDES=\"dxgi=n,b\" %command%";

    fn game(store: Store, target: &Path) -> Game {
        Game {
            id: "synthetic".into(),
            name: "Synthetic game".into(),
            store,
            install_dir: target.into(),
            runtime: Runtime::Proton {
                app_id: 1,
                prefix: None,
            },
            targets: vec![TargetDir {
                path: target.into(),
                executables: vec!["game.exe".into()],
                score: 1,
                dlss: vec![],
                has_receipt: false,
                proxies_present: vec![],
                loaders: vec![],
            }],
            dlss_anywhere: vec![],
            registered: true,
        }
    }

    /// A game page on the Overview: installed (complete receipt, with the Proton launch option) or not.
    fn app(lang: Lang, store: Store, target: &Path, installed: bool) -> RetoqueApp {
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        app.library_state.games.push(game(store, target));
        app.selected = Some("synthetic".into());
        app.view.target = Some(target.into());
        app.view.status = Some(Ok(Status {
            receipt: installed.then(|| retoque_core::receipt::Receipt {
                status: ReceiptStatus::Complete,
                updated: String::new(),
                records: vec![],
            }),
            launch_option: installed.then(|| OPTION.into()),
            ..Default::default()
        }));
        app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
        app.tuning.installed = Some(Ok(vec![]));
        let facts = GameFacts {
            installed,
            runtime: recommend::Runtime::Proton,
            ..Default::default()
        };
        let gpu = app.tuning.gpus.as_deref().and_then(retoque_data::gpu::primary);
        app.view.intelligence.recommendations = Arc::new(recommend::recommend(gpu, &facts));
        app.view.intelligence.facts = Arc::new(facts);
        app
    }

    fn overview(app: RetoqueApp) -> Harness<'static, RetoqueApp> {
        let mut h = Harness::builder()
            .with_size([1000., 1400.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.install_tab(ui), app);
        h.run_steps(2);
        h
    }

    #[test]
    fn launch_words_and_value_follow_the_store() {
        assert_eq!(launch_env_value(OPTION), Some("dxgi=n,b"));
        assert_eq!(
            launch_env_value("WINEDLLOVERRIDES=\"winmm=n,b\" %command%"),
            Some("winmm=n,b")
        );
        assert_eq!(launch_env_value("something else"), None);
        let mut seen = std::collections::BTreeSet::new();
        for store in [Store::Steam, Store::Heroic, Store::Lutris, Store::Manual] {
            let (title, help) = launch_keys(store);
            assert!(seen.insert(title), "{store:?} needs its own title");
            for lang in [Lang::En, Lang::Pt] {
                assert_ne!(t(lang, title), title);
                assert_ne!(t(lang, help), help);
            }
        }
        // Stores that run through Heroic, Lutris or Steam on Linux elsewhere get the general words.
        assert_eq!(launch_keys(Store::Epic), launch_keys(Store::Manual));
    }

    #[test]
    fn before_install_the_overview_says_what_will_happen_and_nothing_else() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let h = overview(app(lang, Store::Steam, temp.path(), false));
            h.get_by_label(t(lang, "inst.plan_title"));
            assert!(
                h.query_by_label_contains(t(lang, "inst.plan_install")).is_some(),
                "the install itself is the first thing listed"
            );
            assert!(h.query_by_label_contains(t(lang, "rec.rtx30_nr.title")).is_some());
            // LSFG needs a choice that only exists after install, so it is not promised here.
            assert!(h.query_by_label_contains(t(lang, "rec.lsfg.title")).is_none());
            // The options stay reachable, closed, inside that card.
            h.get_by_label(t(lang, "card.install_options"));
            for absent in [
                "card.reinstall_options",
                "auto.lsfg",
                "rs.title",
                "auto.title",
                "card.launch",
            ] {
                assert!(h.query_by_label(t(lang, absent)).is_none(), "{absent} before install");
            }
        }
    }

    #[test]
    fn after_install_the_launch_step_comes_first_until_the_game_loads_retoque() {
        for lang in [Lang::En, Lang::Pt] {
            for (store, title, shown) in [
                (Store::Steam, "card.launch", OPTION),
                (Store::Heroic, "card.launch_heroic", "dxgi=n,b"),
                (Store::Lutris, "card.launch_lutris", "dxgi=n,b"),
                (Store::Manual, "card.launch_other", OPTION),
            ] {
                let temp = tempfile::tempdir().unwrap();
                let mut h = overview(app(lang, store, temp.path(), true));
                let top = h.get_by_label(t(lang, title)).rect().min.y;
                h.get_by_label(shown);
                let options = h.get_by_label(t(lang, "card.reinstall_options")).rect().min.y;
                assert!(top < h.get_by_label(t(lang, "auto.title")).rect().min.y);
                assert!(top < options);
                // Folded, but still there for a game without ReShade.
                h.get_by_label(t(lang, "rs.title"));
                assert!(h.query_by_label(t(lang, "inst.plan_title")).is_none());
                assert!(
                    h.query_by_label(t(lang, "auto.lsfg")).is_none(),
                    "LSFG lives in Components"
                );
                // The newest log shows Retoque loaded: the launch option works and leaves the top of the page.
                h.state_mut().view.intelligence.latest_log = Some(Summary {
                    nr_initialized: true,
                    ..Default::default()
                });
                h.run_steps(2);
                assert!(h.query_by_label(t(lang, title)).is_none());
            }
        }
    }

    #[test]
    fn components_tab_reads_nr_model_then_dlss_then_frame_generation() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let mut app = app(lang, Store::Steam, temp.path(), true);
            app.tab = Tab::Model;
            app.view.ini = Some(Ok(None));
            app.tuning.catalog = Some(Ok(vec![]));
            let mut h = Harness::builder()
                .with_size([1400., 1000.])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.game(ui), app);
            h.run_steps(2);
            let y = |key: &str| h.get_by_label(t(lang, key)).rect().min.y;
            let order = [
                y("comp.nr"),
                y("tab.install_first"),
                y("dlss.title"),
                y("auto.components"),
                y("auto.lsfg"),
            ];
            assert!(order.windows(2).all(|w| w[0] < w[1]), "{order:?}");
            h.get_by_role_and_label(egui::Role::Tab, t(lang, "tab.model"));
        }
    }
}
