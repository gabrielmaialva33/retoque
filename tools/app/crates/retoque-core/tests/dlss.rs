use retoque_core::{Step, apply, plan_dlss_update, plan_game_restore, plan_uninstall};
use std::{fs, path::Path};
fn write(root: &Path, path: &str, bytes: &[u8]) {
    let p = root.join(path);
    fs::create_dir_all(p.parent().unwrap()).unwrap();
    fs::write(p, bytes).unwrap();
}
fn apply_plan(p: retoque_core::Plan) {
    apply(&p.into_verified().unwrap(), &mut |_| {}).unwrap();
}
fn source(root: &Path) -> std::path::PathBuf {
    let p = root.join("catalog");
    write(&p, "nvngx_dlss.dll", b"new SR");
    write(&p, "nvngx_dlssg.dll", b"new FG");
    write(&p, "receipt-marker.txt", b"Retoque DLSS receipt v1\n");
    p
}
#[test]
fn existing_only_backup_update_twice_restore_original() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    write(&g, "nvngx_dlss.dll", b"original");
    let files = vec!["nvngx_dlss.dll".into()];
    let p = plan_dlss_update(&g, &g, &s, &files).unwrap();
    assert!(p.steps().iter().any(|s| matches!(s, Step::Backup { .. })));
    assert!(!g.join(".retoque").exists());
    apply_plan(p);
    assert!(!g.join("nvngx_dlssg.dll").exists());
    fs::write(s.join("nvngx_dlss.dll"), b"newer SR").unwrap();
    let p = plan_dlss_update(&g, &g, &s, &files).unwrap();
    assert!(!p.steps().iter().any(|s| matches!(s, Step::Backup { .. })));
    apply_plan(p);
    apply_plan(plan_uninstall(&g).unwrap());
    assert_eq!(fs::read(g.join("nvngx_dlss.dll")).unwrap(), b"original");
}
#[test]
fn unreal_root_receipt_and_two_root_restoration() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    let target = g.join("Project/Binaries/Win64");
    fs::create_dir_all(&target).unwrap();
    let file = "Engine/Plugins/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll";
    write(&g, file, b"old SR");
    let p = plan_dlss_update(&g, &target, &s, &[file.into()]).unwrap();
    assert_eq!(p.game_dir(), g.canonicalize().unwrap());
    apply_plan(p);
    assert!(retoque_core::dlss_root_state(&g).unwrap().is_some());
    for p in plan_game_restore(&g, &target, false).unwrap() {
        apply_plan(p);
    }
    assert_eq!(fs::read(g.join(file)).unwrap(), b"old SR");
    assert!(!g.join(".retoque/receipt.txt").exists());
}
#[test]
fn refuses_missing_traversal_foreign_receipt_modified_and_stale_source() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    let target = g.join("Binaries");
    fs::create_dir_all(&target).unwrap();
    write(&g, "nvngx_dlss.dll", b"old");
    for path in ["../nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssnr.dll"] {
        assert!(plan_dlss_update(&g, &target, &s, &[path.into()]).is_err());
    }
    let p = plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()]).unwrap();
    fs::write(s.join("nvngx_dlss.dll"), b"changed source").unwrap();
    assert!(apply(&p.into_verified().unwrap(), &mut |_| {}).is_err());
    assert_eq!(fs::read(g.join("nvngx_dlss.dll")).unwrap(), b"old");
    apply_plan(plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()]).unwrap());
    fs::write(g.join("nvngx_dlss.dll"), b"user changed").unwrap();
    assert!(plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()]).is_err());
    write(&g, ".retoque/dlss.txt", b"foreign marker");
    assert!(plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()]).is_err());
}
#[cfg(unix)]
#[test]
fn refuses_linked_parent() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    fs::create_dir(&g).unwrap();
    std::os::unix::fs::symlink(&s, g.join("plugin")).unwrap();
    assert!(plan_dlss_update(&g, &g, &s, &["plugin/nvngx_dlss.dll".into()]).is_err());
}
#[cfg(target_os = "linux")]
#[test]
fn running_anywhere_in_install_tree_refuses_before_and_after_review() {
    use std::io::{BufRead, BufReader};
    use std::process::{Command, Stdio};
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    let target = g.join("Binaries");
    write(&target, "nvngx_dlss.dll", b"old");
    write(&g, "launcher.exe", b"mapped launcher");
    let file = "Binaries/nvngx_dlss.dll";
    let before = plan_dlss_update(&g, &target, &s, &[file.into()]).unwrap();
    let mut child=Command::new("python3").args(["-c","import mmap,sys\nf=open(sys.argv[1],'rb')\nm=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ)\nprint('ready',flush=True)\nsys.stdin.read()"])
      .arg(g.join("launcher.exe")).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .unwrap();
    assert_eq!(line.trim(), "ready");
    let result = || {
        assert!(apply(&before.into_verified().unwrap(), &mut |_| {}).is_err());
        let p = plan_dlss_update(&g, &target, &s, &[file.into()]).unwrap();
        assert!(
            p.refusals()
                .iter()
                .any(|r| matches!(r, retoque_core::Refusal::GameRunning { .. }))
        );
    };
    result();
    child.stdin.take();
    child.wait().unwrap();
    assert_eq!(fs::read(target.join("nvngx_dlss.dll")).unwrap(), b"old");
}
#[test]
fn root_restore_retains_identity_when_user_modified_dll() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    let target = g.join("Binaries");
    fs::create_dir_all(&target).unwrap();
    write(&g, "nvngx_dlss.dll", b"old");
    apply_plan(plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()]).unwrap());
    fs::write(g.join("nvngx_dlss.dll"), b"user changed").unwrap();
    for p in plan_game_restore(&g, &target, false).unwrap() {
        apply_plan(p);
    }
    assert!(retoque_core::dlss_root_state(&g).unwrap().is_some());
    fs::write(g.join("nvngx_dlss.dll"), b"new SR").unwrap();
    for p in plan_game_restore(&g, &target, false).unwrap() {
        apply_plan(p);
    }
    assert_eq!(fs::read(g.join("nvngx_dlss.dll")).unwrap(), b"old");
}
#[test]
fn both_receipts_are_preflighted_and_restored() {
    use retoque_core::receipt::{Receipt, ReceiptStatus, Record};
    use sha2::{Digest, Sha256};
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    let target = g.join("Project/Binaries/Win64");
    write(&target, "OptiScaler.ini", b"synthetic config");
    let r = Receipt {
        status: ReceiptStatus::Complete,
        updated: String::new(),
        records: vec![Record::File {
            path: "OptiScaler.ini".into(),
            sha256: format!("{:x}", Sha256::digest(b"synthetic config")),
        }],
    };
    write(&target, ".retoque/receipt.txt", r.to_text().as_bytes());
    write(&g, "Engine/Plugins/DLSS/nvngx_dlss.dll", b"original");
    apply_plan(plan_dlss_update(&g, &target, &s, &["Engine/Plugins/DLSS/nvngx_dlss.dll".into()]).unwrap());
    let plans = plan_game_restore(&g, &target, false).unwrap();
    assert_eq!(plans.len(), 2);
    for p in &plans {
        retoque_core::validate_plan(p).unwrap();
    }
    for p in plans {
        apply_plan(p);
    }
    assert_eq!(
        fs::read(g.join("Engine/Plugins/DLSS/nvngx_dlss.dll")).unwrap(),
        b"original"
    );
    assert!(!target.join("OptiScaler.ini").exists());
    assert!(!g.join(".retoque/receipt.txt").exists());
    assert!(!target.join(".retoque/receipt.txt").exists());
}
#[test]
fn each_completed_step_can_resume_restoration() {
    for stop in 0..3 {
        let t = tempfile::tempdir().unwrap();
        let s = source(t.path());
        let g = t.path().join("game");
        let target = g.join("Binaries");
        fs::create_dir_all(&target).unwrap();
        write(&g, "nvngx_dlss.dll", b"original");
        let plan = plan_dlss_update(&g, &target, &s, &["nvngx_dlss.dll".into()])
            .unwrap()
            .into_verified()
            .unwrap();
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            apply(&plan, &mut |done| {
                if done.index == stop {
                    panic!("synthetic interruption");
                }
            })
            .unwrap();
        }));
        assert!(result.is_err());
        assert_eq!(
            retoque_core::dlss_root_state(&g).unwrap(),
            Some(retoque_core::receipt::ReceiptStatus::Partial)
        );
        for p in plan_game_restore(&g, &target, false).unwrap() {
            apply_plan(p);
        }
        assert_eq!(fs::read(g.join("nvngx_dlss.dll")).unwrap(), b"original");
    }
}
#[test]
fn pending_marker_copy_without_mutation_can_be_reconciled() {
    use retoque_core::receipt::{Receipt, ReceiptStatus, Record};
    use sha2::{Digest, Sha256};
    let t = tempfile::tempdir().unwrap();
    let g = t.path().join("game");
    let target = g.join("Binaries");
    fs::create_dir_all(&target).unwrap();
    let mut r = Receipt {
        status: ReceiptStatus::Partial,
        updated: String::new(),
        records: vec![],
    };
    write(&g, ".retoque/receipt.txt", r.to_text().as_bytes());
    r.records.push(Record::File {
        path: ".retoque/dlss.txt".into(),
        sha256: format!("{:x}", Sha256::digest(b"Retoque DLSS receipt v1\n")),
    });
    write(&g, ".retoque/pending-receipt.txt", r.to_text().as_bytes());
    assert!(retoque_core::dlss_root_state(&g).unwrap().is_some());
    for p in plan_game_restore(&g, &target, false).unwrap() {
        apply_plan(p);
    }
    assert!(!g.join(".retoque/receipt.txt").exists());
    assert!(!g.join(".retoque/pending-receipt.txt").exists());
}
#[test]
fn merging_install_plans_preserves_original_source_fingerprints() {
    let t = tempfile::tempdir().unwrap();
    let s = source(t.path());
    let g = t.path().join("game");
    write(&g, "nvngx_dlss.dll", b"original SR");
    write(&g, "nvngx_dlssg.dll", b"original FG");
    let sr = plan_dlss_update(&g, &g, &s, &["nvngx_dlss.dll".into()]).unwrap();
    let fg = plan_dlss_update(&g, &g, &s, &["nvngx_dlssg.dll".into()]).unwrap();
    fs::write(s.join("nvngx_dlss.dll"), b"unreviewed source").unwrap();
    let merged = retoque_core::combine_install_plans(sr, fg).unwrap();
    assert!(apply(&merged.into_verified().unwrap(), &mut |_| {}).is_err());
    assert_eq!(fs::read(g.join("nvngx_dlss.dll")).unwrap(), b"original SR");
}
