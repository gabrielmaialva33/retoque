//! Every language draws the whole window at 1024×768 (the smallest screen the app is laid out for): the home grid,
//! a game page on each tab, installed and not, and Preferences. Nothing panics, the view's one primary action and the
//! top bar stay inside the window, and top-bar controls never overlap. German and Russian carry the longest text.
//! Synthetic and headless: the game folder is an empty temporary directory and nothing is written to it.
use crate::{app::*, app_tests::shell_tests::app};
use egui::accesskit::Role;
use egui_kittest::{Harness, kittest::Queryable};
use retoque_core::receipt::{Receipt, ReceiptStatus};
use retoque_discovery::TargetDir;

const SIZE: [f32; 2] = [1024., 768.];

fn window_rect() -> egui::Rect {
    egui::Rect::from_min_size(egui::Pos2::ZERO, egui::Vec2::from(SIZE))
}

fn game(target: &Path) -> Game {
    Game {
        id: "synthetic".into(),
        name: "Synthetic Game With A Fairly Long Name".into(),
        store: Store::Steam,
        install_dir: target.into(),
        runtime: Runtime::Proton {
            app_id: 1,
            prefix: None,
        },
        targets: vec![TargetDir {
            path: target.into(),
            executables: vec!["game.exe".into()],
            score: 1,
            dlss: vec![],
            has_receipt: false,
            proxies_present: vec![],
            loaders: vec![],
        }],
        dlss_anywhere: vec![],
        registered: true,
    }
}

/// The app with one game: on its page at `tab` when `tab` is set, else on the home grid.
fn window(lang: Lang, target: &Path, installed: bool, tab: Option<Tab>) -> Harness<'static, RetoqueApp> {
    let mut app = app();
    app.lang = lang;
    app.payload = Some("/synthetic/payload".into());
    app.payload_build = Some(Ok("old".into()));
    app.library_state.games.push(game(target));
    app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
    app.tuning.installed = Some(Ok(vec![]));
    if let Some(tab) = tab {
        app.selected = Some("synthetic".into());
        app.tab = tab;
        app.view.target = Some(target.into());
        app.view.status = Some(Ok(Status {
            receipt: installed.then(|| Receipt {
                status: ReceiptStatus::Complete,
                updated: String::new(),
                records: vec![],
            }),
            build_sha256: installed.then(|| "old".into()),
            ..Default::default()
        }));
    }
    // As RetoqueApp::new does: CJK text comes from a system font (the harness panics on a missing glyph).
    let mut h = Harness::builder()
        .with_size(SIZE)
        .with_context_setup(move |ctx| crate::fonts::apply(ctx, lang))
        .build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.window(ui);
            },
            app,
        );
    h.run_steps(4);
    h
}

#[track_caller]
fn inside(h: &Harness<'_, RetoqueApp>, role: Role, label: &str, what: &str) -> egui::Rect {
    let rect = h.get_by_role_and_label(role, label).rect();
    assert!(
        window_rect().expand(0.5).contains_rect(rect),
        "{what}: \"{label}\" at {rect:?} leaves the {SIZE:?} window"
    );
    rect
}

/// The top bar's controls, each inside the window and none drawn over another.
#[track_caller]
fn top_bar_fits(h: &Harness<'_, RetoqueApp>, lang: Lang, on_game: bool) {
    let mut rects = vec![
        inside(h, Role::Button, t(lang, "brand"), "brand"),
        inside(h, Role::Button, t(lang, "prefs.open"), "top bar"),
        inside(h, Role::Button, t(lang, "app.refresh"), "top bar"),
        inside(h, Role::Button, t(lang, "palette.title"), "top bar"),
        inside(h, Role::ComboBox, t(lang, "prefs.language"), "top bar"),
    ];
    if on_game {
        rects.push(inside(h, Role::Button, t(lang, "shell.back_library"), "top bar"));
        rects.push(inside(h, Role::Button, t(lang, "shell.library_hide"), "top bar"));
    }
    for (i, a) in rects.iter().enumerate() {
        for b in &rects[i + 1..] {
            assert!(
                !a.shrink(0.5).intersects(b.shrink(0.5)),
                "{lang:?}: top-bar controls overlap: {a:?} {b:?}"
            );
        }
    }
}

#[test]
fn every_language_fits_the_main_views_in_a_small_window() {
    let dir = tempfile::tempdir().unwrap();
    for lang in Lang::ALL.into_iter().filter(|l| crate::fonts::can_show(*l)) {
        let mut home = window(lang, dir.path(), false, None);
        top_bar_fits(&home, lang, false);
        assert!(
            home.query_by_label(t(lang, "welcome.title")).is_some(),
            "{lang:?}: home grid"
        );

        for installed in [false, true] {
            let primary = t(
                lang,
                if installed {
                    "shell.optimize"
                } else {
                    "shell.install_optimize"
                },
            );
            for tab in [Tab::Install, Tab::Settings, Tab::Model, Tab::Compare, Tab::Logs] {
                let h = window(lang, dir.path(), installed, Some(tab));
                let what = format!("{lang:?} {tab:?} installed={installed}");
                top_bar_fits(&h, lang, true);
                if tab != Tab::Compare {
                    inside(&h, Role::Button, primary, &what);
                }
                for tab_node in h.query_all_by_role(Role::Tab) {
                    let rect = tab_node.rect();
                    assert!(
                        window_rect().expand(0.5).contains_rect(rect),
                        "{what}: tab strip at {rect:?}"
                    );
                }
            }
        }

        // Preferences, opened over the home grid: the language selector is labelled and the dialog fits.
        home.state_mut().prefs_open();
        home.run_steps(3);
        let labelled = home
            .query_all_by_role_and_label(Role::ComboBox, t(lang, "prefs.language"))
            .count();
        assert_eq!(
            labelled, 2,
            "{lang:?}: the top bar's and the dialog's selectors are both named"
        );
        inside(&home, Role::Button, t(lang, "close"), &format!("{lang:?} preferences"));
    }
}

#[test]
fn choosing_a_language_applies_at_once_and_is_saved() {
    let dir = tempfile::tempdir().unwrap();
    let mut h = window(Lang::En, dir.path(), false, None);
    h.state_mut().prefs_open();
    h.run_steps(3);
    let combos: Vec<_> = h
        .query_all_by_role_and_label(Role::ComboBox, t(Lang::En, "prefs.language"))
        .map(|n| n.rect())
        .collect();
    // The dialog's selector is the lower one; the top bar's sits at the top of the window.
    let dialog = combos
        .iter()
        .copied()
        .max_by(|a, b| a.min.y.total_cmp(&b.min.y))
        .unwrap();
    h.get_all_by_role_and_label(Role::ComboBox, t(Lang::En, "prefs.language"))
        .find(|n| n.rect() == dialog)
        .unwrap()
        .click();
    h.run_steps(2);
    if crate::fonts::can_show(Lang::Ja) {
        // Listed by its own name, which needs the CJK font the provider found.
        h.get_by_label(Lang::Ja.native_name());
    }
    h.get_by_label("Deutsch").click();
    h.run_steps(3);
    assert_eq!(h.state().lang, Lang::De);
    assert_eq!(h.state().saved.lang, Some(Lang::De));
    h.get_by_label(t(Lang::De, "prefs.title"));
    assert_ne!(t(Lang::De, "prefs.title"), t(Lang::En, "prefs.title"));
}
