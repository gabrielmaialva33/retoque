//! One immutable review for ordinary plans and compound component/settings operations.
use crate::{
    app::{refusal_text, step_text},
    autotune,
    commands::{AppCommand, CommandSender},
    i18n::{Lang, diagnostic, t},
    model::PlanKind,
    theme, widgets,
};
use egui::RichText;
use retoque_core::Plan;

/// The start of the install plan's note for a game that has no NR model (retoque-core engine.rs).
const MODEL_NOT_INCLUDED: &str = "Supply your own nvngx_dlssnr.dll";

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
    /// The game folder the review is about. A plan previewed against a staged copy (settings for a game that has no
    /// OptiScaler.ini yet) is shown under this folder, where it will be applied, not under the temporary one.
    pub target: Option<std::path::PathBuf>,
}
impl ReviewSession {
    pub fn allowed(&self) -> bool {
        !self.plans.is_empty()
            && self.plans.iter().all(|p| p.refusals().is_empty())
            && (matches!(self.kind, ReviewKind::Standard(PlanKind::Uninstall))
                || matches!(&self.kind, ReviewKind::Autotune(r) if matches!(r.dlss,Some(crate::dlss_ui::Change::Restore { .. })))
                || self.plans.iter().any(|p| !p.steps().is_empty()))
    }
    /// The review takes something out of a game (Retoque, a component, the updated DLSS files), so its confirm is a
    /// `danger` button.
    pub fn removes(&self) -> bool {
        match &self.kind {
            ReviewKind::Standard(kind) => *kind == PlanKind::Uninstall,
            ReviewKind::Autotune(r) => {
                matches!(r.dlss, Some(crate::dlss_ui::Change::Restore { .. }))
                    || matches!(r.component, Some(autotune::ComponentChange::Remove(_)))
            }
        }
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
    widgets::card_title(ui, t(lang, "inst.plan"));
    ui.label(RichText::new(t(lang, "shell.review_note")).small().color(theme::MUTED));
    egui::ScrollArea::vertical()
        .id_salt("review_steps")
        .max_height((ui.ctx().content_rect().height() - 290.0).max(160.0))
        .show(ui, |ui| {
            // A plan that names its own model file makes the install plan's "bring your own model" note obsolete.
            let model_named = session.plans.iter().any(|p| {
                p.steps().iter().any(|s| {
                    matches!(s, retoque_core::Step::SetIni { section, key, value, .. }
                        if section.eq_ignore_ascii_case("DlssNr") && key.eq_ignore_ascii_case("ModelFile")
                            && !value.trim().eq_ignore_ascii_case("auto"))
                })
            });
            let mut shown_dir: Option<std::path::PathBuf> = None;
            for (index, plan) in session.plans.iter().enumerate() {
                let dir = match &session.target {
                    Some(target) if !plan.game_dir().starts_with(target) && !target.starts_with(plan.game_dir()) => {
                        target.clone()
                    }
                    _ => plan.game_dir().to_owned(),
                };
                // The title names the game; its folder is a detail.
                if shown_dir.as_ref() != Some(&dir) {
                    ui.label(
                        RichText::new(crate::format::display_path(&dir))
                            .small()
                            .color(theme::MUTED),
                    );
                    shown_dir = Some(dir);
                }
                for refusal in plan.refusals() {
                    let (key, detail) = refusal_text(lang, refusal);
                    crate::applog::shown_error(&format!("refusal in {}: {refusal:?}", plan.game_dir().display()));
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
                    // Retoque's own undo records (.retoque/*.ini, components.txt) are bookkeeping, not something the
                    // user decides on: one muted line per file instead of their raw contents.
                    // Removing a file the same plan then restores from its backup is one change for the person
                    // reading this ("restore"), not two: the removal is that restore's first step.
                    let restored: Vec<&str> = plan
                        .steps()
                        .iter()
                        .filter_map(|s| match s {
                            retoque_core::Step::Restore { to, .. } => Some(to.as_str()),
                            _ => None,
                        })
                        .collect();
                    let internal = |s: &retoque_core::Step| {
                        matches!(s, retoque_core::Step::SetIni { file, .. } | retoque_core::Step::UnsetIni { file, .. }
                            if file.starts_with(".retoque/"))
                            || matches!(s, retoque_core::Step::Remove { path } if restored.contains(&path.as_str()))
                    };
                    let visible = plan
                        .steps()
                        .iter()
                        .zip(groups)
                        .filter(|(s, g)| **g == group && !internal(s))
                        .count();
                    let mut records: Vec<&str> = Vec::new();
                    for (step, _) in plan.steps().iter().zip(groups).filter(|(_, g)| **g == group) {
                        if let retoque_core::Step::SetIni { file, .. } | retoque_core::Step::UnsetIni { file, .. } =
                            step
                            && file.starts_with(".retoque/")
                            && !records.contains(&file.as_str())
                        {
                            records.push(file.as_str());
                        }
                    }
                    if visible > 0 {
                        ui.label(RichText::new(format!("{} ({visible})", t(lang, key))).strong());
                    }
                    for (step, _) in plan
                        .steps()
                        .iter()
                        .zip(groups)
                        .filter(|(s, g)| **g == group && !internal(s))
                    {
                        // The verb tells a backup, a rename and a restore apart without relying on their colour.
                        let (verb, text, color) = step_text(lang, step);
                        ui.add(egui::Label::new(RichText::new(format!("{verb}: {text}")).color(color)).wrap());
                    }
                    for file in records {
                        ui.label(
                            RichText::new(format!("{} ({file})", t(lang, "shell.undo_record")))
                                .small()
                                .color(theme::MUTED),
                        );
                    }
                }
                let notes: Vec<&String> = plan
                    .notes()
                    .iter()
                    .filter(|n| !(model_named && n.starts_with(MODEL_NOT_INCLUDED)))
                    .collect();
                if !notes.is_empty() {
                    ui.add_space(12.0);
                    ui.label(RichText::new(t(lang, "inst.notes()")).strong());
                    for note in notes {
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
        // A stale preview hides its confirm (the warning above says to preview again); a refused or empty plan
        // keeps it in place, disabled, with the reason on hover.
        if !stale {
            let button = if session.removes() {
                theme::danger(confirm_label)
            } else {
                theme::primary(confirm_label)
            };
            let allowed = session.allowed();
            let refused = session.plans.iter().any(|p| !p.refusals().is_empty());
            let response = ui.add_enabled(allowed && !busy, button).on_disabled_hover_text(t(
                lang,
                if refused {
                    "shell.resolve"
                } else if !allowed {
                    "inst.nothing"
                } else {
                    "inst.working"
                },
            ));
            if response.clicked() {
                commands.send(AppCommand::ConfirmReview(session.request_id));
            }
        }
    });
}
