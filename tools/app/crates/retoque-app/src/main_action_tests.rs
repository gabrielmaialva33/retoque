//! The game page's main action and its feedback: one primary with "More actions" beside it, the review's title and
//! confirm repeating the clicked verb, toasts that say what was done, the palette's action names and the home count
//! line. Synthetic, headless; nothing is written to a game.
use crate::{app::*, app_tests::shell_tests::app, commands::PaletteAction, modals::palette::Destination};
use egui_kittest::{Harness, kittest::Queryable};
use retoque_core::receipt::{Receipt, ReceiptStatus};

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

/// Status, the payload's build, the primary's key, what "More actions" offers and what it must not.
type Case = (
    retoque_core::Status,
    Option<&'static str>,
    &'static str,
    &'static [&'static str],
    &'static [&'static str],
);

fn receipt(status: ReceiptStatus) -> Option<Receipt> {
    Some(Receipt {
        status,
        updated: String::new(),
        records: vec![],
    })
}

fn banner(lang: Lang, status: retoque_core::Status, payload_build: Option<&str>) -> Harness<'static, RetoqueApp> {
    let mut app = app();
    app.lang = lang;
    app.payload = Some("/synthetic/payload".into());
    app.payload_build = payload_build.map(|b| Ok(b.to_owned()));
    app.view.target = Some("/synthetic/game".into());
    app.view.status = Some(Ok(status));
    let game = game("Synthetic");
    Harness::builder()
        .with_size([1000., 700.])
        .build_ui_state(move |ui, app: &mut RetoqueApp| app.game_banner(ui, &game), app)
}

#[test]
fn banner_names_its_one_action_by_state_and_keeps_the_rest_in_more_actions() {
    for lang in [Lang::En, Lang::Pt] {
        let installed = retoque_core::Status {
            receipt: receipt(ReceiptStatus::Complete),
            build_sha256: Some("old".into()),
            ..Default::default()
        };
        let untracked = retoque_core::Status {
            optiscaler_proxies: vec!["dxgi.dll".into()],
            ..Default::default()
        };
        let partial = retoque_core::Status {
            receipt: receipt(ReceiptStatus::Partial),
            ..Default::default()
        };
        let cases: [Case; 5] = [
            (
                retoque_core::Status::default(),
                None,
                "shell.install_optimize",
                &["shell.install_only", "shell.open_folder"],
                &["shell.uninstall_open", "undo.title"],
            ),
            (
                untracked,
                None,
                "shell.register_optimize",
                &["shell.install_only", "shell.open_folder"],
                &["shell.uninstall_open"],
            ),
            (
                installed.clone(),
                Some("new"),
                "shell.update_optimize",
                &[
                    "shell.update",
                    "shell.uninstall_open",
                    "undo.title",
                    "shell.open_folder",
                ],
                &["shell.reinstall"],
            ),
            (
                installed,
                Some("old"),
                "shell.optimize",
                &[
                    "shell.reinstall",
                    "shell.uninstall_open",
                    "undo.title",
                    "shell.open_folder",
                ],
                &["shell.update"],
            ),
            (
                partial,
                None,
                "shell.resume",
                &["shell.open_folder"],
                &["shell.install_only"],
            ),
        ];
        for (status, build, primary, menu, absent) in cases {
            let mut h = banner(lang, status, build);
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, t(lang, primary));
            if primary != "shell.resume" {
                assert_eq!(h.state().optimize_action_key(), primary);
            }
            // The secondary actions stay out of sight until "More actions" opens.
            for key in menu {
                assert!(h.query_by_label(t(lang, key)).is_none(), "{key} outside the menu");
            }
            h.get_by_role_and_label(egui::Role::Button, t(lang, "shell.more"))
                .click();
            h.run_steps(2);
            for key in menu {
                h.get_by_role_and_label(egui::Role::Button, t(lang, key));
            }
            for key in absent {
                assert!(h.query_by_label(t(lang, key)).is_none(), "{key} should not be offered");
            }
        }
    }
}

#[test]
fn review_title_drops_the_ellipsis_and_the_confirm_repeats_the_verb() {
    for lang in [Lang::En, Lang::Pt] {
        let mut app = app();
        app.lang = lang;
        app.view.status = Some(Ok(retoque_core::Status::default()));
        let title = app.optimize_title();
        assert_eq!(title, t(lang, "shell.install_optimize").trim_end_matches('…'));
        assert!(!title.ends_with('…'));
        assert_eq!(app.optimize_verb(&title), Some(title.as_str()));
        // Other reviews made of the same parts are confirmed with "Apply".
        assert_eq!(app.optimize_verb(t(lang, "rs.review")), None);
        assert_eq!(
            t(lang, "auto.confirm"),
            if lang == Lang::Pt { "Aplicar" } else { "Apply" }
        );
    }
}

#[test]
fn tuning_toast_names_install_update_register_or_optimization() {
    use crate::autotune::Request;
    let install = Request::default().with_install(retoque_core::InstallOptions::default());
    let mut app = app();
    app.view.status = Some(Ok(retoque_core::Status::default()));
    assert_eq!(app.tuning_done_key(&install), "auto.done_install");
    app.view.status = Some(Ok(retoque_core::Status {
        optiscaler_proxies: vec!["winmm.dll".into()],
        ..Default::default()
    }));
    assert_eq!(app.tuning_done_key(&install), "auto.done_register");
    app.view.status = Some(Ok(retoque_core::Status {
        receipt: receipt(ReceiptStatus::Complete),
        ..Default::default()
    }));
    assert_eq!(app.tuning_done_key(&install), "auto.done_update");
    // Without an install: an optimization says so; any other reviewed change is "Changes applied".
    assert_eq!(app.tuning_done_key(&Request::default()), "auto.done");
}

