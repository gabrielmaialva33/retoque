use super::*;
use egui_kittest::{
    Harness,
    kittest::{NodeT, Queryable},
};
fn harness(lang: Lang, size: [f32; 2], appearance: theme::Appearance, target: Target) -> Harness<'static, RetoqueApp> {
    let mut app = crate::app_tests::shell_tests::app();
    app.lang = lang;
    app.prefs.languages.open = true;
    app.prefs.languages.pack = Pack::new(target, Lang::De);
    app.prefs.languages.selected = if target == Target::App { "prefs.title" } else { "Reset" }.into();
    if target == Target::App {
        app.prefs.languages.search = "prefs.".into();
    }
    Harness::builder()
        .with_size(size)
        .with_theme(if appearance == theme::Appearance::Light {
            egui::Theme::Light
        } else {
            egui::Theme::Dark
        })
        .with_context_setup(move |ctx| {
            theme::apply(ctx);
            appearance.apply(ctx);
        })
        .build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.languages_dialog(ui.ctx());
            },
            app,
        )
}
#[test]
fn dialog_matrix_is_accessible_and_fits() {
    for lang in [Lang::En, Lang::Pt] {
        for size in [[1024.0, 768.0], [820.0, 520.0]] {
            for appearance in [theme::Appearance::Dark, theme::Appearance::Light] {
                let mut h = harness(lang, size, appearance, Target::App);
                h.run_steps(3);
                assert_eq!(
                    h.state().context.theme(),
                    if appearance == theme::Appearance::Light {
                        egui::Theme::Light
                    } else {
                        egui::Theme::Dark
                    }
                );
                let bounds = egui::Rect::from_min_size(egui::Pos2::ZERO, size.into()).expand(1.0);
                let close = h.get_by_role_and_label(egui::Role::Button, t(lang, "close"));
                assert!(
                    bounds.contains_rect(close.rect()),
                    "{lang:?} {size:?} {:?}",
                    close.rect()
                );
                h.get_by_label(t(lang, "languages.search"));
                h.get_by_label(t(lang, "languages.untranslated"));
                h.get_by_label(t(lang, "languages.changed"));
                let apply = h.get_by_role_and_label(egui::Role::Button, t(lang, "languages.apply"));
                assert!(bounds.contains_rect(apply.rect()));
                h.key_press(egui::Key::Escape);
                h.run_steps(3);
                assert!(!h.state().prefs.languages.open);
            }
        }
    }
}
#[test]
fn typing_invalid_draft_disables_apply_and_reset_requires_confirmation() {
    let mut h = harness(Lang::En, [1024., 768.], theme::Appearance::Dark, Target::App);
    h.state_mut().prefs.languages.selected = "prefs.open".into();
    h.state_mut()
        .prefs
        .languages
        .document
        .drafts
        .insert("prefs.open".into(), "Invalid".into());
    h.run_steps(3);
    assert!(
        h.get_by_role_and_label(egui::Role::Button, "Apply")
            .accesskit_node()
            .is_disabled()
    );
    h.get_by_role_and_label(egui::Role::Button, "Reset pack…").click();
    h.run_steps(3);
    h.get_by_label("Delete corrections?");
    h.get_by_role_and_label(egui::Role::Button, "Cancel").click();
    h.run_steps(3);
    assert_eq!(h.state().prefs.languages.document.drafts["prefs.open"], "Invalid");
}
#[cfg(feature = "visual-tests")]
#[test]
#[ignore = "manual offscreen renders to the caller's scratch folder"]
fn render_language_editor_matrix() {
    for target in [Target::App, Target::Menu] {
        let out = PathBuf::from(std::env::var("RETOQUE_LOOK_SHOTS").expect("scratch output folder"));
        std::fs::create_dir_all(&out).unwrap();
        for lang in [Lang::En, Lang::Pt] {
            for size in [[1024., 768.], [820., 520.]] {
                for appearance in [theme::Appearance::Dark, theme::Appearance::Light] {
                    let mut h = harness(lang, size, appearance, target);
                    h.run_steps(4);
                    h.render()
                        .unwrap()
                        .save(out.join(format!(
                            "app-i18n-editor-{}-{}-{}-{}.png",
                            target.folder(),
                            lang.code(),
                            size[0],
                            if appearance == theme::Appearance::Dark {
                                "dark"
                            } else {
                                "light"
                            }
                        )))
                        .unwrap();
                }
            }
        }
    }
}
#[test]
fn editing_saves_drafts_and_apply_uses_them_only_after_the_click() {
    let root = tempfile::tempdir().unwrap();
    let mut h = harness(Lang::En, [1024., 768.], theme::Appearance::Light, Target::App);
    h.state_mut().prefs.languages.root = Some(root.path().to_owned());
    h.state_mut()
        .prefs
        .languages
        .document
        .drafts
        .insert("prefs.title".into(), String::new());
    h.run_steps(3);
    h.get_by_role_and_label(egui::Role::MultilineTextInput, "Draft").focus();
    h.run_steps(2);
    h.get_by_role_and_label(egui::Role::MultilineTextInput, "Draft")
        .type_text("Reviewed");
    h.run_steps(3);
    let pack = Pack::new(Target::App, Lang::De);
    let doc = Document::load(root.path(), &pack).unwrap();
    assert_eq!(doc.drafts["prefs.title"], "Reviewed");
    assert!(doc.applied.is_empty());
    h.get_by_role_and_label(egui::Role::Button, "Apply").click();
    h.run_steps(3);
    assert_eq!(crate::i18n::t(Lang::De, "prefs.title"), "Reviewed");
    assert_eq!(
        Document::load(root.path(), &pack).unwrap().applied["prefs.title"],
        "Reviewed"
    );
    // A reset of one key must not apply an unrelated, invalid pending draft.
    h.state_mut()
        .prefs
        .languages
        .document
        .drafts
        .insert("prefs.open".into(), "Invalid".into());
    h.state_mut().prefs.languages.reset = Some(Some("prefs.title".into()));
    h.run_steps(3);
    h.get_by_role_and_label(egui::Role::Button, "Delete").click();
    h.run_steps(3);
    let doc = Document::load(root.path(), &pack).unwrap();
    assert_eq!(doc.drafts["prefs.open"], "Invalid");
    assert!(doc.applied.is_empty());
    assert_eq!(crate::i18n::t(Lang::De, "prefs.title"), pack.base("prefs.title"));
}

