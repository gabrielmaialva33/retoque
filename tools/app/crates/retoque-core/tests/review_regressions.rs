use retoque_core::*;
use std::{fs, path::PathBuf};

struct Fixture {
    temp: tempfile::TempDir,
    game: PathBuf,
    source: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        let source = temp.path().join("source");
        fs::create_dir(&game).unwrap();
        fs::create_dir(&source).unwrap();
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as synthetic").unwrap();
        fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_synthetic").unwrap();
        fs::write(source.join(INI_NAME), b"[DlssNr]\nEnabled=auto\n").unwrap();
        Self { temp, game, source }
    }
    fn plan(&self) -> Plan {
        plan_install(&InstallOptions {
            game_dir: self.game.clone(),
            source_dir: self.source.clone(),
            copy_nvngx: false,
            ..Default::default()
        })
        .unwrap()
    }
    fn install(&self) {
        apply(&self.plan(), &mut |_| {}).unwrap();
    }
}

#[cfg(unix)]
fn directory_link(target: &std::path::Path, link: &std::path::Path) {
    std::os::unix::fs::symlink(target, link).unwrap();
}
#[cfg(windows)]
fn directory_link(target: &std::path::Path, link: &std::path::Path) {
    let output = std::process::Command::new("cmd.exe")
        .args(["/D", "/C", "mklink", "/J"])
        .arg(link)
        .arg(target)
        .output()
        .unwrap();
    assert!(output.status.success(), "{output:?}");
}

#[cfg(any(unix, windows))]
#[test]
fn receipt_never_follows_state_link_inserted_between_steps() {
    let f = Fixture::new();
    let outside = f.temp.path().join("outside");
    fs::create_dir(&outside).unwrap();
    fs::write(outside.join("receipt.txt"), b"outside sentinel").unwrap();
    let result = apply(&f.plan(), &mut |done| {
        if done.index == 0 {
            fs::rename(f.game.join(STATE_DIR), f.temp.path().join("parked-state")).unwrap();
            directory_link(&outside, &f.game.join(STATE_DIR));
        }
    });
    assert!(result.is_err(), "state replacement must stop apply");
    assert_eq!(fs::read(outside.join("receipt.txt")).unwrap(), b"outside sentinel");
    assert_eq!(fs::read_dir(&outside).unwrap().count(), 1);
}

#[cfg(any(unix, windows))]
#[test]
fn error_receipt_never_follows_replaced_game_root() {
    let f = Fixture::new();
    let outside = f.temp.path().join("outside");
    fs::create_dir(&outside).unwrap();
    let result = apply(&f.plan(), &mut |done| {
        if done.index == 0 {
            fs::rename(&f.game, f.temp.path().join("parked-game")).unwrap();
            directory_link(&outside, &f.game);
        }
    });
    assert!(result.is_err());
    assert_eq!(
        fs::read_dir(&outside).unwrap().count(),
        0,
        "error handler wrote outside"
    );
}

#[cfg(any(unix, windows))]
#[test]
fn uninstall_final_cleanup_never_follows_replaced_state_directory() {
    let f = Fixture::new();
    f.install();
    let outside = f.temp.path().join("outside");
    fs::create_dir_all(outside.join("backup/empty")).unwrap();
    fs::write(outside.join("receipt.txt"), b"outside sentinel").unwrap();
    let plan = plan_uninstall(&f.game).unwrap();
    let result = apply(&plan, &mut |done| {
        if done.index + 1 == done.total {
            fs::rename(f.game.join(STATE_DIR), f.temp.path().join("parked-state")).unwrap();
            directory_link(&outside, &f.game.join(STATE_DIR));
        }
    });
    assert!(result.is_err());
    assert_eq!(fs::read(outside.join("receipt.txt")).unwrap(), b"outside sentinel");
    assert!(outside.join("backup/empty").is_dir());
}

#[test]
fn uninstall_stops_before_deleting_user_edit_between_steps() {
    let f = Fixture::new();
    f.install();
    let result = apply(&plan_uninstall(&f.game).unwrap(), &mut |done| {
        if done.index == 0 {
            fs::write(f.game.join(INI_NAME), b"new user configuration").unwrap();
        }
    });
    assert!(result.is_err());
    assert_eq!(fs::read(f.game.join(INI_NAME)).unwrap(), b"new user configuration");
    assert_eq!(
        status(&f.game).unwrap().receipt.unwrap().status,
        receipt::ReceiptStatus::UninstallIncomplete
    );
}

#[test]
fn second_proxy_content_change_invalidates_preview_without_writes() {
    let f = Fixture::new();
    let proxy = f.game.join("winmm.dll");
    fs::write(&proxy, b"foreign existing file").unwrap();
    let plan = f.plan();
    assert!(plan.refusals().is_empty());
    fs::write(&proxy, b"MZ OptiScaler working as another proxy").unwrap();
    assert!(
        f.plan()
            .refusals()
            .iter()
            .any(|r| matches!(r, Refusal::SecondProxy { .. }))
    );
    assert!(apply(&plan, &mut |_| panic!("stale plan made progress")).is_err());
    assert!(!f.game.join(STATE_DIR).exists());
    assert!(!f.game.join("dxgi.dll").exists());
}

