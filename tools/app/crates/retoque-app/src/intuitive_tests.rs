//! Synthetic, headless interaction tests; no real user library/catalog writes.
use crate::{
    app::*,
    app_tests::shell_tests::app,
    drop_import::{Route, classify},
    modals::palette::TABS,
};
use egui_kittest::{Harness, kittest::Queryable};

fn game(name: &str) -> Game {
    Game {
        id: name.into(),
        name: name.into(),
        store: Store::Manual,
        install_dir: PathBuf::new(),
        runtime: Runtime::Unknown,
        targets: vec![],
        registered: true,
        dlss_anywhere: vec![],
    }
}
fn navigation(lang: Lang) -> Harness<'static, RetoqueApp> {
    let mut app = app();
    app.lang = lang;
    app.library_state.games = vec![game("Cyberpunk 2077"), game("Ampliação")];
    Harness::builder()
        .with_size([1000., 800.])
        .with_accessibility_check(true)
        .build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.shell_shortcuts(ui.ctx());
                app.search_field(ui);
                app.library_keyboard(ui);
                app.palette_sheet(ui.ctx());
                app.drop_sheet(ui.ctx());
                app.handle_commands();
            },
            app,
        )
}
#[test]
fn palette_opens_types_selects_game_and_escape_restores_focus_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let mut h = navigation(lang);
        h.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::F);
        h.run_steps(3);
        h.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::K);
        h.run_steps(3);
        h.get_by_role_and_label(egui::Role::Dialog, t(lang, "palette.title"));
        h.get_by_role_and_label(egui::Role::TextInput, t(lang, "palette.search"))
            .type_text("cp77");
        h.run_steps(3);
        assert_eq!(h.state().intuitive.palette.results.len(), 1);
        h.key_press(egui::Key::Enter);
        h.run_steps(3);
        assert_eq!(h.state().selected.as_deref(), Some("Cyberpunk 2077"));
        assert!(!h.state().intuitive.palette.open);
        assert!(
            h.state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
        h.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::P);
        h.run_steps(3);
        h.key_press(egui::Key::ArrowDown);
        h.run_steps(2);
        h.key_press(egui::Key::ArrowUp);
        h.run_steps(2);
        h.key_press(egui::Key::Escape);
        h.run_steps(3);
        assert!(!h.state().intuitive.palette.open);
        assert_eq!(h.state().selected.as_deref(), Some("Cyberpunk 2077"));
    }
}
#[test]
fn tab_shortcuts_and_slash_respect_modals_and_text_entry() {
    for lang in [Lang::En, Lang::Pt] {
        let mut h = navigation(lang);
        for (key, (tab, _, _)) in [
            egui::Key::Num1,
            egui::Key::Num2,
            egui::Key::Num3,
            egui::Key::Num4,
            egui::Key::Num5,
        ]
        .into_iter()
        .zip(TABS)
        {
            h.key_press_modifiers(egui::Modifiers::COMMAND, key);
            h.run_steps(2);
            assert!(h.state().tab == tab);
        }
        h.key_press(egui::Key::Slash);
        h.run_steps(3);
        assert!(
            h.state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
        h.get_by_role_and_label(egui::Role::TextInput, t(lang, "lib.search"))
            .type_text("/");
        h.run_steps(2);
        assert_eq!(h.state().library_state.search, "/");
        h.state_mut().review_open = true;
        h.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::Num1);
        h.run_steps(2);
        assert!(h.state().tab == Tab::Logs);
    }
}
#[test]
fn settings_filter_only_changed_reset_and_composite_accessibility_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let mut app = app();
        app.lang = lang;
        app.selected = Some("synthetic".into());
        app.tab = Tab::Settings;
        app.view.target = Some("/synthetic/game".into());
        let ini = Ini::parse(b"[DlssNr]\nWorkingScale=1.5\nStage=auto\n");
        app.view.ini_values = crate::state::settings::IniValues::read(&ini);
        app.view.ini = Some(Ok(Some(ini)));
        let mut h = Harness::builder()
            .with_size([1100., 1600.])
            .with_accessibility_check(true)
            .build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.shell_shortcuts(ui.ctx());
                    let mut edits = std::mem::take(&mut app.view.edits);
                    app.settings_tab(ui, &mut edits, &app.cmd_tx.clone());
                    app.view.edits = edits;
                },
                app,
            );
        h.key_press(egui::Key::Slash);
        h.run_steps(3);
        assert!(
            h.state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("settings_filter")))
        );
        h.get_by_role_and_label(egui::Role::TextInput, t(lang, "set.filter"))
            .type_text("WorkingScale");
        h.run_steps(3);
        let setting = settings::all().iter().find(|s| s.key == "WorkingScale").unwrap();
        let actual_label = match lang {
            Lang::En => setting.label_en,
            Lang::Pt => setting.label_pt,
        };
        h.get_by_role_and_label(egui::Role::SpinButton, actual_label);
        h.get_by_role_and_label(egui::Role::CheckBox, t(lang, "set.only_changed"))
            .click();
        h.run_steps(2);
        h.get_by_label(t(lang, "set.source_ini"));
        h.get_by_role_and_label(egui::Role::Button, t(lang, "set.reset"))
            .click();
        h.run_steps(3);
        assert_eq!(
            h.state()
                .view
                .edits
                .get(&("DlssNr".into(), "WorkingScale".into()))
                .map(String::as_str),
            Some("auto")
        );
        assert!(!h.state().review_open);
        assert!(h.state().writing.is_empty());
        assert_eq!(h.state().view.ini_values.get("DlssNr", "WorkingScale"), Some("1.5"));
    }
}
#[test]
fn settings_filter_by_localized_label_and_draft_changed_includes_auto_on_disk() {
    let mut filter = crate::state::settings::Filter::default();
    let index = settings::all().iter().position(|s| s.key == "WorkingScale").unwrap();
    let s = &settings::all()[index];
    for lang in [Lang::En, Lang::Pt] {
        filter.query = match lang {
            Lang::En => s.label_en,
            Lang::Pt => s.label_pt,
        }
        .into();
        filter.only_changed = true;
        filter.refresh(lang);
        assert!(!filter.includes(index, "auto", None));
        assert!(filter.includes(index, "auto", Some("1.5")));
        assert!(filter.includes(index, "1.5", Some("auto")));
    }
}
#[test]
fn library_fuzzy_rank_tie_name_and_unchanged_cache() {
    let mut library = crate::state::library::LibraryState::default();
    library.games = vec![
        game("Some Cool Amplification Level Example"),
        game("Scale"),
        game("Ampliacao"),
        game("Ampliação"),
    ];
    library.search = "scale".into();
    library.refresh(&BTreeMap::new(), None, Lang::Pt);
    assert_eq!(library.derived.others, [1, 0]);
    let ptr = library.derived.others.as_ptr();
    library.refresh(&BTreeMap::new(), None, Lang::Pt);
    assert_eq!(ptr, library.derived.others.as_ptr());
    library.search = "ampliacao".into();
    library.refresh(&BTreeMap::new(), None, Lang::Pt);
    assert_eq!(library.derived.others, [2, 3]);
}
#[test]
fn drop_routes_and_cancel_never_imports_or_registers_both_languages() {
    let dir = tempfile::tempdir().unwrap();
    let dll = dir.path().join("nvngx_dlssnr-test.DLL");
    std::fs::write(&dll, b"synthetic").unwrap();
    let component = dir.path().join("component");
    std::fs::create_dir(&component).unwrap();
    std::fs::write(component.join("dlssg_sm86.ini"), b"synthetic").unwrap();
    let game = dir.path().join("game");
    std::fs::create_dir(&game).unwrap();
    let other = dir.path().join("other.dll");
    std::fs::write(&other, b"synthetic").unwrap();
    for (path, route) in [
        (dll, Route::Model),
        (component, Route::Component),
        (game, Route::Game),
        (other, Route::Unsupported),
    ] {
        assert_eq!(classify(&path), route);
        for lang in [Lang::En, Lang::Pt] {
            let mut h = navigation(lang);
            h.state_mut().intuitive.drop.pending.push((path.clone(), route.clone()));
            h.run_steps(3);
            h.get_by_label(t(lang, "drop.title"));
            assert!(h.state().saved.manual.is_empty());
            assert!(h.state().catalog.is_none());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
            h.run_steps(3);
            assert!(!h.state().intuitive.drop.open());
            assert!(h.state().saved.manual.is_empty());
            assert!(h.state().catalog.is_none());
            assert!(h.state().writing.is_empty());
        }
    }
}
#[test]
#[ignore = "manual matching cost measurement"]
fn matching_cost_500_games() {
    use crate::app_tests::measurements::measure;
    use std::hint::black_box;
    let mut app = app();
    app.library_state.games = (0..500)
        .map(|i| game(&format!("Game Ampliação Cyberpunk {i:04}")))
        .collect();
    app.library_state.search = "cyberpunk".into();
    let targets = BTreeMap::new();
    measure("legacy substring 500 games", 1000, || {
        let needle = app.library_state.search.to_lowercase();
        black_box(
            app.library_state
                .games
                .iter()
                .filter(|g| g.name.to_lowercase().contains(&needle))
                .count(),
        );
    });
    measure("fuzzy library rebuild 500 games", 1000, || {
        app.library_state.invalidate();
        app.library_state.refresh(&targets, None, Lang::Pt);
    });
    assert_eq!(
        measure("fuzzy library unchanged", 100_000, || {
            app.library_state.refresh(&targets, None, Lang::Pt);
            black_box(&app.library_state.derived);
        }),
        0
    );
    app.intuitive.palette.entries = app
        .library_state
        .games
        .iter()
        .map(|g| crate::modals::palette::Entry {
            label: g.name.clone(),
            search: String::new(),
            destination: crate::modals::palette::Destination::Game(g.id.clone()),
        })
        .collect();
    app.intuitive.palette.query = "cyberpunk".into();
    measure("palette rebuild 500 games", 1000, || {
        app.intuitive.palette.invalidate();
        app.intuitive.palette.refresh();
    });
    assert_eq!(
        measure("palette unchanged", 100_000, || {
            app.intuitive.palette.refresh();
            black_box(&app.intuitive.palette.results);
        }),
        0
    );
}

