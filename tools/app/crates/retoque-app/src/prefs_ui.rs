//! Preferences: the app's language, what a new install starts with, and the app's own data folder (its size, and the
//! staging an interrupted operation left behind, which the app clears by itself at startup).
use crate::app::*;
use crate::format::{short_path, size_text};
use retoque_data::app_data::Cleaned;

/// The install options a game without Retoque starts from. A game that has Retoque keeps its own.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub(crate) struct InstallDefaults {
    pub(crate) enable_nr: bool,
    pub(crate) proxy: String,
    pub(crate) keep_reshade: bool,
    pub(crate) copy_nvngx: bool,
}
impl Default for InstallDefaults {
    fn default() -> Self {
        Self {
            enable_nr: true,
            proxy: "dxgi.dll".into(),
            keep_reshade: false,
            copy_nvngx: cfg!(not(windows)),
        }
    }
}
impl InstallDefaults {
    pub(crate) fn options(&self) -> InstallOptions {
        InstallOptions {
            proxy: self.proxy.clone(),
            enable_nr: self.enable_nr,
            keep_reshade: self.keep_reshade,
            copy_nvngx: self.copy_nvngx,
            ..Default::default()
        }
    }
}

#[derive(Default)]
pub(crate) struct State {
    pub(crate) open: bool,
    /// Bytes in the data folder, measured when the dialog opens.
    usage: Option<u64>,
    /// What the startup cleanup removed this session.
    cleaned: Option<Cleaned>,
}

/// Staging older than this belongs to nobody: no import or download takes a day.
const STALE_AFTER: std::time::Duration = std::time::Duration::from_secs(24 * 3600);

impl RetoqueApp {
    /// Clears what an interrupted operation left in the data folder. Runs once, in the background, at startup.
    pub(crate) fn clean_app_data(&mut self) {
        self.jobs.spawn("data-cleanup", None, |_| {
            let cleaned = retoque_data::lsfg_prepare::data_dir()
                .ok()
                .and_then(|dir| {
                    retoque_data::app_data::clean_stale(&dir, std::time::SystemTime::now(), STALE_AFTER).ok()
                })
                .unwrap_or_default();
            if cleaned.removed > 0 {
                crate::applog::line(format_args!(
                    "removed {} leftover staging item(s), {} bytes",
                    cleaned.removed, cleaned.bytes
                ));
            }
            Outcome::AppDataCleaned(cleaned)
        });
    }

    pub(crate) fn prefs_event(&mut self, outcome: Outcome) {
        match outcome {
            Outcome::AppDataCleaned(cleaned) => self.prefs.cleaned = Some(cleaned),
            Outcome::AppDataUsage(bytes) => self.prefs.usage = Some(bytes),
            _ => {}
        }
    }

    pub(crate) fn prefs_open(&mut self) {
        self.prefs.open = true;
        self.prefs.usage = None;
        self.jobs.spawn("data-usage", None, |_| {
            let bytes = retoque_data::lsfg_prepare::data_dir()
                .map(|dir| retoque_data::app_data::size(&dir))
                .unwrap_or(0);
            Outcome::AppDataUsage(bytes)
        });
    }

