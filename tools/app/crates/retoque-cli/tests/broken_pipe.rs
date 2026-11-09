//! A reader closed before spawn deterministically exercises EPIPE even for short stdout.
#![cfg(unix)]
use std::{
    fs,
    os::{fd::OwnedFd, unix::net::UnixStream},
    path::PathBuf,
    process::{Command, Output, Stdio},
};
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        let root = std::env::temp_dir().join(format!("retoque-pipe-{}", std::process::id()));
        fs::create_dir_all(root.join("game/.retoque")).unwrap();
        fs::create_dir_all(root.join("home")).unwrap();
        fs::create_dir_all(root.join("payload/OptiScaler")).unwrap();
        fs::write(
            root.join("game/OptiScaler.ini"),
            "[DlssNr]\nEnabled=true\nModelFile=missing.dll\n",
        )
        .unwrap();
        fs::write(root.join("game/.retoque/receipt.txt"), "version\t1\nstatus\tcomplete\n").unwrap();
        fs::write(
            root.join("payload/OptiScaler.dll"),
            b"MZ OptiScaler working as synthetic",
        )
        .unwrap();
        fs::write(root.join("payload/nvngx.dll_dlssnr.dll"), b"MZ dlssnr_call_ synthetic").unwrap();
        fs::write(root.join("payload/OptiScaler.ini"), "[DlssNr]\nEnabled=auto\n").unwrap();
        for (path, bytes) in [
            ("version.dll", "MZ dlssg_sm86 synthetic"),
            ("dlssg_sm86.ini", "[General]\nEnabled=1\n"),
            ("dlssg_sm86/backend/sm86_backend.dll", "MZ synthetic backend"),
            ("dlssg_sm86/kernels/fg_kernels.dll", "MZ synthetic kernels"),
            ("dlssg_sm86/runtime/nvngx_dlssg.dll", "MZ synthetic runtime"),
            ("dlssg_sm86/xefg/xefg_kernels.dll", "MZ synthetic xefg"),
        ] {
            let path = root.join("component").join(path);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
        Self(root)
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn closed_pipe(args: &[&str]) -> Output {
    let (reader, writer) = UnixStream::pair().unwrap();
    drop(reader);
    Command::new(env!("CARGO_BIN_EXE_retoque"))
        .args(args)
        .stdout(Stdio::from(OwnedFd::from(writer)))
        .stderr(Stdio::piped())
        .output()
        .unwrap()
}
#[test]
fn all_stdout_routes_exit_quietly_and_a_write_finishes_when_the_reader_closes() {
    let fixture = Fixture::new();
    let game_path = fixture.0.join("game");
    let home_path = fixture.0.join("home");
    let payload_path = fixture.0.join("payload");
    let component_path = fixture.0.join("component");
    let game = game_path.to_str().unwrap();
    for args in [
        vec!["--help"],
        vec!["status", game],
        vec!["status", "--json", game],
        vec!["doctor", game],
        vec!["doctor", "--json", game],
        vec!["set", "--dry-run", game, "DlssNr", "Enabled", "false"],
        vec![
            "install",
            "--dry-run",
            "--no-nvngx",
            "--source-dir",
            payload_path.to_str().unwrap(),
            game,
        ],
        vec!["uninstall", "--dry-run", game],
        vec![
            "component",
            "install",
            "--dry-run",
            game,
            component_path.to_str().unwrap(),
        ],
        vec!["games", "--json", "--home", home_path.to_str().unwrap()],
        vec!["gpu", "--json"],
    ] {
        let output = closed_pipe(&args);
        assert!(
            output.status.success(),
            "{args:?}: {:?}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr)
        );
        assert!(
            output.stderr.is_empty(),
            "{args:?}: {}",
            String::from_utf8_lossy(&output.stderr)
        );
    }
    assert_eq!(
        fs::read_to_string(game_path.join("OptiScaler.ini")).unwrap(),
        "[DlssNr]\nEnabled=true\nModelFile=missing.dll\n"
    );
    let output = closed_pipe(&["set", game, "DlssNr", "Enabled", "false"]);
    assert!(output.status.success() && output.stderr.is_empty());
    assert_eq!(
        fs::read_to_string(game_path.join("OptiScaler.ini")).unwrap(),
        "[DlssNr]\nEnabled=false\nModelFile=missing.dll\n"
    );
    let output = closed_pipe(&["set", "--invalid"]);
    assert_eq!(
        output.status.code(),
        Some(1),
        "closing stdout must not hide command failures"
    );
    assert!(!String::from_utf8_lossy(&output.stderr).contains("panicked"));
    #[cfg(target_os = "linux")]
    {
        let output = Command::new(env!("CARGO_BIN_EXE_retoque"))
            .arg("--help")
            .stdout(fs::OpenOptions::new().write(true).open("/dev/full").unwrap())
            .stderr(Stdio::piped())
            .output()
            .unwrap();
        assert_eq!(output.status.code(), Some(1), "ENOSPC is an error, not a broken pipe");
        assert!(String::from_utf8_lossy(&output.stderr).contains("Error writing stdout"));
        assert!(!String::from_utf8_lossy(&output.stderr).contains("panicked"));
    }
}
