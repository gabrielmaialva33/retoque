//! The Windows screen is compiled on Linux only in the test harness, never in the Linux product.
use super::{
    process::Session,
    protocol::{Output, Phase, Settings, Source, State},
};
use crate::{
    i18n::{Lang, t},
    theme, widgets,
};
use retoque_data::models::Model;

#[derive(Default)]
pub(crate) struct Screen {
    pub open: bool,
    pub state: State,
    pub draft: Settings,
    pub session: Option<Session>,
    pub available: bool,
    pub review: Option<Settings>,
    pub windows: Vec<super::protocol::Window>,
}
#[derive(Default, PartialEq, Eq, Debug)]
pub(crate) enum Action {
    #[default]
    None,
    Refresh,
    Start,
    Stop,
    Models,
    Back,
}
impl Screen {
    pub fn active(&self) -> bool {
        self.session.is_some()
    }
    pub fn stop(&mut self) {
        if let Some(s) = &self.session {
            s.stop();
            if !matches!(self.state.phase, Phase::Failed | Phase::RestartRequired) {
                self.state.phase = Phase::Stopping;
            }
        }
    }
    pub fn poll(&mut self) {
        if let Some(s) = &self.session {
            s.poll(&mut self.state);
            if s.done() {
                // done is published after the final queued message.
                s.poll(&mut self.state);
                if self.state.catalog_complete {
                    self.available = true;
                    self.windows = self.state.windows.clone();
                }
                self.session = None;
            }
        }
    }
    pub fn draw(&mut self, ui: &mut egui::Ui, lang: Lang, models: &[Model], updating: bool) -> Action {
        let mut action = Action::None;
        let tr = |key| t(lang, key);
        ui.horizontal_wrapped(|ui| {
            if ui.button(tr("shell.back_library")).clicked() {
                action = Action::Back;
            }
            ui.heading(tr("any.title"));
        });
        ui.label(tr("any.experimental"));
        egui::ScrollArea::vertical()
            .id_salt("any-window-page")
            .max_height((ui.available_height() - 86.0).max(100.0))
            .show(ui, |ui| {
                widgets::card(ui, |ui| {
                    widgets::card_title(ui, tr(self.state.phase.key()));
                    if self.state.phase == Phase::Starting
                        && let Some(n) = self.state.countdown
                    {
                        ui.label(tr("any.countdown_active").replace("{seconds}", &n.to_string()));
                    }
                    if let Some(p) = self.state.problem {
                        let color = if matches!(self.state.phase, Phase::Failed | Phase::RestartRequired) {
                            theme::palette(ui).BAD
                        } else {
                            theme::palette(ui).WARN
                        };
                        ui.label(egui::RichText::new(tr(p.key())).color(color));
                    }
                    if !self.state.detail.is_empty() {
                        widgets::collapsible(
                            ui,
                            "any-host-details",
                            tr("technical.details"),
                            widgets::Fold::Details,
                            |ui| {
                                ui.label(&self.state.detail);
                            },
                        );
                    }
                });
                ui.add_space(theme::CARD_GAP);
                if !self.available {
                    if self.active() {
                        return;
                    }
                    ui.label(tr("any.unavailable"));
                    if !self.active() && ui.button(tr("any.refresh")).clicked() {
                        action = Action::Refresh;
                    }
                    return;
                }
                ui.add_enabled_ui(!self.active() && !updating, |ui| {
                    widgets::card(ui, |ui| {
                        widgets::card_title(ui, tr("any.choose"));
                        ui.horizontal_wrapped(|ui| {
                            ui.label(tr("any.model"));
                            let selected = models
                                .iter()
                                .find(|m| m.path == self.draft.model)
                                .map(|m| m.label.as_str())
                                .unwrap_or(tr("any.choose_model"));
                            egui::ComboBox::from_id_salt("any-model")
                                .selected_text(selected)
                                .show_ui(ui, |ui| {
                                    for model in models {
                                        ui.selectable_value(&mut self.draft.model, model.path.clone(), &model.label);
                                    }
                                })
                                .response
                                .widget_info(|| egui::WidgetInfo::labeled(egui::Role::ComboBox, true, tr("any.model")));
                            if ui.button(tr("any.models")).clicked() {
                                action = Action::Models;
                            }
                        });
                        if models.is_empty() {
                            ui.label(tr("any.no_models"));
                        }
                        ui.horizontal_wrapped(|ui| {
                            let countdown = matches!(self.draft.source, Source::Countdown(_));
                            if ui.selectable_label(!countdown, tr("any.from_list")).clicked() {
                                self.draft.source =
                                    Source::Window(self.windows.first().map(|w| w.hwnd.clone()).unwrap_or_default());
                            }
                            if ui.selectable_label(countdown, tr("any.by_countdown")).clicked() {
                                self.draft.source = Source::Countdown(5);
                            }
                        });
                        match &mut self.draft.source {
                            Source::Countdown(seconds) => {
                                ui.add(egui::Slider::new(seconds, 3..=15).text(tr("any.seconds")));
                                ui.label(tr("any.countdown_help"));
                            }
                            Source::Window(hwnd) => {
                                egui::ComboBox::from_id_salt("any-source")
                                    .selected_text(
                                        self.windows
                                            .iter()
                                            .find(|w| w.hwnd == *hwnd)
                                            .map(|w| w.title.as_str())
                                            .unwrap_or(tr("any.choose_window")),
                                    )
                                    .show_ui(ui, |ui| {
                                        for window in &self.windows {
                                            ui.selectable_value(hwnd, window.hwnd.clone(), &window.title);
                                        }
                                    })
                                    .response
                                    .widget_info(|| {
                                        egui::WidgetInfo::labeled(egui::Role::ComboBox, true, tr("any.window"))
                                    });
                                if self.windows.is_empty() {
                                    ui.label(tr("any.no_windows"));
                                }
                            }
                        }
                        if ui.button(tr("any.refresh")).clicked() {
                            action = Action::Refresh;
                        }
                    });
                    ui.add_space(theme::CARD_GAP);
                    widgets::card(ui, |ui| {
                        widgets::card_title(ui, tr("any.processing"));
                        ui.add(
                            egui::Slider::new(&mut self.draft.strength, 0..=100)
                                .suffix("%")
                                .text(tr("any.strength")),
                        );
                        ui.add(
                            egui::Slider::new(&mut self.draft.scale, 25..=100)
                                .suffix("%")
                                .text(tr("any.scale")),
                        );
                        ui.label(tr("any.scale_help"));
                        ui.checkbox(&mut self.draft.motion, tr("any.motion"));
                        ui.checkbox(&mut self.draft.protect_ui, tr("any.protect_ui"));
                        ui.horizontal_wrapped(|ui| {
                            ui.label(tr("any.comparison"));
                            for (n, key) in [(0, "any.compare_off"), (1, "any.compare_side"), (2, "any.compare_wipe")] {
                                ui.selectable_value(&mut self.draft.compare, n, tr(key));
                            }
                        });
                        ui.horizontal_wrapped(|ui| {
                            ui.label(tr("any.output"));
                            ui.selectable_value(&mut self.draft.output, Output::Preview, tr("any.preview"));
                            ui.selectable_value(&mut self.draft.output, Output::Overlay, tr("any.overlay"));
                        });
                        ui.label(tr(if self.draft.output == Output::Overlay {
                            "any.overlay_help"
                        } else {
                            "any.preview_help"
                        }));
                    });
                });
                if updating {
                    ui.label(tr("any.updating"));
                }
            });
        ui.add_space(theme::CARD_GAP);
        if self.active() {
            ui.horizontal_wrapped(|ui| {
                ui.spinner();
                if ui
                    .add_enabled(self.state.phase != Phase::Stopping, theme::primary(ui, tr("any.stop")))
                    .clicked()
                {
                    action = Action::Stop;
                }
                if self.state.overlay {
                    ui.label(tr("any.hotkey"));
                }
            });
        } else if self.available {
            ui.label(tr("any.pending"));
            let valid = !updating && self.draft.validate().is_ok() && models.iter().any(|m| m.path == self.draft.model);
            if ui
                .add_enabled(valid, theme::primary(ui, tr("any.start_review")))
                .clicked()
            {
                self.review = Some(self.draft.clone());
            }
        }
        action
    }
    pub fn review(&mut self, ctx: &egui::Context, lang: Lang, models: &[Model]) -> Action {
        let Some(settings) = &self.review else {
            return Action::None;
        };
        let mut action = Action::None;
        let mut cancel = false;
        let result = egui::Modal::new(egui::Id::unique("any-start-review"))
            .frame(widgets::modal_frame(ctx))
            .show(ctx, |ui| {
                ui.set_max_width(500.0);
                ui.heading(t(lang, "any.start"));
                ui.label(t(lang, "any.review_body"));
                let name = models
                    .iter()
                    .find(|m| m.path == settings.model)
                    .map(|m| m.label.as_str())
                    .unwrap_or(t(lang, "any.choose_model"));
                ui.label(format!("{}: {name}", t(lang, "any.model")));
                match &settings.source {
                    Source::Countdown(n) => {
                        ui.label(t(lang, "any.countdown_active").replace("{seconds}", &n.to_string()));
                    }
                    Source::Window(h) => {
                        ui.label(
                            self.windows
                                .iter()
                                .find(|w| w.hwnd == *h)
                                .map(|w| w.title.as_str())
                                .unwrap_or(t(lang, "any.choose_window")),
                        );
                    }
                }
                ui.label(format!(
                    "{}: {}% · {}: {}%",
                    t(lang, "any.strength"),
                    settings.strength,
                    t(lang, "any.scale"),
                    settings.scale
                ));
                for (key, enabled) in [("any.motion", settings.motion), ("any.protect_ui", settings.protect_ui)] {
                    ui.label(format!(
                        "{}: {}",
                        t(lang, key),
                        t(lang, if enabled { "any.on" } else { "any.compare_off" })
                    ));
                }
                ui.label(t(
                    lang,
                    ["any.compare_off", "any.compare_side", "any.compare_wipe"][settings.compare as usize],
                ));
                ui.label(t(
                    lang,
                    if settings.output == Output::Overlay {
                        "any.overlay_help"
                    } else {
                        "any.preview_help"
                    },
                ));
                ui.horizontal(|ui| {
                    cancel = ui.button(t(lang, "cancel")).clicked();
                    if ui
                        .add_enabled(
                            models.iter().any(|m| m.path == settings.model),
                            theme::primary(ui, t(lang, "any.start")),
                        )
                        .clicked()
                    {
                        action = Action::Start;
                    }
                });
            });
        if cancel || result.should_close() {
            self.review = None;
            action = Action::None;
        }
        action
    }
}