#[test]
fn undo_opens_inverse_review_without_writing_and_conflicts_are_shown_in_review() {
    use std::time::{Duration, Instant};
    for lang in [Lang::En, Lang::Pt] {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("game");
        let root = dir.path().join("history");
        std::fs::create_dir(&target).unwrap();
        let ini = target.join("OptiScaler.ini");
        std::fs::write(&ini, "[DlssNr]\nStage=auto\n").unwrap();
        let plan = retoque_core::plan_settings(&target, &[("DlssNr".into(), "Stage".into(), "1".into())]).unwrap();
        let record = crate::history::capture(&plan);
        retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
        crate::history::record(&root, &target, record).unwrap();
        let mut app = app();
        app.lang = lang;
        app.intuitive.history_root = Some(root);
        app.view.target = Some(target.clone());
        let before = std::fs::read(&ini).unwrap();
        let mut h = Harness::builder()
            .with_size([1000., 800.])
            .with_accessibility_check(true)
            .build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.review_sheet(ui.ctx());
                    app.handle_commands();
                },
                app,
            );
        h.state_mut().start_undo(target.clone());
        let start = Instant::now();
        while h.state().review.is_none() {
            h.step();
            assert!(start.elapsed() < Duration::from_secs(5));
            std::thread::sleep(Duration::from_millis(5));
        }
        h.run_steps(3);
        let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
        assert!(review.title.starts_with(t(lang, "undo.title")));
        assert!(review.allowed());
        assert_eq!(std::fs::read(&ini).unwrap(), before);
        h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
        h.run_steps(3);
        assert_eq!(std::fs::read(&ini).unwrap(), before);
        std::fs::write(&ini, "[DlssNr]\nStage=0\n").unwrap();
        h.state_mut().start_undo(target);
        let start = Instant::now();
        while h.state().review.is_none() {
            h.step();
            assert!(start.elapsed() < Duration::from_secs(5));
            std::thread::sleep(Duration::from_millis(5));
        }
        h.run_steps(3);
        assert!(h.state().review.as_ref().unwrap().is_err());
        h.get_by_label_contains(t(lang, "undo.conflict"));
        assert!(
            h.query_by_role_and_label(egui::Role::Button, t(lang, "set.apply"))
                .is_none()
        );
        assert_eq!(std::fs::read_to_string(ini).unwrap(), "[DlssNr]\nStage=0\n");
    }
}

