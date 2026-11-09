#![cfg(any(unix, windows))]

use retoque_core::*;
use std::{
    fs,
    path::{Path, PathBuf},
};

struct Fixture {
    temp: tempfile::TempDir,
    library: PathBuf,
    game: PathBuf,
    source: PathBuf,
}
impl Fixture {
    fn new() -> Self {
        let temp = tempfile::tempdir().unwrap();
        let library = temp.path().join("library");
        let game = library.join("game");
        let source = temp.path().join("payload");
        fs::create_dir_all(&game).unwrap();
        fs::create_dir_all(source.join("OptiScaler")).unwrap();
        fs::write(source.join("OptiScaler.dll"), b"MZ OptiScaler working as synthetic").unwrap();
        fs::write(source.join(FORWARDER_NAME), b"MZ dlssnr_call_synthetic").unwrap();
        fs::write(source.join(INI_NAME), b"[DlssNr]\nEnabled=auto\n").unwrap();
        fs::write(source.join("OptiScaler/support.txt"), b"support").unwrap();
        Self {
            temp,
            library,
            game,
            source,
        }
    }
    fn options(&self, chosen: &Path) -> InstallOptions {
        InstallOptions {
            game_dir: chosen.to_owned(),
            source_dir: self.source.clone(),
            copy_nvngx: false,
            ..Default::default()
        }
    }
}

#[cfg(unix)]
fn link_dir(target: &Path, link: &Path) {
    std::os::unix::fs::symlink(target, link).unwrap();
}
#[cfg(windows)]
fn link_dir(target: &Path, link: &Path) {
    // Directory junctions exercise reparse handling without requiring symlink creation privileges.
    let output = std::process::Command::new("cmd.exe")
        .args(["/D", "/C", "mklink", "/J"])
        .arg(link)
        .arg(target)
        .output()
        .unwrap();
    assert!(output.status.success(), "mklink /J failed: {:?}", output);
}
#[cfg(unix)]
fn remove_link(link: &Path) {
    fs::remove_file(link).unwrap();
}
#[cfg(windows)]
fn remove_link(link: &Path) {
    fs::remove_dir(link).unwrap();
}

fn refused(result: Result<Plan, Error>) {
    match result {
        Err(Error::Refused(_)) => {}
        Err(error) => panic!("unexpected error instead of safety refusal: {error}"),
        Ok(plan) => {
            assert!(!plan.refusals.is_empty(), "unexpected applicable plan: {plan:?}");
            assert!(apply(&plan, &mut |_| panic!("refused plan made progress")).is_err());
        }
    }
}

fn round_trip(f: &Fixture, chosen: &Path) {
    let plan = plan_install(&f.options(chosen)).unwrap();
    assert!(plan.refusals.is_empty(), "{:?}", plan.refusals);
    assert_eq!(plan.game_dir, f.game.canonicalize().unwrap());
    apply(&plan, &mut |_| {}).unwrap();
    let (ini, bytes) = read_ini(chosen).unwrap().unwrap();
    assert_eq!(ini, f.game.canonicalize().unwrap().join(INI_NAME));
    assert_eq!(bytes, b"[DlssNr]\nEnabled=auto\n");
    let receipt_path = f.game.join(".retoque/receipt.txt");
    let receipt = fs::read(&receipt_path).unwrap();
    for value in ["true", "auto"] {
        let settings = plan_settings(chosen, &[("DlssNr".into(), "Enabled".into(), value.into())]).unwrap();
        assert!(settings.refusals.is_empty());
        apply(&settings, &mut |_| {}).unwrap();
        assert_eq!(fs::read(&receipt_path).unwrap(), receipt);
        assert_eq!(
            status(chosen).unwrap().nr_enabled,
            match value {
                "true" => Some(true),
                _ => None,
            }
        );
    }
    apply(&plan_uninstall(chosen).unwrap(), &mut |_| {}).unwrap();
    assert!(!receipt_path.exists());
    assert!(!f.game.join(INI_NAME).exists());
    assert!(!f.game.join("dxgi.dll").exists());
    assert_eq!(fs::read_dir(f.game.join("OptiScaler")).unwrap().count(), 0);
}

