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
        id: "steam:1".into(),
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
    window_with_appearance(lang, target, installed, tab, size, theme::Appearance::Dark)
}

fn window_with_appearance(
    lang: Lang,
    target: &Path,
    installed: bool,
    tab: Option<Tab>,
    size: [f32; 2],
    appearance: theme::Appearance,
) -> Harness<'static, RetoqueApp> {
    let mut app = app();
    app.lang = lang;
    app.saved.appearance = appearance;
    app.payload = Some("/synthetic/payload".into());
    app.payload_build = Some(Ok("old".into()));
    app.library_state.games.push(game(target));
    app.saved.favorites.insert("steam:1".into());
    app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
    app.tuning.installed = Some(Ok(vec![]));
    if let Some(tab) = tab {
        app.selected = Some("steam:1".into());
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
    let expected_theme = if appearance == theme::Appearance::Light {
        egui::Theme::Light
    } else {
        egui::Theme::Dark
    };
    let mut h = Harness::builder()
        .with_size(size)
        .with_theme(expected_theme)
        .with_context_setup(move |ctx| {
            crate::theme::apply(ctx);
            appearance.apply(ctx);
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
    assert_eq!(h.state().context.theme(), expected_theme);
    assert!(
        h.output()
            .shapes
            .iter()
            .any(|s| matches!(&s.shape, egui::Shape::Rect(r) if r.fill == theme::for_theme(expected_theme).BG)),
        "main view uses the chosen palette"
    );
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
    for appearance in [theme::Appearance::Dark, theme::Appearance::Light] {
        for size in [SIZE, NARROW] {
            for lang in Lang::ALL.into_iter().filter(|l| crate::fonts::can_show(*l)) {
                if appearance == theme::Appearance::Light && !matches!(lang, Lang::En | Lang::Pt) {
                    continue;
                }
                let mut home = window_with_appearance(lang, dir.path(), false, None, size, appearance);
                let folded = top_bar_fits(&mut home, lang, false, size);
                assert!(
                    home.query_by_label(t(lang, "welcome.title")).is_some(),
                    "{lang:?}: home grid"
                );
                let home_what = format!("{lang:?} {size:?} home");
                let search = inside(&home, size, Role::TextInput, t(lang, "lib.search"), &home_what);
                assert!(search.width() >= 180.0, "{home_what}: search width {}", search.width());
                let home_controls = [
                    search,
                    inside(&home, size, Role::CheckBox, t(lang, "lib.installed_only"), &home_what),
                    inside(&home, size, Role::CheckBox, t(lang, "lib.attention_only"), &home_what),
                    inside(&home, size, Role::Button, t(lang, "lib.add_folder"), &home_what),
                    inside(&home, size, Role::Button, t(lang, "folders.title"), &home_what),
                ];
                for (i, a) in home_controls.iter().enumerate() {
                    for b in &home_controls[i + 1..] {
                        assert!(
                            !a.shrink(0.5).intersects(b.shrink(0.5)),
                            "{home_what}: filter row overlap {a:?} {b:?}"
                        );
                    }
                }
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
                        let mut h = window_with_appearance(lang, dir.path(), installed, Some(tab), size, appearance);
                        let what = format!("{lang:?} {size:?} {tab:?} installed={installed}");
                        if tab == Tab::Install {
                            let only = inside(&h, size, Role::CheckBox, t(lang, "lib.installed_only"), &what);
                            let folders = inside(&h, size, Role::Button, t(lang, "folders.title"), &what);
                            assert!(
                                !only.shrink(0.5).intersects(folders.shrink(0.5)),
                                "{what}: sidebar filter overlap {only:?} {folders:?}"
                            );
                        }
                        if tab != Tab::Compare {
                            let main = inside(&h, size, Role::Button, primary, &what);
                            let play = inside(&h, size, Role::Button, t(lang, "play.action"), &what);
                            assert!(
                                !main.shrink(0.5).intersects(play.shrink(0.5)),
                                "{what}: Play overlaps primary"
                            );
                            for tab in h.query_all_by_role(Role::Tab) {
                                assert!(play.bottom() <= tab.rect().top(), "{what}: Play overlaps tab content");
                            }
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
}

#[test]
fn choosing_a_language_applies_at_once_and_is_saved() {
    let dir = tempfile::tempdir().unwrap();
    let mut h = window(Lang::En, dir.path(), false, None, SIZE);
    h.state_mut().prefs_open();
    h.run_steps(8);
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

#[test]
fn appearance_changes_immediately_and_survives_restart_in_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let dir = tempfile::tempdir().unwrap();
        let mut h = window(lang, dir.path(), false, None, SIZE);
        h.state_mut().prefs.open = true;
        h.run_steps(8);
        for choice in [
            theme::Appearance::Light,
            theme::Appearance::Dark,
            theme::Appearance::System,
        ] {
            h.get_by_role_and_label(Role::ComboBox, t(lang, "prefs.appearance"))
                .click();
            h.run_steps(2);
            h.get_by_label(t(lang, choice.label())).click();
            h.run_steps(3);
            assert_eq!(h.state().saved.appearance, choice);
            let ctx = &h.state().context;
            let expected = match choice {
                theme::Appearance::Light => egui::Theme::Light,
                theme::Appearance::Dark => egui::Theme::Dark,
                theme::Appearance::System => ctx.system_theme().unwrap_or(egui::Theme::Dark),
            };
            assert_eq!(ctx.theme(), expected);
            assert!(
                h.output()
                    .shapes
                    .iter()
                    .any(|s| matches!(&s.shape, egui::Shape::Rect(r) if r.fill == theme::for_theme(expected).BG))
            );
            let saved: Saved = serde_json::from_value(serde_json::to_value(&h.state().saved).unwrap()).unwrap();
            let restored = egui::Context::default();
            theme::apply(&restored);
            saved.appearance.apply(&restored);
            assert_eq!(restored.theme(), expected);
        }
        let mut old = serde_json::to_value(&h.state().saved).unwrap();
        old.as_object_mut().unwrap().remove("appearance");
        assert_eq!(
            serde_json::from_value::<Saved>(old).unwrap().appearance,
            theme::Appearance::Dark
        );
    }
}

/// Physical window sizes divided by zoom give egui's logical viewport. Exercise all translations, including
/// the owner's 1.2 zoom and the minimum window at 1.5, without depending on a compositor or graphics driver.
#[test]
fn audited_banner_and_tiles_fit_at_every_zoom_and_language() {
    let dir = tempfile::tempdir().unwrap();
    let logo = dir.path().join("logo.png");
    std::fs::write(&logo, include_bytes!("../assets/retoque-256.png")).unwrap();
    for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
        for zoom in [1.0, 1.2, 1.5] {
            for lang in Lang::ALL {
                assert!(
                    crate::fonts::can_show(lang),
                    "layout qualification needs a font for {lang:?}"
                );
                for compact in [false, true] {
                    let mut state = app();
                    state.lang = lang;
                    state.tab = if compact { Tab::Compare } else { Tab::Install };
                    state.view.status = Some(Ok(Status {
                        optiscaler_proxies: vec!["dxgi.dll".into()],
                        proxy_version: Some("10.0.0.1".into()),
                        forwarder_version: Some("10.0.0.1".into()),
                        ..Default::default()
                    }));
                    state.library_state.art.insert(
                        "steam:1".into(),
                        retoque_discovery::Artwork {
                            logo: Some(logo.clone()),
                            ..Default::default()
                        },
                    );
                    let game = game(dir.path());
                    let mut h = Harness::builder()
                        .with_size([size[0] / zoom, size[1] / zoom])
                        .with_context_setup(move |ctx| {
                            crate::theme::apply(ctx);
                            crate::fonts::apply(ctx, lang);
                            egui_extras::install_image_loaders(ctx);
                        })
                        .build_ui_state(
                            move |ui, state: &mut RetoqueApp| {
                                state.game_banner(ui, &game);
                                state.feature_tiles(ui);
                            },
                            state,
                        );
                    h.run_steps(4);
                    let banner = h
                        .output()
                        .shapes
                        .iter()
                        .find_map(|s| match &s.shape {
                            egui::Shape::Rect(r)
                                if r.fill == theme::DARK.CARD && r.rect.width() > 400. && r.rect.top() < 20. =>
                            {
                                Some(r.rect)
                            }
                            _ => None,
                        })
                        .expect("banner background");
                    for button in h.query_all_by_role(Role::Button) {
                        assert!(
                            banner.expand(0.5).contains_rect(button.rect()),
                            "{lang:?} {size:?} zoom={zoom} compact={compact}: button {:?} outside {banner:?}",
                            button.rect()
                        );
                    }
                    assert!(
                        h.query_by_label("OptiScaler 10.0.0.1 · NR component 10.0.0.1")
                            .is_none()
                    );
                    let tiles: Vec<_> = h
                        .query_all_by_role(Role::Label)
                        .filter(|n| n.rect().height() == 80.)
                        .map(|n| n.rect())
                        .collect();
                    assert_eq!(tiles.len(), 5);
                    let mut rows: Vec<Vec<egui::Rect>> = vec![];
                    for tile in tiles {
                        assert!(tile.top() >= banner.bottom());
                        assert!(tile.right() <= size[0] / zoom);
                        if rows.last().is_none_or(|row| row[0].top() != tile.top()) {
                            rows.push(vec![]);
                        }
                        rows.last_mut().unwrap().push(tile);
                    }
                    assert!(rows.len() <= 2, "{lang:?} {size:?} zoom={zoom}: 5 or 3+2 tiles");
                    for row in &rows {
                        assert!(row.len() >= 2, "orphan tile");
                        assert!(
                            (row.last().unwrap().right() - banner.right()).abs() < 2.,
                            "row does not fill width: {row:?} {banner:?}"
                        );
                        for tile in row {
                            assert!((tile.width() - row[0].width()).abs() < 1.);
                        }
                    }
                }
            }
        }
    }
}

/// Explicit local visual review only. Reads the installed library, never applies a plan or persists app state.
/// Images stay in the caller's scratch directory, not snapshots checked into the repository.
#[cfg(feature = "visual-tests")]
#[test]
#[ignore = "manual read-only library render; requires RETOQUE_LOOK_SHOTS"]
fn render_audited_library() {
    render_library_appearance(theme::Appearance::Dark, false);
}

#[cfg(feature = "visual-tests")]
#[test]
#[ignore = "manual read-only light theme render; requires RETOQUE_LOOK_SHOTS"]
fn render_light_theme() {
    render_library_appearance(theme::Appearance::Light, true);
}

#[cfg(feature = "visual-tests")]
#[test]
#[ignore = "manual read-only dark theme render; requires RETOQUE_LOOK_SHOTS"]
fn render_dark_theme() {
    render_library_appearance(theme::Appearance::Dark, true);
}

#[cfg(feature = "visual-tests")]
fn render_library_appearance(appearance: theme::Appearance, theme_review: bool) {
    let out = PathBuf::from(std::env::var("RETOQUE_LOOK_SHOTS").expect("scratch output folder"));
    let phase = std::env::var("RETOQUE_LOOK_PHASE").unwrap_or_else(|_| "after".into());
    let (games, _) = retoque_discovery::discover(&retoque_discovery::Config::default());
    assert!(!games.is_empty(), "real library is required for this manual review");
    let statuses: HashMap<_, _> = games
        .iter()
        .flat_map(|g| &g.targets)
        .filter_map(|t| retoque_core::status(&t.path).ok().map(|s| (t.path.clone(), s)))
        .collect();
    let light = appearance == theme::Appearance::Light;
    let review_dir = tempfile::tempdir().unwrap();
    std::fs::write(review_dir.path().join("OptiScaler.ini"), "[DlssNr]\nEnabled=false\n").unwrap();
    let groups = if theme_review {
        vec![
            ("library", None),
            ("game", Some(Tab::Install)),
            ("settings", Some(Tab::Settings)),
            ("review", Some(Tab::Install)),
            ("preferences", None),
        ]
    } else {
        vec![
            ("defects", Some(Tab::Install)),
            ("identity", Some(Tab::Install)),
            ("library", None),
            ("attention-synthetic", None),
            ("chrome", Some(Tab::Settings)),
            ("minimum", Some(Tab::Install)),
        ]
    };
    for zoom in [1.0, 1.2] {
        for &(group, tab) in &groups {
            let mut state = app();
            state.lang = Lang::Pt;
            state.saved.appearance = appearance;
            state.tuning.gpus = Some(vec![]);
            state.payload = std::env::var_os("RETOQUE_SOURCE_DIR").map(PathBuf::from);
            state.library_state.games = games.clone();
            state.library_state.status = statuses.clone();
            if group == "attention-synthetic" {
                // Extra visual fixture only: the real-library images above retain their on-disk states.
                state.library_state.attention_only = true;
                for (index, game) in games.iter().take(3).enumerate() {
                    let path = &game.targets[0].path;
                    if let Some(status) = state.library_state.status.get_mut(path) {
                        if index == 0
                            && let Some(receipt) = &mut status.receipt
                        {
                            receipt.status = ReceiptStatus::Partial;
                        }
                        if index == 1 {
                            status.build_sha256 = Some("synthetic-other-build".into());
                        }
                    }
                    if index == 2 {
                        state.doctor_badges.insert(path.clone(), 1);
                    }
                }
            }
            for game in &games {
                if let Runtime::Proton { app_id, .. } = game.runtime {
                    state.library_state.art.insert(
                        game.id.clone(),
                        retoque_discovery::steam_artwork(app_id, dirs::home_dir().as_deref()),
                    );
                }
            }
            let game = games
                .iter()
                .find(|g| g.name.to_lowercase().contains("cyberpunk"))
                .expect("review game");
            let target = &game.targets[0].path;
            state.payload_build = statuses.get(target).and_then(|s| s.build_sha256.clone()).map(Ok);
            if let Some(tab) = tab {
                state.selected = Some(game.id.clone());
                state.tab = tab;
                state.view.target = Some(target.clone());
                state.view.status = statuses.get(target).cloned().map(Ok);
                if let Ok(bytes) = std::fs::read(target.join("OptiScaler.ini")) {
                    let ini = Ini::parse(&bytes);
                    state.view.ini_values = crate::state::settings::IniValues::read(&ini);
                    state.view.ini = Some(Ok(Some(ini)));
                }
            }
            if group == "preferences" {
                state.prefs.open = true;
            }
            if group == "review" {
                let plan = retoque_core::plan_settings(
                    review_dir.path(),
                    &[("DlssNr".into(), "Enabled".into(), "true".into())],
                )
                .unwrap();
                state.review_open = true;
                state.review = Some(Ok(ReviewSession {
                    title: t(Lang::Pt, "set.save").trim_end_matches('…').into(),
                    kind: ReviewKind::Standard(PlanKind::Settings),
                    groups: vec![
                        plan.steps()
                            .iter()
                            .map(|step| step_group(step, plan.steps(), true))
                            .collect(),
                    ],
                    plans: vec![plan],
                    request_id: 0,
                    target: Some(target.clone()),
                }));
            }
            let expected_theme = if light { egui::Theme::Light } else { egui::Theme::Dark };
            let mut h = Harness::builder()
                .with_theme(expected_theme)
                .with_size(if group == "minimum" {
                    [820. / zoom, 520. / zoom]
                } else {
                    [1280. / zoom, 800. / zoom]
                })
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    appearance.apply(ctx);
                    crate::fonts::apply(ctx, Lang::Pt);
                    egui_extras::install_image_loaders(ctx);
                })
                .build_ui_state(
                    move |ui, state: &mut RetoqueApp| {
                        state.context = ui.ctx().clone();
                        if group == "chrome" {
                            let mut edits = std::mem::take(&mut state.view.edits);
                            let commands = state.cmd_tx.clone();
                            state.settings_tab(ui, &mut edits, &commands);
                            state.view.edits = edits;
                        } else {
                            state.window(ui);
                        }
                    },
                    state,
                );
            // File-image loaders work asynchronously; advance frames until their textures are ready.
            for _ in 0..30 {
                h.run_steps(1);
                std::thread::sleep(std::time::Duration::from_millis(20));
            }
            if !matches!(group, "chrome" | "review" | "preferences") {
                let size = if group == "minimum" {
                    [820. / zoom, 520. / zoom]
                } else {
                    [1280. / zoom, 800. / zoom]
                };
                top_bar_fits(&mut h, Lang::Pt, tab.is_some(), size);
            }
            assert_eq!(h.state().context.theme(), expected_theme);
            h.render()
                .expect("offscreen render")
                .save(out.join(if light {
                    format!("app-theme-{group}-{zoom:.1}x.png")
                } else if theme_review {
                    format!("app-theme-dark-{group}-{zoom:.1}x.png")
                } else {
                    format!("app-look-{group}-{phase}-{zoom:.1}x.png")
                }))
                .unwrap();
        }
    }
}

#[test]
fn audited_install_intent_precedes_information_and_versions_live_in_details() {
    use retoque_data::doctor::{Finding, Severity};
    let dir = tempfile::tempdir().unwrap();
    for lang in Lang::ALL {
        for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
            let mut state = app();
            state.lang = lang;
            state.view.target = Some(dir.path().into());
            state.view.status = Some(Ok(Status::default()));
            state.view.doctor = Some(crate::tabs::doctor::DoctorCard {
                findings: vec![Finding {
                    id: "informational",
                    severity: Severity::Info,
                    title_key: "tile.dlss",
                    why_key: "tile.dlss_detail",
                    evidence: vec![],
                    repair: None,
                }],
            });
            let mut h = Harness::builder()
                .with_size(size)
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    crate::fonts::apply(ctx, lang);
                })
                .build_ui_state(|ui, state: &mut RetoqueApp| state.install_tab(ui), state);
            h.run_steps(2);
            assert!(
                h.get_by_label(t(lang, "inst.plan_title")).rect().top()
                    < h.get_by_label(t(lang, "doctor.title")).rect().top()
            );
        }
        let mut h = window(lang, dir.path(), true, Some(Tab::Install), [1280., 800.]);
        let status = h.state_mut().view.status.as_mut().unwrap().as_mut().unwrap();
        status.optiscaler_proxies.push("dxgi.dll".into());
        status.proxy_version = Some("10.0.0.1".into());
        status.forwarder_version = Some("10.0.0.1".into());
        h.run_steps(2);
        assert!(h.query_by_label_contains("10.0.0.1").is_none());
        h.get_by_label(t(lang, "shell.game_details")).click();
        h.run_steps(3);
        h.get_by_label_contains("OptiScaler 10.0.0.1");
        h.get_by_label(&format!(
            "{} · {}",
            store_name(lang, Store::Steam),
            runtime_text(lang, &game(dir.path()).runtime)
        ));
    }
}

