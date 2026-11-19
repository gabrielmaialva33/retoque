//! Selected-game evidence is read on the worker; review acknowledgment is scoped to the game identity.
use crate::{
    app::RetoqueApp,
    i18n::{Lang, t},
    modals::review::{ReviewKind, ReviewSession},
    model::PlanKind,
};
use retoque_core::{
    pe_version::Machine,
    safety::{AntiCheatScan, Executables},
};
use std::{collections::BTreeMap, path::PathBuf};

#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    pub anti_cheat: AntiCheatScan,
    pub architectures: BTreeMap<PathBuf, Executables>,
}
impl Snapshot {
    pub fn read(root: PathBuf, targets: Vec<PathBuf>) -> Self {
        Self {
            anti_cheat: retoque_core::safety::anti_cheat(&root),
            architectures: targets
                .into_iter()
                .map(|p| {
                    let e = retoque_core::safety::executables(&p);
                    (p, e)
                })
                .collect(),
        }
    }
    pub fn warning(&self, lang: Lang) -> Option<String> {
        if self.anti_cheat.findings.is_empty() {
            return None;
        }
        let evidence = self
            .anti_cheat
            .findings
            .iter()
            .map(|f| format!("{} ({})", f.product, f.path.display()))
            .collect::<Vec<_>>()
            .join("; ");
        Some(format!(
            "{}\n{}",
            t(lang, "safety.anti_cheat").replace("{evidence}", &evidence),
            t(lang, "safety.anti_cheat_risk")
        ))
    }
    pub fn bitness(&self, target: &std::path::Path, lang: Lang) -> String {
        let Some(exes) = self.architectures.get(target) else {
            return String::new();
        };
        exes.files
            .iter()
            .filter_map(|(name, machine)| {
                let key = match machine {
                    Machine::X64 => "safety.x64",
                    Machine::X86 => "safety.x86",
                    Machine::Arm64 => "safety.arm64",
                    Machine::Unknown => return None,
                };
                Some(format!("{name}: {}", t(lang, key)))
            })
            .collect::<Vec<_>>()
            .join(" · ")
    }
}
fn installs(review: &ReviewSession) -> bool {
    matches!(review.kind, ReviewKind::Standard(PlanKind::Install))
        || matches!(&review.kind, ReviewKind::Autotune(r) if matches!(r.component, Some(crate::autotune::ComponentChange::Retoque { .. })))
}
impl RetoqueApp {
    fn safety_key(&self) -> Option<String> {
        self.selected_game()
            .map(|g| format!("{}|{}", g.id, crate::format::path_key(&g.install_dir)))
            .or_else(|| self.view.target.as_ref().map(|p| crate::format::path_key(p)))
    }
    pub(crate) fn safety_allowed(&self, review: &ReviewSession) -> bool {
        if !installs(review) {
            return true;
        }
        let Some(evidence) = &self.view.safety else {
            return false;
        };
        (evidence.anti_cheat.findings.is_empty()
            && !review
                .plans
                .iter()
                .flat_map(|p| p.notes())
                .any(|n| n.starts_with(retoque_core::safety::ANTI_CHEAT_NOTE)))
            || self
                .safety_key()
                .is_some_and(|key| self.saved.anti_cheat_acknowledged.contains(&key))
            || self.view.safety_ack == Some(review.request_id)
    }
    pub(crate) fn remember_safety_ack(&mut self, review: &ReviewSession) {
        if installs(review)
            && self.view.safety_ack == Some(review.request_id)
            && let Some(key) = self.safety_key()
        {
            self.saved.anti_cheat_acknowledged.insert(key);
        }
    }
    pub(crate) fn safety_review(&mut self, ui: &mut egui::Ui) {
        let Some(Ok(review)) = &self.review else {
            return;
        };
        if !installs(review) {
            return;
        }
        let request_id = review.request_id;
        let Some(evidence) = &self.view.safety else {
            crate::widgets::busy(ui, self.lang, t(self.lang, "safety.reading"), None, None);
            return;
        };
        let Some(warning) = evidence.warning(self.lang).or_else(|| {
            review
                .plans
                .iter()
                .flat_map(|p| p.notes())
                .find(|n| n.starts_with(retoque_core::safety::ANTI_CHEAT_NOTE))
                .map(|n| crate::i18n::diagnostic(self.lang, n))
        }) else {
            return;
        };
        if self
            .safety_key()
            .is_some_and(|key| self.saved.anti_cheat_acknowledged.contains(&key))
        {
            return;
        }
        crate::error::notice(ui, &crate::error::AppError::warning(warning));
        let mut checked = self.view.safety_ack == Some(request_id);
        if ui.checkbox(&mut checked, t(self.lang, "safety.acknowledge")).changed() {
            self.view.safety_ack = checked.then_some(request_id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::{
        app::{PlanInputs, Saved},
        app_tests::shell_tests::app,
        autotune,
        modals::review::ReviewSession,
    };
    use egui_kittest::{
        Harness,
        kittest::{NodeT, Queryable},
    };
    use retoque_core::{InstallOptions, safety::AntiCheatFinding};

    #[test]
    fn acknowledgment_gates_both_reviews_and_survives_saved_state() {
        for lang in [Lang::En, Lang::Pt] {
            for compound in [false, true] {
                let temp = tempfile::tempdir().unwrap();
                let game = temp.path().join("game");
                let source = temp.path().join("source");
                std::fs::create_dir(&game).unwrap();
                std::fs::create_dir(game.join("BattlEye")).unwrap();
                std::fs::create_dir(&source).unwrap();
                std::fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as synthetic").unwrap();
                std::fs::write(source.join("nvngx.dll_dlssnr.dll"), b"MZ dlssnr_call_create synthetic").unwrap();
                let options = InstallOptions {
                    game_dir: game.clone(),
                    source_dir: source.clone(),
                    copy_nvngx: false,
                    ..Default::default()
                };
                let plan = retoque_core::plan_install(&options).unwrap();
                let review = ReviewSession {
                    title: t(lang, "inst.install").into(),
                    kind: if compound {
                        ReviewKind::Autotune(Box::new(autotune::Request::default().with_install(options.clone())))
                    } else {
                        ReviewKind::Standard(PlanKind::Install)
                    },
                    groups: vec![vec![0; plan.steps().len()]],
                    plans: vec![plan],
                    request_id: 42,
                    target: Some(game.clone()),
                };
                let mut app = app();
                app.lang = lang;
                app.payload = Some(source);
                app.view.target = Some(game.clone());
                app.view.options = options;
                app.view.inputs = app.install_inputs().map(PlanInputs::Install);
                app.view.safety = Some(Snapshot {
                    anti_cheat: AntiCheatScan {
                        findings: vec![AntiCheatFinding {
                            product: "BattlEye",
                            path: "BattlEye".into(),
                        }],
                        complete: true,
                    },
                    ..Default::default()
                });
                app.review = Some(Ok(review.clone()));
                app.review_open = true;
                // Direct commands must not bypass the disabled button.
                app.confirm_review(42);
                assert!(app.writing.is_empty());
                assert!(app.saved.anti_cheat_acknowledged.is_empty());
                let mut harness = Harness::builder().with_size([1000., 1200.]).build_ui_state(
                    |ui, app: &mut RetoqueApp| {
                        app.context = ui.ctx().clone();
                        app.review_sheet(ui.ctx());
                        app.handle_commands();
                    },
                    app,
                );
                let confirm = if compound {
                    t(lang, "auto.confirm")
                } else {
                    t(lang, "inst.install")
                };
                assert!(
                    harness
                        .get_by_role_and_label(egui::Role::Button, confirm)
                        .accesskit_node()
                        .is_disabled()
                );
                harness.get_by_label(t(lang, "safety.acknowledge")).click();
                harness.run_steps(3);
                assert!(
                    !harness
                        .get_by_role_and_label(egui::Role::Button, confirm)
                        .accesskit_node()
                        .is_disabled()
                );
                assert!(
                    harness.state().saved.anti_cheat_acknowledged.is_empty(),
                    "checking alone does not persist consent"
                );
                assert_eq!(
                    std::fs::read_dir(&game).unwrap().count(),
                    1,
                    "review has not written to the synthetic game"
                );
                harness.get_by_role_and_label(egui::Role::Button, confirm).click();
                harness.run_steps(3);
                let deadline = std::time::Instant::now() + std::time::Duration::from_secs(30);
                while harness.state().jobs.busy() {
                    harness.state_mut().receive();
                    assert!(std::time::Instant::now() < deadline, "synthetic apply timed out");
                    std::thread::yield_now();
                }
                assert!(!harness.state().saved.anti_cheat_acknowledged.is_empty());
                harness.state_mut().review = Some(Ok(review.clone()));
                harness.state_mut().review_open = true;
                let json = serde_json::to_string(&harness.state().saved).unwrap();
                let restored: Saved = serde_json::from_str(&json).unwrap();
                harness.state_mut().saved = restored;
                harness.state_mut().view.safety_ack = None;
                harness.run_steps(3);
                assert!(harness.query_by_label(t(lang, "safety.acknowledge")).is_none());
                assert!(harness.state().safety_allowed(&review));
                harness.state_mut().view.target = Some(temp.path().join("other"));
                assert!(!harness.state().safety_allowed(&review), "acknowledgment is per game");
            }
        }
    }
    #[test]
    fn old_saved_state_and_no_finding_do_not_add_a_gate_to_settings_or_removal() {
        let mut value = serde_json::to_value(Saved::default()).unwrap();
        value.as_object_mut().unwrap().remove("anti_cheat_acknowledged");
        let saved: Saved = serde_json::from_value(value).unwrap();
        assert!(saved.anti_cheat_acknowledged.is_empty());
        let mut app = app();
        app.view.safety = Some(Snapshot::default());
        let mut review = ReviewSession {
            title: String::new(),
            kind: ReviewKind::Standard(PlanKind::Install),
            plans: vec![],
            groups: vec![],
            request_id: 1,
            target: None,
        };
        assert!(app.safety_allowed(&review));
        app.view.safety = None;
        assert!(!app.safety_allowed(&review));
        for kind in [PlanKind::Settings, PlanKind::Uninstall] {
            review.kind = ReviewKind::Standard(kind);
            assert!(app.safety_allowed(&review));
        }
    }
}