#[cfg(unix)]
#[test]
fn source_symlink_target_changes_cannot_supply_unreviewed_bytes() {
    let f = Fixture::new();
    let real = f.source.join("real.dll");
    fs::rename(f.source.join("OptiScaler.dll"), &real).unwrap();
    std::os::unix::fs::symlink(&real, f.source.join("OptiScaler.dll")).unwrap();
    let plan = f.plan();
    fs::write(&real, b"changed payload after preview").unwrap();
    assert!(apply(&plan, &mut |_| panic!("unreviewed bytes made progress")).is_err());
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
}

#[test]
fn clearing_copied_refusals_cannot_authorize_a_sealed_plan() {
    let f = Fixture::new();
    fs::write(f.game.join("winmm.dll"), b"MZ OptiScaler working as another proxy").unwrap();
    let plan = f.plan();
    assert!(!plan.refusals().is_empty());
    let mut copied = plan.refusals().to_vec();
    copied.clear();
    assert!(copied.is_empty());
    assert!(!plan.refusals().is_empty());
    assert!(apply(&plan, &mut |_| panic!("refused plan made progress")).is_err());
    assert!(!f.game.join(STATE_DIR).exists());
}

#[cfg(target_os = "linux")]
mod linux {
    use super::*;
    use std::{
        io::{BufRead, BufReader},
        process::{Child, Command, Stdio},
    };
    struct Mapped(Child);
    impl Mapped {
        fn new(path: &std::path::Path) -> Self {
            let mut child = Self(Command::new("python3").args(["-c", "import mmap,sys; f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); print('ready',flush=True); sys.stdin.read()"])
                .arg(path).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap());
            let mut ready = String::new();
            BufReader::new(child.0.stdout.take().unwrap())
                .read_line(&mut ready)
                .unwrap();
            assert_eq!(ready.trim(), "ready");
            child
        }
    }
    impl Drop for Mapped {
        fn drop(&mut self) {
            let _ = self.0.kill();
            let _ = self.0.wait();
        }
    }
    #[test]
    fn process_starting_between_install_steps_stops_further_dll_writes() {
        let f = Fixture::new();
        let mut child = None;
        let mut count = 0;
        let result = apply(&f.plan(), &mut |done| {
            count += 1;
            if done.index == 0 {
                child = Some(Mapped::new(&f.game.join("dxgi.dll")));
            }
        });
        assert!(
            running_processes(&f.game)
                .iter()
                .any(|(pid, _)| *pid == child.as_ref().unwrap().0.id())
        );
        assert!(result.is_err());
        assert_eq!(count, 1);
        assert!(!f.game.join("OptiScaler.dll").exists());
    }
    fn check_path(name: &str) {
        let mut f = Fixture::new();
        let new = f.temp.path().join(name);
        fs::rename(&f.game, &new).unwrap();
        f.game = new;
        fs::write(f.game.join("mapped.bin"), b"mapped fixture").unwrap();
        let child = Mapped::new(&f.game.join("mapped.bin"));
        assert!(running_processes(&f.game).iter().any(|(pid, _)| *pid == child.0.id()));
        let plan = f.plan();
        assert!(plan.refusals().iter().any(|r| matches!(r, Refusal::GameRunning { .. })));
        assert!(apply(&plan, &mut |_| {}).is_err());
        assert!(!f.game.join(STATE_DIR).exists());
    }
    #[test]
    fn literal_backslash_in_game_directory_does_not_hide_mapping() {
        check_path("backslash\\game");
    }
    #[test]
    fn newline_in_game_directory_does_not_hide_mapping() {
        check_path("newline\ngame");
    }
}

#[cfg(unix)]
#[test]
fn unchanged_symlink_payload_is_still_supported() {
    let f = Fixture::new();
    let real = f.source.join("real.dll");
    fs::rename(f.source.join("OptiScaler.dll"), &real).unwrap();
    std::os::unix::fs::symlink(&real, f.source.join("OptiScaler.dll")).unwrap();
    f.install();
    assert_eq!(fs::read(f.game.join("dxgi.dll")).unwrap(), fs::read(real).unwrap());
}

#[test]
fn incomplete_or_changed_backup_is_never_restored() {
    let f = Fixture::new();
    fs::write(f.game.join("dxgi.dll"), b"foreign original").unwrap();
    let options = InstallOptions {
        game_dir: f.game.clone(),
        source_dir: f.source.clone(),
        replace_foreign: true,
        copy_nvngx: false,
        ..Default::default()
    };
    apply(&plan_install(&options).unwrap(), &mut |_| {}).unwrap();
    let copy = status(&f.game)
        .unwrap()
        .receipt
        .unwrap()
        .records
        .into_iter()
        .find_map(|r| {
            if let receipt::Record::Backup { copy, .. } = r {
                Some(copy)
            } else {
                None
            }
        })
        .unwrap();
    fs::write(f.game.join(&copy), b"incomplete backup").unwrap();
    let plan = plan_uninstall(&f.game).unwrap();
    assert!(
        plan.steps()
            .iter()
            .any(|s| matches!(s, Step::Keep { path,why } if path == &copy && why.contains("incomplete")))
    );
    apply(&plan, &mut |_| {}).unwrap();
    assert!(!f.game.join("dxgi.dll").exists());
    assert_eq!(fs::read(f.game.join(copy)).unwrap(), b"incomplete backup");
    assert!(f.game.join(".retoque/receipt.txt").exists());
}

// Test adapter keeps existing refusal/staleness probes at the same boundary after typestate migration.
fn apply(
    plan: &retoque_core::Plan,
    progress: &mut dyn FnMut(retoque_core::StepDone),
) -> Result<(), retoque_core::Error> {
    retoque_core::apply(&plan.clone().into_verified()?, progress)
}