    pub(crate) fn prefs_dialog(&mut self, ctx: &egui::Context) {
        if !self.prefs.open {
            return;
        }
        let lang = self.lang;
        let before = self.saved.install_defaults.clone();
        let mut close = false;
        let modal = egui::Modal::new(egui::Id::unique("preferences"))
            .frame(widgets::modal_frame())
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 560.0));
                // The dialog's own title sits above its section headers, as in "Retoque files".
                ui.heading(t(lang, "prefs.title"));
                ui.add_space(8.0);

                ui.horizontal(|ui| {
                    let label = ui.label(t(lang, "prefs.language"));
                    self.language_combo(ui, "prefs_language").labelled_by(label.id);
                });
                ui.add(
                    egui::Label::new(
                        RichText::new(t(lang, "prefs.language_help"))
                            .small()
                            .color(theme::MUTED),
                    )
                    .wrap(),
                );
                if lang.is_draft() {
                    ui.add(
                        egui::Label::new(
                            RichText::new(t(lang, "prefs.language_draft"))
                                .small()
                                .color(theme::MUTED),
                        )
                        .wrap(),
                    );
                }
                let missing = crate::fonts::unavailable();
                if !missing.is_empty() {
                    let names: Vec<&str> = missing.iter().map(|l| language_name(lang, *l)).collect();
                    let note = t(lang, "prefs.language_no_font").replace("{languages}", &names.join(", "));
                    ui.add(egui::Label::new(RichText::new(note).small().color(theme::MUTED)).wrap());
                }

                widgets::section_header(ui, t(lang, "prefs.installs"));
                ui.add(egui::Label::new(RichText::new(t(lang, "prefs.installs_help")).color(theme::MUTED)).wrap());
                ui.add_space(4.0);
                let defaults = &mut self.saved.install_defaults;
                ui.checkbox(&mut defaults.enable_nr, t(lang, "inst.enable_nr"));
                ui.checkbox(&mut defaults.keep_reshade, t(lang, "inst.keep_reshade"));
                if cfg!(not(windows)) {
                    ui.checkbox(&mut defaults.copy_nvngx, t(lang, "inst.copy_nvngx"));
                }
                ui.horizontal(|ui| {
                    let label = ui.label(t(lang, "inst.proxy"));
                    egui::ComboBox::from_id_salt("default_proxy")
                        .selected_text(defaults.proxy.clone())
                        .show_ui(ui, |ui| {
                            for name in retoque_core::PROXY_NAMES {
                                ui.selectable_value(&mut defaults.proxy, name.to_string(), name);
                            }
                        })
                        .response
                        .labelled_by(label.id);
                });
                ui.label(RichText::new(t(lang, "inst.proxy_help")).small().color(theme::MUTED));

                #[cfg(windows)]
                self.app_install_controls(ui);

                widgets::section_header(ui, t(lang, "prefs.data"));
                let dir = retoque_data::lsfg_prepare::data_dir().ok();
                ui.horizontal_wrapped(|ui| {
                    match self.prefs.usage {
                        Some(bytes) => ui.label(t(lang, "prefs.data_usage").replace("{size}", &size_text(lang, bytes))),
                        None => ui.label(t(lang, "prefs.data_measuring")),
                    };
                    if let Some(dir) = &dir
                        && ui.button(t(lang, "log.open_folder")).clicked()
                    {
                        model::open_folder(dir);
                    }
                });
                if let Some(dir) = &dir {
                    ui.label(RichText::new(short_path(dir)).small().monospace().color(theme::MUTED));
                }
                let cleaned = match self.prefs.cleaned {
                    Some(c) if c.removed > 0 => t(lang, "prefs.cleaned")
                        .replace("{n}", &c.removed.to_string())
                        .replace("{size}", &size_text(lang, c.bytes)),
                    _ => t(lang, "prefs.clean").to_owned(),
                };
                ui.add(egui::Label::new(RichText::new(cleaned).small().color(theme::MUTED)).wrap());

                ui.add_space(12.0);
                close = ui.button(t(lang, "close")).clicked();
            });
        if self.saved.install_defaults != before {
            self.apply_install_defaults();
        }
        if close || modal.should_close() {
            self.prefs.open = false;
        }
    }

    /// Switch the app's language: the text changes on the next frame and the choice is saved.
    pub(crate) fn set_lang(&mut self, lang: Lang) {
        if self.lang != lang {
            self.lang = lang;
            self.saved.lang = Some(lang);
            crate::fonts::apply(&self.context, lang);
        }
    }

    /// The language drop-down: every language by its own name. One this computer cannot draw (Chinese, Japanese or
    /// Korean without a CJK font) is listed disabled, by its name in the current language, with the reason.
    pub(crate) fn language_combo(&mut self, ui: &mut egui::Ui, id_salt: &str) -> egui::Response {
        let lang = self.lang;
        let mut picked = None;
        let response = egui::ComboBox::from_id_salt(id_salt)
            .selected_text(lang.native_name())
            .show_ui(ui, |ui| {
                for option in Lang::ALL {
                    if crate::fonts::can_show(option) {
                        if ui.selectable_label(option == lang, option.native_name()).clicked() {
                            picked = Some(option);
                        }
                    } else {
                        ui.add_enabled(false, egui::Button::selectable(false, language_name(lang, option)))
                            .on_disabled_hover_text(
                                t(lang, "prefs.language_no_font").replace("{languages}", language_name(lang, option)),
                            );
                    }
                }
            })
            .response;
        if let Some(option) = picked {
            self.set_lang(option);
        }
        response
    }

    /// A game that has no Retoque yet follows the defaults as soon as they change.
    fn apply_install_defaults(&mut self) {
        // Only a game with no OptiScaler at all: an old install keeps the loader file name it already uses.
        let untouched =
            matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_none() && s.optiscaler_proxies.is_empty());
        if untouched {
            let game_dir = self.view.options.game_dir.clone();
            self.view.options = self.saved.install_defaults.options();
            self.view.options.game_dir = game_dir;
        }
    }
}

/// A language's name in the reader's language, for a language whose own script cannot be drawn here.
fn language_name(reader: Lang, lang: Lang) -> &'static str {
    match lang {
        Lang::Ja => t(reader, "lang.ja"),
        Lang::Ko => t(reader, "lang.ko"),
        Lang::ZhCn => t(reader, "lang.zh_cn"),
        _ => lang.native_name(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preferences_saved_before_install_defaults_load_with_them() {
        let old: InstallDefaults = serde_json::from_str("{}").unwrap();
        assert_eq!(old, InstallDefaults::default());
        assert!(
            old.enable_nr,
            "a new install keeps turning NR on unless the user says otherwise"
        );
        let partial: InstallDefaults = serde_json::from_str(r#"{"enable_nr": false}"#).unwrap();
        assert!(!partial.enable_nr);
        assert_eq!(partial.proxy, "dxgi.dll");
    }

    #[test]
    fn a_game_without_retoque_starts_from_the_defaults() {
        let mut app = crate::app_tests::shell_tests::app();
        app.saved.install_defaults = InstallDefaults {
            enable_nr: false,
            proxy: "winmm.dll".into(),
            keep_reshade: true,
            copy_nvngx: false,
        };
        let game = tempfile::tempdir().unwrap();
        app.open_target(Some(game.path().to_path_buf()));
        assert!(!app.view.options.enable_nr);
        assert_eq!(app.view.options.proxy, "winmm.dll");
        assert!(app.view.options.keep_reshade);
        assert_eq!(app.view.options.game_dir, game.path());
    }
}
