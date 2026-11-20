use super::*;
use std::io::Cursor;
fn pe(machine: u16) -> Vec<u8> {
    let mut b = vec![0u8; 1024];
    b[..2].copy_from_slice(b"MZ");
    b[60..64].copy_from_slice(&64u32.to_le_bytes());
    b[64..68].copy_from_slice(b"PE\0\0");
    b[68..70].copy_from_slice(&machine.to_le_bytes());
    b[70..72].copy_from_slice(&1u16.to_le_bytes());
    b[84..86].copy_from_slice(&240u16.to_le_bytes());
    b[88..90].copy_from_slice(&(if machine == 0x14c { 0x10bu16 } else { 0x20b }).to_le_bytes());
    let dir = 88 + if machine == 0x14c { 96 } else { 112 };
    b[dir - 4..dir].copy_from_slice(&16u32.to_le_bytes());
    b[dir + 8..dir + 12].copy_from_slice(&0x1000u32.to_le_bytes());
    b[dir + 12..dir + 16].copy_from_slice(&40u32.to_le_bytes());
    let s = 88 + 240;
    b[s + 12..s + 16].copy_from_slice(&0x1000u32.to_le_bytes());
    b[s + 16..s + 20].copy_from_slice(&512u32.to_le_bytes());
    b[s + 20..s + 24].copy_from_slice(&512u32.to_le_bytes());
    b[524..528].copy_from_slice(&0x1040u32.to_le_bytes());
    b[576..586].copy_from_slice(b"d3d10.dll\0");
    b
}
fn known() -> Inventory {
    Inventory {
        executable: Image {
            sha256: Some(digest(b"game")),
            architecture: Architecture::X64,
            format: Format::Pe,
            header_observation: Fact::Observed,
            imports_complete: true,
            imports: vec!["d3d10.dll".into()],
        },
        process: Process {
            architecture: Architecture::X64,
            identity: Some(digest(b"process")),
            stable: true,
            modules: vec!["d3d10core.dll".into(), "winevulkan.dll".into()],
            modules_complete: true,
            matches_executable: true,
            ..Default::default()
        },
        ..Default::default()
    }
}
fn request() -> Request {
    Request {
        pid: Some(123),
        runtime: Runtime::Proton,
        mode: Mode::Windowed,
        output: DesiredOutput::InGame,
        ..Default::default()
    }
}
fn make(raw: Inventory) -> Report {
    report(&request(), raw, Duration::from_millis(1))
}
#[test]
fn default_off_does_no_work() {
    struct PanicSource;
    impl Source for PanicSource {
        fn snapshot(&mut self, _: &Request, _: Instant) -> Inventory {
            panic!("off path performed inspection")
        }
    }
    assert!(inspect_with(None, &mut PanicSource).is_none());
}
#[test]
fn pe_headers_imports_and_truncation() {
    for machine in [0x14c, 0x8664, 0xaa64] {
        let b = pe(machine);
        assert!(pe_version::read_header(&mut Cursor::new(&b)).is_some());
        assert_eq!(
            pe_version::read_imports(&mut Cursor::new(&b)),
            Some((vec!["d3d10.dll".into()], true))
        );
        for n in 0..90 {
            assert!(pe_version::read_header(&mut Cursor::new(&b[..n])).is_none());
        }
        for n in 90..586 {
            assert!(pe_version::read_imports(&mut Cursor::new(&b[..n])).is_none());
        }
        let mut bad = b.clone();
        bad[524..528].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(pe_version::read_imports(&mut Cursor::new(bad)).is_none());
    }
}
#[test]
fn elf_headers_are_not_guessed_from_magic_or_class() {
    for (class, machine, len) in [(1, 3u16, 52), (2, 62, 64), (2, 183, 64)] {
        let mut b = vec![0u8; len];
        b[..4].copy_from_slice(b"\x7fELF");
        b[4] = class;
        b[5] = 1;
        b[6] = 1;
        b[16] = 2;
        b[20] = 1;
        b[18..20].copy_from_slice(&machine.to_le_bytes());
        let offset = if class == 1 { 40 } else { 52 };
        b[offset..offset + 2].copy_from_slice(&(len as u16).to_le_bytes());
        assert_ne!(elf_architecture(&b), Architecture::Unknown);
        for n in 0..len {
            assert_eq!(elf_architecture(&b[..n]), Architecture::Unknown);
        }
        b[5] = 2;
        assert_eq!(elf_architecture(&b), Architecture::Unknown);
    }
}
#[test]
fn launcher_architecture_never_becomes_child_architecture() {
    let mut raw = known();
    raw.launcher = Some(Image {
        architecture: Architecture::X86,
        ..Default::default()
    });
    let r = make(raw);
    assert_eq!(r.process_architecture, Architecture::X64);
    assert_eq!(r.executable.architecture, Architecture::X64);
    assert_eq!(r.launcher.unwrap().architecture, Architecture::X86);
}
#[test]
fn stale_identity_discards_modules_and_architecture() {
    let mut raw = known();
    raw.process.stable = false;
    let r = make(raw);
    assert!(!r.inspection_complete);
    assert_eq!(r.process_architecture, Architecture::Unknown);
    assert_eq!(r.process_identity, Fact::Unknown);
    assert!(r.apis.iter().all(|a| a.module == Fact::Unknown));
    assert!(r.issues.contains(&"probe.identity"));
}
#[test]
fn mismatch_is_a_named_refusal() {
    let mut raw = known();
    raw.process.matches_executable = false;
    let r = make(raw);
    assert!(!r.inspection_complete);
    assert!(r.issues.contains(&"probe.mismatch"));
}
#[test]
fn incomplete_inventory_cannot_authorize_a_decision() {
    let mut raw = known();
    raw.process.modules_complete = false;
    let r = make(raw);
    assert!(!r.inspection_complete);
    assert_eq!(r.renderer_decision, "probe.incomplete");
    let mut raw = known();
    raw.executable.imports_complete = false;
    assert!(!make(raw).inspection_complete);
    let mut raw = known();
    raw.executable.sha256 = None;
    assert!(!make(raw).inspection_complete);
}
#[test]
fn launcher_only_is_a_named_refusal() {
    let mut q = request();
    q.launcher_only = true;
    let r = report(&q, known(), Duration::ZERO);
    assert!(!r.inspection_complete);
    assert!(r.issues.contains(&"probe.launcher_only"));
}
#[test]
fn access_denied_and_unknown_mode_remain_unknown() {
    let raw = Inventory {
        process: Process {
            issue: Some("probe.process_unreadable"),
            ..Default::default()
        },
        ..Default::default()
    };
    let r = report(
        &Request {
            pid: Some(123),
            ..Default::default()
        },
        raw,
        Duration::ZERO,
    );
    assert!(!r.inspection_complete);
    assert_eq!(r.window_mode_observation, Fact::Unknown);
    assert_eq!(r.window_geometry_observation, Fact::Unknown);
    assert!(r.issues.contains(&"probe.process_unreadable") && r.issues.contains(&"probe.mode_unknown"));
}
#[test]
fn mixed_apis_do_not_choose_a_renderer() {
    let mut raw = known();
    raw.process.modules.extend(["d3d12.dll".into(), "opengl32.dll".into()]);
    let r = make(raw);
    assert_eq!(r.apis.iter().filter(|a| a.module == Fact::Observed).count(), 4);
    assert_eq!(r.renderer_decision, "probe.no_verdict");
    assert!(r.routes.iter().all(|r| r.state != "supported"));
    assert_eq!(r.log.current_process_binding, Fact::Unknown);
}
#[test]
fn all_platform_matrix_rows_are_conditional_and_x86_is_refused() {
    for runtime in [Runtime::Windows, Runtime::Proton, Runtime::Linux, Runtime::Unknown] {
        for arch in [Architecture::X86, Architecture::X64, Architecture::Unknown] {
            let mut raw = known();
            raw.executable.architecture = arch;
            raw.process
                .modules
                .extend(["opengl32.dll".into(), "wined3d.dll".into()]);
            let mut q = request();
            q.runtime = runtime;
            let r = report(&q, raw, Duration::ZERO);
            assert_eq!(r.routes.len(), 10);
            if arch == Architecture::X86 {
                for name in ["d3d10_dxvk", "d3d11_bridge", "d3d12_present", "vulkan_present"] {
                    assert_eq!(r.routes.iter().find(|r| r.route == name).unwrap().state, "unavailable");
                }
            }
        }
    }
}
#[test]
fn privacy_only_allows_known_basenames_and_digests() {
    let mut raw = known();
    raw.process.modules.extend([
        "/private/person/title.dll".into(),
        "0xdeadbeef".into(),
        "private window title".into(),
    ]);
    raw.executable.imports.push("C:\\private\\person\\private.dll".into());
    let s = serde_json::to_string(&make(raw)).unwrap();
    for secret in ["/private", "person", "0xdeadbeef", "private window", "C:"] {
        assert!(!s.contains(secret));
    }
    assert!(s.len() < OUTPUT_LIMIT);
}
#[test]
fn time_and_inventory_budgets_are_fail_closed() {
    let mut raw = known();
    raw.timed_out = true;
    let r = make(raw);
    assert!(!r.inspection_complete);
    assert!(r.issues.contains(&"probe.budget"));
    assert!(
        r.apis
            .iter()
            .all(|a| a.module == Fact::Unknown && a.import == Fact::Unknown)
    );
    let r = report(&request(), known(), TIME_LIMIT);
    assert!(!r.inspection_complete);
    let mut raw = known();
    raw.process.modules = vec!["d3d12.dll".into(); INVENTORY_LIMIT + 1];
    assert!(!make(raw).inspection_complete);
}
#[test]
fn log_creation_present_and_install_are_distinct() {
    let installed = log_evidence(b"DLSS-NR Vulkan present host: installed\n");
    assert_eq!(installed.vulkan_creation, Fact::Unknown);
    assert_eq!(installed.vulkan_present, Fact::Unknown);
    let log=log_evidence(b"DLSS-NR Vulkan present host: device 0x123 #1 created by private.exe\nDLSS-NR Vulkan present host (cold NR): window 0x456 (first present); presents 1, swapchains 1\nDx11wDx12SC 1 created, real: 0x123\nDLSS-NR present: the pass runs at the game's own Present, ahead of its frame generation, on the GPU\n");
    assert_eq!(log.vulkan_creation, Fact::Observed);
    assert_eq!(log.vulkan_present, Fact::Observed);
    assert_eq!(log.d3d11_bridge_creation, Fact::Observed);
    assert_eq!(log.d3d12_present, Fact::Observed);
    assert_eq!(log.current_process_binding, Fact::Unknown);
    assert!(!serde_json::to_string(&log).unwrap().contains("private.exe"));
}
#[test]
fn unreadable_files_links_and_oversized_logs_are_bounded() {
    let temp = tempfile::tempdir().unwrap();
    let path = temp.path().join("game.exe");
    assert_eq!(
        image(&path, Instant::now() + TIME_LIMIT).architecture,
        Architecture::Unknown
    );
    std::fs::write(&path, pe(0x8664)).unwrap();
    assert_eq!(
        image(&path, Instant::now() + TIME_LIMIT).architecture,
        Architecture::X64
    );
    let log = temp.path().join("OptiScaler.log");
    std::fs::write(&log, vec![b'a'; READ_LIMIT + 1]).unwrap();
    assert!(!read_log(&log, Instant::now() + TIME_LIMIT).complete);
    #[cfg(unix)]
    {
        let link = temp.path().join("link.exe");
        std::os::unix::fs::symlink(&path, &link).unwrap();
        assert!(image(&link, Instant::now() + TIME_LIMIT).sha256.is_none());
    }
}
#[test]
fn log_outside_selected_game_is_refused() {
    let a = tempfile::tempdir().unwrap();
    let b = tempfile::tempdir().unwrap();
    let exe = a.path().join("game.exe");
    std::fs::write(&exe, pe(0x8664)).unwrap();
    let log = b.path().join("OptiScaler.log");
    std::fs::write(&log, b"Dx11wDx12SC 1 created, real: 0x123").unwrap();
    let q = Request {
        executable: Some(exe),
        log: Some(log),
        ..Default::default()
    };
    let r = inspect(Some(&q)).unwrap();
    assert!(r.issues.contains(&"probe.log_scope"));
    assert_eq!(r.log.d3d11_bridge_creation, Fact::Unknown);
}