#[test]
fn audited_sidebar_keeps_the_game_banner_inside_the_zoomed_window() {
    let dir = tempfile::tempdir().unwrap();
    for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
        for zoom in [1.0, 1.2, 1.5] {
            let logical = [size[0] / zoom, size[1] / zoom];
            for lang in Lang::ALL {
                let mut h = window(lang, dir.path(), true, Some(Tab::Install), logical);
                let rect = h
                    .output()
                    .shapes
                    .iter()
                    .find_map(|s| match &s.shape {
                        egui::Shape::Rect(r)
                            if r.fill == theme::DARK.CARD
                                && r.rect.left() > logical[0] * 0.25
                                && r.rect.top() < 160.
                                && r.rect.width() > 100. =>
                        {
                            Some(r.rect)
                        }
                        _ => None,
                    })
                    .expect("game banner");
                assert!(rect.right() <= logical[0], "{lang:?} {size:?} at {zoom}: {rect:?}");
                top_bar_fits(&mut h, lang, true, logical);
            }
        }
    }
}

#[test]
fn audited_brand_mark_is_inside_the_named_home_button() {
    let dir = tempfile::tempdir().unwrap();
    for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
        for lang in Lang::ALL {
            let mut h = window(lang, dir.path(), true, Some(Tab::Install), size);
            let button = h.get_by_role_and_label(Role::Button, t(lang, "brand")).rect();
            let mark = h
                .output()
                .shapes
                .iter()
                .find_map(|s| match &s.shape {
                    egui::Shape::Rect(r) if r.fill == theme::DARK.CANVAS && (r.rect.width() - 28.).abs() < 0.5 => {
                        Some(r.rect)
                    }
                    _ => None,
                })
                .expect("Retoque mark is painted");
            assert!(button.contains_rect(mark));
            let text = h
                .output()
                .shapes
                .iter()
                .find_map(|s| match &s.shape {
                    egui::Shape::Text(t) if t.galley.job.text == "Retoque" && t.pos.y < 60. => Some(t.pos),
                    _ => None,
                })
                .unwrap();
            assert!(mark.right() < text.x, "mark stays beside the word");
            top_bar_fits(&mut h, lang, true, size);
            h.get_by_role_and_label(Role::Button, t(lang, "brand")).click();
            h.run_steps(2);
            assert!(h.state().selected.is_none(), "whole brand remains the way home");
        }
    }
}

