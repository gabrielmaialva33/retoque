use std::{
    fs,
    path::PathBuf,
    process::{Command, Output},
    sync::atomic::{AtomicUsize, Ordering},
};

struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let root = std::env::temp_dir().join(format!(
            "retoque-cli-settings-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&root).unwrap();
        Self(root)
    }
    fn run(&self, args: &[&str]) -> Output {
        Command::new(env!("CARGO_BIN_EXE_retoque"))
            .args(args)
            .arg(&self.0)
            .output()
            .unwrap()
    }
    fn set(&self, dry: bool, value: &str) -> Output {
        let mut cmd = Command::new(env!("CARGO_BIN_EXE_retoque"));
        cmd.arg("set");
        if dry {
            cmd.arg("--dry-run");
        }
        cmd.arg(&self.0)
            .args(["DlssNr", "WorkingScale", value])
            .output()
            .unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn set_dry_run_shows_old_value_and_apply_has_no_receipt() {
    let f = Fixture::new();
    let ini = f.0.join("OptiScaler.ini");
    let before = b"[DlssNr]\r\nWorkingScale=0.5\r\n";
    fs::write(&ini, before).unwrap();
    let dry = f.set(true, "0.75");
    assert!(dry.status.success(), "{}", String::from_utf8_lossy(&dry.stderr));
    let out = String::from_utf8_lossy(&dry.stdout);
    assert!(out.contains("WorkingScale=0.75") && out.contains("was 0.5"));
    assert_eq!(fs::read(&ini).unwrap(), before);
    assert_eq!(fs::read_dir(&f.0).unwrap().count(), 1);
    let applied = f.set(false, "0.75");
    assert!(applied.status.success(), "{}", String::from_utf8_lossy(&applied.stderr));
    assert_eq!(fs::read(&ini).unwrap(), b"[DlssNr]\r\nWorkingScale=0.75\r\n");
    assert!(!f.0.join(".retoque").exists());
}
#[test]
fn set_missing_ini_refuses_and_negative_value_is_not_a_flag() {
    let f = Fixture::new();
    assert_eq!(f.set(false, "-1").status.code(), Some(1));
    assert_eq!(fs::read_dir(&f.0).unwrap().count(), 0);
    fs::write(f.0.join("OptiScaler.ini"), b"[DlssNr]\nWorkingScale=auto\n").unwrap();
    assert!(f.set(false, "-1").status.success());
    assert_eq!(
        fs::read(f.0.join("OptiScaler.ini")).unwrap(),
        b"[DlssNr]\nWorkingScale=-1\n"
    );
    assert_eq!(f.run(&["set"]).status.code(), Some(1));
}
