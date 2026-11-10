//! One process-global fixture: no real Steam library, settings, game, or model is changed.
#![cfg(target_os = "linux")]

use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    thread,
    time::{Duration, Instant},
};

use egui::accesskit::Role;
use egui_kittest::{
    Harness,
    kittest::{self, NodeT as _, Queryable},
};
use retoque_app::RetoqueApp;
use retoque_core::{ini::Ini, receipt::ReceiptStatus};
use retoque_data::models::Catalog;

type Window = Harness<'static, RetoqueApp>;
// A settings write is told by its toast ("Settings applied · {game}"), not by a line on the page.
const SAVED: &str = "Settings applied";

fn window() -> Window {
    // Include the naming audit: composite settings inputs are labelled by their row.
    Harness::builder()
        .with_size([1400., 900.])
        .with_accessibility_check(true)
        .build_eframe(|cc| RetoqueApp::new(cc))
}

#[track_caller]
fn wait(ui: &mut Window, description: &str, ready: impl Fn(&Window) -> bool) {
    let start = Instant::now();
    loop {
        ui.step();
        if ready(ui) {
            return;
        }
        assert!(
            // Generous: the whole workspace's suites run beside this window and can starve it of CPU.
            start.elapsed() < Duration::from_secs(30),
            "timed out: {description}\n{:#?}",
            ui.root()
        );
        thread::sleep(Duration::from_millis(10));
    }
}

fn label(ui: &Window, text: &str) -> bool {
    ui.query_all_by_label_contains(text).next().is_some()
}

fn wait_label(ui: &mut Window, text: &str) {
    wait(ui, text, |ui| label(ui, text));
}

fn click(ui: &mut Window, role: Role, text: &str) {
    ui.get_by_role_and_label(role, text).click_accesskit();
    ui.run_steps(3);
}

/// The install previews sit in a closed section on the Overview: "Install options" in the card that says what
/// Retoque will do before install, "Reinstall options" at the end once installed.
fn open_install_options(ui: &mut Window) {
    if ui.query_by_label("Preview install").is_none() {
        wait(ui, "install options section", |ui| {
            label(ui, "Install options") || label(ui, "Reinstall options")
        });
        let section = if label(ui, "Reinstall options") {
            "Reinstall options"
        } else {
            "Install options"
        };
        ui.get_by_label(section).click_accesskit();
        ui.run_steps(3);
    }
    wait_label(ui, "Preview install");
}

fn select_game(ui: &mut Window) {
    // A library entry is announced as "{name}, {install state}".
    let row = |n: &kittest::AccessKitNode<'_>| {
        n.role() == Role::Button && n.label().is_some_and(|l| l.starts_with("FakeGame, "))
    };
    wait(ui, "FakeGame library row", |ui| ui.query_by(row).is_some());
    ui.get_by(row).click_accesskit();
    ui.run_steps(3);
    open_install_options(ui);
}

fn choose_stage(ui: &mut Window, value: &str) {
    click(ui, Role::Tab, "Settings");
    wait_label(ui, "NR stage");
    // The combo box is labelled by its setting's name (labelled_by), so it is found by role and name.
    let choices: Vec<_> = ui.query_all_by_role_and_label(Role::ComboBox, "NR stage").collect();
    assert_eq!(choices.len(), 1, "NR stage should have one choice widget");
    choices[0].click_accesskit();
    ui.run_steps(3);
    ui.get_by_label(match value {
        "1" => "Before the upscaler (experimental)",
        "0" => "After the upscaler",
        "auto" => "Automatic",
        _ => value,
    })
    .click_accesskit();
    ui.run_steps(3);
}

fn review_stage(ui: &mut Window, game: &Path, before: &str, after: &str) {
    let unchanged = snapshot(game);
    choose_stage(ui, after);
    click(ui, Role::Button, "Review changes…");
    wait_label(ui, &format!("[DlssNr] Stage: {before} -> {after}"));
    assert_eq!(snapshot(game), unchanged, "preview must not write");
    click(ui, Role::Button, "Apply");
    wait(ui, "settings write and UI result", |ui| {
        label(ui, SAVED) && ini_value(game, "Stage") == after
    });
}

fn ini_value(game: &Path, key: &str) -> String {
    Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap())
        .get("DlssNr", key)
        .unwrap()
        .to_owned()
}

fn snapshot(root: &Path) -> BTreeMap<PathBuf, Option<Vec<u8>>> {
    fn visit(root: &Path, dir: &Path, out: &mut BTreeMap<PathBuf, Option<Vec<u8>>>) {
        for entry in fs::read_dir(dir).unwrap() {
            let path = entry.unwrap().path();
            let value = if path.is_dir() {
                None
            } else {
                Some(fs::read(&path).unwrap())
            };
            out.insert(path.strip_prefix(root).unwrap().to_owned(), value);
            if path.is_dir() {
                visit(root, &path, out);
            }
        }
    }
    let mut out = BTreeMap::new();
    visit(root, root, &mut out);
    out
}

