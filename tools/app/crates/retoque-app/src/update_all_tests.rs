//! Synthetic installs only: the batch uses the real sealed plan/apply engine.
use crate::{app::*, update_all::*};
use egui_kittest::{Harness, kittest::Queryable};
use std::{fs, path::Path, sync::mpsc};

struct Fixture {
    _temp: tempfile::TempDir,
    source: PathBuf,
    games: Vec<(String, PathBuf)>,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let source = temp.path().join("payload");
        fs::create_dir(&source).unwrap();
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as %s old").unwrap();
        fs::write(source.join(retoque_core::FORWARDER_NAME), b"MZ dlssnr_call_create old").unwrap();
        fs::write(source.join(retoque_core::INI_NAME), b"[DlssNr]\nEnabled=false\n").unwrap();
        let mut games = Vec::new();
        for (name, proxy, nr, reshade) in [("Alpha", "winmm.dll", true, false), ("Beta", "dxgi.dll", false, true)] {
            let dir = temp.path().join(name);
            fs::create_dir(&dir).unwrap();
            if reshade {
                fs::write(dir.join(proxy), b"MZ ReShade original").unwrap();
            }
            let options = InstallOptions {
                game_dir: dir.clone(),
                source_dir: source.clone(),
                proxy: proxy.into(),
                enable_nr: nr,
                keep_reshade: reshade,
                copy_nvngx: false,
                ..Default::default()
            };
            let plan = retoque_core::plan_install(&options).unwrap().into_verified().unwrap();
            retoque_core::apply(&plan, &mut |_| {}).unwrap();
            games.push((name.into(), dir));
        }
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as %s new").unwrap();
        fs::write(source.join(retoque_core::FORWARDER_NAME), b"MZ dlssnr_call_create new").unwrap();
        Self {
            _temp: temp,
            source,
            games,
        }
    }
    fn rows(&self) -> Vec<Row> {
        prepare(self.games.clone(), &self.source, retoque_core::MenuLanguage::Keep)
    }
}
fn bytes(dir: &Path) -> Vec<(PathBuf, Vec<u8>)> {
    let mut files = Vec::new();
    for entry in fs::read_dir(dir).unwrap() {
        let path = entry.unwrap().path();
        if path.is_dir() {
            files.extend(bytes(&path));
        } else {
            files.push((path.clone(), fs::read(path).unwrap()));
        }
    }
    files.sort();
    files
}

#[test]
fn update_all_preserves_each_games_options_and_uses_only_reviewed_plans() {
    let f = Fixture::new();
    let before: Vec<_> = f.games.iter().map(|(_, p)| bytes(p)).collect();
    let rows = f.rows();
    assert!(rows.iter().all(|r| matches!(r.result, RowResult::Ready)));
    for (i, (_, dir)) in f.games.iter().enumerate() {
        assert_eq!(bytes(dir), before[i]);
    }
    for (i, row) in rows.iter().enumerate() {
        let status = retoque_core::status(&row.target).unwrap();
        let (_, ini) = retoque_core::read_ini(&row.target).unwrap().unwrap();
        let options = update_options(
            &row.target,
            &f.source,
            &status,
            &Ini::parse(&ini),
            retoque_core::MenuLanguage::Keep,
        )
        .unwrap();
        assert_eq!(options.proxy, if i == 0 { "winmm.dll" } else { "dxgi.dll" });
        assert_eq!(options.enable_nr, i == 0);
        assert_eq!(options.keep_reshade, i == 1);
        assert!(!options.copy_nvngx);
        assert!(!options.replace_foreign && !options.allow_second_proxy);
    }
    let plans = rows
        .iter()
        .enumerate()
        .map(|(i, r)| (i, r.plan.clone().unwrap()))
        .collect();
    let (tx, rx) = mpsc::channel();
    assert!(matches!(
        apply_batch(plans, &tx, &egui::Context::default()),
        Outcome::UpdateAll(Event::Done)
    ));
    let completed: Vec<_> = rx
        .try_iter()
        .filter_map(|e| match e {
            Outcome::UpdateAll(Event::Completed(i, result)) => Some((i, result)),
            _ => None,
        })
        .collect();
    assert_eq!(completed.len(), 2);
    assert!(matches!(completed[0], (0, RowResult::Updated)));
    assert!(matches!(completed[1], (1, RowResult::Updated)));
    for (i, (_, dir)) in f.games.iter().enumerate() {
        let status = retoque_core::status(dir).unwrap();
        assert_eq!(status.nr_enabled, Some(i == 0));
        assert_eq!(
            fs::read(dir.join(if i == 0 { "winmm.dll" } else { "dxgi.dll" })).unwrap(),
            fs::read(f.source.join("OptiScaler.dll")).unwrap()
        );
        assert!(!dir.join("_nvngx.dll").exists());
    }
    assert_eq!(
        fs::read(f.games[1].1.join("ReShade64.dll")).unwrap(),
        b"MZ ReShade original"
    );
}