#[test]
fn delayed_imports_are_partial_and_overlapping_sections_are_unknown() {
    let mut b = pe(0x8664);
    b[88 + 112 + 13 * 8..88 + 112 + 13 * 8 + 4].copy_from_slice(&0x1100u32.to_le_bytes());
    assert!(!pe_version::read_imports(&mut Cursor::new(&b)).unwrap().1);
    b[70..72].copy_from_slice(&2u16.to_le_bytes());
    let section = b[328..368].to_vec();
    b[368..408].copy_from_slice(&section);
    assert!(pe_version::read_imports(&mut Cursor::new(&b)).is_none());
}
#[test]
fn unknown_process_never_inherits_executable_architecture() {
    let mut raw = known();
    raw.process.architecture = Architecture::Unknown;
    assert_eq!(make(raw).process_architecture, Architecture::Unknown);
}
#[test]
fn no_file_mutations_and_no_scan_outside_explicit_selection() {
    let temp = tempfile::tempdir().unwrap();
    let game = temp.path().join("game.exe");
    let ini = temp.path().join("OptiScaler.ini");
    std::fs::write(&game, pe(0x8664)).unwrap();
    std::fs::write(&ini, b"unchanged").unwrap();
    let before = std::fs::read(&game).unwrap();
    let r = inspect(Some(&Request {
        executable: Some(game.clone()),
        ..Default::default()
    }))
    .unwrap();
    assert_eq!(r.executable.architecture, Architecture::X64);
    assert!(r.apis.iter().all(|a| a.module == Fact::Unknown));
    assert_eq!(std::fs::read(&game).unwrap(), before);
    assert_eq!(std::fs::read(&ini).unwrap(), b"unchanged");
    assert_eq!(std::fs::read_dir(temp.path()).unwrap().count(), 2);
}