#[test]
fn success_toasts_repeat_the_action_without_details() {
    for lang in [Lang::En, Lang::Pt] {
        let target = PathBuf::from("/synthetic/game");
        let mut app = app();
        app.lang = lang;
        app.view.target = Some(target.clone());
        app.view.status = Some(Ok(retoque_core::Status::default()));
        app.notify_action(PlanKind::Install, &target, "Synthetic", &Ok(()));
        app.view.status = Some(Ok(retoque_core::Status {
            receipt: receipt(ReceiptStatus::Complete),
            ..Default::default()
        }));
        app.notify_action(PlanKind::Install, &target, "Synthetic", &Ok(()));
        app.notify_action(PlanKind::Settings, &target, "Synthetic", &Ok(()));
        let expected = ["shell.installed_done", "shell.updated_done", "shell.settings_done"];
        assert_eq!(app.toasts.len(), expected.len());
        for (toast, key) in app.toasts.iter().zip(expected) {
            assert_eq!(
                toast.message,
                format!("{} · Synthetic\n{}", t(lang, key), t(lang, "shell.restart_hint"))
            );
            assert!(toast.detail.is_none() && !toast.failed);
        }
        // A failure keeps its details.
        app.notify_action(PlanKind::Install, &target, "Synthetic", &Err("synthetic".into()));
        let failed = app.toasts.last().unwrap();
        assert!(failed.failed && failed.detail.is_some());
    }
}

#[test]
fn palette_actions_are_verbs_and_settings_hide_their_ini_names() {
    for lang in [Lang::En, Lang::Pt] {
        let mut app = app();
        app.lang = lang;
        app.view.intelligence.facts = std::sync::Arc::new(retoque_data::recommend::GameFacts {
            installed: true,
            ..Default::default()
        });
        let mut h = Harness::builder().with_size([1000., 800.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.palette_sheet(ui.ctx());
            },
            app,
        );
        let ctx = h.state().context.clone();
        h.state_mut().intuitive.palette.open(&ctx);
        h.run_steps(2);
        let entries = &h.state().intuitive.palette.entries;
        for entry in entries {
            assert!(!entry.label.contains('['), "{}", entry.label);
        }
        let optimize = entries
            .iter()
            .position(|e| matches!(e.destination, Destination::Action(PaletteAction::Optimize)))
            .unwrap();
        assert_eq!(entries[optimize].label, t(lang, "shell.optimize"));
        for action in [PaletteAction::ReviewUninstall, PaletteAction::ReviewSettings] {
            let entry = entries
                .iter()
                .find(|e| matches!(e.destination, Destination::Action(a) if a == action))
                .unwrap();
            assert!(entry.label.ends_with('…') && entry.label.chars().next().unwrap().is_uppercase());
        }
        // The main action is found by its install name too, whatever the open game shows.
        h.get_by_role_and_label(egui::Role::TextInput, t(lang, "palette.search"))
            .type_text(t(lang, "shell.install_optimize").trim_end_matches('…'));
        h.run_steps(3);
        assert!(h.state().intuitive.palette.results.contains(&optimize));
        h.get_by_label(t(lang, "palette.keys"));
    }
}

#[test]
fn home_count_line_uses_singular_and_omits_zero_updates() {
    let mut app = app();
    app.library_state.games = vec![game("One")];
    assert_eq!(app.home_count_line(1, 0), "1 game · 1 with Retoque");
    app.lang = Lang::Pt;
    assert_eq!(
        app.home_count_line(1, 1),
        "1 jogo · 1 com Retoque · 1 atualização disponível"
    );
    app.library_state.games.push(game("Two"));
    assert_eq!(
        app.home_count_line(0, 2),
        "2 jogos · 0 com Retoque · 2 atualizações disponíveis"
    );
    app.lang = Lang::En;
    assert_eq!(app.home_count_line(2, 0), "2 games · 2 with Retoque");
}

#[test]
fn update_all_action_appears_only_when_library_has_updates() {
    for lang in [Lang::En, Lang::Pt] {
        for available in [false, true] {
            let mut app = app();
            app.lang = lang;
            let path = PathBuf::from("/synthetic/update-all");
            let mut game = game("Update candidate");
            game.targets.push(retoque_discovery::TargetDir {
                path: path.clone(),
                executables: vec![],
                score: 0,
                dlss: vec![],
                has_receipt: true,
                proxies_present: vec![],
                loaders: vec![],
            });
            app.library_state.games.push(game);
            app.library_state.status.insert(
                path,
                retoque_core::Status {
                    receipt: receipt(ReceiptStatus::Complete),
                    build_sha256: Some("old".into()),
                    ..Default::default()
                },
            );
            app.payload_build = Some(Ok(if available { "new" } else { "old" }.into()));
            app.payload = Some("/synthetic/payload".into());
            let mut h = Harness::builder()
                .with_size([1024., 768.])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.home_grid(ui), app);
            h.run_steps(2);
            let label = if lang == Lang::En {
                "Update all…"
            } else {
                "Atualizar todos…"
            };
            assert_eq!(h.query_by_label(label).is_some(), available);
        }
    }
}