#[test]
fn menu_dialog_matrix_apply_selects_install_preference_without_a_game() {
    for lang in [Lang::En, Lang::Pt] {
        for size in [[1024., 768.], [820., 520.]] {
            for appearance in [theme::Appearance::Dark, theme::Appearance::Light] {
                let root = tempfile::tempdir().unwrap();
                let mut h = harness(lang, size, appearance, Target::Menu);
                h.state_mut().prefs.languages.root = Some(root.path().to_owned());
                h.state_mut()
                    .prefs
                    .languages
                    .document
                    .drafts
                    .insert("Reset".into(), "Reviewed".into());
                h.run_steps(3);
                let bounds = egui::Rect::from_min_size(egui::Pos2::ZERO, size.into()).expand(1.0);
                let apply = h.get_by_role_and_label(egui::Role::Button, t(lang, "languages.apply"));
                assert!(bounds.contains_rect(apply.rect()));
                apply.click();
                h.run_steps(3);
                assert_eq!(
                    h.state().saved.install_defaults.menu_language,
                    crate::prefs_ui::MenuLanguagePref::Pack("de".into())
                );
                assert!(matches!(
                    h.state()
                        .prefs
                        .languages
                        .menu_language(retoque_core::MenuLanguage::Pack("de".into())),
                    retoque_core::MenuLanguage::UserPack(_)
                ));
                assert!(h.state().view.target.is_none());
            }
        }
    }
}

#[test]
fn import_worker_result_merges_sparse_drafts_and_rejects_whole_invalid_files() {
    let root = tempfile::tempdir().unwrap();
    let mut state = State {
        root: Some(root.path().to_owned()),
        ..Default::default()
    };
    state.document.drafts.insert("prefs.title".into(), "Keep me".into());
    let (tx, rx) = mpsc::channel();
    state.picker = Some(rx);
    tx.send(Ok(FileResult::Import(
        b"prefs.title\tValid first row\nnot_a_translation_key\tNo\n".to_vec(),
    )))
    .unwrap();
    state.poll();
    assert_eq!(state.error, Some("unknown"));
    assert_eq!(state.document.drafts["prefs.title"], "Keep me");
    let (tx, rx) = mpsc::channel();
    state.picker = Some(rx);
    tx.send(Ok(FileResult::Import(b"prefs.open\tReviewed...\n".to_vec())))
        .unwrap();
    state.poll();
    assert_eq!(state.error, Some("ellipsis"));
    assert!(!state.document.drafts.contains_key("prefs.open"));
    let (tx, rx) = mpsc::channel();
    state.picker = Some(rx);
    tx.send(Ok(FileResult::Import("prefs.open\tReviewed…\n".as_bytes().to_vec())))
        .unwrap();
    state.poll();
    assert!(state.error.is_none());
    assert_eq!(state.document.drafts.len(), 2);
    assert!(state.document.applied.is_empty());
}

#[test]
fn storage_failure_does_not_trap_the_app_or_drop_pending_drafts() {
    let mut h = harness(Lang::En, [820., 520.], theme::Appearance::Dark, Target::App);
    h.state_mut()
        .prefs
        .languages
        .document
        .drafts
        .insert("prefs.title".into(), "Unsaved".into());
    h.state_mut().prefs.languages.unsaved = true;
    h.run_steps(3);
    h.key_press(egui::Key::Escape);
    h.run_steps(3);
    assert!(!h.state().prefs.languages.open);
    assert!(h.state().prefs.languages.unsaved);
    assert_eq!(h.state().prefs.languages.document.drafts["prefs.title"], "Unsaved");
}

#[test]
fn resetting_a_long_menu_source_keeps_confirmation_inside_the_window() {
    let mut h = harness(Lang::Pt, [820., 520.], theme::Appearance::Light, Target::Menu);
    let key = h
        .state()
        .prefs
        .languages
        .pack
        .keys
        .iter()
        .max_by_key(|k| k.len())
        .unwrap()
        .clone();
    h.state_mut().prefs.languages.reset = Some(Some(key));
    h.run_steps(3);
    let bounds = egui::Rect::from_min_size(egui::Pos2::ZERO, egui::vec2(820., 520.)).expand(1.);
    assert!(
        bounds.contains_rect(
            h.get_by_role_and_label(egui::Role::Button, t(Lang::Pt, "delete"))
                .rect()
        )
    );
}
