//! The in-game menu's language file through install, re-install and uninstall.
use retoque_core::*;
use std::{fs, path::PathBuf};

struct Fixture {
    _temp: tempfile::TempDir,
    game: PathBuf,
    source: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let game = temp.path().join("game");
        let source = temp.path().join("release");
        fs::create_dir(&game).unwrap();
        fs::create_dir_all(source.join("translations/optional")).unwrap();
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as %s v1").unwrap();
        fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_create v1").unwrap();
        fs::write(source.join(INI_NAME), b"[DlssNr]\r\nEnabled=auto\r\n").unwrap();
        fs::write(source.join("translations/optional/pt-BR.lang"), b"Reset\tRedefinir\n").unwrap();
        fs::write(source.join("translations/optional/de.lang"), b"Reset\tZuruecksetzen\n").unwrap();
        Self {
            _temp: temp,
            game,
            source,
        }
    }
    fn install(&self, menu_language: MenuLanguage) -> Vec<String> {
        let plan = plan_install(&InstallOptions {
            game_dir: self.game.clone(),
            source_dir: self.source.clone(),
            copy_nvngx: false,
            menu_language,
            ..Default::default()
        })
        .unwrap();
        assert!(plan.refusals().is_empty(), "{:?}", plan.refusals());
        let notes = plan.notes().to_vec();
        apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
        notes
    }
    fn lang(&self) -> Option<Vec<u8>> {
        fs::read(self.game.join(MENU_LANG_NAME)).ok()
    }
}

#[test]
fn a_pack_is_installed_switched_removed_and_uninstalled() {
    let f = Fixture::new();
    f.install(MenuLanguage::Pack("pt-BR".into()));
    assert_eq!(f.lang().unwrap(), b"Reset\tRedefinir\n");
    // Another pack replaces ours.
    f.install(MenuLanguage::Pack("de".into()));
    assert_eq!(f.lang().unwrap(), b"Reset\tZuruecksetzen\n");
    // The scripts' and the CLI's default leaves it as it is.
    f.install(MenuLanguage::Keep);
    assert_eq!(f.lang().unwrap(), b"Reset\tZuruecksetzen\n");
    // English removes the file an install put there.
    f.install(MenuLanguage::English);
    assert!(f.lang().is_none());
    // And uninstall takes away one that is still there.
    f.install(MenuLanguage::Pack("pt-BR".into()));
    apply(&plan_uninstall(&f.game).unwrap().into_verified().unwrap(), &mut |_| {}).unwrap();
    assert!(f.lang().is_none());
}

#[test]
fn the_users_own_language_file_is_never_touched() {
    let f = Fixture::new();
    fs::write(f.game.join(MENU_LANG_NAME), b"mine\tminha\n").unwrap();
    let notes = f.install(MenuLanguage::Pack("pt-BR".into()));
    assert!(notes.iter().any(|n| n.contains("not Retoque's")), "{notes:?}");
    assert_eq!(f.lang().unwrap(), b"mine\tminha\n");
    f.install(MenuLanguage::English);
    assert_eq!(f.lang().unwrap(), b"mine\tminha\n");
    // Uninstall leaves it too: it was never recorded as Retoque's.
    apply(&plan_uninstall(&f.game).unwrap().into_verified().unwrap(), &mut |_| {}).unwrap();
    assert_eq!(f.lang().unwrap(), b"mine\tminha\n");
}

#[test]
fn an_unknown_code_changes_nothing() {
    let f = Fixture::new();
    let notes = f.install(MenuLanguage::Pack("../x".into()));
    assert!(f.lang().is_none());
    assert!(notes.iter().any(|n| n.contains("not one of Retoque's packs")));
}

#[test]
fn the_packs_named_here_are_the_ones_the_repository_ships() {
    let optional = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../../../retoque/translations/optional");
    for code in MENU_PACKS {
        assert!(optional.join(format!("{code}.lang")).is_file(), "{code}");
    }
}
