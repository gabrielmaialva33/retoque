use retoque_core::{
    receipt::{Receipt, ReceiptStatus, Record},
    *,
};
use std::{fs, path::PathBuf};

struct Fixture {
    _temp: tempfile::TempDir,
    game: PathBuf,
    ini: PathBuf,
}
impl Fixture {
    fn new(name: &str, bytes: &[u8]) -> Self {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        fs::create_dir(&game).unwrap();
        let ini = game.join(name);
        fs::write(&ini, bytes).unwrap();
        Self { _temp: temp, game, ini }
    }
    fn receipt(&self) -> PathBuf {
        let state = self.game.join(STATE_DIR);
        fs::create_dir(&state).unwrap();
        let path = state.join("receipt.txt");
        fs::write(&path, b"opaque receipt: settings must not parse or rewrite this\n").unwrap();
        path
    }
}
fn edit(section: &str, key: &str, value: &str) -> (String, String, String) {
    (section.into(), key.into(), value.into())
}
fn usage(plan: &Plan) -> bool {
    plan.refusals.iter().any(|r| matches!(r, Refusal::Usage(_)))
}

#[test]
fn actual_case_variant_bytes_and_missing_file() {
    let f = Fixture::new("optiscaler.INI", b";\xff\r\n[DlssNr]\r\nEnabled=false\r\n");
    assert_eq!(
        read_ini(&f.game).unwrap(),
        Some((f.ini.canonicalize().unwrap(), fs::read(&f.ini).unwrap()))
    );
    fs::remove_file(&f.ini).unwrap();
    assert_eq!(read_ini(&f.game).unwrap(), None);
    assert!(usage(&plan_settings(&f.game, &[edit("A", "k", "v")]).unwrap()));
}
#[test]
fn plans_exact_changes_with_sequential_old_values_and_never_touches_receipt() {
    let original = b";\xff\r\n[DlssNr]\r\nEnabled=auto\r\nStage=0\r\n";
    let f = Fixture::new("optiscaler.ini", original);
    let receipt = f.receipt();
    let receipt_bytes = fs::read(&receipt).unwrap();
    let plan = plan_settings(
        &f.game,
        &[
            edit("DlssNr", "Enabled", "auto"),
            edit("DlssNr", "Enabled", "true"),
            edit("DlssNr", "Enabled", "false"),
            edit("DlssNr", "ModelFile", "Z:\\models\\one.dll"),
        ],
    )
    .unwrap();
    assert_eq!(fs::read(&f.ini).unwrap(), original);
    assert_eq!(plan.steps.len(), 3);
    let old: Vec<_> = plan
        .steps
        .iter()
        .map(|s| {
            if let Step::SetIni { old, .. } = s {
                old.as_deref()
            } else {
                panic!("settings only")
            }
        })
        .collect();
    assert_eq!(old, vec![Some("auto"), Some("true"), None]);
    let mut progress = Vec::new();
    apply(&plan, &mut |step| progress.push(step)).unwrap();
    assert_eq!(progress.len(), 3);
    assert!(progress.iter().enumerate().all(|(i, p)| p.index == i && p.total == 3));
    assert_eq!(
        fs::read(&f.ini).unwrap(),
        b";\xff\r\n[DlssNr]\r\nModelFile=Z:\\models\\one.dll\r\nEnabled=false\r\nStage=0\r\n"
    );
    assert_eq!(fs::read(&receipt).unwrap(), receipt_bytes);
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 2);
}
#[test]
fn noops_leave_file_and_directory_unchanged_and_create_no_receipt() {
    let f = Fixture::new(INI_NAME, b"[A]\nk=v\n");
    let before = fs::metadata(&f.ini).unwrap().modified().unwrap();
    for edits in [vec![], vec![edit("a", "k", "v")]] {
        let plan = plan_settings(&f.game, &edits).unwrap();
        assert!(plan.steps.is_empty());
        apply(&plan, &mut |_| panic!("no progress for zero steps")).unwrap();
        assert_eq!(fs::metadata(&f.ini).unwrap().modified().unwrap(), before);
        assert_eq!(fs::read_dir(&f.game).unwrap().count(), 1);
    }
}
#[test]
fn duplicate_keys_old_is_last_and_every_copy_is_replaced() {
    let f = Fixture::new(INI_NAME, b"[A]\nk=one\nk=two\n");
    let plan = plan_settings(&f.game, &[edit("A", "k", "two")]).unwrap();
    assert!(matches!(&plan.steps[0],Step::SetIni { old: Some(old),.. } if old == "two"));
    apply(&plan, &mut |_| {}).unwrap();
    assert_eq!(fs::read(&f.ini).unwrap(), b"[A]\nk=two\nk=two\n");
}
#[test]
fn changed_ini_rejects_without_receipt_or_writes() {
    let f = Fixture::new(INI_NAME, b"[A]\nk=one\n");
    let plan = plan_settings(&f.game, &[edit("A", "k", "two")]).unwrap();
    fs::write(&f.ini, b"user changed it after preview\n").unwrap();
    assert!(
        apply(&plan, &mut |_| {})
            .unwrap_err()
            .to_string()
            .contains("changed since planning")
    );
    assert_eq!(fs::read(&f.ini).unwrap(), b"user changed it after preview\n");
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 1);
}
#[test]
fn user_change_between_steps_is_not_overwritten() {
    let f = Fixture::new(INI_NAME, b"[A]\nk=one\nj=one\n");
    let plan = plan_settings(&f.game, &[edit("A", "k", "two"), edit("A", "j", "two")]).unwrap();
    let mut count = 0;
    let result = apply(&plan, &mut |_| {
        count += 1;
        fs::write(&f.ini, b"external writer after first step\n").unwrap();
    });
    assert!(result.is_err());
    assert_eq!(count, 1);
    assert_eq!(fs::read(&f.ini).unwrap(), b"external writer after first step\n");
    assert!(!f.game.join(STATE_DIR).exists());
}
#[test]
fn rejects_multiline_edits_without_writes() {
    let f = Fixture::new(INI_NAME, b"[A]\nk=one\n");
    for e in [
        edit("A\n[B]", "k", "v"),
        edit("A", "k=x", "v"),
        edit("A", "k", "v\nother=true"),
    ] {
        let plan = plan_settings(&f.game, &[e]).unwrap();
        assert!(usage(&plan));
        assert!(apply(&plan, &mut |_| {}).is_err());
    }
    assert_eq!(fs::read(&f.ini).unwrap(), b"[A]\nk=one\n");
}
#[test]
fn zero_step_uninstall_removes_proxy_only_partial_receipt() {
    let temp = tempfile::tempdir().unwrap();
    let state = temp.path().join(STATE_DIR);
    fs::create_dir(&state).unwrap();
    let receipt = Receipt {
        status: ReceiptStatus::Partial,
        updated: "2026-11-05T12:34:56Z".into(),
        records: vec![Record::Proxy {
            name: "dxgi.dll".into(),
        }],
    };
    fs::write(state.join("receipt.txt"), receipt.to_text()).unwrap();
    let plan = plan_uninstall(temp.path()).unwrap();
    assert!(plan.steps.is_empty());
    assert!(plan.refusals.is_empty());
    apply(&plan, &mut |_| panic!("no file steps")).unwrap();
    assert_eq!(fs::read_dir(temp.path()).unwrap().count(), 0);
}
#[cfg(unix)]
#[test]
fn ambiguous_and_linked_ini_files_are_refused() {
    use std::os::unix::fs::symlink;
    let f = Fixture::new(INI_NAME, b"[A]\nk=v\n");
    fs::write(f.game.join("optiscaler.ini"), b"other").unwrap();
    assert!(read_ini(&f.game).is_err());
    assert!(usage(&plan_settings(&f.game, &[]).unwrap()));
    fs::remove_file(f.game.join("optiscaler.ini")).unwrap();
    fs::remove_file(&f.ini).unwrap();
    let external = f._temp.path().join("external.ini");
    fs::write(&external, b"external").unwrap();
    symlink(&external, &f.ini).unwrap();
    assert!(read_ini(&f.game).is_err());
    assert!(usage(&plan_settings(&f.game, &[]).unwrap()));
    fs::remove_file(&f.ini).unwrap();
    fs::write(&f.ini, b"[A]\nk=v\n").unwrap();
    assert_eq!(fs::read(&external).unwrap(), b"external");
}
#[cfg(unix)]
#[test]
fn preserves_mode_and_does_not_follow_preexisting_temporary_name() {
    use std::os::unix::fs::{PermissionsExt, symlink};
    let f = Fixture::new(INI_NAME, b"[A]\nk=v\n");
    fs::set_permissions(&f.ini, fs::Permissions::from_mode(0o600)).unwrap();
    let victim = f._temp.path().join("victim");
    fs::write(&victim, b"do not modify").unwrap();
    let old_temp = f.game.join(".OptiScaler.ini.retoque-tmp");
    symlink(&victim, &old_temp).unwrap();
    let inode = {
        use std::os::unix::fs::MetadataExt;
        fs::metadata(&f.ini).unwrap().ino()
    };
    apply(&plan_settings(&f.game, &[edit("A", "k", "new")]).unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read(&victim).unwrap(), b"do not modify");
    assert!(old_temp.is_symlink());
    assert_eq!(fs::metadata(&f.ini).unwrap().permissions().mode() & 0o777, 0o600);
    {
        use std::os::unix::fs::MetadataExt;
        assert_ne!(fs::metadata(&f.ini).unwrap().ino(), inode);
    }
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 2);
    assert!(!f.game.join(STATE_DIR).exists());
}
#[cfg(target_os = "linux")]
#[test]
fn running_game_refused_during_plan_and_when_starting_after_preview() {
    use std::{
        io::{BufRead, BufReader},
        process::{Command, Stdio},
    };
    let f = Fixture::new(INI_NAME, b"[A]\nk=v\n");
    let edits = [edit("A", "k", "new")];
    let plan = plan_settings(&f.game, &edits).unwrap();
    let mut child=Command::new("python3").args(["-c","import mmap,sys\nf=open(sys.argv[1],'rb')\nm=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)\nprint('ready',flush=True)\nsys.stdin.read()"]).arg(&f.ini).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .unwrap();
    let refused = plan_settings(&f.game, &edits).unwrap();
    let result = apply(&plan, &mut |_| {});
    drop(child.stdin.take());
    child.wait().unwrap();
    assert!(refused.refusals.iter().any(|r| r.exit_code() == 3));
    assert!(matches!(result,Err(Error::Refused(r)) if r.iter().any(|r|r.exit_code()==3)));
    assert_eq!(fs::read(&f.ini).unwrap(), b"[A]\nk=v\n");
    assert!(!f.game.join(STATE_DIR).exists());
}

