//! One immutable review for ordinary plans and compound component/settings operations.
use crate::{
    app::{refusal_text, step_text},
    autotune,
    commands::{AppCommand, CommandSender},
    i18n::{Lang, diagnostic, t},
    model::PlanKind,
    theme,
};
use egui::RichText;
use retoque_core::Plan;

#[derive(Clone)]
pub(crate) enum ReviewKind {
    Standard(PlanKind),
    Autotune(Box<autotune::Request>),
}
#[derive(Clone)]
pub(crate) struct ReviewSession {
    pub title: String,
    pub kind: ReviewKind,
    pub plans: Vec<Plan>,
    pub groups: Vec<Vec<usize>>,
    pub request_id: u64,
}
impl ReviewSession {
    pub fn allowed(&self) -> bool {
        !self.plans.is_empty()
            && self.plans.iter().all(|p| p.refusals().is_empty())
            && (matches!(self.kind, ReviewKind::Standard(PlanKind::Uninstall))
                || matches!(&self.kind, ReviewKind::Autotune(r) if matches!(r.dlss,Some(crate::dlss_ui::Change::Restore { .. })))
                || self.plans.iter().any(|p| !p.steps().is_empty()))
    }
}
pub(crate) fn show(
    ui: &mut egui::Ui,
    session: &ReviewSession,
    lang: Lang,
    stale: bool,
    busy: bool,
    confirm_label: &'static str,
    commands: &CommandSender,
) {
    ui.heading(&session.title);
    ui.label(t(lang, "inst.plan"));
    ui.label(RichText::new(t(lang, "shell.review_note")).small().color(theme::MUTED));
    egui::ScrollArea::vertical()
        .id_salt("review_steps")
        .max_height((ui.ctx().content_rect().height() - 290.0).max(160.0))
        .show(ui, |ui| {
            for (index, plan) in session.plans.iter().enumerate() {
                ui.label(crate::format::display_path(plan.game_dir()));
                for refusal in plan.refusals() {
                    let (key, detail) = refusal_text(lang, refusal);
                    ui.colored_label(theme::BAD, format!("{} {detail}", t(lang, key)));
                    for remedy in crate::error::remedies_for_refusal(refusal) {
                        crate::error::button(ui, remedy, Some(plan.game_dir().to_owned()), lang, commands);
                    }
                }
                if !plan.refusals().is_empty() {
                    ui.label(t(lang, "shell.resolve"));
                }
                if plan.steps().is_empty() {
                    ui.label(t(lang, "inst.nothing"));
                }
                let groups = &session.groups[index];
                for (group, key) in [
                    "shell.add",
                    "shell.replace",
                    "shell.keep",
                    "shell.remove",
                    "shell.settings",
                ]
                .into_iter()
                .enumerate()
                {
                    let count = groups.iter().filter(|g| **g == group).count();
                    if count == 0 {
                        continue;
                    }
                    ui.add_space(10.0);
                    ui.label(RichText::new(format!("{} ({count})", t(lang, key))).strong());
                    for (step, _) in plan.steps().iter().zip(groups).filter(|(_, g)| **g == group) {
                        let (_, text, color) = step_text(lang, step);
                        ui.add(egui::Label::new(RichText::new(text).color(color)).wrap());
                    }
                }
                if !plan.notes().is_empty() {
                    ui.add_space(12.0);
                    ui.label(RichText::new(t(lang, "inst.notes()")).strong());
                    for note in plan.notes() {
                        ui.label(RichText::new(diagnostic(lang, note)).small().color(theme::MUTED));
                    }
                }
            }
        });
    ui.separator();
    if stale {
        ui.colored_label(theme::WARN, t(lang, "inst.stale"));
    }
    ui.horizontal_wrapped(|ui| {
        if ui.button(t(lang, "cancel")).clicked() {
            commands.send(AppCommand::CancelReview);
        }
        if session.allowed() && !stale {
            let button = if matches!(session.kind, ReviewKind::Standard(PlanKind::Uninstall)) {
                egui::Button::new(confirm_label).fill(theme::BAD)
            } else {
                theme::primary(confirm_label)
            };
            if ui.add_enabled(!busy, button).clicked() {
                commands.send(AppCommand::ConfirmReview(session.request_id));
            }
        }
    });
}