#[test]
fn audited_attention_filter_combines_search_favorites_and_live_findings() {
    let dir = tempfile::tempdir().unwrap();
    for lang in Lang::ALL {
        for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
            let mut state = app();
            state.lang = lang;
            state.payload_build = Some(Ok("build".into()));
            for index in 0..6 {
                let path = dir.path().join(index.to_string());
                let mut g = game(&path);
                g.id = format!("steam:{}", index + 1);
                g.name = format!("Game {index}");
                let status = Status {
                    receipt: (index != 4 && index != 5).then(|| Receipt {
                        status: if index == 1 {
                            ReceiptStatus::Partial
                        } else {
                            ReceiptStatus::Complete
                        },
                        updated: String::new(),
                        records: vec![],
                    }),
                    build_sha256: Some(if index == 3 { "old" } else { "build" }.into()),
                    optiscaler_proxies: if index == 5 { vec!["dxgi.dll".into()] } else { vec![] },
                    ..Default::default()
                };
                if index == 2 {
                    state.doctor_badges.insert(path.clone(), 1);
                }
                if index == 4 {
                    g.store = Store::Manual;
                    g.runtime = Runtime::Unknown;
                }
                state.library_state.status.insert(path, status);
                state.library_state.games.push(g);
            }
            state.saved.favorites.extend(["steam:1".into(), "steam:4".into()]);
            let mut h = Harness::builder()
                .with_size(size)
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    crate::fonts::apply(ctx, lang);
                })
                .build_ui_state(
                    |ui, state: &mut RetoqueApp| {
                        ui.set_width(320.);
                        state.library(ui);
                    },
                    state,
                );
            h.run_steps(2);
            let metadata = format!(
                "{} · {}",
                store_name(lang, Store::Steam),
                runtime_short(lang, &game(dir.path()).runtime)
            );
            let painted_context = |h: &Harness<'_, RetoqueApp>| {
                h.output()
                    .shapes
                    .iter()
                    .any(|s| matches!(&s.shape, egui::Shape::Text(t) if t.galley.job.text == metadata))
            };
            assert!(painted_context(&h), "mixed list names its store/runtime");
            h.get_by_label(t(lang, "lib.attention_only")).click();
            h.run_steps(3);
            let groups = h.state().library_groups();
            assert_eq!(groups[0].1, [3], "quiet favorite is filtered too");
            assert_eq!(groups[1].1, [1, 2, 5]);
            assert!(groups[2].1.is_empty());
            assert!(!painted_context(&h), "homogeneous filtered list omits repeated context");
            h.state_mut().doctor_badges.clear();
            h.run_steps(2);
            assert_eq!(
                h.state().library_groups()[1].1,
                [1, 5],
                "updated findings take effect without a rescan"
            );
            h.get_by_label(t(lang, "lib.installed_only")).click();
            h.run_steps(2);
            assert_eq!(h.state().library_groups()[0].1, [3]);
            h.state_mut().library_state.search = "Game 0".into();
            h.run_steps(2);
            h.get_by_label(t(lang, "lib.no_attention"));
            assert!(h.state().library_groups().iter().all(|(_, games)| games.is_empty()));
        }
    }
}

