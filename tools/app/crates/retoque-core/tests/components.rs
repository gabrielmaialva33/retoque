use retoque_core::*;
use std::{fs, path::PathBuf};

struct Fixture {
    _temp: tempfile::TempDir,
    game: PathBuf,
    component: ComponentFiles,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        let source = temp.path().join("component");
        fs::create_dir(&game).unwrap();
        fs::create_dir_all(source.join("dlssg_sm86/runtime")).unwrap();
        for (path, bytes) in [
            ("version.dll", "MZ dlssg_sm86 new"),
            ("dlssg_sm86.ini", "[General]\nEnabled=1\n"),
            ("dlssg_sm86/runtime/nvngx_dlssg.dll", "synthetic runtime"),
        ] {
            fs::write(source.join(path), bytes).unwrap();
        }
        Self {
            _temp: temp,
            game,
            component: ComponentFiles {
                id: "synthetic-1".into(),
                source_root: source,
                files: vec![
                    "version.dll".into(),
                    "dlssg_sm86.ini".into(),
                    "dlssg_sm86/runtime/nvngx_dlssg.dll".into(),
                ],
            },
        }
    }
    fn install(&self) {
        apply(
            &plan_component_install(&self.game, &self.component).unwrap(),
            &mut |_| {},
        )
        .unwrap();
    }
}
#[test]
fn component_roundtrip_keeps_unrelated_install_and_uses_normal_records() {
    let f = Fixture::new();
    fs::write(f.game.join("dxgi.dll"), b"OptiScaler working as fixture").unwrap();
    f.install();
    assert_eq!(installed_components(&f.game).unwrap(), ["synthetic-1"]);
    let state = status(&f.game).unwrap();
    assert!(
        state
            .receipt
            .unwrap()
            .records
            .iter()
            .any(|r| matches!(r, receipt::Record::File {path,..} if path == "version.dll"))
    );
    apply(&plan_component_uninstall(&f.game, "synthetic-1").unwrap(), &mut |_| {}).unwrap();
    assert!(installed_components(&f.game).unwrap().is_empty());
    assert!(f.game.join("dxgi.dll").exists());
    assert!(!f.game.join("version.dll").exists());
}
#[test]
fn full_uninstall_removes_component_and_index() {
    let f = Fixture::new();
    f.install();
    apply(&plan_uninstall(&f.game).unwrap(), &mut |_| {}).unwrap();
    assert!(!f.game.join("version.dll").exists());
    assert!(!f.game.join(".retoque/components.txt").exists());
}
#[test]
fn legacy_sm86_is_backed_up_and_restored() {
    let f = Fixture::new();
    fs::write(f.game.join("version.dll"), b"old sm86 opaque loader").unwrap();
    fs::write(f.game.join("dlssg_sm86.ini"), b"old config").unwrap();
    f.install();
    apply(&plan_component_uninstall(&f.game, "synthetic-1").unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read(f.game.join("version.dll")).unwrap(), b"old sm86 opaque loader");
    assert_eq!(fs::read(f.game.join("dlssg_sm86.ini")).unwrap(), b"old config");
}
#[test]
fn foreign_own_proxy_and_second_loader_are_refused() {
    for (path, bytes) in [
        ("version.dll", "unknown"),
        ("version.dll", "OptiScaler working as"),
        ("winmm.dll", "dlssg_sm86 loader"),
    ] {
        let f = Fixture::new();
        fs::write(f.game.join(path), bytes).unwrap();
        let p = plan_component_install(&f.game, &f.component).unwrap();
        assert!(!p.refusals().is_empty(), "{path}: {bytes}");
        assert!(apply(&p, &mut |_| {}).is_err());
        assert!(!f.game.join(".retoque").exists());
    }
}
#[test]
fn source_and_observed_loader_changes_invalidate_plan() {
    for source in [true, false] {
        let f = Fixture::new();
        fs::write(f.game.join("winmm.dll"), "foreign unrelated").unwrap();
        let p = plan_component_install(&f.game, &f.component).unwrap();
        fs::write(
            if source {
                f.component.source_root.join("version.dll")
            } else {
                f.game.join("winmm.dll")
            },
            "dlssg_sm86 changed",
        )
        .unwrap();
        assert!(apply(&p, &mut |_| {}).is_err());
        assert!(!f.game.join(".retoque").exists());
    }
}
#[test]
fn modified_component_is_kept_and_stays_registered() {
    let f = Fixture::new();
    f.install();
    fs::write(f.game.join("version.dll"), "user modified").unwrap();
    apply(&plan_component_uninstall(&f.game, "synthetic-1").unwrap(), &mut |_| {}).unwrap();
    assert_eq!(fs::read(f.game.join("version.dll")).unwrap(), b"user modified");
    assert_eq!(installed_components(&f.game).unwrap(), ["synthetic-1"]);
}
#[test]
fn invalid_manifest_paths_and_duplicates_are_refused() {
    for path in [
        "../escape",
        ".retoque/receipt.txt",
        "OptiScaler.ini",
        "VERSION.DLL",
        "dlssg_sm86/../escape",
        "dlssg_sm86/nvngx_dlssnr.dll",
    ] {
        let mut f = Fixture::new();
        f.component.files.push(path.into());
        assert!(plan_component_install(&f.game, &f.component).is_err(), "{path}");
    }
}
#[cfg(unix)]
#[test]
fn symlink_component_parent_is_refused() {
    let f = Fixture::new();
    std::os::unix::fs::symlink(&f.component.source_root, f.game.join("dlssg_sm86")).unwrap();
    assert!(plan_component_install(&f.game, &f.component).is_err());
}

