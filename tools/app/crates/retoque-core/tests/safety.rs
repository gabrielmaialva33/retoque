use retoque_core::{pe_version::Machine, *};
use std::fs;
fn pe(machine: u16) -> Vec<u8> {
    let mut b = vec![0; 90];
    b[..2].copy_from_slice(b"MZ");
    b[60] = 64;
    b[64..68].copy_from_slice(b"PE\0\0");
    b[68..70].copy_from_slice(&machine.to_le_bytes());
    b[84] = 2;
    b[88..90].copy_from_slice(&(if machine == 0x14c { 0x10bu16 } else { 0x20b }).to_le_bytes());
    b
}
#[test]
fn refusal_is_install_only_sealed_and_rechecked() {
    let root = tempfile::tempdir().unwrap();
    let game = root.path().join("game");
    let source = root.path().join("source");
    fs::create_dir(&game).unwrap();
    fs::create_dir(&source).unwrap();
    fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as synthetic").unwrap();
    fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_create synthetic").unwrap();
    let options = InstallOptions {
        game_dir: game.clone(),
        source_dir: source,
        copy_nvngx: false,
        ..Default::default()
    };
    let exe = game.join("game.exe");
    fs::write(&exe, pe(0x14c)).unwrap();
    let plan = plan_install(&options).unwrap();
    assert_eq!(
        plan.refusals(),
        [Refusal::Executables32Bit {
            names: vec!["game.exe".into()]
        }]
    );
    assert_eq!(plan.refusals()[0].exit_code(), 7);
    assert!(plan.steps().is_empty());
    assert!(plan.into_verified().is_err());
    assert!(status(&game).is_ok());
    fs::write(game.join(INI_NAME), b"[Test]\nKey=old\n").unwrap();
    assert!(
        plan_settings(&game, &[("Test".into(), "Key".into(), "value".into())])
            .unwrap()
            .refusals()
            .is_empty()
    );
    fs::write(&exe, pe(0x8664)).unwrap();
    let ready = plan_install(&options).unwrap().into_verified().unwrap();
    fs::write(&exe, pe(0x14c)).unwrap();
    assert!(matches!(apply(&ready, &mut |_| {}), Err(Error::StalePlan(_))));
    assert!(!game.join(".retoque").exists());
    fs::write(&exe, pe(0x8664)).unwrap();
    fs::write(game.join("helper.exe"), pe(0x14c)).unwrap();
    fs::create_dir(game.join("BattlEye")).unwrap();
    let plan = plan_install(&options).unwrap();
    assert!(plan.refusals().is_empty());
    assert!(plan.notes().iter().any(|s| s.contains("helper.exe")));
    assert!(
        plan.notes()
            .iter()
            .any(|s| s.contains("BattlEye") && s.contains("account ban"))
    );
    apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
    fs::write(&exe, pe(0x14c)).unwrap();
    apply(&plan_uninstall(&game).unwrap().into_verified().unwrap(), &mut |_| {}).unwrap();
    assert_eq!(safety::executables(&game).names(Machine::X86).len(), 2);
}
