use retoque_core::{apply, reshade::*};
use std::fs;

fn run(plan: retoque_core::Plan) {
    apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
}

fn fixture() -> (tempfile::TempDir, std::path::PathBuf, std::path::PathBuf) {
    let t = tempfile::tempdir().unwrap();
    let g = t.path().join("game");
    fs::create_dir(&g).unwrap();
    fs::write(g.join("dxgi.dll"), b"OptiScaler working as fixture").unwrap();
    fs::write(
        g.join("OptiScaler.ini"),
        b"; untouched\r\n[Plugins]\r\nLoadAsiPlugins=auto",
    )
    .unwrap();
    let s = t.path().join("ReShade64.dll");
    fs::write(&s, pe(b"ReShade LoadFromDllMain")).unwrap();
    (t, g, s)
}

#[test]
fn install_remove_restores_ini_bytes_and_preserves_user_assets() {
    let (_t, g, s) = fixture();
    let before = fs::read(g.join("OptiScaler.ini")).unwrap();
    fs::write(g.join("ReShade.ini"), b"[GENERAL]\r\nPresetPath=mine.ini\r\n").unwrap();
    let plan = plan_install(&g, &g, &s).unwrap();
    assert!(!g.join(".retoque").exists());
    assert!(
        plan.steps()
            .iter()
            .any(|s| matches!(s, retoque_core::Step::SetIni { key, .. } if key == "LoadReshade"))
    );
    run(plan);
    assert_eq!(fs::read(g.join("ReShade64.dll")).unwrap(), fs::read(s).unwrap());
    let remove = plan_uninstall(&g).unwrap();
    assert!(
        remove
            .steps()
            .iter()
            .any(|s| matches!(s, retoque_core::Step::UnsetIni { key, .. } if key == "LoadReshade"))
    );
    run(remove);
    assert_eq!(fs::read(g.join("OptiScaler.ini")).unwrap(), before);
    assert!(!g.join("ReShade64.dll").exists());
    assert_eq!(
        fs::read(g.join("ReShade.ini")).unwrap(),
        b"[GENERAL]\r\nPresetPath=mine.ini\r\n"
    );
}

#[test]
fn full_limited_and_unknown_are_distinct() {
    assert_eq!(build(b"MZ ReShade LoadFromDllMain"), Build::Full);
    assert_eq!(build(b"MZ ReShade only limited add-on functionality"), Build::Limited);
    assert_eq!(build(b"MZ ReShade"), Build::Unknown);
    assert_eq!(
        build(b"MZ LoadFromDllMain only limited add-on functionality"),
        Build::Unknown
    );
}

#[test]
fn proxy_special_k_and_different_exe_directory_refuse() {
    let (_t, g, s) = fixture();
    fs::write(g.join("d3d12.dll"), b"MZ ReShade").unwrap();
    assert!(!plan_install(&g, &g, &s).unwrap().refusals().is_empty());
    fs::remove_file(g.join("d3d12.dll")).unwrap();
    fs::write(g.join("SpecialK64.dll"), b"MZ Special K").unwrap();
    assert!(!plan_install(&g, &g, &s).unwrap().refusals().is_empty());
    fs::remove_file(g.join("SpecialK64.dll")).unwrap();
    let exe = g.join("Bin");
    fs::create_dir(&exe).unwrap();
    assert!(!plan_install(&g, &exe, &s).unwrap().refusals().is_empty());
}

