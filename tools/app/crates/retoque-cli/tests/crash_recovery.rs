#![cfg(target_os = "linux")]
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};
struct Fixture(PathBuf);
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn cli() -> Command {
    Command::new(env!("CARGO_BIN_EXE_retoque"))
}
fn install(game: &Path, source: &Path) -> Command {
    let mut command = cli();
    command
        .args(["install", "--no-nvngx", "--keep-reshade", "--source-dir"])
        .arg(source)
        .arg(game);
    command
}
#[test]
fn abrupt_exit_after_rename_recovers_original_mod() {
    let root = Fixture(std::env::temp_dir().join(format!("retoque-crash-{}", std::process::id())));
    fs::create_dir(&root.0).unwrap();
    let library = root.0.join("crash.so");
    let built = Command::new("cc")
        .args(["-shared", "-fPIC", "-Wall", "-Wextra", "-Werror"])
        .arg(Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/crash_rename.c"))
        .args(["-ldl", "-o"])
        .arg(&library)
        .output()
        .unwrap();
    assert!(built.status.success(), "{}", String::from_utf8_lossy(&built.stderr));
    let source = root.0.join("payload");
    fs::create_dir(&source).unwrap();
    fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as fake").unwrap();
    fs::write(source.join("nvngx.dll_dlssnr.dll"), b"MZ dlssnr_call_fake").unwrap();
    fs::write(source.join("OptiScaler.ini"), b"[Plugins]\nLoadReshade=false\n").unwrap();
    let original = b"MZ ReShade original user mod";
    let mut count = 0;
    let mut faults = Vec::new();
    // The original review scenario is independently injected at libc's actual rename, without
    // relying on an engine checkpoint. tempfile uses raw syscalls, so other stages use named exits.
    for phase in ["before", "after"] {
        faults.push((None, Some(phase)));
    }
    for step in [0, 1, 4, 5] {
        // ReShade rename, proxy copy, initial INI copy, LoadReshade INI edit
        for point in [
            "before-intent",
            "after-intent",
            "after-mutation",
            "before-receipt",
            "after-receipt",
        ] {
            faults.push((Some(format!("{point}:{step}")), None));
        }
    }
    for (checkpoint, phase) in faults {
        let game = root.0.join(format!("game-{count}"));
        count += 1;
        fs::create_dir(&game).unwrap();
        fs::write(game.join("dxgi.dll"), original).unwrap();
        let mut command = install(&game, &source);
        if let Some(point) = &checkpoint {
            command.env("RETOQUE_DEPLOY_TEST_CRASH_AT", point);
        } else {
            command
                .env("LD_PRELOAD", &library)
                .env("RETOQUE_CRASH_SUFFIX", "ReShade64.dll")
                .env("RETOQUE_CRASH_PHASE", phase.unwrap());
        }
        let crashed = command.output().unwrap();
        assert_eq!(crashed.status.code(), Some(86), "{checkpoint:?} {phase:?}: {crashed:?}");
        let recovery = cli().arg("uninstall").arg(&game).output().unwrap();
        assert!(recovery.status.success(), "{checkpoint:?} {phase:?}: {recovery:?}");
        assert_eq!(
            fs::read(game.join("dxgi.dll")).unwrap(),
            original,
            "{checkpoint:?} {phase:?}"
        );
        for name in [
            "ReShade64.dll",
            "OptiScaler.dll",
            "OptiScaler.ini",
            "nvngx.dll_dlssnr.dll",
            ".retoque/receipt.txt",
            ".retoque/pending-receipt.txt",
        ] {
            assert!(!game.join(name).exists(), "{checkpoint:?} {phase:?} left {name}");
        }
    }
    // Updating an already owned file must recover both possible hashes, depending on which side
    // of the write the child stopped. A future hash alone would strand the previous installation.
    for point in [
        "before-intent:0",
        "after-intent:0",
        "after-mutation:0",
        "before-receipt:0",
        "after-receipt:0",
    ] {
        let game = root.0.join(format!("upgrade-{count}"));
        count += 1;
        fs::create_dir(&game).unwrap();
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as v1").unwrap();
        let first = install(&game, &source).output().unwrap();
        assert!(first.status.success(), "{first:?}");
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as v2").unwrap();
        let crashed = install(&game, &source)
            .env("RETOQUE_DEPLOY_TEST_CRASH_AT", point)
            .output()
            .unwrap();
        assert_eq!(crashed.status.code(), Some(86), "{point}: {crashed:?}");
        let recovery = cli().arg("uninstall").arg(&game).output().unwrap();
        assert!(recovery.status.success(), "{point}: {recovery:?}");
        assert_eq!(fs::read_dir(&game).unwrap().count(), 0, "{point} left owned files");
    }
    eprintln!("Recovered {count} abrupt-exit boundaries");
}
