use std::{fs, process::Command};
fn cmd(args: &[&str]) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_retoque"))
        .args(args)
        .env("RETOQUE_LANG", "en")
        .output()
        .unwrap()
}
fn ok(args: &[&str]) {
    let o = cmd(args);
    assert!(o.status.success(), "{}", String::from_utf8_lossy(&o.stderr));
}
#[test]
fn cli_reshade_addon_dry_and_roundtrip() {
    let temp = tempfile::tempdir().unwrap();
    let root = temp.path();
    fs::create_dir_all(root).unwrap();
    let game = root.join("game");
    fs::create_dir_all(&game).unwrap();
    fs::write(game.join("dxgi.dll"), b"OptiScaler working as fixture").unwrap();
    let ini = b"; user\r\n[DlssNr]\r\nEnabled=true";
    fs::write(game.join("OptiScaler.ini"), ini).unwrap();
    let dll = root.join("ReShade64.dll");
    fs::write(&dll, pe(b"ReShade LoadFromDllMain")).unwrap();
    let addon = root.join("renodx-dlss5.addon64");
    fs::write(&addon, pe(b"addon")).unwrap();
    let g = game.to_str().unwrap();
    let d = dll.to_str().unwrap();
    let a = addon.to_str().unwrap();
    ok(&["reshade", "install", "--dry-run", g, d]);
    assert!(!game.join(".retoque").exists());
    ok(&["reshade", "install", g, d]);
    ok(&["addon", "add", "--dry-run", g, a]);
    assert!(!game.join("renodx-dlss5.addon64").exists());
    ok(&["addon", "add", g, a]);
    assert!(String::from_utf8_lossy(&cmd(&["addon", "list", g]).stdout).contains("renodx-dlss5.addon64"));
    ok(&["addon", "disable", g, "renodx-dlss5.addon64"]);
    ok(&["addon", "enable", g, "renodx-dlss5.addon64"]);
    ok(&["addon", "remove", g, "renodx-dlss5.addon64"]);
    ok(&["reshade", "uninstall", g]);
    assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), ini);
    assert!(!game.join("ReShade64.dll").exists());
}

fn pe(markers: &[u8]) -> Vec<u8> {
    let mut b = vec![0; 328];
    b[..2].copy_from_slice(b"MZ");
    b[60..64].copy_from_slice(&64u32.to_le_bytes());
    b[64..68].copy_from_slice(b"PE\0\0");
    b[68..70].copy_from_slice(&0x8664u16.to_le_bytes());
    b[84..86].copy_from_slice(&240u16.to_le_bytes());
    b[86..88].copy_from_slice(&0x2000u16.to_le_bytes());
    b[88..90].copy_from_slice(&0x20bu16.to_le_bytes());
    b.extend_from_slice(markers);
    b
}
#[test]
fn interrupted_component_install_recovers_at_each_step() {
    for limited in [false, true] {
        for step in 0..if limited { 4 } else { 3 } {
            let temp = tempfile::tempdir().unwrap();
            let game = temp.path().join("game");
            fs::create_dir(&game).unwrap();
            fs::write(game.join("dxgi.dll"), b"OptiScaler working as fixture").unwrap();
            let ini = b"[Plugins]\r\nOther=true";
            fs::write(game.join("OptiScaler.ini"), ini).unwrap();
            let old = pe(b"ReShade only limited add-on functionality");
            if limited {
                fs::write(game.join("ReShade64.dll"), &old).unwrap();
            }
            let source = temp.path().join("ReShade64.dll");
            fs::write(&source, pe(b"ReShade LoadFromDllMain")).unwrap();
            let output = Command::new(env!("CARGO_BIN_EXE_retoque"))
                .args(["reshade", "install"])
                .arg(&game)
                .arg(&source)
                .env("RETOQUE_DEPLOY_TEST_CRASH_AT", format!("after-mutation:{step}"))
                .output()
                .unwrap();
            assert_eq!(output.status.code(), Some(86));
            ok(&["reshade", "uninstall", game.to_str().unwrap()]);
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), ini);
            if limited {
                assert_eq!(fs::read(game.join("ReShade64.dll")).unwrap(), old);
            } else {
                assert!(!game.join("ReShade64.dll").exists());
            }
        }
    }
}
