use super::*;
#[test]
fn shipped_packs_round_trip_without_altering_unreviewed_rows() {
    for target in [Target::App, Target::Menu] {
        for lang in Lang::ALL.into_iter().filter(|l| {
            if target == Target::App {
                l.is_draft()
            } else {
                *l != Lang::En
            }
        }) {
            let pack = Pack::new(target, lang);
            let initial = pack.export(&Entries::new()).unwrap();
            let imported = pack.import(initial.as_bytes()).unwrap();
            assert_eq!(pack.export(&imported).unwrap(), initial, "{target:?} {lang:?}");
            let key = pack
                .keys
                .iter()
                .find(|k| !pack.english[*k].contains(['{', '%']) && !pack.english[*k].ends_with('…'))
                .unwrap();
            let changed = Entries::from([(key.clone(), "Reviewed correction".into())]);
            let exported = pack.export(&changed).unwrap();
            let imported = pack.import(exported.as_bytes()).unwrap();
            assert_eq!(pack.export(&imported).unwrap(), exported);
            assert_eq!(imported[key], "Reviewed correction");
            for (k, v) in &pack.embedded {
                if k != key {
                    assert_eq!(&imported[k], v);
                }
            }
        }
    }
}
#[test]
fn app_validation_and_hostile_imports_fail_closed() {
    let pack = Pack::new(Target::App, Lang::De);
    assert_eq!(pack.validate("missing.key", "x"), Err("unknown"));
    assert_eq!(pack.validate("prefs.open", "Open"), Err("ellipsis"));
    assert_eq!(pack.validate("prefs.title", "Open…"), Err("ellipsis"));
    assert_eq!(pack.validate("prefs.title", "..."), Err("ellipsis"));
    assert_eq!(pack.validate("prefs.title", " padded "), Err("padding"));
    assert_eq!(pack.validate("prefs.title", "x\0"), Err("control"));
    assert_eq!(pack.validate("prefs.title", "x\u{202e}"), Err("control"));
    let key = pack.keys.iter().find(|k| pack.english[*k].contains("{n}")).unwrap();
    assert_eq!(pack.validate(key, "none"), Err("placeholder"));
    assert_eq!(
        pack.validate(key, &format!("{} {{n}}", pack.english[key])),
        Err("placeholder")
    );
    assert_eq!(pack.import(b"prefs.title\ta\nprefs.title\tb"), Err("duplicate"));
    assert_eq!(pack.import(b"prefs.title\t\xff"), Err("encoding"));
    assert_eq!(pack.import(&vec![b'x'; menu_pack::MAX_BYTES + 1]), Err("size"));
    assert_eq!(pack.import(b"prefs.title\ta\tb"), Err("columns"));
    assert_eq!(pack.import(b"#\0\nprefs.title\tx"), Err("control"));
    assert_eq!(pack.import(b"prefs.title\tx\x1b"), Err("control"));
}
#[test]
fn invalid_drafts_persist_but_do_not_override_and_reset_restores_fallback() {
    let root = tempfile::tempdir().unwrap();
    let pack = Pack::new(Target::App, Lang::De);
    let mut doc = Document::default();
    doc.drafts.insert("prefs.title".into(), "Korrekturen".into());
    doc.apply(root.path(), &pack).unwrap();
    assert_eq!(i18n::t(Lang::De, "prefs.title"), "Korrekturen");
    assert_eq!(i18n::t(Lang::De, "languages.title"), "Languages");
    doc.drafts.insert("prefs.open".into(), "missing ellipsis".into());
    doc.save(root.path(), &pack).unwrap();
    assert_eq!(doc.apply(root.path(), &pack), Err("ellipsis"));
    let mut loaded = Document::load(root.path(), &pack).unwrap();
    assert!(loaded.drafts.contains_key("prefs.open"));
    assert!(!loaded.applied.contains_key("prefs.open"));
    load_active(root.path()).unwrap();
    assert_eq!(i18n::t(Lang::De, "prefs.title"), "Korrekturen");
    loaded.drafts.clear();
    loaded.apply(root.path(), &pack).unwrap();
    assert_eq!(i18n::t(Lang::De, "prefs.title"), pack.base("prefs.title"));
}
#[test]
fn settings_choices_and_plurals_use_applied_overrides() {
    let root = tempfile::tempdir().unwrap();
    let pack = Pack::new(Target::App, Lang::Pl);
    let mut doc = Document::default();
    for key in pack
        .keys
        .iter()
        .filter(|k| k.starts_with("setting.") || k.starts_with("choice.") || k.ends_with("_few"))
    {
        if !pack.english[key].contains('{') && !pack.english[key].ends_with('…') {
            doc.drafts.insert(key.clone(), "Corrected".into());
        }
    }
    doc.apply(root.path(), &pack).unwrap();
    for setting in retoque_data::settings::all() {
        let key = format!("setting.{}.{}", setting.section, setting.key);
        if doc.applied.contains_key(&key) {
            assert_eq!(i18n::setting_label(Lang::Pl, setting), "Corrected");
        }
    }
    ACTIVE.with(|active| active.borrow_mut().clear());
}
#[test]
fn preview_is_plain_sample_substitution() {
    assert_eq!(preview("{n}: {name} ({n})", Target::App), "3: Retoque (3)");
    assert_eq!(preview("%llu %s %.2f %%", Target::Menu), "3 Retoque 3 %");
    assert_eq!(preview("%n", Target::Menu), "%n");
}
#[test]
fn menu_apply_only_writes_app_owned_files() {
    let root = tempfile::tempdir().unwrap();
    let pack = Pack::new(Target::Menu, Lang::De);
    let mut doc = Document::default();
    doc.drafts.insert("Reset".into(), "Wiederherstellen".into());
    doc.apply(root.path(), &pack).unwrap();
    let bytes = menu_pack::read(&menu_path(root.path(), Lang::De, &pack.export(&doc.applied).unwrap())).unwrap();
    assert_eq!(pack.import(&bytes).unwrap()["Reset"], "Wiederherstellen");
    assert!(!root.path().join("OptiScaler.lang").exists());
    assert_eq!(Document::load(root.path(), &pack).unwrap().applied, doc.applied);
}

#[test]
fn failed_publication_keeps_the_previous_active_translation() {
    let root = tempfile::tempdir().unwrap();
    let pack = Pack::new(Target::App, Lang::De);
    let mut doc = Document::default();
    doc.drafts.insert("prefs.title".into(), "Original correction".into());
    doc.apply(root.path(), &pack).unwrap();
    let file = document_path(root.path(), &pack);
    std::fs::remove_file(&file).unwrap();
    std::fs::create_dir(&file).unwrap();
    doc.drafts.insert("prefs.title".into(), "Should not apply".into());
    assert_eq!(doc.apply(root.path(), &pack), Err("io"));
    assert_eq!(i18n::t(Lang::De, "prefs.title"), "Original correction");
    ACTIVE.with(|active| active.borrow_mut().clear());
}

#[cfg(unix)]
#[test]
fn imported_links_and_linked_export_destinations_are_refused() {
    let root = tempfile::tempdir().unwrap();
    let original = root.path().join("original");
    let link = root.path().join("link");
    std::fs::write(&original, b"prefs.title\tOriginal\n").unwrap();
    std::os::unix::fs::symlink(&original, &link).unwrap();
    assert_eq!(menu_pack::read(&link), Err("io"));
    assert_eq!(write(&link, b"replacement"), Err("io"));
    assert_eq!(std::fs::read(&original).unwrap(), b"prefs.title\tOriginal\n");
}