#[test]
fn linked_ancestor_is_accepted_for_install_settings_status_and_uninstall() {
    let f = Fixture::new();
    let alias = f.temp.path().join("library-alias");
    link_dir(&f.library, &alias);
    round_trip(&f, &alias.join("game"));
}

#[test]
fn selected_directory_alias_is_accepted_like_bash_cd_p() {
    let f = Fixture::new();
    let alias = f.temp.path().join("game-alias");
    link_dir(&f.game, &alias);
    round_trip(&f, &alias);
}

#[test]
fn retargeting_selected_alias_after_preview_does_not_redirect_apply() {
    let f = Fixture::new();
    let alias = f.temp.path().join("game-alias");
    link_dir(&f.game, &alias);
    let plan = plan_install(&f.options(&alias)).unwrap();
    assert!(plan.refusals.is_empty());
    let outside = f.temp.path().join("outside");
    fs::create_dir(&outside).unwrap();
    remove_link(&alias);
    link_dir(&outside, &alias);
    apply(&plan, &mut |_| {}).unwrap();
    assert!(f.game.join("dxgi.dll").is_file());
    assert_eq!(fs::read_dir(&outside).unwrap().count(), 0);
}

#[test]
fn state_and_support_links_inside_game_are_still_refused() {
    for relative in [".retoque", "OptiScaler"] {
        let f = Fixture::new();
        let outside = f.temp.path().join("outside");
        fs::create_dir(&outside).unwrap();
        fs::write(outside.join("sentinel"), b"untouched").unwrap();
        link_dir(&outside, &f.game.join(relative));
        refused(plan_install(&f.options(&f.game)));
        assert_eq!(fs::read(outside.join("sentinel")).unwrap(), b"untouched");
        assert_eq!(fs::read_dir(&outside).unwrap().count(), 1);
        assert!(!f.game.join("dxgi.dll").exists());
    }
}

#[test]
fn replacing_resolved_game_root_after_preview_is_refused() {
    let f = Fixture::new();
    let plan = plan_install(&f.options(&f.game)).unwrap();
    let parked = f.temp.path().join("parked");
    fs::rename(&f.game, &parked).unwrap();
    link_dir(&parked, &f.game);
    assert!(apply(&plan, &mut |_| panic!("must reject before writes")).is_err());
    assert_eq!(fs::read_dir(&parked).unwrap().count(), 0);
}

#[cfg(windows)]
#[test]
fn windows_junction_reparse_boundary_is_trusted_only_outside_game() {
    use std::os::windows::fs::MetadataExt;
    let f = Fixture::new();
    let alias = f.temp.path().join("junction");
    link_dir(&f.library, &alias);
    assert_ne!(fs::symlink_metadata(&alias).unwrap().file_attributes() & 0x400, 0);
    round_trip(&f, &alias.join("game"));
    link_dir(&f.source, &f.game.join(".retoque"));
    refused(plan_install(&f.options(&alias.join("game"))));
}

#[test]
fn replacing_resolved_root_between_settings_steps_stops_writes() {
    let f = Fixture::new();
    fs::write(f.game.join(INI_NAME), b"[A]\nk=one\nj=one\n").unwrap();
    let plan = plan_settings(
        &f.game,
        &[
            ("A".into(), "k".into(), "two".into()),
            ("A".into(), "j".into(), "two".into()),
        ],
    )
    .unwrap();
    let parked = f.temp.path().join("parked");
    let mut progress = 0;
    let result = apply(&plan, &mut |_| {
        progress += 1;
        fs::rename(&f.game, &parked).unwrap();
        link_dir(&parked, &f.game);
    });
    assert!(
        result
            .unwrap_err()
            .to_string()
            .contains("resolved game directory is a link")
    );
    assert_eq!(progress, 1);
    assert_eq!(fs::read(parked.join(INI_NAME)).unwrap(), b"[A]\nk=two\nj=one\n");
    assert!(!parked.join(STATE_DIR).exists());
}
