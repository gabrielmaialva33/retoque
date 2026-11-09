use retoque_core::{receipt::ReceiptStatus, *};
use std::{fs, path::PathBuf};

struct Fixture {
    _temp: tempfile::TempDir,
    game: PathBuf,
    source: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game [synthetic]");
        let source = temp.path().join("release");
        fs::create_dir(&game).unwrap();
        fs::create_dir(&source).unwrap();
        fs::write(
            source.join("OptiScaler.dll"),
            b"MZ OptiScaler working as %s ReShade64.dll v1",
        )
        .unwrap();
        fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_create v1").unwrap();
        fs::write(source.join(INI_NAME), b"[DlssNr]\r\nEnabled=auto\r\n").unwrap();
        Self {
            _temp: temp,
            game,
            source,
        }
    }
    fn options(&self) -> InstallOptions {
        InstallOptions {
            game_dir: self.game.clone(),
            source_dir: self.source.clone(),
            copy_nvngx: false,
            ..Default::default()
        }
    }
    fn install(&self) {
        apply(&plan_install(&self.options()).unwrap(), &mut |_| {}).unwrap();
    }
}
#[test]
fn changed_target_invalidates_plan_without_writes() {
    let f = Fixture::new();
    let plan = plan_install(&f.options()).unwrap();
    fs::write(f.game.join("dxgi.dll"), b"foreign after confirmation").unwrap();
    assert!(
        apply(&plan, &mut |_| {})
            .unwrap_err()
            .to_string()
            .contains("changed since planning")
    );
    assert!(!f.game.join(STATE_DIR).exists());
    assert_eq!(
        fs::read(f.game.join("dxgi.dll")).unwrap(),
        b"foreign after confirmation"
    );
}
#[test]
fn changed_payload_invalidates_plan_without_writes() {
    let f = Fixture::new();
    let plan = plan_install(&f.options()).unwrap();
    fs::write(f.source.join("OptiScaler.dll"), b"MZ changed payload").unwrap();
    assert!(apply(&plan, &mut |_| {}).is_err());
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
}
#[test]
fn modified_public_steps_cannot_bypass_model_protection() {
    let f = Fixture::new();
    let mut plan = plan_install(&f.options()).unwrap();
    plan.steps.push(Step::Remove {
        path: MODEL_NAME.into(),
    });
    assert!(
        apply(&plan, &mut |_| {})
            .unwrap_err()
            .to_string()
            .contains("plan was modified")
    );
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
}
#[test]
fn status_and_reversible_ini_and_progress() {
    let f = Fixture::new();
    let mut opts = f.options();
    opts.enable_nr = true;
    let plan = plan_install(&opts).unwrap();
    let mut steps = Vec::new();
    apply(&plan, &mut |done| steps.push(done)).unwrap();
    assert_eq!(steps.len(), plan.steps.len());
    assert!(
        steps
            .iter()
            .enumerate()
            .all(|(index, s)| s.index == index && s.total == steps.len())
    );
    let s = status(&f.game).unwrap();
    assert_eq!(s.nr_enabled, Some(true));
    assert!(!s.model_present);
    assert_eq!(s.receipt.unwrap().status, ReceiptStatus::Complete);
    #[cfg(target_os = "linux")]
    assert_eq!(
        s.launch_option.as_deref(),
        Some("WINEDLLOVERRIDES=\"dxgi=n,b\" %command%")
    );
    apply(&plan_uninstall(&f.game).unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
}
#[test]
fn stale_uninstall_does_not_delete_user_edit() {
    let f = Fixture::new();
    f.install();
    let plan = plan_uninstall(&f.game).unwrap();
    fs::write(f.game.join(INI_NAME), b"user changes after plan").unwrap();
    assert!(apply(&plan, &mut |_| {}).is_err());
    assert!(f.game.join("dxgi.dll").exists());
    apply(&plan_uninstall(&f.game).unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read(f.game.join(INI_NAME)).unwrap(), b"user changes after plan");
    assert_eq!(
        status(&f.game).unwrap().receipt.unwrap().status,
        ReceiptStatus::UninstallIncomplete
    );
}
#[test]
fn asi_and_optiscaler_proxy_do_not_duplicate_file_steps() {
    for name in ["OptiScaler.dll", "synthetic.asi"] {
        let f = Fixture::new();
        let mut opts = f.options();
        opts.proxy = name.into();
        let plan = plan_install(&opts).unwrap();
        assert!(plan.refusals.is_empty());
        assert_eq!(
            plan.steps
                .iter()
                .filter(|s| matches!(s,Step::Copy { to,.. } if to == "OptiScaler.dll"))
                .count(),
            1
        );
        apply(&plan, &mut |_| {}).unwrap();
        apply(&plan_uninstall(&f.game).unwrap(), &mut |_| {}).unwrap();
        assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
    }
}
#[cfg(unix)]
#[test]
fn replaced_parent_after_planning_is_refused() {
    let f = Fixture::new();
    fs::create_dir(f.source.join("OptiScaler")).unwrap();
    fs::write(f.source.join("OptiScaler/helper.dll"), b"helper").unwrap();
    let plan = plan_install(&f.options()).unwrap();
    let outside = f._temp.path().join("outside");
    fs::create_dir(&outside).unwrap();
    std::os::unix::fs::symlink(&outside, f.game.join("OptiScaler")).unwrap();
    assert!(apply(&plan, &mut |_| {}).is_err());
    assert_eq!(fs::read_dir(outside).unwrap().count(), 0);
    assert!(!f.game.join(STATE_DIR).exists());
}
#[cfg(unix)]
#[test]
fn foreign_symlink_is_backed_up_as_link_and_restored() {
    let f = Fixture::new();
    let original = f._temp.path().join("external.dll");
    fs::write(&original, b"external").unwrap();
    std::os::unix::fs::symlink(&original, f.game.join("dxgi.dll")).unwrap();
    let mut opts = f.options();
    opts.replace_foreign = true;
    apply(&plan_install(&opts).unwrap(), &mut |_| {}).unwrap();
    assert!(!f.game.join("dxgi.dll").is_symlink());
    apply(&plan_uninstall(&f.game).unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read_link(f.game.join("dxgi.dll")).unwrap(), original);
    assert_eq!(fs::read(&original).unwrap(), b"external");
}
#[cfg(target_os = "linux")]
#[test]
fn process_starting_after_plan_blocks_apply() {
    use std::{
        io::{BufRead, BufReader},
        process::{Command, Stdio},
    };
    let f = Fixture::new();
    f.install();
    let plan = plan_uninstall(&f.game).unwrap();
    let mut child = Command::new("python3").args(["-c","import mmap,sys\nf=open(sys.argv[1],'rb')\nm=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)\nprint('ready',flush=True)\nsys.stdin.read()"]).arg(f.game.join("dxgi.dll")).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .unwrap();
    let processes = running_processes(&f.game);
    let result = apply(&plan, &mut |_| {});
    drop(child.stdin.take());
    child.wait().unwrap();
    assert_eq!(line.trim(), "ready");
    assert!(processes.iter().any(|(pid, _)| *pid == child.id()));
    assert!(matches!(result,Err(Error::Refused(r)) if r.iter().any(|r| r.exit_code() == 3)));
    assert!(f.game.join("dxgi.dll").exists());
}