#[test]
fn audited_library_titles_wrap_twice_and_only_attention_states_are_painted() {
    let name = "A Very Long Game Name With An Edition And Subtitle";
    for lang in Lang::ALL {
        for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
            let mut h = Harness::builder()
                .with_size(size)
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    crate::fonts::apply(ctx, lang);
                })
                .build_ui(move |ui| {
                    ui.set_width((size[0] * 0.32).clamp(180., 320.));
                    widgets::game_row(
                        ui,
                        name,
                        "",
                        (t(lang, "st.current"), theme::DARK.GOOD),
                        None,
                        false,
                        None,
                        None,
                        false,
                    );
                    widgets::game_row(
                        ui,
                        "Attention",
                        "",
                        (t(lang, "st.partial"), theme::DARK.BAD),
                        None,
                        false,
                        None,
                        None,
                        true,
                    );
                    widgets::cover_card(
                        ui,
                        160.,
                        name,
                        None,
                        (t(lang, "st.current"), theme::DARK.GOOD),
                        None,
                        false,
                    );
                });
            h.run_steps(2);
            let titles: Vec<_> = h
                .output()
                .shapes
                .iter()
                .filter_map(|s| match &s.shape {
                    egui::Shape::Text(text) if text.galley.job.text == name => Some(text),
                    _ => None,
                })
                .collect();
            assert!(titles.len() >= 2);
            assert!(
                titles.iter().all(|t| t.galley.rows.len() == 2),
                "{lang:?}: row and card names wrap before ellipsis"
            );
            let painted = |key| {
                h.output()
                    .shapes
                    .iter()
                    .any(|s| matches!(&s.shape, egui::Shape::Text(text) if text.galley.job.text == t(lang, key)))
            };
            assert!(!painted("st.current"), "quiet state has no repeated visual caption");
            assert!(
                painted("st.partial"),
                "attention still has a word, not just a colored edge"
            );
            assert_eq!(
                h.query_all_by_role_and_label(Role::Button, &format!("{name}, {}", t(lang, "st.current")))
                    .count(),
                2,
                "both row and cover retain the full accessible name and state"
            );
        }
    }
}