#[test]
fn addon_import_toggle_remove_and_owner() {
    let (t, g, _) = fixture();
    let source = t.path().join("renodx-dlss5.addon64");
    fs::write(&source, pe(b"synthetic addon")).unwrap();
    let ini = b"; user\r\n[ADDON]\r\nDisabledAddons=Generic Depth\r\n";
    fs::write(g.join("ReShade.ini"), ini).unwrap();
    run(plan_add(&g, &source).unwrap());
    assert!(source.exists());
    let items = addons(&g).unwrap();
    assert_eq!(items.len(), 1);
    assert!(items[0].enabled);
    assert_eq!(nr_owner(true, None, &items), NrOwner::Addon);
    assert_eq!(nr_owner(true, Some("false"), &items), NrOwner::Retoque);
    assert_eq!(nr_owner(true, Some("auto"), &items), NrOwner::Addon);
    assert_eq!(nr_owner(true, Some("true"), &items), NrOwner::Addon);
    assert_eq!(nr_owner(true, Some(" FALSE "), &items), NrOwner::Retoque);
    run(plan_enabled(&g, "renodx-dlss5.addon64", false).unwrap());
    assert!(!addons(&g).unwrap()[0].enabled);
    assert_eq!(nr_owner(true, None, &addons(&g).unwrap()), NrOwner::Retoque);
    run(plan_remove(&g, "renodx-dlss5.addon64").unwrap());
    assert!(!g.join("renodx-dlss5.addon64").exists());
    assert_eq!(fs::read(g.join("ReShade.ini")).unwrap(), ini);
    fs::write(g.join("renodx-game.addon64"), b"MZ HDR mod").unwrap();
    assert_eq!(nr_owner(true, None, &addons(&g).unwrap()), NrOwner::Retoque);
}

#[test]
fn addon_path_and_disabled_values_match_reshade() {
    let (_t, g, _) = fixture();
    fs::create_dir(g.join("mods")).unwrap();
    fs::write(g.join("mods/a.addon64"), b"MZ mod").unwrap();
    fs::write(g.join("mods/b.addon"), b"MZ mod").unwrap();
    fs::write(g.join("outside.addon64"), b"MZ outside").unwrap();
    fs::write(
        g.join("ReShade.ini"),
        b"[ADDON]\nAddonPath=mods\nDisabledAddons=Depth,, Custom@a.addon64\nDisabledAddons=@b.addon\n",
    )
    .unwrap();
    let a = addons(&g).unwrap();
    assert_eq!(a.len(), 2);
    assert!(a.iter().all(|a| !a.enabled));
    assert_eq!(decode_list("one,,two,three"), ["one,two", "three"]);
}

