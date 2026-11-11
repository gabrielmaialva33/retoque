//! Every language draws the whole window at 1024×768 (the smallest screen the app is laid out for) and at the window's
//! minimum width (820, `main.rs`): the home grid, a game page on each tab, installed and not, and Preferences.
//! Nothing panics, the view's one primary action and the top bar stay inside the window, top-bar controls never
//! overlap, and an action the bar has no room for is in its "More actions" menu. German and Russian carry the longest
//! text. The window has the app's own theme (fonts, padding, spacing), so widths are the real ones.
//! Synthetic and headless: the game folder is an empty temporary directory and nothing is written to it.
use crate::{app::*, app_tests::shell_tests::app};
use egui::accesskit::Role;
use egui_kittest::{Harness, kittest::Queryable};
use retoque_core::receipt::{Receipt, ReceiptStatus};
use retoque_discovery::TargetDir;

const SIZE: [f32; 2] = [1024., 768.];
/// The window's minimum width (`main.rs`, `with_min_inner_size`).
const NARROW: [f32; 2] = [820., 768.];

fn window_rect(size: [f32; 2]) -> egui::Rect {
    egui::Rect::from_min_size(egui::Pos2::ZERO, egui::Vec2::from(size))
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
fn window(
    lang: Lang,
    target: &Path,
    installed: bool,
    tab: Option<Tab>,
    size: [f32; 2],
) -> Harness<'static, RetoqueApp> {
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
    // As RetoqueApp::new does: the theme, and CJK text from a system font (the harness panics on a missing glyph).
    let mut h = Harness::builder()
        .with_size(size)
        .with_context_setup(move |ctx| {
            crate::theme::apply(ctx);
            crate::fonts::apply(ctx, lang);
        })
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
fn inside(h: &Harness<'_, RetoqueApp>, size: [f32; 2], role: Role, label: &str, what: &str) -> egui::Rect {
    let rect = h.get_by_role_and_label(role, label).rect();
    assert!(
        window_rect(size).expand(0.5).contains_rect(rect),
        "{what}: \"{label}\" at {rect:?} leaves the {size:?} window"
    );
    rect
}

/// The top bar's controls, each inside the window and none drawn over another; one the bar has no room for is in its
/// "More actions" menu, which opens inside the window. Returns the labels found in that menu.
#[track_caller]
fn top_bar_fits(h: &mut Harness<'_, RetoqueApp>, lang: Lang, on_game: bool, size: [f32; 2]) -> Vec<&'static str> {
    let what = format!("{lang:?} {size:?} top bar");
    let brand = inside(h, size, Role::Button, t(lang, "brand"), &what);
    let mut rects = vec![brand, inside(h, size, Role::Button, t(lang, "palette.title"), &what)];
    if on_game {
        rects.push(inside(h, size, Role::Button, t(lang, "shell.back_library"), &what));
    }
    let mut optional = vec![
        (Role::Button, t(lang, "prefs.open")),
        (Role::Button, t(lang, "app.refresh")),
        (Role::ComboBox, t(lang, "prefs.language")),
    ];
    if on_game {
        optional.push((Role::Button, t(lang, "shell.library_hide")));
    }
    let bar_bottom = brand.bottom() + 12.0;
    let mut missing = Vec::new();
    for (role, label) in optional {
        let in_bar: Vec<egui::Rect> = h
            .query_all_by_role_and_label(role, label)
            .map(|n| n.rect())
            .filter(|r| r.top() < bar_bottom)
            .collect();
        match in_bar.as_slice() {
            [] => missing.push(label),
            [rect] => {
                assert!(
                    window_rect(size).expand(0.5).contains_rect(*rect),
                    "{what}: \"{label}\" at {rect:?} leaves the window"
                );
                rects.push(*rect);
            }
            _ => panic!("{what}: \"{label}\" twice"),
        }
    }
    // The banner has its own "More actions"; the bar's is the one in the bar.
    let more: Vec<egui::Rect> = h
        .query_all_by_role_and_label(Role::Button, t(lang, "shell.more"))
        .map(|n| n.rect())
        .filter(|r| r.top() < bar_bottom)
        .collect();
    assert_eq!(
        more.len(),
        usize::from(!missing.is_empty()),
        "{what}: More actions only when needed"
    );
    rects.extend(&more);
    for rect in &more {
        assert!(
            window_rect(size).expand(0.5).contains_rect(*rect),
            "{what}: More actions at {rect:?}"
        );
    }
    for (i, a) in rects.iter().enumerate() {
        for b in &rects[i + 1..] {
            assert!(
                !a.shrink(0.5).intersects(b.shrink(0.5)),
                "{what}: top-bar controls overlap: {a:?} {b:?}"
            );
        }
    }
    if let Some(more) = more.first() {
        h.get_all_by_role_and_label(Role::Button, t(lang, "shell.more"))
            .find(|n| n.rect() == *more)
            .unwrap()
            .click();
        h.run_steps(3);
        for label in &missing {
            // In the menu the language is a submenu, so a button like the others.
            let rect = h.get_by_label(label).rect();
            assert!(
                window_rect(size).expand(0.5).contains_rect(rect),
                "{what}: \"{label}\" in More actions at {rect:?} leaves the window"
            );
        }
        h.key_press(egui::Key::Escape);
        h.run_steps(3);
        for label in &missing {
            assert!(h.query_by_label(label).is_none(), "{what}: the menu closes");
        }
    }
    missing
}

#[test]
fn every_language_fits_the_main_views_in_a_small_window() {
    let dir = tempfile::tempdir().unwrap();
    for size in [SIZE, NARROW] {
        for lang in Lang::ALL.into_iter().filter(|l| crate::fonts::can_show(*l)) {
            let mut home = window(lang, dir.path(), false, None, size);
            let folded = top_bar_fits(&mut home, lang, false, size);
            assert!(
                home.query_by_label(t(lang, "welcome.title")).is_some(),
                "{lang:?}: home grid"
            );
            if size == SIZE && matches!(lang, Lang::En | Lang::Pt) {
                assert!(folded.is_empty(), "{lang:?}: the whole bar fits at 1024: {folded:?}");
            }

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
                    let mut h = window(lang, dir.path(), installed, Some(tab), size);
                    let what = format!("{lang:?} {size:?} {tab:?} installed={installed}");
                    if tab != Tab::Compare {
                        inside(&h, size, Role::Button, primary, &what);
                    }
                    for tab_node in h.query_all_by_role(Role::Tab) {
                        let rect = tab_node.rect();
                        assert!(
                            window_rect(size).expand(0.5).contains_rect(rect),
                            "{what}: tab strip at {rect:?}"
                        );
                    }
                    top_bar_fits(&mut h, lang, true, size);
                }
            }

            // Preferences, opened over the home grid: the language selector is labelled and the dialog fits.
            home.state_mut().prefs_open();
            home.run_steps(3);
            let labelled = home
                .query_all_by_role_and_label(Role::ComboBox, t(lang, "prefs.language"))
                .count();
            let in_bar = usize::from(!folded.contains(&t(lang, "prefs.language")));
            assert_eq!(
                labelled,
                1 + in_bar,
                "{lang:?}: the top bar's and the dialog's selectors are both named"
            );
            inside(
                &home,
                size,
                Role::Button,
                t(lang, "close"),
                &format!("{lang:?} {size:?} preferences"),
            );
        }
    }
}

#[test]
fn choosing_a_language_applies_at_once_and_is_saved() {
    let dir = tempfile::tempdir().unwrap();
    let mut h = window(Lang::En, dir.path(), false, None, SIZE);
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