#[test]
fn audited_settings_have_one_muted_line_and_one_toolbar_row() {
    let dir = tempfile::tempdir().unwrap();
    for lang in Lang::ALL {
        for size in [[1280., 800.], [1024., 768.], [820., 520.], [320., 800.]] {
            let mut state = app();
            state.lang = lang;
            state.view.target = Some(dir.path().into());
            state.view.ini = Some(Ok(Some(Ini::parse(b"[DlssNr]\nEnabled=true\n"))));
            let mut h = Harness::builder()
                .with_size(size)
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    crate::fonts::apply(ctx, lang);
                })
                .build_ui_state(
                    |ui, state: &mut RetoqueApp| {
                        let mut edits = std::mem::take(&mut state.view.edits);
                        let commands = state.cmd_tx.clone();
                        state.settings_tab(ui, &mut edits, &commands);
                        state.view.edits = edits;
                    },
                    state,
                );
            h.run_steps(2);
            assert!(
                h.query_by_label(t(lang, "set.saved_title")).is_none(),
                "no explanation card"
            );
            let note = h.get_by_label(t(lang, "set.saved_summary")).rect();
            assert!(note.height() <= 22., "one line even when narrow");
            let search = h.get_by_role_and_label(Role::TextInput, t(lang, "set.filter")).rect();
            assert!(search.width() >= 150.);
            assert!(note.bottom() <= search.top());
            let controls = if let Some(menu) = h.query_by_role_and_label(Role::Button, t(lang, "set.tools")) {
                let rect = menu.rect();
                menu.click();
                h.run_steps(2);
                h.get_by_label(t(lang, "set.only_changed"));
                let undo = h.get_by_label(t(lang, "undo.title")).rect();
                assert!(window_rect(size).contains_rect(undo), "overflow menu fits");
                h.key_press(egui::Key::Escape);
                h.run_steps(2);
                vec![rect]
            } else {
                vec![
                    h.get_by_label(t(lang, "set.only_changed")).rect(),
                    h.get_by_label(t(lang, "undo.title")).rect(),
                ]
            };
            for control in controls {
                assert!(
                    (search.center().y - control.center().y).abs() < 1.0,
                    "toolbar stays on one row"
                );
                assert!(!search.intersects(control));
                assert!(window_rect(size).contains_rect(control));
            }
        }
    }
}

