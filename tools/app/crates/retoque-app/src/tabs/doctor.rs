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
    t(lang, key)
}
pub(crate) fn evidence(ui: &mut egui::Ui, lang: Lang, evidence: &[FactEvidence]) {
    for fact in evidence {
        ui.horizontal_wrapped(|ui| {
            ui.strong(text(lang, fact.key));
            ui.label(&fact.value);
        });
    }
}
/// Findings that ask for attention: the badge counts warnings and errors, never information (a newer DLSS
/// being available is news, not a problem).
pub(crate) fn attention_count(findings: &[Finding]) -> usize {
    findings
        .iter()
        .filter(|f| !matches!(f.severity, Severity::Info))
        .count()
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
        // The button names what its review will do, in the words the rest of the app uses for that action.
        let label = match repair {
            Repair::FetchCompatibleModel { .. } => t(lang, "nr_fetch.get_compatible"),
            Repair::UpdateDlss => t(lang, "dlss.update"),
            Repair::Reinstall => t(lang, "shell.reinstall"),
            Repair::ReconcileInstallation => t(lang, "shell.resume"),
            Repair::InstallReShade => t(lang, "rs.install"),
            Repair::ReShadeMenuInput => t(lang, "rs.input_fix"),
            _ => t(lang, "doctor.prepare"),
        };
        let button = ui.add_enabled(enabled, egui::Button::new(label));
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
    // A healthy game needs no sentence: the Doctor shows up only when it has something to say.
    if card.findings.is_empty() {
        return;
    }
    widgets::card(ui, |ui| {
        ui.horizontal(|ui| {
            widgets::card_title(ui, t(lang, "doctor.title"));
            badge(ui, attention_count(&card.findings), lang);
        });
        for finding in &card.findings {
            ui.push_id(finding.id, |ui| {
                let (severity, color) = match finding.severity {
                    Severity::Info => ("severity.info", theme::MUTED),
                    Severity::Error => ("severity.error", theme::BAD),
                    Severity::Warning => ("severity.warning", theme::WARN),
                };
                ui.horizontal_wrapped(|ui| {
                    ui.colored_label(color, text(lang, severity));
                    ui.strong(text(lang, finding.title_key));
                });
                ui.label(text(lang, finding.why_key));
                let title = t(lang, "doctor.evidence");
                widgets::collapsible(ui, "evidence", title, widgets::Fold::Details, |ui| {
                    evidence(ui, lang, &finding.evidence)
                });
                repair_button(ui, finding.repair.as_ref(), target, lang, commands, can_reinstall);
                ui.add_space(8.0);
            });
        }
    });
    ui.add_space(theme::CARD_GAP);
}

/// The library row's mark: a warning dot with the number of findings, painted inside the row's own rect (rows take
/// the full width, so a label after them would wrap), with the explanation on hover. Paints nothing for a healthy
/// game. Screen readers get the same number from `spoken_findings` in the row's own label.
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
        badge_count(count),
        egui::FontId::proportional(if count > 9 { 10.5 } else { 13.0 }),
        theme::ON_ACCENT,
    );
    row.on_hover_text(format!("{} ({count})", t(lang, "doctor.badge")))
}

/// The number drawn inside the row's badge; two digits do not fit the 18 px circle.
fn badge_count(count: usize) -> String {
    if count > 9 { "9+".into() } else { count.to_string() }
}

/// What a screen reader adds to a library row with findings ("2 problemas"); `None` for a healthy game.
pub(crate) fn spoken_findings(count: usize, lang: Lang) -> Option<String> {
    match count {
        0 => None,
        1 => Some(t(lang, "doctor.issues_one").to_owned()),
        n => Some(t(lang, "doctor.issues").replace("{n}", &n.to_string())),
    }
}

/// `count` is the cached per-target count in RetoqueApp::doctor_badges; never diagnose in a row.
pub(crate) fn badge(ui: &mut egui::Ui, count: usize, lang: Lang) {
    if count > 0 {
        ui.colored_label(theme::WARN, "!")
            .on_hover_text(t(lang, "doctor.badge"));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn finding(severity: Severity) -> Finding {
        Finding {
            id: "synthetic",
            severity,
            title_key: "doctor.title",
            why_key: "doctor.title",
            evidence: vec![],
            repair: Some(Repair::UpdateDlss),
        }
    }
    #[test]
    fn the_badge_counts_problems_not_news() {
        // A newer DLSS being available is information: it shows in the card, but no badge or "1 issue".
        assert_eq!(attention_count(&[finding(Severity::Info)]), 0);
        let mixed = [
            finding(Severity::Info),
            finding(Severity::Warning),
            finding(Severity::Error),
        ];
        assert_eq!(attention_count(&mixed), 2);
    }
}