#[test]
fn palette_setting_keys_and_cached_results_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let mut h = navigation(lang);
        h.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::K);
        h.run_steps(3);
        h.get_by_role_and_label(egui::Role::TextInput, t(lang, "palette.search"))
            .type_text("WorkingScale");
        h.run_steps(3);
        let results = &h.state().intuitive.palette.results;
        assert_eq!(results.len(), 1);
        let pointer = results.as_ptr();
        h.run_steps(3);
        assert_eq!(pointer, h.state().intuitive.palette.results.as_ptr());
        h.key_press(egui::Key::Enter);
        h.run_steps(3);
        assert!(h.state().tab == Tab::Settings);
        assert_eq!(h.state().intuitive.settings.query, "WorkingScale");
        assert!(h.state().view.edits.is_empty());
        assert!(!h.state().review_open);
    }
}

#[derive(Debug)]
struct SyntheticDrop(PathBuf);
impl egui::DroppedFile for SyntheticDrop {
    fn path(&self) -> &Path {
        &self.0
    }
    fn bytes(&self) -> Result<Vec<u8>, String> {
        Err("drop routing must not read DLL bytes".into())
    }
}
#[test]
fn raw_drop_is_consumed_before_tabs_and_game_registration_requires_confirmation() {
    for lang in [Lang::En, Lang::Pt] {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("synthetic-game");
        std::fs::create_dir(&target).unwrap();
        std::fs::write(target.join("game.exe"), b"MZ synthetic").unwrap();
        let mut app = app();
        app.lang = lang;
        // Keep the confirmation's rescan queued rather than discovering the host library in this fixture.
        app.scanning = true;
        let mut h = Harness::builder()
            .with_size([1000., 800.])
            .with_accessibility_check(true)
            .build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.global_drop_input(ui.ctx());
                    assert!(ui.input(|i| i.raw.dropped_files.is_empty()));
                    app.drop_sheet(ui.ctx());
                },
                app,
            );
        h.input_mut()
            .dropped_files
            .push(std::sync::Arc::new(SyntheticDrop(target.clone())));
        let start = std::time::Instant::now();
        while h.state().intuitive.drop.pending.is_empty() {
            h.step();
            assert!(start.elapsed() < std::time::Duration::from_secs(5));
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        h.run_steps(3);
        assert!(h.state().saved.manual.is_empty());
        h.get_by_role_and_label(egui::Role::Button, t(lang, "drop.confirm"))
            .click();
        h.run_steps(3);
        assert_eq!(h.state().saved.manual.as_slice(), std::slice::from_ref(&target));
        assert!(h.state().rescan_pending);
        assert!(!target.join(".retoque").exists());
        assert!(!target.join("OptiScaler.ini").exists());
    }
}