#[test]
fn component_index_tampering_and_case_aliases_are_refused() {
    let f = Fixture::new();
    f.install();
    fs::write(f.game.join(".retoque/components.txt"), "[DlssgSm86]\nId=forged\n").unwrap();
    assert!(installed_components(&f.game).is_err());
    assert!(plan_component_uninstall(&f.game, "synthetic-1").is_err());
    let f = Fixture::new();
    fs::create_dir(f.game.join("DLSSG_SM86")).unwrap();
    assert!(plan_component_install(&f.game, &f.component).is_err());
}

#[test]
fn upgrade_same_layout_and_partial_uninstall_retry() {
    let mut f = Fixture::new();
    f.install();
    f.component.id = "synthetic-2".into();
    fs::write(f.component.source_root.join("version.dll"), "dlssg_sm86 v2").unwrap();
    f.install();
    assert_eq!(installed_components(&f.game).unwrap(), ["synthetic-2"]);
    fs::write(f.game.join("version.dll"), "edited").unwrap();
    apply(&plan_component_uninstall(&f.game, "synthetic-2").unwrap(), &mut |_| {}).unwrap();
    fs::remove_file(f.game.join("version.dll")).unwrap();
    apply(&plan_component_uninstall(&f.game, "synthetic-2").unwrap(), &mut |_| {}).unwrap();
    assert!(installed_components(&f.game).unwrap().is_empty());
}

#[cfg(target_os = "linux")]
#[test]
fn mapped_game_refuses_plan_and_already_reviewed_apply() {
    use std::{
        io::{BufRead, BufReader},
        process::{Command, Stdio},
    };
    let f = Fixture::new();
    let file = f.game.join("game.exe");
    fs::write(&file, "synthetic executable").unwrap();
    let plan = plan_component_install(&f.game, &f.component).unwrap();
    let mut child = Command::new("python3").args(["-c", "import mmap,sys; f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); print('ready',flush=True); sys.stdin.read()"])
        .arg(file).stdin(Stdio::piped()).stdout(Stdio::piped()).spawn().unwrap();
    let mut ready = String::new();
    BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut ready)
        .unwrap();
    let refused = plan_component_install(&f.game, &f.component).unwrap();
    let result = apply(&plan, &mut |_| {});
    child.stdin.take();
    child.wait().unwrap();
    assert!(
        refused
            .refusals()
            .iter()
            .any(|r| matches!(r, Refusal::GameRunning { .. }))
    );
    assert!(result.is_err());
    assert!(!f.game.join(".retoque").exists());
}

#[test]
fn component_uninstall_preserves_existing_retoque_receipt_and_status() {
    let f = Fixture::new();
    fs::write(
        f.component.source_root.join("OptiScaler.dll"),
        "OptiScaler working as fixture",
    )
    .unwrap();
    fs::write(f.component.source_root.join(FORWARDER_NAME), "dlssnr_call_fixture").unwrap();
    fs::write(f.component.source_root.join(INI_NAME), "[LSFG]\nEnabled=false\n").unwrap();
    let opts = InstallOptions {
        game_dir: f.game.clone(),
        source_dir: f.component.source_root.clone(),
        copy_nvngx: false,
        ..Default::default()
    };
    apply(&plan_install(&opts).unwrap(), &mut |_| {}).unwrap();
    let before = status(&f.game).unwrap().receipt.unwrap();
    f.install();
    apply(&plan_component_uninstall(&f.game, "synthetic-1").unwrap(), &mut |_| {}).unwrap();
    let after = status(&f.game).unwrap().receipt.unwrap();
    assert_eq!(after.records, before.records);
    assert_eq!(after.status, receipt::ReceiptStatus::Complete);
}

#[test]
fn component_operations_do_not_mark_an_unrelated_partial_receipt_complete() {
    let f = Fixture::new();
    fs::create_dir(f.game.join(".retoque")).unwrap();
    let before = receipt::Receipt {
        status: receipt::ReceiptStatus::Partial,
        updated: String::new(),
        records: vec![receipt::Record::Proxy {
            name: "dxgi.dll".into(),
        }],
    };
    fs::write(f.game.join(".retoque/receipt.txt"), before.to_text()).unwrap();
    f.install();
    assert_eq!(
        status(&f.game).unwrap().receipt.unwrap().status,
        receipt::ReceiptStatus::Partial
    );
    apply(&plan_component_uninstall(&f.game, "synthetic-1").unwrap(), &mut |_| {}).unwrap();
    assert_eq!(
        status(&f.game).unwrap().receipt.unwrap().status,
        receipt::ReceiptStatus::Partial
    );
}

// Test adapter keeps existing refusal/staleness probes at the same boundary after typestate migration.
fn apply(
    plan: &retoque_core::Plan,
    progress: &mut dyn FnMut(retoque_core::StepDone),
) -> Result<(), retoque_core::Error> {
    retoque_core::apply(&plan.clone().into_verified()?, progress)
}