#[test]
fn unknown_pe_machine_is_never_x86() {
    let b = pe(0x1234);
    assert_eq!(
        pe_version::read_header(&mut Cursor::new(&b)).unwrap().machine,
        Machine::Unknown
    );
}

#[test]
fn capture_availability_is_not_inferred_from_the_requested_os() {
    let mut q = request();
    q.runtime = Runtime::Windows;
    q.output = DesiredOutput::Overlay;
    for (available, expected) in [
        (None, "unknown"),
        (Some(false), "unavailable"),
        (Some(true), "candidate"),
    ] {
        let mut raw = known();
        raw.capture_available = available;
        let r = report(&q, raw, Duration::ZERO);
        assert_eq!(
            r.routes.iter().find(|r| r.route == "windows_capture").unwrap().state,
            expected
        );
    }
}

fn write_dated(path: &Path, bytes: &[u8], seconds: u64) {
    std::fs::write(path, bytes).unwrap();
    File::options()
        .write(true)
        .open(path)
        .unwrap()
        .set_modified(std::time::UNIX_EPOCH + Duration::from_secs(seconds))
        .unwrap();
}
const VK_UPSCALED: &[u8] = b"DLSS-NR Vulkan: running natively at private/path/0x123";
#[test]
fn automatic_logs_are_newest_first_and_explicit_log_wins() {
    let temp = tempfile::tempdir().unwrap();
    let exe = temp.path().join("game.exe");
    std::fs::write(&exe, pe(0x8664)).unwrap();
    let older = temp.path().join("OptiScaler.previous.log");
    let newer = temp.path().join("OptiScaler_2.log");
    write_dated(&older, b"Dx11wDx12SC 1 created, real: 0x123", 1);
    write_dated(&newer, VK_UPSCALED, 2);
    let mut q = Request {
        executable: Some(exe),
        ..Default::default()
    };
    let r = inspect(Some(&q)).unwrap();
    assert_eq!(r.log.vulkan_upscaled, Fact::Observed);
    assert_eq!(r.log.d3d11_bridge_creation, Fact::Unknown);
    assert_eq!(r.log.current_process_binding, Fact::Unknown);
    assert_eq!(r.log.file_sha256, Some(digest(VK_UPSCALED)));
    q.log = Some(older.clone());
    assert_eq!(inspect(Some(&q)).unwrap().log.d3d11_bridge_creation, Fact::Observed);
    q.log = None;
    write_dated(&newer, b"no route evidence", 2);
    assert_eq!(inspect(Some(&q)).unwrap().log.d3d11_bridge_creation, Fact::Observed);
    let text = serde_json::to_string(&inspect(Some(&q)).unwrap()).unwrap();
    assert!(!text.contains("private/path") && !text.contains("0x123"));
}
#[test]
fn automatic_logs_never_follow_other_folders_names_or_links() {
    let root = tempfile::tempdir().unwrap();
    let folder = root.path().join("game");
    std::fs::create_dir(&folder).unwrap();
    std::fs::create_dir(folder.join("nested")).unwrap();
    let exe = folder.join("game.exe");
    std::fs::write(&exe, pe(0x8664)).unwrap();
    for path in [
        root.path().join("OptiScaler.log"),
        folder.join("nested/OptiScaler.log"),
        folder.join("Other.log"),
        folder.join("OptiScaler_bad.log"),
    ] {
        std::fs::write(path, VK_UPSCALED).unwrap();
    }
    std::fs::write(folder.join("OptiScaler.ini"), b"[Log]\nFile=../OptiScaler.log\n").unwrap();
    #[cfg(unix)]
    std::os::unix::fs::symlink(root.path().join("OptiScaler.log"), folder.join("OptiScaler.log")).unwrap();
    let q = Request {
        executable: Some(exe),
        ..Default::default()
    };
    assert_eq!(inspect(Some(&q)).unwrap().log.vulkan_upscaled, Fact::Unknown);
}
#[test]
fn automatic_log_read_and_file_budgets_are_shared() {
    let temp = tempfile::tempdir().unwrap();
    let exe = temp.path().join("game.exe");
    write_dated(&temp.path().join("OptiScaler.log"), &vec![b'x'; READ_LIMIT + 1], 100);
    write_dated(&temp.path().join("OptiScaler.previous.log"), VK_UPSCALED, 1);
    let mut issues = Vec::new();
    let e = discover_log(&exe, Instant::now() + TIME_LIMIT, &mut issues);
    assert_eq!(e.vulkan_upscaled, Fact::Unknown);
    assert!(issues.contains(&"probe.log_search_partial") && !e.complete && e.file_sha256.is_none());
    std::fs::remove_file(temp.path().join("OptiScaler.log")).unwrap();
    for n in 0..16 {
        write_dated(&temp.path().join(format!("OptiScaler_{n}.log")), b"", n + 2);
    }
    issues.clear();
    assert_eq!(
        discover_log(&exe, Instant::now() + TIME_LIMIT, &mut issues).vulkan_upscaled,
        Fact::Unknown
    );
    assert!(issues.contains(&"probe.log_search_partial"));
}
#[test]
fn automatic_log_directory_budget_is_fail_closed() {
    let temp = tempfile::tempdir().unwrap();
    for n in 0..INVENTORY_LIMIT {
        std::fs::write(temp.path().join(format!("entry{n}")), b"").unwrap();
    }
    std::fs::write(temp.path().join("OptiScaler.log"), VK_UPSCALED).unwrap();
    let mut issues = Vec::new();
    let e = discover_log(&temp.path().join("game.exe"), Instant::now() + TIME_LIMIT, &mut issues);
    assert_eq!(e.vulkan_upscaled, Fact::Unknown);
    assert!(issues.contains(&"probe.log_search_partial"));
}
