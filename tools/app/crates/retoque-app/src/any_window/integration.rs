use super::{
    process::{Limits, Session},
    protocol::Problem,
    screen::Action,
};
use crate::app::RetoqueApp;
impl RetoqueApp {
    pub(crate) fn any_window_open(&mut self) {
        self.selected = None;
        self.open_target(None);
        self.any_window.open = true;
        if !self.any_window.available && !self.any_window.active() {
            self.any_window_launch(false);
        }
    }
    pub(crate) fn any_window_launch(&mut self, run: bool) {
        if self.any_window.active() || self.app_installer.busy() {
            return;
        }
        let Some(payload) = self.payload.clone() else {
            self.any_window.state.fail(Problem::Files, "no selected payload");
            return;
        };
        let settings = if run {
            let Some(s) = self.any_window.review.take() else {
                return;
            };
            if s.validate().is_err()
                || !self
                    .catalog
                    .as_ref()
                    .and_then(|r| r.as_ref().ok())
                    .is_some_and(|ms| ms.iter().any(|m| m.path == s.model))
            {
                return;
            }
            Some(s)
        } else {
            None
        };
        self.any_window.state.begin(settings.clone());
        let revision = self.any_window.state.revision;
        self.any_window.session = Some(Session::start(
            move |cancelled| super::windows::prepare(&payload, settings.as_ref(), cancelled),
            revision,
            Limits::default(),
        ));
    }
    pub(crate) fn any_window_page(&mut self, ui: &mut egui::Ui) {
        let models = self
            .catalog
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .map(Vec::as_slice)
            .unwrap_or_default();
        let mut action = self.any_window.draw(ui, self.lang, models, self.app_installer.busy());
        let reviewed = self.any_window.review(ui.ctx(), self.lang, models);
        if reviewed != Action::None {
            action = reviewed;
        }
        match action {
            Action::None => {}
            Action::Back => self.any_window.open = false,
            Action::Refresh => self.any_window_launch(false),
            Action::Start => self.any_window_launch(true),
            Action::Stop => self.any_window.stop(),
            Action::Models => self.any_window_models = true,
        }
        if self.any_window_models {
            let modal = egui::Modal::new(egui::Id::unique("any-window-models"))
                .frame(crate::widgets::modal_frame())
                .show(ui.ctx(), |ui| {
                    ui.set_max_width(560.0);
                    ui.heading(self.tr("any.models_title"));
                    self.model_import_area(ui);
                    self.nr_fetch_card(ui);
                    if ui.button(self.tr("close")).clicked() {
                        self.any_window_models = false;
                    }
                });
            if modal.should_close() {
                self.any_window_models = false;
            }
        }
    }
}
