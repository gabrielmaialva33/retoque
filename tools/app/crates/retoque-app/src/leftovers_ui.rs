//! What uninstalled games left behind: a card on the home screen and a review before anything is removed. The
//! scan already inspected each folder (`retoque_core::leftovers`); removal re-checks it and keeps anything that is
//! not Retoque's.

use crate::app::*;

/// "1,9 GB", "330 MB", "12 KB", with the language's decimal separator.
pub(crate) use crate::format::size_text;

fn folder_name(dir: &std::path::Path) -> String {
    dir.file_name()
        .map(|n| n.to_string_lossy().into_owned())
        .unwrap_or_else(|| crate::format::display_path(dir))
}

impl RetoqueApp {
    pub(crate) fn leftovers_card(&mut self, ui: &mut egui::Ui) {
        if self.library_state.leftovers.is_empty() {
            return;
        }
        let bytes: u64 = self.library_state.leftovers.iter().map(|l| l.remove_bytes()).sum();
        let count = self.library_state.leftovers.len();
        widgets::card(ui, |ui| {
            widgets::card_title(ui, self.tr("left.title"));
            ui.add(egui::Label::new(RichText::new(self.tr("left.why")).color(theme::MUTED)).wrap());
            ui.label(
                self.tr("left.summary")
                    .replace("{count}", &count.to_string())
                    .replace("{size}", &size_text(self.lang, bytes)),
            );
            ui.horizontal(|ui| {
                let busy = self.library_state.leftovers_busy;
                if ui
                    .add_enabled(!busy && !self.busy(), egui::Button::new(self.tr("left.review")))
                    .clicked()
                {
                    self.library_state.leftovers_review = true;
                }
                if busy {
                    ui.spinner();
                    ui.label(RichText::new(self.tr("left.removing")).small());
                }
            });
        });
        ui.add_space(theme::CARD_GAP);
    }

    pub(crate) fn leftovers_modal(&mut self, ctx: &egui::Context) {
        if !self.library_state.leftovers_review {
            return;
        }
        let lang = self.lang;
        let mut confirm = false;
        let mut cancel = false;
        let bytes: u64 = self.library_state.leftovers.iter().map(|l| l.remove_bytes()).sum();
        let response = egui::Modal::new(egui::Id::unique("leftovers_review")).show(ctx, |ui| {
            ui.set_max_width(640.0_f32.min(ctx.content_rect().width() - 48.0));
            ui.heading(t(lang, "left.review_title"));
            ui.add(egui::Label::new(RichText::new(t(lang, "left.review_note")).small().color(theme::MUTED)).wrap());
            ui.add_space(8.0);
            egui::ScrollArea::vertical()
                .max_height((ctx.content_rect().height() - 260.0).max(160.0))
                .show(ui, |ui| {
                    for leftover in &self.library_state.leftovers {
                        ui.push_id(&leftover.dir, |ui| {
                            ui.horizontal(|ui| {
                                ui.label(RichText::new(folder_name(&leftover.dir)).strong());
                                ui.label(
                                    RichText::new(
                                        t(lang, "left.folder_line")
                                            .replace("{files}", &leftover.remove.len().to_string())
                                            .replace("{size}", &size_text(lang, leftover.remove_bytes())),
                                    )
                                    .color(theme::TEXT),
                                );
                            });
                            ui.label(
                                RichText::new(crate::format::display_path(&leftover.dir))
                                    .small()
                                    .monospace()
                                    .color(theme::MUTED),
                            );
                            if !leftover.keep.is_empty() {
                                let keeps = t(lang, "left.keeps").replace("{count}", &leftover.keep.len().to_string());
                                widgets::collapsible(ui, "keeps", &keeps, widgets::Fold::Details, |ui| {
                                    for path in leftover.keep.iter().take(12) {
                                        ui.label(RichText::new(path).small().monospace());
                                    }
                                    if leftover.keep.len() > 12 {
                                        ui.label(RichText::new("…").small());
                                    }
                                });
                            }
                            ui.add_space(8.0);
                        });
                    }
                });
            ui.separator();
            ui.horizontal(|ui| {
                cancel = ui.button(t(lang, "cancel")).clicked();
                let label = t(lang, "left.confirm").replace("{size}", &size_text(lang, bytes));
                confirm = ui
                    .add(egui::Button::new(RichText::new(label).strong()).fill(theme::BAD))
                    .clicked();
            });
        });
        if response.should_close() {
            cancel = true;
        }
        if confirm {
            self.library_state.leftovers_review = false;
            self.library_state.leftovers_busy = true;
            let reviewed = self.library_state.leftovers.clone();
            self.jobs
                .spawn("leftovers", None, move |_| crate::model::remove_leftovers(reviewed));
        } else if cancel {
            self.library_state.leftovers_review = false;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};
    use std::fs;

    #[test]
    fn review_lists_what_goes_and_what_stays_then_removes_only_retoques_files_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let dir = temp.path().join("Subnautica2");
            let bin = dir.join("Subnautica2/Binaries/Win64");
            fs::create_dir_all(&bin).unwrap();
            for f in ["dxgi.dll", "OptiScaler.ini", "nvngx.dll_dlssnr.dll"] {
                fs::write(bin.join(f), b"0123456789").unwrap();
            }
            let save = dir.join("Subnautica2/Saved/SaveGames/steam_autocloud.vdf");
            fs::create_dir_all(save.parent().unwrap()).unwrap();
            fs::write(&save, b"save").unwrap();
            let leftover = retoque_core::leftovers::inspect(&dir).unwrap();

            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.library_state.leftovers = vec![leftover];
            let mut h = Harness::builder().with_size([1000., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.leftovers_card(ui);
                    app.leftovers_modal(ui.ctx());
                },
                app,
            );
            h.get_by_label(t(lang, "left.title"));
            h.get_by_label_contains(&t(lang, "left.summary").replace("{count}", "1").replace("{size}", ""));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "left.review"))
                .click();
            h.run_steps(3);
            h.get_by_label(t(lang, "left.review_title"));
            h.get_by_label("Subnautica2");
            h.get_by_label_contains(&t(lang, "left.keeps").replace("{count}", "1"));
            let confirm = t(lang, "left.confirm").replace("{size}", &size_text(lang, 30));
            h.get_by_role_and_label(egui::Role::Button, &confirm);
            // Cancel closes without touching anything.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
            h.run_steps(3);
            assert!(!h.state().library_state.leftovers_review);
            assert!(bin.join("dxgi.dll").is_file());

            // What the confirm button runs: only Retoque's files go; the save and its folder stay.
            let reviewed = h.state().library_state.leftovers.clone();
            match crate::model::remove_leftovers(reviewed) {
                crate::model::Outcome::LeftoversRemoved(Ok(done)) => assert_eq!((done.files, done.bytes), (3, 30)),
                _ => panic!("cleanup failed"),
            }
            assert!(save.is_file());
            assert!(!dir.join("Subnautica2/Binaries").exists());
        }
    }

    #[test]
    fn sizes_read_naturally_in_both_languages() {
        assert_eq!(size_text(Lang::Pt, 2_040_109_465), "1,9 GB");
        assert_eq!(size_text(Lang::En, 2_040_109_465), "1.9 GB");
        assert_eq!(size_text(Lang::Pt, 330 * 1024 * 1024), "330 MB");
        assert_eq!(size_text(Lang::En, 10), "1 KB");
    }
}