struct RunningGame(Child);
impl Drop for RunningGame {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

#[test]
fn real_window_install_settings_model_refusal_uninstall_and_stale_plan() {
    let started = Instant::now();
    let temp = tempfile::tempdir().unwrap();
    let home = temp.path().join("home");
    let steam = home.join(".local/share/Steam");
    let apps = steam.join("steamapps");
    let game = apps.join("common/FakeGame");
    let source = temp.path().join("payload");
    fs::create_dir_all(&game).unwrap();
    fs::create_dir_all(apps.join("compatdata/4242/pfx")).unwrap();
    fs::create_dir_all(source.join("OptiScaler")).unwrap();
    fs::write(game.join("FakeGame.exe"), b"MZ synthetic game fixture").unwrap();
    fs::write(
        apps.join("libraryfolders.vdf"),
        format!(r#""libraryfolders" {{ "0" {{ "path" "{}" }} }}"#, steam.display()),
    )
    .unwrap();
    fs::write(
        apps.join("appmanifest_4242.acf"),
        r#""AppState" { "appid" "4242" "name" "FakeGame" "installdir" "FakeGame" }"#,
    )
    .unwrap();
    fs::write(
        source.join("OptiScaler.dll"),
        b"MZ OptiScaler working as synthetic fixture",
    )
    .unwrap();
    fs::write(
        source.join("nvngx.dll_dlssnr.dll"),
        b"MZ dlssnr_call_ synthetic forwarder",
    )
    .unwrap();
    fs::write(source.join("OptiScaler/support.txt"), b"synthetic support").unwrap();
    let repo = Path::new(env!("CARGO_MANIFEST_DIR")).ancestors().nth(4).unwrap();
    fs::copy(repo.join("OptiScaler.ini"), source.join("OptiScaler.ini")).unwrap();
    let driver = temp.path().join("driver.dll");
    fs::write(&driver, b"synthetic driver").unwrap();
    // This binary has exactly one test. Set all environment variables before creating any app workers.
    unsafe {
        for (key, value) in [
            ("HOME", home.clone()),
            ("XDG_DATA_HOME", home.join(".local/share")),
            ("XDG_CONFIG_HOME", home.join(".config")),
            ("XDG_CACHE_HOME", home.join(".cache")),
            ("RETOQUE_SOURCE_DIR", source.clone()),
            ("RETOQUE_DEPLOY_TEST_NVNGX", driver),
        ] {
            std::env::set_var(key, value);
        }
        std::env::set_var("RETOQUE_LANG", "en");
        std::env::remove_var("RETOQUE_SELECT");
        std::env::remove_var("RETOQUE_TAB");
        std::env::remove_var("RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP");
    }
    let original = snapshot(&game);
    let mut ui = window();
    select_game(&mut ui);
    wait_label(&mut ui, "Not installed");
    open_install_options(&mut ui);
    click(&mut ui, Role::Button, "Preview install");
    wait(&mut ui, "install plan", |ui| {
        ui.query_by_role_and_label(Role::Button, "Install").is_some()
    });
    assert!(label(&ui, "What will be done"));
    assert_eq!(snapshot(&game), original);
    click(&mut ui, Role::Button, "Install");
    // The Retoque tile says it now: "Retoque: Up to date, Local build" (the hash is in its hover).
    wait_label(&mut ui, "Retoque: Up to date");
    for file in [
        "dxgi.dll",
        "OptiScaler.dll",
        "nvngx.dll_dlssnr.dll",
        "OptiScaler.ini",
        "OptiScaler/support.txt",
        "_nvngx.dll",
    ] {
        assert!(game.join(file).is_file(), "missing installed {file}");
    }
    assert_eq!(
        fs::read(game.join("dxgi.dll")).unwrap(),
        fs::read(source.join("OptiScaler.dll")).unwrap()
    );
    assert_eq!(
        fs::read(game.join("OptiScaler.dll")).unwrap(),
        fs::read(source.join("OptiScaler.dll")).unwrap()
    );
    assert_eq!(
        fs::read(game.join("nvngx.dll_dlssnr.dll")).unwrap(),
        fs::read(source.join("nvngx.dll_dlssnr.dll")).unwrap()
    );
    let receipt_path = game.join(".retoque/receipt.txt");
    let receipt = fs::read(&receipt_path).unwrap();
    assert_eq!(
        retoque_core::status(&game).unwrap().receipt.unwrap().status,
        ReceiptStatus::Complete
    );
    let installed_ini = fs::read(game.join("OptiScaler.ini")).unwrap();
    review_stage(&mut ui, &game, "auto", "1");
    assert_eq!(fs::read(&receipt_path).unwrap(), receipt);

    let model_source = temp.path().join("synthetic-model.dll");
    fs::write(&model_source, b"MZ synthetic user-owned model fixture").unwrap();
    let model = Catalog::open(None)
        .unwrap()
        .import(&model_source, Some("Synthetic model"))
        .unwrap();
    let model_value = Catalog::model_file_value(&model, true);
    assert!(model_value.starts_with("Z:\\"));
    // No catalog refresh action exists yet. Reopening uses the production startup loading path.
    drop(ui);
    let mut ui = window();
    select_game(&mut ui);
    click(&mut ui, Role::Tab, "Components");
    // The button is named after its model for screen readers ("Use in this game: Synthetic model").
    wait_label(&mut ui, "Use in this game: Synthetic model");
    let before_model = snapshot(&game);
    // Every model card has this button, so it is named after its model for screen readers.
    click(&mut ui, Role::Button, "Use in this game: Synthetic model");
    wait_label(&mut ui, &format!("[DlssNr] ModelFile: auto -> {model_value}"));
    assert_eq!(snapshot(&game), before_model);
    click(&mut ui, Role::Button, "Apply");
    wait(&mut ui, "model write", |ui| {
        label(ui, SAVED) && ini_value(&game, "ModelFile") == model_value
    });
    assert_eq!(fs::read(&receipt_path).unwrap(), receipt);
    assert_eq!(fs::read(&model.path).unwrap(), fs::read(&model_source).unwrap());

    let ready = temp.path().join("mapped-ready");
    let child = RunningGame(
        Command::new("python3")
            .args([
                "-c",
                concat!(
                    "import mmap,pathlib,sys,time; ",
                    "f=open(sys.argv[1],'rb'); ",
                    "m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); ",
                    "pathlib.Path(sys.argv[2]).touch(); time.sleep(300)"
                ),
            ])
            .arg(game.join("dxgi.dll"))
            .arg(&ready)
            .stdout(Stdio::null())
            .spawn()
            .unwrap(),
    );
    wait(&mut ui, "synthetic game maps proxy", |_| ready.exists());
    assert!(
        retoque_core::running_processes(&game)
            .iter()
            .any(|(pid, _)| *pid == child.0.id())
    );
    let before_refusal = snapshot(&game);
    click(&mut ui, Role::Tab, "Overview");
    open_install_options(&mut ui);
    click(&mut ui, Role::Button, "Preview install");
    wait_label(&mut ui, "The game is running");
    // A refused plan keeps its confirm in place, disabled.
    assert!(
        ui.get_by_role_and_label(Role::Button, "Update")
            .accesskit_node()
            .is_disabled()
    );
    assert_eq!(snapshot(&game), before_refusal);
    drop(child);

    // Modified settings are user-owned: uninstall must show Keep, not silently remove this INI.
    open_install_options(&mut ui);
    click(&mut ui, Role::Button, "Preview uninstall");
    wait_label(&mut ui, "left in place, it changed since this script wrote it");
    assert_eq!(snapshot(&game), before_refusal);
    click(&mut ui, Role::Button, "Cancel");
    click(&mut ui, Role::Tab, "Components");
    click(&mut ui, Role::Button, "Use the default search…");
    wait_label(&mut ui, &format!("[DlssNr] ModelFile: {model_value} -> auto"));
    click(&mut ui, Role::Button, "Apply");
    wait(&mut ui, "restore default model", |ui| {
        label(ui, SAVED) && ini_value(&game, "ModelFile") == "auto"
    });
    review_stage(&mut ui, &game, "1", "auto");
    assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), installed_ini);
    assert_eq!(fs::read(&receipt_path).unwrap(), receipt);
    click(&mut ui, Role::Tab, "Overview");
    open_install_options(&mut ui);
    click(&mut ui, Role::Button, "Preview uninstall");
    wait(&mut ui, "uninstall plan", |ui| {
        ui.query_by_role_and_label(Role::Button, "Uninstall").is_some()
    });
    click(&mut ui, Role::Button, "Uninstall");
    wait(&mut ui, "uninstalled on disk and in UI", |ui| {
        label(ui, "Not installed") && !receipt_path.exists()
    });
    let remaining = snapshot(&game);
    // The engine removes owned files and .retoque, but keeps the now-empty support directory.
    let mut expected = original.clone();
    expected.insert(PathBuf::from("OptiScaler"), None);
    assert_eq!(remaining, expected);
    assert!(model.path.is_file(), "uninstall must leave the user catalog intact");

    open_install_options(&mut ui);

    click(&mut ui, Role::Button, "Preview install");
    wait(&mut ui, "fresh reinstall plan", |ui| {
        ui.query_by_role_and_label(Role::Button, "Install").is_some()
    });
    let before_stale = snapshot(&game);
    click(&mut ui, Role::CheckBox, "Turn Neural Rendering (NR) on");
    wait_label(&mut ui, "changed after this preview: preview again.");
    assert!(ui.query_by_role_and_label(Role::Button, "Install").is_none());
    assert_eq!(snapshot(&game), before_stale);
    eprintln!(
        "GUI end-to-end flow completed in {:.3}s",
        started.elapsed().as_secs_f64()
    );
}