#[test]
fn real_optiscaler_markers_do_not_look_like_a_second_loader() {
    let (_t, g, s) = fixture();
    fs::write(
        g.join("dxgi.dll"),
        b"OptiScaler working as ReShade SpecialK64.dll Special K",
    )
    .unwrap();
    assert!(plan_install(&g, &g, &s).unwrap().refusals().is_empty());
}
#[test]
fn limited_build_upgrade_restores_original_dll() {
    let (_t, g, s) = fixture();
    let old = b"MZ ReShade only limited add-on functionality";
    fs::write(g.join("ReShade64.dll"), old).unwrap();
    run(plan_install(&g, &g, &s).unwrap());
    run(plan_uninstall(&g).unwrap());
    assert_eq!(fs::read(g.join("ReShade64.dll")).unwrap(), old);
}
#[test]
fn identical_addon_is_kept_and_different_collision_is_refused() {
    let (t, g, _) = fixture();
    let s = t.path().join("a.addon64");
    fs::write(&s, pe(b"same")).unwrap();
    fs::write(g.join("a.addon64"), pe(b"same")).unwrap();
    let plan = plan_add(&g, &s).unwrap();
    assert!(
        plan.steps()
            .iter()
            .any(|s| matches!(s, retoque_core::Step::Keep { .. }))
    );
    run(plan);
    run(plan_remove(&g, "a.addon64").unwrap());
    assert!(g.join("a.addon64").exists());
    fs::write(&s, pe(b"different")).unwrap();
    assert!(!plan_add(&g, &s).unwrap().refusals().is_empty());
}
#[test]
fn refusal_and_stale_plan_leave_game_unchanged() {
    let (_t, g, s) = fixture();
    let plan = plan_install(&g, &g, &s).unwrap();
    fs::write(g.join("SpecialK64.dll"), b"MZ Special K").unwrap();
    assert!(apply(&plan.into_verified().unwrap(), &mut |_| {}).is_err());
    assert!(!g.join("ReShade64.dll").exists());
    assert!(!g.join(".retoque").exists());
}
#[test]
fn duplicate_ini_values_and_unterminated_tail_restore_exactly() {
    let (_t, g, s) = fixture();
    let before = b"\xef\xbb\xbf; x\xff\r\n[Plugins]\r\nLoadReshade = false\r\n[Plugins]\r\nLoadReshade=auto";
    fs::write(g.join("OptiScaler.ini"), before).unwrap();
    run(plan_install(&g, &g, &s).unwrap());
    run(plan_uninstall(&g).unwrap());
    assert_eq!(fs::read(g.join("OptiScaler.ini")).unwrap(), before);
}
#[test]
fn addon_created_ini_disappears_on_exact_removal_but_user_changes_survive() {
    let (t, g, _) = fixture();
    let source = t.path().join("a.addon64");
    fs::write(&source, pe(b"addon")).unwrap();
    run(plan_add(&g, &source).unwrap());
    run(plan_enabled(&g, "a.addon64", false).unwrap());
    run(plan_remove(&g, "a.addon64").unwrap());
    assert!(!g.join("ReShade.ini").exists());
    run(plan_add(&g, &source).unwrap());
    run(plan_enabled(&g, "a.addon64", false).unwrap());
    let mut bytes = fs::read(g.join("ReShade.ini")).unwrap();
    bytes.extend_from_slice(b"[GENERAL]\nPresetPath=user.ini\n");
    fs::write(g.join("ReShade.ini"), bytes).unwrap();
    run(plan_remove(&g, "a.addon64").unwrap());
    assert!(
        fs::read_to_string(g.join("ReShade.ini"))
            .unwrap()
            .contains("PresetPath=user.ini")
    );
}
#[test]
fn case_sensitive_reshade_sections_and_escaped_paths() {
    let (_t, g, _) = fixture();
    fs::create_dir(g.join("mods,one")).unwrap();
    fs::write(g.join("mods,one/x.addon64"), b"MZ addon").unwrap();
    fs::write(
        g.join("ReShade.ini"),
        b"[ADDON] ; header\nAddonPath=mods,,one\nDisabledAddons=@x.addon64\n[addon]\nDisabledAddons=ignored\n",
    )
    .unwrap();
    assert!(!addons(&g).unwrap()[0].enabled);
    run(plan_enabled(&g, "x.addon64", true).unwrap());
    assert!(addons(&g).unwrap()[0].enabled);
    assert!(
        fs::read_to_string(g.join("ReShade.ini"))
            .unwrap()
            .contains("[addon]\nDisabledAddons=ignored")
    );
}
#[cfg(unix)]
#[test]
fn mapped_game_refuses_all_writes() {
    use std::process::{Command, Stdio};
    let (t, g, s) = fixture();
    let addon = t.path().join("a.addon64");
    fs::write(&addon, pe(b"addon")).unwrap();
    let script = "import mmap,sys; f=open(sys.argv[1],'rb'); m=mmap.mmap(f.fileno(),0,access=mmap.ACCESS_READ); print('ready',flush=True); sys.stdin.read()";
    let mut child = Command::new("python3")
        .args(["-c", script])
        .arg(g.join("dxgi.dll"))
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap();
    use std::io::BufRead;
    let mut line = String::new();
    std::io::BufReader::new(child.stdout.take().unwrap())
        .read_line(&mut line)
        .unwrap();
    for plan in [plan_install(&g, &g, &s).unwrap(), plan_add(&g, &addon).unwrap()] {
        assert!(
            plan.refusals()
                .iter()
                .any(|r| matches!(r, retoque_core::Refusal::GameRunning { .. }))
        );
    }
    drop(child.stdin.take());
    child.wait().unwrap();
    assert!(!g.join(".retoque").exists());
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
fn full_uninstall_reverses_loader_and_addon_ini_state() {
    let (t, g, s) = fixture();
    let before = fs::read(g.join("OptiScaler.ini")).unwrap();
    let source = t.path().join("a.addon64");
    fs::write(&source, pe(b"addon")).unwrap();
    run(plan_install(&g, &g, &s).unwrap());
    run(plan_add(&g, &source).unwrap());
    run(plan_enabled(&g, "a.addon64", false).unwrap());
    run(retoque_core::plan_uninstall(&g).unwrap());
    assert_eq!(fs::read(g.join("OptiScaler.ini")).unwrap(), before);
    assert!(!g.join("ReShade64.dll").exists());
    assert!(!g.join("ReShade.ini").exists());
    assert!(!g.join("a.addon64").exists());
}
#[test]
fn preexisting_empty_lowercase_config_and_comma_filename_are_preserved() {
    let (t, g, _) = fixture();
    fs::write(g.join("reshade.ini"), b"").unwrap();
    let source = t.path().join("a,b.addon64");
    fs::write(&source, pe(b"addon")).unwrap();
    run(plan_add(&g, &source).unwrap());
    run(plan_enabled(&g, "a,b.addon64", false).unwrap());
    assert!(!addons(&g).unwrap()[0].enabled);
    run(plan_remove(&g, "a,b.addon64").unwrap());
    assert_eq!(fs::read(g.join("reshade.ini")).unwrap(), b"");
}
#[test]
fn changed_owned_reshade_is_not_adopted_as_a_legacy_component() {
    let (_t, g, s) = fixture();
    run(plan_install(&g, &g, &s).unwrap());
    fs::write(
        g.join("ReShade64.dll"),
        pe(b"ReShade only limited add-on functionality"),
    )
    .unwrap();
    assert!(!plan_install(&g, &g, &s).unwrap().refusals().is_empty());
    assert!(plan_uninstall(&g).is_err());
}
#[test]
fn exported_addon_name_honors_name_only_disabled_entries() {
    let (t, g, _) = fixture();
    let mut b = pe(b"");
    b.resize(1024, 0);
    b[70..72].copy_from_slice(&1u16.to_le_bytes());
    b[112..120].copy_from_slice(&0x180000000u64.to_le_bytes());
    for (at, n) in [
        (200, 0x1000u32),
        (204, 40),
        (340, 0x1000),
        (344, 512),
        (348, 512),
        (532, 1),
        (536, 1),
        (540, 0x1040),
        (544, 0x1044),
        (548, 0x1048),
        (576, 0x1080),
        (580, 0x1060),
    ] {
        b[at..at + 4].copy_from_slice(&n.to_le_bytes());
    }
    b[608..613].copy_from_slice(b"NAME\0");
    b[640..648].copy_from_slice(&0x180001090u64.to_le_bytes());
    b[656..668].copy_from_slice(b"Custom Name\0");
    let source = t.path().join("custom.addon64");
    fs::write(&source, b).unwrap();
    let original = b"[ADDON]\nDisabledAddons=Custom Name,Generic Depth\n";
    fs::write(g.join("ReShade.ini"), original).unwrap();
    run(plan_add(&g, &source).unwrap());
    let listed = addons(&g).unwrap();
    assert_eq!(listed[0].name, "Custom Name");
    assert!(!listed[0].enabled);
    run(plan_enabled(&g, "custom.addon64", true).unwrap());
    assert!(addons(&g).unwrap()[0].enabled);
    run(plan_remove(&g, "custom.addon64").unwrap());
    assert_eq!(fs::read(g.join("ReShade.ini")).unwrap(), original);
}

#[test]
fn user_config_created_after_import_survives_undo() {
    let (t, g, _) = fixture();
    let source = t.path().join("a.addon64");
    fs::write(&source, pe(b"addon")).unwrap();
    run(plan_add(&g, &source).unwrap());
    fs::write(g.join("ReShade.ini"), b"").unwrap();
    run(plan_enabled(&g, "a.addon64", false).unwrap());
    run(plan_remove(&g, "a.addon64").unwrap());
    assert_eq!(fs::read(g.join("ReShade.ini")).unwrap(), b"");
}

fn input_mode(g: &std::path::Path) -> Option<String> {
    let bytes = fs::read(g.join("ReShade.ini")).ok()?;
    retoque_core::ini::Ini::parse_sensitive(&bytes)
        .get("INPUT", "InputProcessing")
        .map(str::to_owned)
}

#[test]
fn install_lets_both_menus_use_the_mouse_and_uninstall_removes_the_config_it_created() {
    let (_t, g, s) = fixture();
    assert!(menu_input_blocked(b""));
    let plan = plan_install(&g, &g, &s).unwrap();
    assert!(plan.steps().iter().any(
        |s| matches!(s, retoque_core::Step::SetIni { file, section, key, value, .. }
        if file == "ReShade.ini" && section == "INPUT" && key == "InputProcessing" && value == "1")
    ));
    run(plan);
    assert_eq!(input_mode(&g).as_deref(), Some("1"));
    assert!(!menu_input_blocked_in(&g).unwrap());
    run(plan_uninstall(&g).unwrap());
    assert!(!g.join("ReShade.ini").exists(), "only the key Retoque wrote was there");
    assert!(!g.join(".retoque/reshade-input.ini").exists());
}

#[test]
fn reshade_rewriting_its_config_still_restores_the_input_mode_and_keeps_its_settings() {
    let (_t, g, s) = fixture();
    fs::write(
        g.join("ReShade.ini"),
        b"[INPUT]\r\nInputProcessing=2\r\nKeyOverlay=36,0,0,0\r\n",
    )
    .unwrap();
    run(plan_install(&g, &g, &s).unwrap());
    assert_eq!(input_mode(&g).as_deref(), Some("1"));
    // ReShade saves its whole configuration on exit.
    let mut saved = fs::read(g.join("ReShade.ini")).unwrap();
    saved.extend_from_slice(b"[OVERLAY]\r\nTutorialProgress=4\r\n");
    fs::write(g.join("ReShade.ini"), &saved).unwrap();
    run(plan_uninstall(&g).unwrap());
    assert_eq!(input_mode(&g).as_deref(), Some("2"));
    let text = fs::read_to_string(g.join("ReShade.ini")).unwrap();
    assert!(text.contains("TutorialProgress=4") && text.contains("KeyOverlay=36,0,0,0"));
}

#[test]
fn a_mode_the_user_chose_later_survives_uninstall() {
    let (_t, g, s) = fixture();
    run(plan_install(&g, &g, &s).unwrap());
    let changed = fs::read_to_string(g.join("ReShade.ini"))
        .unwrap()
        .replace("InputProcessing=1", "InputProcessing=0");
    fs::write(g.join("ReShade.ini"), changed).unwrap();
    run(plan_uninstall(&g).unwrap());
    assert_eq!(input_mode(&g).as_deref(), Some("0"));
}

#[test]
fn menu_input_repair_alone_on_an_existing_reshade() {
    let (_t, g, _s) = fixture();
    fs::write(g.join("ReShade.ini"), b"[INPUT]\r\nInputProcessing=2\r\n").unwrap();
    assert!(menu_input_blocked_in(&g).unwrap());
    run(plan_menu_input(&g).unwrap());
    assert_eq!(input_mode(&g).as_deref(), Some("1"));
    assert!(plan_menu_input(&g).unwrap().steps().is_empty());
    for mode in ["0", "1", " 1"] {
        assert!(!menu_input_blocked(
            format!("[INPUT]\nInputProcessing={mode}\n").as_bytes()
        ));
    }
    assert!(menu_input_blocked(b"[INPUT]\nInputProcessing=2\n"));
    // ReShade compares names case-sensitively: a differently cased key is not its setting.
    assert!(menu_input_blocked(b"[input]\ninputprocessing=1\n"));
}

#[test]
fn nr_addon_markers_match_the_runtime_list() {
    // The runtime decides who owns NR from retoque/dlssnr/DlssNr_ConsumerMarkers.h; the app must name the same add-ons.
    let header =
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../retoque/dlssnr/DlssNr_ConsumerMarkers.h");
    let source = fs::read_to_string(&header).unwrap();
    let table = &source[source.find("kCompetitors[]").expect("kCompetitors moved")..];
    let table = &table[..table.find("};").unwrap()];
    let mut runtime: Vec<&str> = table
        .split("L\"")
        .skip(1)
        .map(|rest| &rest[..rest.find('"').unwrap()])
        .collect();
    let mut app = NR_ADDON_MARKERS.to_vec();
    runtime.sort_unstable();
    app.sort_unstable();
    assert_eq!(app, runtime);
}

#[test]
fn every_neural_addon_the_runtime_yields_to_is_an_nr_candidate() {
    for file in [
        "renodx-dlss5.addon64",
        "RenoDX_DLSS.addon64",
        "DLSS5-Feeder.addon64",
        "dlss5_feed.addon",
        "deep-fried-chicken.addon64",
        "AMDLSS5.addon64",
        "nr_native_adapter.dll",
    ] {
        assert!(is_nr_addon(file), "{file}");
    }
    for file in ["ReShade64.dll", "renodx-ff7rebirth.addon64", "framegen.addon64"] {
        assert!(!is_nr_addon(file), "{file}");
    }
}
