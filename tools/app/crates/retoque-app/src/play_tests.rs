//! Synthetic UI tests: no OS opener or real launcher/game is used.
use crate::{app::*, app_tests::shell_tests::app, play::*};
use egui_kittest::{
    Harness,
    kittest::{NodeT as _, Queryable},
};

fn game(store: Store, id: &str) -> Game {
    Game {
        id: id.into(),
        name: "Synthetic".into(),
        store,
        install_dir: "/synthetic/game".into(),
        runtime: Runtime::Windows,
        targets: vec![],
        dlss_anywhere: vec![],
        registered: true,
    }
}

#[test]
fn launcher_uri_contracts_and_platforms() {
    for windows in [true, false] {
        assert_eq!(
            launch_uri(&game(Store::Steam, "steam:123"), windows).as_deref(),
            Some("steam://rungameid/123")
        );
        for runner in ["legendary", "gog", "nile"] {
            assert_eq!(
                launch_uri(&game(Store::Heroic, &format!("heroic:{runner}:a &/#?é")), windows),
                Some(format!(
                    "heroic://launch?appName=a%20%26%2F%23%3F%C3%A9&runner={runner}"
                ))
            );
        }
        assert_eq!(
            launch_uri(&game(Store::Lutris, "lutris:some-game"), windows),
            (!windows).then(|| "lutris:rungame/some-game".into())
        );
        assert_eq!(
            launch_uri(&game(Store::Epic, "epic:artifact&x"), windows),
            windows.then(|| "com.epicgames.launcher://apps/artifact%26x?action=launch&silent=true".into())
        );
        for (store, id) in [
            (Store::Manual, "manual:folder"),
            (Store::Gog, "gog:123"),
            (Store::Xbox, "xbox:folder"),
            (Store::Steam, "steam-folder:folder"),
            (Store::Steam, "steam:0"),
            (Store::Steam, "steam:+1"),
            (Store::Steam, "steam:1/quit"),
            (Store::Steam, "steam:4294967296"),
            (Store::Steam, "heroic:1"),
            (Store::Heroic, "heroic:unknown:abc"),
            (Store::Heroic, "heroic:gog:"),
            (Store::Lutris, "lutris:123"),
            (Store::Lutris, "lutris:foo/bar"),
            (Store::Epic, "epic:"),
        ] {
            assert!(launch_uri(&game(store, id), windows).is_none(), "{store:?} {id}");
        }
        let mut g = game(Store::Steam, "steam:1");
        g.registered = false;
        assert!(launch_uri(&g, windows).is_none());
        g.registered = true;
        g.runtime = Runtime::Proton {
            app_id: 2,
            prefix: None,
        };
        assert!(launch_uri(&g, windows).is_none());
    }
}

fn fixture(g: Game, lang: Lang) -> RetoqueApp {
    let mut app = app();
    app.lang = lang;
    app.selected = Some(g.id.clone());
    app.view.target = Some(g.install_dir.clone());
    app.view.status = Some(Ok(Status::default()));
    app.library_state.games = vec![g];
    app
}
fn banner(app: RetoqueApp) -> Harness<'static, RetoqueApp> {
    Harness::builder().with_size([820., 768.]).build_ui_state(
        |ui, app: &mut RetoqueApp| {
            let game = app.library_state.games[0].clone();
            app.game_banner(ui, &game);
        },
        app,
    )
}

#[test]
fn play_presence_write_reason_and_favorite_toggle_on_page() {
    for lang in [Lang::En, Lang::Pt] {
        for (store, id, present) in [
            (Store::Steam, "steam:123", true),
            (Store::Manual, "manual:x", false),
            (Store::Gog, "gog:123", false),
            (Store::Xbox, "xbox:x", false),
        ] {
            let mut h = banner(fixture(game(store, id), lang));
            h.run_steps(2);
            assert_eq!(h.query_by_label(t(lang, "play.action")).is_some(), present);
            h.get_by_label(&t(lang, "favorites.add").replace("{game}", "Synthetic"))
                .click();
            h.run_steps(2);
            assert!(h.state().saved.favorites.contains(id));
            h.get_by_label(&t(lang, "favorites.remove").replace("{game}", "Synthetic"));
            if present {
                h.state_mut().writing.insert("/synthetic/game/subfolder".into(), (0, 1));
                h.run_steps(2);
                assert!(h.get_by_label(t(lang, "play.action")).accesskit_node().is_disabled());
                h.get_by_label(t(lang, "play.wait_write"));
                h.state_mut().writing.clear();
                h.state_mut().writing.insert("/synthetic/unrelated".into(), (0, 1));
                h.run_steps(2);
                assert!(!h.get_by_label(t(lang, "play.action")).accesskit_node().is_disabled());
            }
        }
    }
}

#[test]
fn launch_result_is_a_request_not_running_and_writes_cannot_bypass_it() {
    let g = game(Store::Steam, "steam:123");
    let mut app = fixture(g.clone(), Lang::En);
    app.play_game_with(&g, |uri| {
        assert_eq!(uri, "steam://rungameid/123");
        Ok(())
    });
    assert_eq!(app.toasts.last().unwrap().message, "Asked Steam to start Synthetic");
    app.play_game_with(&g, |_| Err(std::io::Error::other("synthetic refusal")));
    assert!(app.toasts.last().unwrap().failed);
    app.writing.insert(g.install_dir.join("other-target"), (0, 1));
    app.play_game_with(&g, |_| {
        panic!("must not dispatch while any game folder is being written")
    });
    assert_eq!(app.toasts.last().unwrap().message, t(Lang::En, "play.wait_write"));
}

