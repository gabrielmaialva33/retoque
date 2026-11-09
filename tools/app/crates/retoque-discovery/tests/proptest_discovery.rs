use proptest::prelude::*;
use retoque_discovery::{
    parse_appmanifest_acf, parse_compat_tool_mapping_vdf, parse_epic_manifest_item, parse_gaming_root,
    parse_heroic_game_config, parse_heroic_installed_json, parse_library_folders_vdf, parse_lutris_yaml, parse_vdf,
    try_parse_lutris_yaml,
};
use std::path::PathBuf;

#[test]
fn regression_lutris_yaml_deep_nesting_and_billion_laughs_do_not_crash_or_oom() {
    let deep_seq = "[".repeat(20_000) + &"]".repeat(20_000);
    let res_seq = std::thread::Builder::new()
        .stack_size(2 * 1024 * 1024)
        .spawn(move || try_parse_lutris_yaml(&deep_seq))
        .unwrap()
        .join()
        .expect("deep YAML sequence must not overflow stack");
    assert!(res_seq.is_err());

    let deep_map = "{a: ".repeat(20_000) + "1" + &"}".repeat(20_000);
    let res_map = std::thread::Builder::new()
        .stack_size(2 * 1024 * 1024)
        .spawn(move || try_parse_lutris_yaml(&deep_map))
        .unwrap()
        .join()
        .expect("deep YAML mapping must not overflow stack");
    assert!(res_map.is_err());

    // Billion Laughs exponential anchor/alias payload
    let mut bomb = String::from("a0: &a0 [1, 1, 1, 1, 1, 1, 1, 1]\n");
    for level in 1..=25 {
        let prev = level - 1;
        bomb.push_str(&format!(
            "a{level}: &a{level} [*a{prev}, *a{prev}, *a{prev}, *a{prev}, *a{prev}, *a{prev}, *a{prev}, *a{prev}]\n"
        ));
    }
    let res_bomb = try_parse_lutris_yaml(&bomb);
    assert!(res_bomb.is_err(), "Billion Laughs YAML alias bomb must be rejected");
}

#[test]
fn regression_lutris_wal_mismatched_page_size_rejected() {
    let dir = std::env::temp_dir().join(format!("retoque-wal-ps-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    let db_path = dir.join("pga.db");
    let wal_path = dir.join("pga.db-wal");

    // Minimal 100-byte SQLite header with page_size = 4096 (0x1000 at bytes 16..18)
    let mut db = vec![0u8; 4096];
    db[..16].copy_from_slice(b"SQLite format 3\0");
    db[16..18].copy_from_slice(&4096u16.to_be_bytes());
    std::fs::write(&db_path, &db).unwrap();

    // WAL header with valid magic 0x377f0682 but mismatched page_size = 8192 (0x00002000 at bytes 8..12)
    let mut wal = vec![0u8; 64];
    wal[0..4].copy_from_slice(&0x377f_0682u32.to_be_bytes());
    wal[8..12].copy_from_slice(&8192u32.to_be_bytes());
    std::fs::write(&wal_path, &wal).unwrap();

    let res = retoque_discovery::read_lutris_db_file(&db_path);
    assert!(res.is_err(), "mismatched DB/WAL page size must be rejected");
    let _ = std::fs::remove_dir_all(&dir);
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(64))]

    #[test]
    fn prop_vdf_acf_roundtrip_and_arbitrary_input_safety(
        app_id in 1u32..=9_999_999u32,
        name in "[A-Za-z0-9 _:-]{1,32}",
        install_dir in "[A-Za-z0-9_-]{1,24}",
        lib_path in "/[a-z]{1,8}/[a-z]{1,8}",
        arbitrary in "\\PC{0,256}",
    ) {
        let acf = format!(
            "\"AppState\"\n{{\n\t\"appid\"\t\t\"{app_id}\"\n\t\"name\"\t\t\"{name}\"\n\t\"installdir\"\t\t\"{install_dir}\"\n}}\n"
        );
        let parsed_acf = parse_appmanifest_acf(&acf).expect("valid synthetic ACF must parse");
        prop_assert_eq!(parsed_acf.app_id, app_id);
        prop_assert_eq!(parsed_acf.name, name);
        prop_assert_eq!(parsed_acf.install_dir, install_dir);

        let lf = format!(
            "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{lib_path}\"\n\t}}\n}}\n"
        );
        let parsed_lf = parse_library_folders_vdf(&lf).expect("valid synthetic libraryfolders.vdf must parse");
        prop_assert_eq!(parsed_lf, vec![PathBuf::from(&lib_path)]);

        // Arbitrary strings never panic in any VDF/ACF parser.
        let _ = parse_vdf(&arbitrary);
        let _ = parse_library_folders_vdf(&arbitrary);
        let _ = parse_appmanifest_acf(&arbitrary);
        let _ = parse_compat_tool_mapping_vdf(&arbitrary);
    }

    #[test]
    fn prop_lutris_epic_heroic_and_gaming_root_arbitrary_safety(
        exe in "/[a-z]{1,8}/[a-z]{1,8}\\.exe",
        prefix in "/[a-z]{1,8}/pfx",
        app_name in "[A-Za-z0-9_-]{1,16}",
        arbitrary_text in "\\PC{0,256}",
        arbitrary_bytes in prop::collection::vec(any::<u8>(), 0..=256),
    ) {
        let yaml = format!("game:\n  exe: '{exe}'\n  prefix: '{prefix}'\n");
        let parsed_yaml = try_parse_lutris_yaml(&yaml).expect("valid synthetic Lutris YAML must parse");
        prop_assert_eq!(parsed_yaml.exe, Some(PathBuf::from(&exe)));
        prop_assert_eq!(parsed_yaml.prefix, Some(PathBuf::from(&prefix)));

        let epic = serde_json::json!({
            "AppName": app_name,
            "DisplayName": app_name,
            "InstallLocation": prefix,
            "LaunchExecutable": exe,
            "bIsIncompleteInstall": false,
        })
        .to_string();
        let parsed_epic = parse_epic_manifest_item(&epic).expect("valid synthetic Epic manifest must parse");
        prop_assert_eq!(&parsed_epic.app_name, &app_name);

        // Arbitrary text/bytes never panic.
        let _ = try_parse_lutris_yaml(&arbitrary_text);
        let _ = parse_lutris_yaml(&arbitrary_text);
        let _ = parse_epic_manifest_item(&arbitrary_text);
        let _ = parse_heroic_installed_json(&arbitrary_text, "legendary");
        let _ = parse_heroic_game_config(&arbitrary_text, &app_name);
        let _ = parse_gaming_root(&arbitrary_bytes);
    }
}