#[test]
fn install_ini_steps_include_old_effective_value() {
    let f = Fixture::new(INI_NAME, b"[DlssNr]\nEnabled=auto\n");
    let source = f._temp.path().join("release");
    fs::create_dir(&source).unwrap();
    fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as %s").unwrap();
    fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_create").unwrap();
    let opts = InstallOptions {
        game_dir: f.game.clone(),
        source_dir: source,
        copy_nvngx: false,
        enable_nr: true,
        ..Default::default()
    };
    let plan = plan_install(&opts).unwrap();
    assert!(plan.refusals.is_empty());
    assert!(plan.steps.iter().any(
        |s| matches!(s,Step::SetIni {old:Some(old),key,value,..} if key=="Enabled" && old=="auto" && value=="true")
    ));
}

#[test]
fn settings_are_user_changes_and_uninstall_keeps_them() {
    let temp = tempfile::tempdir().unwrap();
    let game = temp.path().join("game");
    let source = temp.path().join("source");
    fs::create_dir(&game).unwrap();
    fs::create_dir(&source).unwrap();
    fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as %s").unwrap();
    fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_create").unwrap();
    fs::write(source.join(INI_NAME), b"[DlssNr]\nEnabled=auto\n").unwrap();
    let options = InstallOptions {
        game_dir: game.clone(),
        source_dir: source,
        copy_nvngx: false,
        ..Default::default()
    };
    apply(&plan_install(&options).unwrap(), &mut |_| {}).unwrap();
    let receipt = fs::read(game.join(".retoque/receipt.txt")).unwrap();
    apply(
        &plan_settings(&game, &[edit("DlssNr", "Enabled", "true")]).unwrap(),
        &mut |_| {},
    )
    .unwrap();
    assert_eq!(fs::read(game.join(".retoque/receipt.txt")).unwrap(), receipt);
    apply(&plan_uninstall(&game).unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read(game.join(INI_NAME)).unwrap(), b"[DlssNr]\nEnabled=true\n");
    assert_eq!(
        status(&game).unwrap().receipt.unwrap().status,
        ReceiptStatus::UninstallIncomplete
    );
    assert!(!game.join("dxgi.dll").exists());
}
