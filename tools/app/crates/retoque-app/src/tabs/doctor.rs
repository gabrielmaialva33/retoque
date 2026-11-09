//! Evidence is cached by the controller; repairs only enqueue a fresh reviewed plan.
use crate::app::*;
use retoque_data::{
    doctor::{Finding, Repair, Severity},
    recommend::FactEvidence,
};
pub(crate) struct DoctorCard {
    pub findings: Vec<Finding>,
}

pub(crate) fn text(lang: Lang, key: &str) -> &str {
    retoque_data::messages::text(
        key,
        match lang {
            Lang::En => retoque_data::messages::Language::English,
            Lang::Pt => retoque_data::messages::Language::Portuguese,
        },
    )
}
pub(crate) fn evidence(ui: &mut egui::Ui, lang: Lang, evidence: &[FactEvidence]) {
    for fact in evidence {
        ui.horizontal_wrapped(|ui| {
            ui.strong(text(lang, fact.key));
            ui.label(&fact.value);
        });
    }
}
pub(crate) fn repair_button(
    ui: &mut egui::Ui,
    repair: Option<&Repair>,
    target: &Path,
    lang: Lang,
    commands: &CommandSender,
    can_reinstall: bool,
) {
    if let Some(repair) = repair {
        let enabled = !matches!(repair, Repair::Reinstall) || can_reinstall;
        let button = ui.add_enabled(enabled, egui::Button::new(t(lang, "doctor.prepare")));
        if !enabled {
            button.clone().on_hover_text(t(lang, "app.payload.none"));
        }
        if button.clicked() {
            commands.send(AppCommand::PrepareRepair(target.to_owned(), repair.clone()));
        }
    }
}
pub(crate) fn show(
    ui: &mut egui::Ui,
    card: &DoctorCard,
    target: &Path,
    lang: Lang,
    commands: &CommandSender,
    can_reinstall: bool,
) {
    if card.findings.is_empty() {
        ui.label(RichText::new(text(lang, "doctor.healthy")).small().color(theme::MUTED));
        return;
    }
    widgets::card(ui, |ui| {
        ui.horizontal(|ui| {
            widgets::card_title(ui, t(lang, "doctor.title"));
            badge(ui, card.findings.len(), lang);
        });
        for finding in &card.findings {
            ui.push_id(finding.id, |ui| {
                let (severity, color) = match finding.severity {
                    Severity::Error => ("severity.error", theme::BAD),
                    Severity::Warning => ("severity.warning", theme::WARN),
                };
                ui.horizontal_wrapped(|ui| {
                    ui.colored_label(color, text(lang, severity));
                    ui.strong(text(lang, finding.title_key));
                });
                ui.label(text(lang, finding.why_key));
                ui.collapsing(t(lang, "doctor.evidence"), |ui| evidence(ui, lang, &finding.evidence));
                repair_button(ui, finding.repair.as_ref(), target, lang, commands, can_reinstall);
                ui.add_space(8.0);
            });
        }
    });
}

/// The library row's mark: a warning dot painted inside the row's own rect (rows take the full width, so a
/// label after them would wrap), with the explanation on hover. Paints nothing for a healthy game.
pub(crate) fn row_badge(ui: &egui::Ui, row: egui::Response, count: usize, lang: Lang) -> egui::Response {
    if count == 0 {
        return row;
    }
    let center = egui::pos2(row.rect.right() - 16.0, row.rect.center().y);
    let painter = ui.painter_at(row.rect);
    painter.circle_filled(center, 9.0, theme::WARN);
    painter.text(
        center,
        egui::Align2::CENTER_CENTER,
        "!",
        egui::FontId::proportional(13.0),
        egui::Color32::from_rgb(0x1A, 0x14, 0x0A),
    );
    row.on_hover_text(format!("{} ({count})", t(lang, "doctor.badge")))
}

/// `count` is the cached per-target count in RetoqueApp::doctor_badges; never diagnose in a row.
pub(crate) fn badge(ui: &mut egui::Ui, count: usize, lang: Lang) {
    if count > 0 {
        ui.colored_label(theme::WARN, "!")
            .on_hover_text(t(lang, "doctor.badge"));
    }
}
