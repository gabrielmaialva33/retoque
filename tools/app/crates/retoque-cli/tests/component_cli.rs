use std::{fs, process::Command};

// A hand-copied sm86 (unowned runtime files) is refused, and the advice names a step the
// component command can take: it has no --replace-foreign, unlike install.
#[test]
fn component_refusal_for_hand_copied_runtime_gives_usable_advice() {
    let temp = tempfile::tempdir().unwrap();
    let game = temp.path().join("game");
    let source = temp.path().join("sm86");
    for dir in [&game, &source] {
        for sub in ["backend", "kernels", "runtime", "xefg"] {
            fs::create_dir_all(dir.join("dlssg_sm86").join(sub)).unwrap();
        }
    }
    for (path, bytes) in [
        ("version.dll", "MZ dlssg_sm86 new"),
        ("dlssg_sm86.ini", "[General]\nEnabled=1\n"),
        ("dlssg_sm86/backend/sm86_backend.dll", "MZ new backend"),
        ("dlssg_sm86/kernels/fg_kernels.dll", "MZ new kernels"),
        ("dlssg_sm86/runtime/nvngx_dlssg.dll", "MZ new runtime"),
        ("dlssg_sm86/xefg/xefg_kernels.dll", "MZ new xefg"),
    ] {
        fs::write(source.join(path), bytes).unwrap();
    }
    for (path, bytes) in [
        ("version.dll", "MZ dlssg_sm86 old"),
        ("dlssg_sm86.ini", "[General]\nEnabled=1\n; mine\n"),
        ("dlssg_sm86/backend/sm86_backend.dll", "MZ old backend"),
        ("dlssg_sm86/kernels/fg_kernels.dll", "MZ old kernels"),
        ("dlssg_sm86/runtime/nvngx_dlssg.dll", "MZ old runtime"),
        ("dlssg_sm86/xefg/xefg_kernels.dll", "MZ old xefg"),
    ] {
        fs::write(game.join(path), bytes).unwrap();
    }
    let out = Command::new(env!("CARGO_BIN_EXE_retoque"))
        .args(["component", "install", "--dry-run"])
        .arg(&game)
        .arg(&source)
        .env("RETOQUE_LANG", "en")
        .output()
        .unwrap();
    let stderr = String::from_utf8_lossy(&out.stderr);
    assert!(!out.status.success(), "{stderr}");
    assert!(stderr.contains("is not ours"), "{stderr}");
    assert!(stderr.contains("move the dlssg_sm86 folder"), "{stderr}");
    assert!(!stderr.contains("--replace-foreign"), "{stderr}");
    assert_eq!(
        fs::read(game.join("dlssg_sm86/runtime/nvngx_dlssg.dll")).unwrap(),
        b"MZ old runtime"
    );

    // Following the advice works, and uninstall gives the hand-copied loader and settings back.
    fs::rename(game.join("dlssg_sm86"), temp.path().join("old-dlssg_sm86")).unwrap();
    let install = Command::new(env!("CARGO_BIN_EXE_retoque"))
        .args(["component", "install"])
        .arg(&game)
        .arg(&source)
        .output()
        .unwrap();
    assert!(install.status.success(), "{}", String::from_utf8_lossy(&install.stderr));
    assert_eq!(
        fs::read(game.join("dlssg_sm86/runtime/nvngx_dlssg.dll")).unwrap(),
        b"MZ new runtime"
    );
    let full = Command::new(env!("CARGO_BIN_EXE_retoque"))
        .arg("uninstall")
        .arg(&game)
        .output()
        .unwrap();
    assert!(full.status.success(), "{}", String::from_utf8_lossy(&full.stderr));
    assert_eq!(fs::read(game.join("version.dll")).unwrap(), b"MZ dlssg_sm86 old");
    assert_eq!(
        fs::read(game.join("dlssg_sm86.ini")).unwrap(),
        b"[General]\nEnabled=1\n; mine\n"
    );
}