#[test]
fn favorites_survive_save_and_old_state_still_loads() {
    let mut a = app();
    a.toggle_favorite("steam:123");
    let value = serde_json::to_value(&a.saved).unwrap();
    let saved: Saved = serde_json::from_value(value.clone()).unwrap();
    assert!(saved.favorites.contains("steam:123"));
    let mut old = value;
    old.as_object_mut().unwrap().remove("favorites");
    assert!(serde_json::from_value::<Saved>(old).unwrap().favorites.is_empty());
}

#[test]
fn both_library_views_group_favorites_first_and_toggle_without_selecting() {
    for sidebar in [false, true] {
        let mut a = app();
        let mut alpha = game(Store::Steam, "steam:1");
        alpha.name = "Alpha".into();
        let mut beta = game(Store::Steam, "steam:2");
        beta.name = "Beta".into();
        a.library_state.games = vec![alpha, beta];
        let mut h = Harness::builder().with_size([1024., 768.]).build_ui_state(
            move |ui, app: &mut RetoqueApp| {
                if sidebar {
                    app.library(ui);
                } else {
                    app.home_grid(ui);
                }
            },
            a,
        );
        h.run_steps(2);
        assert!(h.query_by_label("Favorites").is_none());
        assert!(h.query_by_label("Play").is_none());
        h.get_by_label("Add to favorites: Beta").click();
        h.run_steps(3);
        assert!(h.state().selected.is_none());
        h.get_by_label("Favorites");
        assert_eq!(h.state().library_groups()[0].1, vec![1]);
        let star = h.get_by_label("Remove from favorites: Beta");
        let other = h.get_by_label("Add to favorites: Alpha");
        assert!(star.rect().top() < other.rect().top());
        // The row/card exposes favorite state as text to assistive technology too.
        assert!(
            h.query_by_label("Beta, Not installed, Favorite").is_some()
                || h.query_by_label("Beta, No Retoque, Favorite").is_some()
                || h.query_all_by_role(egui::Role::Button).any(|n| n
                    .accesskit_node()
                    .label()
                    .is_some_and(|s| s.starts_with("Beta,") && s.contains("Favorite")))
        );
        h.get_by_label("Remove from favorites: Beta").click();
        h.run_steps(2);
        assert!(h.query_by_label("Favorites").is_none());
    }
}

#[test]
fn favorite_is_keyboard_reachable_and_reports_its_state() {
    let g = game(Store::Steam, "steam:123");
    let mut h = Harness::new_ui_state(
        move |ui, favorite: &mut bool| {
            if favorite_button(ui, &g, *favorite, Lang::En).clicked() {
                *favorite = !*favorite;
            }
        },
        false,
    );
    h.key_press(egui::Key::Tab);
    h.run_steps(2);
    assert!(h.get_by_label("Add to favorites: Synthetic").is_focused());
    h.key_press(egui::Key::Space);
    h.run_steps(2);
    assert!(*h.state());
    h.get_by_label("Remove from favorites: Synthetic");
}

#[test]
fn quick_actions_only_exist_for_files_in_selected_target() {
    let dir = tempfile::tempdir().unwrap();
    let mut a = app();
    a.view.target = Some(dir.path().into());
    let mut h = Harness::new_ui_state(|ui, app: &mut RetoqueApp| app.quick_actions(ui), a);
    assert!(h.query_by_label("Open settings file").is_none());
    assert!(h.query_by_label("Open captures folder").is_none());
    std::fs::write(dir.path().join("OptiScaler.ini"), "").unwrap();
    std::fs::create_dir(dir.path().join("retoque-captures")).unwrap();
    h.run_steps(2);
    h.get_by_label("Open settings file");
    h.get_by_label("Open captures folder");
    h.state_mut().view.target = None;
    h.run_steps(2);
    assert!(h.query_by_label("Open settings file").is_none());
    assert!(h.query_by_label("Open captures folder").is_none());
}

#[test]
fn palette_tracks_selected_game_and_write_state() {
    let a = fixture(game(Store::Steam, "steam:123"), Lang::En);
    let mut h = Harness::new_ui_state(
        |ui, app: &mut RetoqueApp| {
            app.context = ui.ctx().clone();
            app.palette_sheet(ui.ctx());
        },
        a,
    );
    let ctx = h.state().context.clone();
    h.state_mut().intuitive.palette.open(&ctx);
    h.state_mut().intuitive.palette.query = "Play Synthetic".into();
    h.run_steps(3);
    h.get_by_label("Play Synthetic");
    h.state_mut().writing.insert("/synthetic/game".into(), (0, 1));
    h.run_steps(2);
    assert!(h.query_by_label("Play Synthetic").is_none());
    h.state_mut().writing.clear();
    h.state_mut().selected = None;
    h.run_steps(2);
    assert!(h.query_by_label("Play Synthetic").is_none());
}
