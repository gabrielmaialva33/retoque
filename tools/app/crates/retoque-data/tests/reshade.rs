use retoque_data::reshade::*;
use std::io::{Cursor, Write};
fn exe() -> Vec<u8> {
    let mut zip = zip::ZipWriter::new(Cursor::new(Vec::new()));
    zip.start_file("ReShade64.dll", zip::write::SimpleFileOptions::default())
        .unwrap();
    zip.write_all(&pe(b"ReShade LoadFromDllMain")).unwrap();
    zip.start_file("ReShade32.dll", zip::write::SimpleFileOptions::default())
        .unwrap();
    zip.write_all(b"never extract").unwrap();
    let mut bytes = b"MZ synthetic .NET executable\0".to_vec();
    bytes.extend(zip.finish().unwrap().into_inner());
    bytes
}
#[test]
fn zip_appended_to_exe_extracts_only_x64_and_checks_pins() {
    let bytes = exe();
    assert_eq!(extract(&bytes).unwrap(), pe(b"ReShade LoadFromDllMain"));
    assert!(verify(&bytes, bytes.len() as u64, &"0".repeat(64)).is_err());
    assert!(extract(&vec![0; MAX_INPUT + 1]).is_err());
}
#[test]
fn import_is_content_addressed_and_checks_cache_on_reuse() {
    let t = tempfile::tempdir().unwrap();
    let source = t.path().join("setup.exe");
    std::fs::write(&source, exe()).unwrap();
    let first = import(t.path(), &source).unwrap();
    assert_eq!(first, import(t.path(), &source).unwrap());
    assert_eq!(first.build, retoque_core::reshade::Build::Full);
    assert!(!first.component.root.join("ReShade32.dll").exists());
    assert_eq!(
        std::fs::read_to_string(first.component.root.join("LICENSE.txt")).unwrap(),
        LICENSE
    );
    std::fs::write(first.dll(), b"changed").unwrap();
    assert!(import(t.path(), &source).is_err());
}
#[test]
fn no_consent_never_calls_fetcher() {
    struct Never;
    impl retoque_data::sm86::Fetcher for Never {
        fn get(&self, _: &str, _: u64, _: &mut dyn FnMut(u64)) -> std::io::Result<Vec<u8>> {
            panic!("network without consent")
        }
    }
    let t = tempfile::tempdir().unwrap();
    assert!(download(t.path(), false, &Never, &mut |_| {}).is_err());
    assert!(!t.path().join("reshade").exists());
}

#[test]
#[ignore = "local official artifacts supplied explicitly; no network and no redistribution"]
fn verify_local_official_builds() {
    let input =
        std::path::PathBuf::from(std::env::var_os("RETOQUE_RESHADE_TEST_INPUTS").expect("explicit fixture directory"));
    assert_eq!(
        std::fs::read_to_string(input.join("RESHADE-LICENSE-v6.8.0.md")).unwrap(),
        LICENSE
    );
    let full = std::fs::read(input.join("ReShade_Setup_6.8.0_Addon.exe")).unwrap();
    verify(&full, SETUP_SIZE, SETUP_SHA).unwrap();
    let dll = extract(&full).unwrap();
    verify(&dll, DLL_SIZE, DLL_SHA).unwrap();
    assert_eq!(retoque_core::reshade::build(&dll), retoque_core::reshade::Build::Full);
    let limited = extract(&std::fs::read(input.join("ReShade_Setup_6.8.0.exe")).unwrap()).unwrap();
    verify(
        &limited,
        5_255_448,
        "b2945c29e7095491a901746b400e58db9b1592ab092bacf2a888ce37f02d08da",
    )
    .unwrap();
    assert_eq!(
        retoque_core::reshade::build(&limited),
        retoque_core::reshade::Build::Limited
    );
    assert_eq!(
        retoque_core::pe_version::read_details(&mut Cursor::new(&dll))
            .unwrap()
            .product
            .as_deref(),
        Some("ReShade")
    );
    assert_eq!(
        retoque_core::pe_version::read_version(&mut Cursor::new(&dll)).as_deref(),
        Some("6.8.0.2155")
    );
}
#[test]
fn doctor_and_recommendation_are_static_and_do_not_enable_anything() {
    let t = tempfile::tempdir().unwrap();
    std::fs::write(t.path().join("a.addon64"), b"MZ addon").unwrap();
    let snapshot = inspect(t.path()).unwrap();
    let rec = recommendation(&snapshot).unwrap();
    assert_eq!(rec.confidence, retoque_data::recommend::Confidence::StaticMatchUntested);
    assert_eq!(
        retoque_data::doctor::diagnose_reshade(&snapshot)[0].repair,
        Some(retoque_data::doctor::Repair::InstallReShade)
    );
    assert!(!t.path().join(".retoque").exists());
}

#[test]
fn doctor_offers_both_menus_the_mouse_only_while_reshade_blocks_it() {
    let t = tempfile::tempdir().unwrap();
    std::fs::write(t.path().join("ReShade64.dll"), pe(b"ReShade LoadFromDllMain")).unwrap();
    let blocked = inspect(t.path()).unwrap();
    assert!(blocked.menu_input_blocked);
    let finding = retoque_data::doctor::diagnose_reshade(&blocked)
        .into_iter()
        .find(|f| f.id == "reshade_menu_input")
        .unwrap();
    assert_eq!(finding.repair, Some(retoque_data::doctor::Repair::ReShadeMenuInput));
    std::fs::write(t.path().join("ReShade.ini"), b"[INPUT]\r\nInputProcessing=1\r\n").unwrap();
    let open = inspect(t.path()).unwrap();
    assert!(!open.menu_input_blocked);
    assert!(
        retoque_data::doctor::diagnose_reshade(&open)
            .iter()
            .all(|f| f.id != "reshade_menu_input")
    );
    // Without ReShade there is nothing to block.
    let none = tempfile::tempdir().unwrap();
    assert!(!inspect(none.path()).unwrap().menu_input_blocked);
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
fn local_unknown_version_is_allowed_but_x86_and_non_dll_are_refused() {
    let t = tempfile::tempdir().unwrap();
    let source = t.path().join("ReShade64.dll");
    let mut bytes = pe(b"ReShade old version");
    std::fs::write(&source, &bytes).unwrap();
    assert_eq!(
        import(t.path(), &source).unwrap().build,
        retoque_core::reshade::Build::Unknown
    );
    bytes[68..70].copy_from_slice(&0x14cu16.to_le_bytes());
    std::fs::write(&source, &bytes).unwrap();
    assert!(import(t.path(), &source).is_err());
    bytes = pe(b"ReShade LoadFromDllMain");
    bytes[86..88].copy_from_slice(&0u16.to_le_bytes());
    std::fs::write(source, bytes).unwrap();
    assert!(import(t.path(), &t.path().join("ReShade64.dll")).is_err());
}
#[test]
fn injected_provider_rejects_hash_mismatch_and_oversize_before_publication() {
    struct Bad(bool);
    impl retoque_data::sm86::Fetcher for Bad {
        fn get(&self, url: &str, limit: u64, _: &mut dyn FnMut(u64)) -> std::io::Result<Vec<u8>> {
            assert_eq!(url, URL);
            assert_eq!(limit, SETUP_SIZE);
            Ok(vec![0; SETUP_SIZE as usize + usize::from(self.0)])
        }
    }
    for oversized in [false, true] {
        let t = tempfile::tempdir().unwrap();
        assert!(download(t.path(), true, &Bad(oversized), &mut |_| {}).is_err());
        assert!(!t.path().join("reshade").exists());
    }
}