#[test]
fn audited_compare_controls_have_descriptive_names_and_tooltips_in_every_language() {
    use egui_kittest::kittest::NodeT as _;
    for lang in Lang::ALL {
        for size in [[1280., 800.], [1024., 768.], [820., 520.]] {
            let mut h = Harness::builder()
                .with_size(size)
                .with_context_setup(move |ctx| {
                    crate::theme::apply(ctx);
                    crate::fonts::apply(ctx, lang);
                })
                .build_ui_state(
                    move |ui, view: &mut crate::compare::CompareView| view.audit_toolbar(ui, lang),
                    crate::compare::CompareView::default(),
                );
            h.run_steps(3);
            for button in h.query_all_by_role(Role::Button) {
                let label = button.accesskit_node().label().unwrap_or_default();
                assert!(
                    !label.trim().is_empty() && label != "0" && label != "…",
                    "unnamed comparison control"
                );
                assert!(
                    window_rect(size).contains_rect(button.rect()),
                    "{lang:?} {size:?}: {label} leaves toolbar"
                );
            }
            for key in ["cmp.actual_size", "exposure_reset", "cmp.tools", "cmp.expand"] {
                h.get_by_role_and_label(Role::Button, t(lang, key)).hover();
                h.run_steps(5);
                assert!(
                    h.output()
                        .shapes
                        .iter()
                        .any(|s| matches!(&s.shape, egui::Shape::Text(text) if text.galley.job.text == t(lang, key))),
                    "{lang:?} {size:?}: {key} has a visible tooltip"
                );
            }
            h.get_by_role_and_label(Role::Slider, t(lang, "exposure"));
        }
    }
}
