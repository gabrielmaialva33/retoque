//! Synthetic v1 receipt producer used by the offline cross-script parity suite.
use std::{fs, path::PathBuf};
fn main() {
    let root = PathBuf::from(std::env::args_os().nth(1).expect("synthetic scratch directory"));
    assert!(
        !root.join("game").exists(),
        "fixture requires an empty scratch directory"
    );
    let game = root.join("game");
    let target = game.join("Project/Binaries/Win64");
    let source = root.join("catalog");
    let file = "Engine/Plugins/Nvidia/DLSS/Binaries/ThirdParty/Win64/nvngx_dlss.dll";
    fs::create_dir_all(&target).unwrap();
    fs::create_dir_all(&source).unwrap();
    fs::create_dir_all(game.join(file).parent().unwrap()).unwrap();
    fs::write(game.join(file), b"synthetic original DLSS").unwrap();
    fs::write(source.join("nvngx_dlss.dll"), b"synthetic updated DLSS").unwrap();
    fs::write(source.join("receipt-marker.txt"), b"Retoque DLSS receipt v1\n").unwrap();
    let plan = retoque_core::plan_dlss_update(&game, &target, &source, &[file.into()]).unwrap();
    retoque_core::apply(&plan.into_verified().unwrap(), &mut |_| {}).unwrap();
}
