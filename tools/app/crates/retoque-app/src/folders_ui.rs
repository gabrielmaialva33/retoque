//! User-owned discovery roots. Removing a registration never removes files.
use super::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum FolderKind {
    Game,
    Library,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum FolderDialog {
    Add,
    Manage,
}
impl Saved {
    fn folder_list(&mut self, kind: FolderKind) -> &mut Vec<PathBuf> {
        match kind {
            FolderKind::Game => &mut self.manual,
            FolderKind::Library => &mut self.libraries,
        }
    }
    pub(super) fn register_folder(&mut self, kind: FolderKind, path: PathBuf) -> bool {
        let paths = self.folder_list(kind);
        if paths.contains(&path) {
            return false;
        }
        paths.push(path);
        true
    }
    pub(super) fn forget_folder(&mut self, kind: FolderKind, path: &Path) -> bool {
        let paths = self.folder_list(kind);
        let before = paths.len();
        paths.retain(|p| p != path);
        paths.len() != before
    }
}
impl RetoqueApp {
    pub(super) fn folders_open(&mut self, dialog: FolderDialog) {
        self.folder_dialog = Some(dialog);
        self.folder_focus = self.context.memory(|m| m.focused());
    }
    pub(super) fn folders_button(&mut self, ui: &mut egui::Ui) {
        if ui.small_button(self.tr("folders.title")).clicked() {
            self.folders_open(FolderDialog::Manage);
        }
    }
    fn folders_pick(&mut self, kind: FolderKind) {
        let title = self.tr(match kind {
            FolderKind::Game => "folders.game",
            FolderKind::Library => "folders.library",
        });
        if let Some(path) = rfd::FileDialog::new().set_title(title).pick_folder() {
            if self.saved.register_folder(kind, path) {
                self.rescan();
            }
            self.folder_dialog = Some(FolderDialog::Manage);
        }
    }
    pub(super) fn folders_dialog(&mut self, ctx: &egui::Context) {
        if self.folder_restore > 0 {
            if let Some(id) = self.folder_focus {
                ctx.memory_mut(|m| m.request_focus(id));
            }
            self.folder_restore -= 1;
            ctx.request_repaint();
        }
        let Some(dialog) = self.folder_dialog else { return };
        let modal = egui::Modal::new(egui::Id::unique("user_folders"))
            .frame(
                egui::Frame::new()
                    .fill(theme::CARD)
                    .stroke(egui::Stroke::new(1., theme::CARD_EDGE))
                    .corner_radius(12)
                    .inner_margin(20),
            )
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.).clamp(260., 620.));
                match dialog {
                    FolderDialog::Add => {
                        ui.heading(self.tr("lib.add_folder"));
                        ui.add(egui::Label::new(self.tr("folders.question")).wrap());
                        ui.add_space(12.);
                        if ui.button(self.tr("folders.game")).clicked() {
                            self.folders_pick(FolderKind::Game);
                        }
                        ui.add(
                            egui::Label::new(RichText::new(self.tr("folders.game_help")).small().color(theme::MUTED))
                                .wrap(),
                        );
                        ui.add_space(12.);
                        if ui.button(self.tr("folders.library")).clicked() {
                            self.folders_pick(FolderKind::Library);
                        }
                        ui.add(
                            egui::Label::new(
                                RichText::new(self.tr("folders.library_help"))
                                    .small()
                                    .color(theme::MUTED),
                            )
                            .wrap(),
                        );
                    }
                    FolderDialog::Manage => {
                        ui.heading(self.tr("folders.title"));
                        ui.add(egui::Label::new(self.tr("folders.help")).wrap());
                        let mut remove = None;
                        egui::ScrollArea::vertical()
                            .id_salt("folder_entries")
                            .max_height((ctx.content_rect().height() - 250.).clamp(120., 420.))
                            .show(ui, |ui| {
                                if self.saved.manual.is_empty() && self.saved.libraries.is_empty() {
                                    ui.label(self.tr("folders.empty"));
                                }
                                for (kind, paths) in [
                                    (FolderKind::Game, &self.saved.manual),
                                    (FolderKind::Library, &self.saved.libraries),
                                ] {
                                    if paths.is_empty() {
                                        continue;
                                    }
                                    ui.add_space(10.);
                                    ui.label(
                                        RichText::new(self.tr(match kind {
                                            FolderKind::Game => "folders.games",
                                            FolderKind::Library => "folders.libraries",
                                        }))
                                        .strong(),
                                    );
                                    for path in paths {
                                        ui.push_id((kind as u8, path), |ui| {
                                            ui.horizontal(|ui| {
                                                let width = (ui.available_width() - 100.).max(60.);
                                                ui.add_sized(
                                                    [width, 20.],
                                                    egui::Label::new(short_path(path)).truncate(),
                                                )
                                                .on_hover_text(path.display().to_string());
                                                let name =
                                                    format!("{} · {}", self.tr("folders.remove"), short_path(path));
                                                let response = ui
                                                    .small_button(self.tr("folders.remove"))
                                                    .on_hover_text(self.tr("folders.remove_help"));
                                                response.widget_info(|| {
                                                    egui::WidgetInfo::labeled(egui::Role::Button, true, name.clone())
                                                });
                                                if response.clicked() {
                                                    remove = Some((kind, path.clone()));
                                                }
                                            });
                                        });
                                    }
                                }
                            });
                        if let Some((kind, path)) = remove
                            && self.saved.forget_folder(kind, &path)
                        {
                            self.rescan();
                        }
                        ui.add_space(12.);
                        if ui.button(self.tr("lib.add_folder")).clicked() {
                            self.folder_dialog = Some(FolderDialog::Add);
                        }
                    }
                }
                ui.separator();
                let close = self.tr(if dialog == FolderDialog::Add {
                    "cancel"
                } else {
                    "folders.close"
                });
                if ui.button(close).clicked() {
                    self.folder_dialog = None;
                }
            });
        if modal.should_close() {
            self.folder_dialog = None;
        }
        if self.folder_dialog.is_none() {
            self.folder_restore = 2;
            ctx.request_repaint();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn both_kinds_persist_and_old_saved_state_defaults_libraries() {
        let mut saved: Saved =
            serde_json::from_str(r#"{"lang":null,"manual":[],"payload":null,"targets":{}}"#).unwrap();
        assert!(saved.libraries.is_empty());
        assert!(saved.register_folder(FolderKind::Game, "/synthetic/one".into()));
        assert!(saved.register_folder(FolderKind::Library, "/synthetic/many".into()));
        assert!(!saved.register_folder(FolderKind::Library, "/synthetic/many".into()));
        let again: Saved = serde_json::from_str(&serde_json::to_string(&saved).unwrap()).unwrap();
        assert_eq!(again.manual, vec![PathBuf::from("/synthetic/one")]);
        assert_eq!(again.libraries, vec![PathBuf::from("/synthetic/many")]);
    }
    #[test]
    fn removal_only_forgets_selected_registration_and_preserves_files() {
        let root = tempfile::tempdir().unwrap();
        let file = root.path().join("game.exe");
        std::fs::write(&file, b"MZ synthetic").unwrap();
        let mut saved = Saved::default();
        saved.manual.push(root.path().into());
        saved.libraries.push(root.path().into());
        assert!(saved.forget_folder(FolderKind::Library, root.path()));
        assert!(saved.libraries.is_empty());
        assert_eq!(saved.manual.len(), 1);
        assert_eq!(std::fs::read(&file).unwrap(), b"MZ synthetic");
        assert!(!saved.forget_folder(FolderKind::Library, root.path()));
    }
}
