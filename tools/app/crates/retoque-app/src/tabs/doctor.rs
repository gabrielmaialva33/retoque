//! Phase-two integration seam: data supplies findings; every repair remains a reviewed command.
use crate::app::*;
pub(crate) struct DoctorCard {
    pub title: String,
    pub detail: String,
    pub repair: Option<(PlanKind, PlanInputs)>,
}
pub(crate) fn show(ui: &mut egui::Ui, card: &DoctorCard, target: &Path, lang: Lang, commands: &CommandSender) {
    widgets::card(ui, |ui| {
        widgets::card_title(ui, &card.title);
        ui.label(&card.detail);
        if let Some((kind, inputs)) = &card.repair
            && ui.button(t(lang, "doctor.review")).clicked()
        {
            commands.send(AppCommand::RequestPlan(target.to_owned(), *kind, inputs.clone()));
        }
    });
}
