use std::{
    fs,
    path::PathBuf,
    process::Command,
    sync::atomic::{AtomicUsize, Ordering},
};
struct Fixture(PathBuf);
impl Fixture {
    fn new() -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let p = std::env::temp_dir().join(format!(
            "retoque-doctor-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&p).unwrap();
        Self(p)
    }
    fn run(&self, json: bool, language: &str) -> std::process::Output {
        let mut command = Command::new(env!("CARGO_BIN_EXE_retoque"));
        command.arg("doctor").env("RETOQUE_LANG", language);
        if json {
            command.arg("--json");
        }
        command.arg(&self.0).output().unwrap()
    }
}
impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
#[test]
fn doctor_is_read_only_with_healthy_findings_and_usage_exit_codes() {
    let fixture = Fixture::new();
    let output = fixture.run(true, "en");
    assert_eq!(
        output.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    assert_eq!(
        serde_json::from_slice::<serde_json::Value>(&output.stdout).unwrap(),
        serde_json::json!([])
    );
    assert_eq!(fs::read_dir(&fixture.0).unwrap().count(), 0);
    let bytes = b"; preserve\r\n[DlssNr]\r\nEnabled=true\r\nModelFile=missing.dll\r\n";
    fs::write(fixture.0.join("OptiScaler.ini"), bytes).unwrap();
    let output = fixture.run(true, "en");
    assert_eq!(output.status.code(), Some(6));
    let json: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert!(
        json.as_array()
            .unwrap()
            .iter()
            .any(|f| f["id"] == "model_missing" && !f["evidence"].as_array().unwrap().is_empty())
    );
    for (lang, phrase) in [("en", "NR model missing"), ("pt", "Modelo NR ausente")] {
        let output = fixture.run(false, lang);
        assert_eq!(output.status.code(), Some(6));
        assert!(String::from_utf8_lossy(&output.stdout).contains(phrase));
    }
    assert_eq!(fs::read(fixture.0.join("OptiScaler.ini")).unwrap(), bytes);
    assert_eq!(fs::read_dir(&fixture.0).unwrap().count(), 1);
    for args in [
        vec!["doctor"],
        vec!["doctor", "--dry-run"],
        vec!["doctor", "--json", "a", "b"],
    ] {
        assert_eq!(
            Command::new(env!("CARGO_BIN_EXE_retoque"))
                .args(args)
                .output()
                .unwrap()
                .status
                .code(),
            Some(1)
        );
    }
}
#[test]
fn doctor_reads_partial_receipt_and_logs_without_modifying_them() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join(".retoque")).unwrap();
    let receipt = b"version\t1\nstatus\tpartial\nupdated\tsynthetic\nproxy\tdxgi.dll\n";
    fs::write(fixture.0.join(".retoque/receipt.txt"), receipt).unwrap();
    let log = "[E] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 0 at /home/fixtureperson/model.dll\n"; // privacy: allow
    fs::write(fixture.0.join("OptiScaler.log"), log).unwrap();
    let output = fixture.run(true, "en");
    assert_eq!(
        output.status.code(),
        Some(6),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    let text = String::from_utf8_lossy(&output.stdout);
    assert!(text.contains("unfinished_install") && text.contains("ngx_init_failed"));
    assert!(!text.contains("fixtureperson"));
    assert_eq!(fs::read(fixture.0.join(".retoque/receipt.txt")).unwrap(), receipt);
    assert_eq!(fs::read_to_string(fixture.0.join("OptiScaler.log")).unwrap(), log);
}
#[test]
fn unresolved_wine_drive_is_unknown_not_missing() {
    if cfg!(windows) {
        return;
    }
    let fixture = Fixture::new();
    fs::write(
        fixture.0.join("OptiScaler.ini"),
        b"[DlssNr]\nEnabled=true\nModelFile=C:\\Models\\model.dll\n",
    )
    .unwrap();
    assert_eq!(fixture.run(true, "en").status.code(), Some(0));
}

#[test]
fn orphan_pending_journal_is_a_finding_and_remains_untouched() {
    let fixture = Fixture::new();
    fs::create_dir(fixture.0.join(".retoque")).unwrap();
    let path = fixture.0.join(".retoque/pending-receipt.txt");
    fs::write(&path, b"malformed pending journal").unwrap();
    let output = fixture.run(true, "en");
    assert_eq!(output.status.code(), Some(6));
    let text = String::from_utf8_lossy(&output.stdout);
    assert!(text.contains("unfinished_install") && text.contains("doctor.journal_unreadable.why"));
    assert_eq!(fs::read(&path).unwrap(), b"malformed pending journal");
    assert!(!fixture.0.join(".retoque/receipt.txt").exists());
}

#[test]
fn doctor_reports_outdated_dlss_with_review_proposal_in_both_languages() {
    fn put16(b: &mut [u8], at: usize, n: u16) {
        b[at..at + 2].copy_from_slice(&n.to_le_bytes());
    }
    fn put32(b: &mut [u8], at: usize, n: u32) {
        b[at..at + 4].copy_from_slice(&n.to_le_bytes());
    }
    let fixture = Fixture::new();
    let mut bytes = vec![0u8; 1024];
    bytes[..2].copy_from_slice(b"MZ");
    put32(&mut bytes, 60, 64);
    bytes[64..68].copy_from_slice(b"PE\0\0");
    put16(&mut bytes, 70, 1);
    put16(&mut bytes, 84, 240);
    put16(&mut bytes, 88, 0x20b);
    put32(&mut bytes, 196, 16);
    put32(&mut bytes, 216, 0x1000);
    put32(&mut bytes, 220, 256);
    put32(&mut bytes, 340, 0x1000);
    put32(&mut bytes, 344, 256);
    put32(&mut bytes, 348, 512);
    let r = &mut bytes[512..];
    for (at, id, to) in [(0, 16, 0x80000020), (32, 1, 0x80000040), (64, 1033, 96)] {
        put16(r, at + 14, 1);
        put32(r, at + 16, id);
        put32(r, at + 20, to);
    }
    put32(r, 96, 0x1080);
    put32(r, 100, 92);
    put16(r, 128, 92);
    put16(r, 130, 52);
    for (i, c) in "VS_VERSION_INFO\0".encode_utf16().enumerate() {
        put16(r, 134 + i * 2, c);
    }
    put32(r, 168, 0xfeef04bd);
    put32(r, 172, 0x10000);
    put32(r, 176, 310 << 16 | 1);
    put32(r, 180, 0);
    let directory = fixture.0.join("Engine/Plugins/DLSS");
    fs::create_dir_all(&directory).unwrap();
    let path = directory.join("nvngx_dlss.dll");
    fs::write(&path, &bytes).unwrap();
    let output = fixture.run(true, "en");
    assert_eq!(output.status.code(), Some(6));
    let findings: serde_json::Value = serde_json::from_slice(&output.stdout).unwrap();
    assert!(
        findings
            .as_array()
            .unwrap()
            .iter()
            .any(|f| f["id"] == "dlss_outdated" && f["severity"] == "Info" && f["repair"] == "UpdateDlss")
    );
    for (lang, phrase) in [
        ("en", "A newer DLSS is available"),
        ("pt", "Uma versão mais nova do DLSS está disponível"),
    ] {
        assert!(String::from_utf8_lossy(&fixture.run(false, lang).stdout).contains(phrase));
    }
    assert_eq!(fs::read(&path).unwrap(), bytes);
    assert!(!fixture.0.join(".retoque").exists());
}