#[test]
fn compound_settings_review_records_history_only_after_success() {
    for stale in [false, true] {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("game");
        let root = dir.path().join("history");
        std::fs::create_dir(&target).unwrap();
        let ini = target.join("OptiScaler.ini");
        std::fs::write(&ini, "[DlssNr]\nStage=auto\n").unwrap();
        let prepared = crate::autotune::prepare(
            &target,
            crate::autotune::Request {
                dlss: None,
                component: None,
                edits: vec![("DlssNr".into(), "Stage".into(), "1".into())],
            },
        )
        .unwrap();
        let mut app = app();
        app.view.target = Some(target.clone());
        app.intuitive.history_root = Some(root.clone());
        app.review_open = true;
        app.review = Some(Ok(ReviewSession {
            title: "synthetic settings".into(),
            kind: ReviewKind::Autotune(Box::new(prepared.request)),
            plans: prepared.plans,
            groups: prepared.groups,
            request_id: 1,
            target: None,
        }));
        if stale {
            std::fs::write(&ini, "[DlssNr]\nStage=0\n").unwrap();
        }
        assert!(!root.exists());
        app.confirm_review(1);
        let start = std::time::Instant::now();
        while app.jobs.busy() {
            app.receive();
            assert!(start.elapsed() < std::time::Duration::from_secs(10));
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(app.intuitive.compound_history.is_empty());
        if stale {
            assert!(!root.exists());
            assert_eq!(std::fs::read_to_string(ini).unwrap(), "[DlssNr]\nStage=0\n");
        } else {
            let inverse = crate::history::undo_plan(&root, &target, Lang::En).unwrap();
            assert!(matches!(&inverse.steps()[0],Step::SetIni { old: Some(v), value, .. } if v=="1" && value=="auto"));
        }
    }
}