#[test]
fn update_all_review_lists_ready_partial_foreign_unknown_and_refused() {
    let mut f = Fixture::new();
    let root = f._temp.path();
    for name in ["Partial", "Foreign", "Unknown", "Refused"] {
        let dir = root.join(name);
        fs::create_dir(&dir).unwrap();
        if name == "Partial" || name == "Refused" {
            fs::create_dir(dir.join(".retoque")).unwrap();
            fs::copy(
                f.games[0].1.join(".retoque/receipt.txt"),
                dir.join(".retoque/receipt.txt"),
            )
            .unwrap();
            fs::copy(f.games[0].1.join("winmm.dll"), dir.join("winmm.dll")).unwrap();
            fs::copy(f.games[0].1.join("OptiScaler.ini"), dir.join("OptiScaler.ini")).unwrap();
            if name == "Partial" {
                let receipt = dir.join(".retoque/receipt.txt");
                let text = fs::read_to_string(&receipt)
                    .unwrap()
                    .replace("status\tcomplete", "status\tpartial");
                fs::write(receipt, text).unwrap();
            } else {
                fs::write(dir.join(retoque_core::FORWARDER_NAME), b"foreign forwarder").unwrap();
            }
        } else if name == "Foreign" {
            fs::write(dir.join("dxgi.dll"), b"MZ OptiScaler working as %s legacy").unwrap();
        } else {
            fs::create_dir(dir.join(".retoque")).unwrap();
            fs::write(dir.join(".retoque/receipt.txt"), b"invalid receipt").unwrap();
        }
        f.games.push((name.into(), dir));
    }
    let rows = f.rows();
    assert_eq!(rows.len(), 6);
    assert!(matches!(&rows[2].result, RowResult::Skipped(s) if s.key == "update_all.partial"));
    assert!(matches!(&rows[3].result, RowResult::Skipped(s) if s.key == "update_all.foreign"));
    assert!(matches!(&rows[4].result, RowResult::Skipped(s) if s.key == "update_all.unknown"));
    assert!(matches!(&rows[5].result, RowResult::Skipped(s) if s.key == "update_all.refused"));
    assert!(rows[2..].iter().all(|r| r.plan.is_none()));
    for lang in [Lang::En, Lang::Pt] {
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        app.library_state.update_all.open = true;
        app.library_state.update_all.rows = rows.clone();
        let mut h = Harness::builder()
            .with_size([1024., 1000.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.update_all_modal(ui.ctx()), app);
        h.run_steps(2);
        for (name, _) in &f.games {
            h.get_by_label(name);
        }
        h.get_by_role_and_label(
            egui::Role::Button,
            &t(lang, "update_all.confirm").replace("{count}", "2"),
        );
        assert!(h.state().writing.is_empty());
    }
}

#[test]
fn update_all_stale_game_fails_without_blocking_the_next_game() {
    let f = Fixture::new();
    let rows = f.rows();
    fs::write(f.games[0].1.join("winmm.dll"), b"changed after review").unwrap();
    let before = bytes(&f.games[0].1);
    let (tx, rx) = mpsc::channel();
    apply_batch(
        rows.iter()
            .enumerate()
            .map(|(i, r)| (i, r.plan.clone().unwrap()))
            .collect(),
        &tx,
        &egui::Context::default(),
    );
    let completed: Vec<_> = rx
        .try_iter()
        .filter_map(|e| match e {
            Outcome::UpdateAll(Event::Completed(i, r)) => Some((i, r)),
            _ => None,
        })
        .collect();
    assert!(matches!(completed[0], (0, RowResult::Failed(_))));
    assert!(matches!(completed[1], (1, RowResult::Updated)));
    assert_eq!(bytes(&f.games[0].1), before);
}

#[cfg(target_os = "linux")]
#[test]
fn update_all_running_game_is_skipped_at_review_and_again_before_writes() {
    use std::{
        io::BufRead,
        process::{Command, Stdio},
    };
    struct Mapping(std::process::Child);
    impl Drop for Mapping {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    let f = Fixture::new();
    let rows_before_running = f.rows();
    let mut mapping = Mapping(Command::new("python3").args(["-c", "import mmap,sys; f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); print('ready',flush=True); sys.stdin.read()"])
        .arg(f.games[0].1.join("winmm.dll")).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap());
    let mut ready = String::new();
    std::io::BufReader::new(mapping.0.stdout.take().unwrap())
        .read_line(&mut ready)
        .unwrap();
    assert_eq!(ready.trim(), "ready");
    let before = bytes(&f.games[0].1);
    let rows = f.rows();
    assert!(matches!(&rows[0].result, RowResult::Skipped(s) if s.key == "update_all.running"));
    assert!(rows[0].plan.is_none());
    let (tx, rx) = mpsc::channel();
    apply_batch(
        rows_before_running
            .iter()
            .enumerate()
            .map(|(i, r)| (i, r.plan.clone().unwrap()))
            .collect(),
        &tx,
        &egui::Context::default(),
    );
    let results: Vec<_> = rx
        .try_iter()
        .filter_map(|e| match e {
            Outcome::UpdateAll(Event::Completed(i, r)) => Some((i, r)),
            _ => None,
        })
        .collect();
    assert!(matches!(&results[0], (0, RowResult::Skipped(s)) if s.key == "update_all.running"));
    assert!(matches!(results[1], (1, RowResult::Updated)));
    assert_eq!(bytes(&f.games[0].1), before);
}

fn wait_jobs(h: &mut Harness<'_, RetoqueApp>) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_secs(20);
    while h.state().jobs.busy() {
        assert!(std::time::Instant::now() < deadline, "batch worker did not finish");
        std::thread::sleep(std::time::Duration::from_millis(10));
        h.run_steps(2);
    }
    h.run();
}

#[test]
fn update_all_home_review_cancel_confirm_and_result_in_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let f = Fixture::new();
        let before: Vec<_> = f.games.iter().map(|(_, p)| bytes(p)).collect();
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        // Deliberately disagree with both games: these must have no effect on a batch update.
        app.saved.install_defaults.proxy = "version.dll".into();
        app.saved.install_defaults.enable_nr = true;
        app.saved.install_defaults.copy_nvngx = true;
        app.saved.install_defaults.keep_reshade = false;
        app.payload = Some(f.source.clone());
        app.payload_build = Some(Ok(retoque_core::status(&f.source).unwrap().build_sha256.unwrap()));
        for (name, dir) in &f.games {
            app.library_state.games.push(Game {
                id: name.clone(),
                name: name.clone(),
                store: Store::Manual,
                install_dir: dir.clone(),
                runtime: Runtime::Unknown,
                registered: true,
                dlss_anywhere: vec![],
                targets: vec![retoque_discovery::TargetDir {
                    path: dir.clone(),
                    executables: vec![],
                    score: 0,
                    dlss: vec![],
                    has_receipt: true,
                    proxies_present: vec![],
                    loaders: vec![],
                }],
            });
            app.library_state
                .status
                .insert(dir.clone(), retoque_core::status(dir).unwrap());
        }
        let mut h = Harness::builder().with_size([1024., 900.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.receive();
                app.home_grid(ui);
                app.update_all_modal(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        h.run_steps(2);
        h.get_by_role_and_label(egui::Role::Button, t(lang, "update_all.open"))
            .click();
        h.run_steps(2);
        wait_jobs(&mut h);
        for (name, _) in &f.games {
            h.get_by_label(name);
        }
        for (i, (_, dir)) in f.games.iter().enumerate() {
            assert_eq!(bytes(dir), before[i]);
        }
        h.get_by_role_and_label(egui::Role::Button, t(lang, "cancel")).click();
        h.run_steps(2);
        assert!(!h.state().library_state.update_all.open);
        h.get_by_role_and_label(egui::Role::Button, t(lang, "update_all.open"))
            .click();
        h.run_steps(2);
        wait_jobs(&mut h);
        let confirm = t(lang, "update_all.confirm").replace("{count}", "2");
        h.get_by_role_and_label(egui::Role::Button, &confirm).click();
        h.run_steps(2);
        wait_jobs(&mut h);
        assert!(h.state().library_state.update_all.finished);
        assert!(
            h.state()
                .library_state
                .update_all
                .rows
                .iter()
                .all(|r| matches!(r.result, RowResult::Updated))
        );
        assert!(h.state().writing.is_empty());
        let toast = t(lang, "update_all.done")
            .replace("{updated}", "2")
            .replace("{skipped}", "0")
            .replace("{failed}", "0");
        assert!(h.state().toasts.iter().any(|t| t.message == toast));
        for (i, (_, dir)) in f.games.iter().enumerate() {
            assert!(!dir.join("version.dll").exists());
            assert!(!dir.join("_nvngx.dll").exists());
            assert_eq!(retoque_core::status(dir).unwrap().nr_enabled, Some(i == 0));
        }
        h.get_by_role_and_label(egui::Role::Button, t(lang, "close")).click();
        h.run_steps(2);
        assert!(h.query_by_label(t(lang, "update_all.open")).is_none());
    }
}

#[test]
fn update_all_worker_failure_releases_all_reserved_games_and_names_failures() {
    let f = Fixture::new();
    let mut app = crate::app_tests::shell_tests::app();
    app.library_state.update_all.rows = f.rows();
    app.library_state.update_all.running = true;
    for (_, target) in &f.games {
        app.writing.insert(target.clone(), (0, 1));
    }
    app.update_all_failed("update-all-apply", "synthetic panic".into());
    assert!(app.writing.is_empty());
    assert!(!app.library_state.update_all.running);
    assert!(app.library_state.update_all.finished);
    assert!(
        app.library_state
            .update_all
            .rows
            .iter()
            .all(|r| matches!(r.result, RowResult::Failed(_)))
    );
}
