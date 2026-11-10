use std::{
    fs,
    path::PathBuf,
    process::{Command, Output},
    sync::atomic::{AtomicUsize, Ordering},
};
struct Fixture {
    root: PathBuf,
    game: PathBuf,
    source: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "retoque-cli-component-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let game = root.join("game");
        let source = root.join("release");
        fs::create_dir_all(&game).unwrap();
        for (path, bytes) in [
            ("version.dll", "MZ dlssg_sm86 synthetic"),
            ("dlssg_sm86.ini", "[General]\nEnabled=1\n"),
            ("dlssg_sm86/backend/sm86_backend.dll", "MZ synthetic backend"),
            ("dlssg_sm86/kernels/fg_kernels.dll", "MZ synthetic kernels"),
            ("dlssg_sm86/runtime/nvngx_dlssg.dll", "MZ synthetic runtime"),
            ("dlssg_sm86/xefg/xefg_kernels.dll", "MZ synthetic xefg"),
        ] {
            fs::create_dir_all(source.join(path).parent().unwrap()).unwrap();
            fs::write(source.join(path), bytes).unwrap();
        }
        Self { root, game, source }
    }
    fn command(&self, action: &str) -> Command {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_retoque"));
        cmd.args(["component", action]).arg(&self.game).arg(&self.source);
        cmd
    }
    fn ok(output: Output) -> Output {
        assert!(
            output.status.success(),
            "{}\n{}",
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr)
        );
        output
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.root);
    }
}
#[test]
fn component_dry_run_preview_install_uninstall_and_invalid_args() {
    let f = Fixture::new();
    let out = Fixture::ok(f.command("install").arg("--dry-run").output().unwrap());
    assert!(String::from_utf8_lossy(&out.stdout).contains("components.txt"));
    assert_eq!(fs::read_dir(&f.game).unwrap().count(), 0);
    let out = Fixture::ok(f.command("install").output().unwrap());
    let out = String::from_utf8_lossy(&out.stdout);
    assert!(out.find("[plan]").unwrap() < out.find("[+]").unwrap());
    Fixture::ok(f.command("uninstall").output().unwrap());
    assert!(!f.game.join("version.dll").exists());
    assert!(!f.game.join(".retoque/components.txt").exists());
    assert!(!f.command("invalid").output().unwrap().status.success());
}
#[test]
fn component_crash_journal_recovers_via_full_uninstall() {
    for checkpoint in [
        "before-intent:0",
        "after-mutation:0",
        "after-mutation:1",
        "after-mutation:2",
        "after-mutation:3",
        "after-mutation:4",
        "after-mutation:5",
        "after-mutation:6",
        "after-mutation:7",
        "after-mutation:8",
        "after-mutation:9",
        "after-mutation:10",
    ] {
        let f = Fixture::new();
        fs::write(f.game.join("version.dll"), "old legacy loader").unwrap();
        fs::write(f.game.join("dlssg_sm86.ini"), "old legacy config").unwrap();
        let out = f
            .command("install")
            .env("RETOQUE_DEPLOY_TEST_CRASH_AT", checkpoint)
            .output()
            .unwrap();
        assert_eq!(
            out.status.code(),
            Some(86),
            "{checkpoint}: {} {}",
            String::from_utf8_lossy(&out.stdout),
            String::from_utf8_lossy(&out.stderr)
        );
        Fixture::ok(
            Command::new(env!("CARGO_BIN_EXE_retoque"))
                .arg("uninstall")
                .arg(&f.game)
                .output()
                .unwrap(),
        );
        assert_eq!(
            fs::read(f.game.join("version.dll")).unwrap(),
            b"old legacy loader",
            "{checkpoint}"
        );
        assert_eq!(
            fs::read(f.game.join("dlssg_sm86.ini")).unwrap(),
            b"old legacy config",
            "{checkpoint}"
        );
        assert!(!f.game.join(".retoque/pending-receipt.txt").exists(), "{checkpoint}");
        assert!(!f.game.join(".retoque/components.txt").exists(), "{checkpoint}");
        if let Ok(text) = fs::read_to_string(f.game.join(".retoque/receipt.txt")) {
            // A crash immediately after a backup leaves the untouched original in its way. Preserve
            // that redundant backup and its receipt, as the full uninstall contract requires.
            let receipt = retoque_core::receipt::Receipt::parse(&text).unwrap();
            assert_eq!(
                receipt.status,
                retoque_core::receipt::ReceiptStatus::UninstallIncomplete
            );
            assert!(
                receipt
                    .records
                    .iter()
                    .all(|r| matches!(r, retoque_core::receipt::Record::Backup { .. })),
                "{checkpoint}"
            );
        }
    }
}
#[cfg(target_os = "linux")]
#[test]
fn both_legacy_scripts_remove_component_records_and_restore_originals() {
    let repo = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../../..");
    for powershell in [false, true] {
        let f = Fixture::new();
        fs::write(f.game.join("version.dll"), "old legacy loader").unwrap();
        fs::write(f.game.join("dlssg_sm86.ini"), "old legacy config").unwrap();
        Fixture::ok(f.command("install").output().unwrap());
        let mut cmd = if powershell {
            let mut c = Command::new("pwsh");
            c.args(["-NoProfile", "-NonInteractive", "-File"])
                .arg(repo.join("tools/install-windows.ps1"))
                .args(["-Uninstall", "-GameDir"]);
            c
        } else {
            let mut c = Command::new("bash");
            c.arg(repo.join("tools/deploy-to-game.sh")).arg("--uninstall");
            c
        };
        Fixture::ok(cmd.arg(&f.game).output().unwrap());
        assert_eq!(fs::read(f.game.join("version.dll")).unwrap(), b"old legacy loader");
        assert_eq!(fs::read(f.game.join("dlssg_sm86.ini")).unwrap(), b"old legacy config");
        assert!(!f.game.join(".retoque/components.txt").exists());
        assert!(!f.game.join("dlssg_sm86/runtime/nvngx_dlssg.dll").exists());
    }
}
#[cfg(target_os = "linux")]
#[test]
fn gpu_and_games_json_use_mock_gpu_and_synthetic_steam_home() {
    use std::os::unix::fs::PermissionsExt;
    let f = Fixture::new();
    let bin = f.root.join("bin");
    fs::create_dir(&bin).unwrap();
    let smi = bin.join("nvidia-smi");
    fs::write(
        &smi,
        "#!/bin/sh\nprintf 'NVIDIA GeForce RTX 3060, 8.6, 999.0, 12288\\n'\n",
    )
    .unwrap();
    fs::set_permissions(&smi, fs::Permissions::from_mode(0o755)).unwrap();
    let mut paths = vec![bin];
    paths.extend(std::env::split_paths(&std::env::var_os("PATH").unwrap()));
    let path = std::env::join_paths(paths).unwrap();
    let out = Fixture::ok(
        Command::new(env!("CARGO_BIN_EXE_retoque"))
            .args(["gpu", "--json"])
            .env("PATH", &path)
            .output()
            .unwrap(),
    );
    let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(json[0]["arch"], "Ampere");
    let steam = f.root.join(".local/share/Steam/steamapps");
    let game = steam.join("common/Synthetic");
    fs::create_dir_all(&game).unwrap();
    fs::write(
        steam.join("appmanifest_42.acf"),
        "\"AppState\" { \"appid\" \"42\" \"name\" \"Synthetic\" \"installdir\" \"Synthetic\" }",
    )
    .unwrap();
    fs::write(game.join("Synthetic.exe"), "MZ fake executable").unwrap();
    fs::write(game.join("nvngx_dlssg.dll"), "MZ fake FG").unwrap();
    let out = Fixture::ok(
        Command::new(env!("CARGO_BIN_EXE_retoque"))
            .args(["games", "--json", "--home"])
            .arg(&f.root)
            .env("PATH", &path)
            .output()
            .unwrap(),
    );
    let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(json["games"][0]["name"], "Synthetic");
    assert!(json["games"][0]["targets"][0]["recommendations"].is_array());
    assert!(!game.join(".retoque").exists());
    // Engine plugin DLSS-G must influence the executable target; the loader's own runtime must not
    // be needed for that fact, and an unmanaged old loader must never trigger an LSFG suggestion.
    fs::remove_file(game.join("nvngx_dlssg.dll")).unwrap();
    let plugin = game.join("Engine/Plugins/Runtime/Nvidia/StreamlineCore/Binaries/ThirdParty/Win64/nvngx_dlssg.dll");
    fs::create_dir_all(plugin.parent().unwrap()).unwrap();
    fs::write(plugin, "MZ fake plugin FG").unwrap();
    fs::write(game.join("dxgi.dll"), "MZ OptiScaler working as fixture").unwrap();
    fs::write(game.join("version.dll"), "MZ legacy dlssg_sm86").unwrap();
    fs::write(game.join("dlssg_sm86.ini"), "[General]\nEnabled=1\n").unwrap();
    fs::write(game.join("OptiScaler.ini"), "[LSFG]\nEnabled=0\n").unwrap();
    let inspect = || {
        let out = Fixture::ok(
            Command::new(env!("CARGO_BIN_EXE_retoque"))
                .args(["games", "--json", "--home"])
                .arg(&f.root)
                .env("PATH", &path)
                .output()
                .unwrap(),
        );
        serde_json::from_slice::<serde_json::Value>(&out.stdout).unwrap()
    };
    let json = inspect();
    let target = &json["games"][0]["targets"][0];
    assert_eq!(target["has_dlssg"], true);
    assert_eq!(target["sm86_installed"], true);
    assert_eq!(target["untracked"], true);
    assert_eq!(target["lsfg_enabled"], false);
    assert_eq!(target["recommendations"][0]["id"], "adopt_retoque");
    fs::create_dir(game.join(".retoque")).unwrap();
    fs::write(
        game.join(".retoque/receipt.txt"),
        retoque_core::receipt::Receipt {
            status: retoque_core::receipt::ReceiptStatus::Complete,
            updated: String::new(),
            records: vec![retoque_core::receipt::Record::Proxy {
                name: "dxgi.dll".into(),
            }],
        }
        .to_text(),
    )
    .unwrap();
    let json = inspect();
    let target = &json["games"][0]["targets"][0];
    assert_eq!(target["sm86_legacy"], true);
    assert_eq!(target["recommendations"][0]["id"], "sm86_update");
    for member in [
        "backend/sm86_backend.dll",
        "kernels/fg_kernels.dll",
        "runtime/nvngx_dlssg.dll",
        "xefg/xefg_kernels.dll",
    ] {
        let relative = format!("dlssg_sm86/{member}");
        fs::create_dir_all(game.join(&relative).parent().unwrap()).unwrap();
        fs::copy(f.source.join(&relative), game.join(&relative)).unwrap();
    }
    let json = inspect();
    let target = &json["games"][0]["targets"][0];
    assert_eq!(target["sm86_legacy"], false);
    // With a current sm86 nothing chooses a provider any more; only the RTX 30 profile tunes what is there.
    let ids: Vec<&str> = target["recommendations"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| r["id"].as_str().unwrap())
        .collect();
    assert!(
        ids.iter().all(|id| retoque_data::recommend::PROFILE_IDS.contains(id)),
        "{ids:?}"
    );
    assert!(ids.contains(&"sm86_mfg"), "{ids:?}");
}
