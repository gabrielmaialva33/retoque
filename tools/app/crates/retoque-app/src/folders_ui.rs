//! User-owned discovery roots. Removing a registration never removes files.
use crate::app::*;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FolderKind {
    Game,
    Library,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FolderDialog {
    Add,
    Manage,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct FolderMigration {
    pub moved_to_libraries: Vec<PathBuf>,
    pub dropped_drive_roots: Vec<PathBuf>,
}
impl FolderMigration {
    pub(crate) fn is_empty(&self) -> bool {
        self.moved_to_libraries.is_empty() && self.dropped_drive_roots.is_empty()
    }
    pub(crate) fn merge(&mut self, other: Self) {
        for p in other.moved_to_libraries {
            if !self.moved_to_libraries.contains(&p) {
                self.moved_to_libraries.push(p);
            }
        }
        for p in other.dropped_drive_roots {
            if !self.dropped_drive_roots.contains(&p) {
                self.dropped_drive_roots.push(p);
            }
        }
    }
    pub(crate) fn notice(&self, lang: Lang) -> Option<String> {
        if self.is_empty() {
            return None;
        }
        let moved: Vec<String> = self.moved_to_libraries.iter().map(|p| short_path(p)).collect();
        let dropped: Vec<String> = self.dropped_drive_roots.iter().map(|p| short_path(p)).collect();
        Some(match (lang, moved.is_empty(), dropped.is_empty()) {
            (Lang::En, false, false) => format!(
                "Updated your folders: moved {} to folders with several games; ignored drive root {}.",
                moved.join(", "),
                dropped.join(", ")
            ),
            (Lang::En, false, true) => format!(
                "Updated your folders: moved {} to folders with several games.",
                moved.join(", ")
            ),
            (Lang::En, true, false) => format!("Updated your folders: ignored drive root {}.", dropped.join(", ")),
            (Lang::Pt, false, false) => format!(
                "Suas pastas foram atualizadas: {} movida(s) para pastas com vários jogos; raiz de unidade {} ignorada.",
                moved.join(", "),
                dropped.join(", ")
            ),
            (Lang::Pt, false, true) => format!(
                "Suas pastas foram atualizadas: {} movida(s) para pastas com vários jogos.",
                moved.join(", ")
            ),
            (Lang::Pt, true, false) => format!(
                "Suas pastas foram atualizadas: raiz de unidade {} ignorada.",
                dropped.join(", ")
            ),
            (_, true, true) => unreachable!(),
        })
    }
}
impl Saved {
    fn folder_list(&mut self, kind: FolderKind) -> &mut Vec<PathBuf> {
        match kind {
            FolderKind::Game => &mut self.manual,
            FolderKind::Library => &mut self.libraries,
        }
    }
    pub(crate) fn register_folder(&mut self, kind: FolderKind, path: PathBuf) -> bool {
        if retoque_discovery::is_drive_root(&path) {
            return false;
        }
        let effective_kind = match kind {
            FolderKind::Game
                if retoque_discovery::classify_manual_folder(&path) == retoque_discovery::ManualFolderKind::Library =>
            {
                FolderKind::Library
            }
            other => other,
        };
        let paths = self.folder_list(effective_kind);
        if paths.contains(&path) {
            return false;
        }
        paths.push(path);
        true
    }
    pub(crate) fn forget_folder(&mut self, kind: FolderKind, path: &Path) -> bool {
        let paths = self.folder_list(kind);
        let before = paths.len();
        paths.retain(|p| p != path);
        paths.len() != before
    }
    pub(crate) fn migrate_folders(&mut self) -> FolderMigration {
        let mut migration = FolderMigration::default();
        let old_manual = std::mem::take(&mut self.manual);
        for path in old_manual {
            match retoque_discovery::classify_manual_folder(&path) {
                retoque_discovery::ManualFolderKind::DriveRoot => {
                    let norm = retoque_discovery::normalize_path_buf(&path);
                    if !migration.dropped_drive_roots.contains(&norm) {
                        migration.dropped_drive_roots.push(norm);
                    }
                }
                retoque_discovery::ManualFolderKind::Library => {
                    let norm = retoque_discovery::normalize_path_buf(&path);
                    if !self
                        .libraries
                        .iter()
                        .any(|p| retoque_discovery::normalize_path_buf(p) == norm)
                    {
                        self.libraries.push(norm.clone());
                    }
                    if !migration.moved_to_libraries.contains(&norm) {
                        migration.moved_to_libraries.push(norm);
                    }
                }
                retoque_discovery::ManualFolderKind::SingleGame => {
                    if !self.manual.contains(&path) {
                        self.manual.push(path);
                    }
                }
            }
        }
        let old_libs = std::mem::take(&mut self.libraries);
        for path in old_libs {
            if retoque_discovery::is_drive_root(&path) {
                let norm = retoque_discovery::normalize_path_buf(&path);
                if !migration.dropped_drive_roots.contains(&norm) {
                    migration.dropped_drive_roots.push(norm);
                }
            } else if !self.libraries.contains(&path) {
                self.libraries.push(path);
            }
        }
        migration
    }
}
impl RetoqueApp {
    pub(crate) fn migrate_saved_folders(&mut self) {
        let migration = self.saved.migrate_folders();
        if !migration.is_empty() {
            if let Some(notice) = migration.notice(self.lang) {
                self.toast(notice, None, false);
            }
            self.library_state.migration.merge(migration);
            self.library_state.invalidate();
        }
    }
    pub(crate) fn migration_notice_ui(&mut self, ui: &mut egui::Ui) {
        let Some(notice) = self.library_state.migration.notice(self.lang) else {
            return;
        };
        ui.horizontal(|ui| {
            ui.label(RichText::new(notice).small().color(theme::MUTED));
            if widgets::close_button(ui, self.tr("dismiss.help")).clicked() {
                self.library_state.migration = FolderMigration::default();
            }
        });
    }
    pub(crate) fn folders_open(&mut self, dialog: FolderDialog) {
        self.migrate_saved_folders();
        self.folder_dialog = Some(dialog);
        self.folder_focus = self.context.memory(|m| m.focused());
    }
    pub(crate) fn folders_button(&mut self, ui: &mut egui::Ui) {
        if ui.button(self.tr("folders.title")).clicked() {
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
    pub(crate) fn folders_dialog(&mut self, ctx: &egui::Context) {
        self.migrate_saved_folders();
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
                        self.migration_notice_ui(ui);
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
                                                let width = (ui.available_width() - 160.).max(60.);
                                                ui.add_sized(
                                                    [width, 20.],
                                                    egui::Label::new(short_path(path)).truncate(),
                                                )
                                                .on_hover_text(crate::format::display_path(path));
                                                let response = ui
                                                    .button(self.tr("folders.remove"))
                                                    .on_hover_text(self.tr("folders.remove_help"));
                                                widgets::name_after(
                                                    &response,
                                                    self.tr("folders.remove"),
                                                    &short_path(path),
                                                );
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
    #[test]
    fn migrates_drive_root_steam_library_and_multi_game_folder_with_notice() {
        use egui_kittest::{Harness, kittest::Queryable};

        let temp = tempfile::tempdir().unwrap();
        let multi = temp.path().join("SeveralGames");
        std::fs::create_dir_all(multi.join("GameA")).unwrap();
        std::fs::create_dir_all(multi.join("GameB")).unwrap();
        std::fs::write(multi.join("GameA/GameA.exe"), b"MZ").unwrap();
        std::fs::write(multi.join("GameB/GameB.exe"), b"MZ").unwrap();
        let single = temp.path().join("SingleGame");
        std::fs::create_dir_all(&single).unwrap();
        std::fs::write(single.join("Single.exe"), b"MZ").unwrap();

        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.saved.manual = vec![
                PathBuf::from(r"F:\"),
                PathBuf::from(r"F:\SteamLibrary"),
                multi.clone(),
                single.clone(),
            ];
            let mut h = Harness::builder().with_size([900., 700.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.library(ui);
                },
                app,
            );
            h.run_steps(2);
            assert_eq!(h.state().saved.manual, vec![single.clone()]);
            assert_eq!(
                h.state().saved.libraries,
                vec![PathBuf::from(r"F:\SteamLibrary"), multi.clone()]
            );
            let expected_sub = match lang {
                Lang::En => "Updated your folders: moved",
                Lang::Pt => "Suas pastas foram atualizadas:",
            };
            assert!(h.query_by_label_contains(expected_sub).is_some());
        }

        // Direct registration also refuses drive roots and routes multi-game / SteamLibrary folders to libraries.
        let mut saved = Saved::default();
        assert!(!saved.register_folder(FolderKind::Game, PathBuf::from(r"F:\")));
        assert!(saved.manual.is_empty() && saved.libraries.is_empty());
        assert!(saved.register_folder(FolderKind::Game, PathBuf::from(r"F:\SteamLibrary")));
        assert!(saved.manual.is_empty());
        assert_eq!(saved.libraries, vec![PathBuf::from(r"F:\SteamLibrary")]);
    }
    #[test]
    fn skipped_stale_manifest_renders_as_muted_collapsible_note() {
        use egui_kittest::{Harness, kittest::Queryable};

        let mut app = crate::app_tests::shell_tests::app();
        let stale = r"epic (Hand of Fate 2): install directory not found: F:\Games noSteam\HandOfFate2".to_string();
        app.skipped = vec![stale.clone()];
        let mut h = Harness::builder()
            .with_size([900., 700.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.library(ui), app);
        h.run_steps(2);

        // Collapsed by default: the header shows the count, and the raw path is hidden until expanded.
        assert!(h.query_by_label_contains("Could not read (1)").is_some());
        assert!(h.query_by_label_contains("Hand of Fate 2").is_none());

        h.get_by_label_contains("Could not read (1)").click();
        h.run_steps(2);
        assert!(h.query_by_label_contains("Hand of Fate 2").is_some());
    }
}
