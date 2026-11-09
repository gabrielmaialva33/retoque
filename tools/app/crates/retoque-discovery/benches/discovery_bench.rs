use std::fs;
use std::path::PathBuf;
use std::sync::OnceLock;

use retoque_discovery::{
    Config, discover, find_dlss_anywhere, find_dlss_anywhere_with_targets, parse_appmanifest_acf,
    parse_library_folders_vdf, targets,
};

#[global_allocator]
static ALLOC: divan::AllocProfiler = divan::AllocProfiler::system();

fn main() {
    divan::main();
}

struct SyntheticEnv {
    home: PathBuf,
    ue_game_dir: PathBuf,
}

impl Drop for SyntheticEnv {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.home);
    }
}

fn synthetic_env() -> &'static SyntheticEnv {
    static ENV: OnceLock<SyntheticEnv> = OnceLock::new();
    ENV.get_or_init(|| {
        let home = std::env::temp_dir().join(format!("retoque-disc-bench-{}", std::process::id()));
        let _ = fs::remove_dir_all(&home);
        let steamapps = home.join(".local/share/Steam/steamapps");
        let common = steamapps.join("common");
        fs::create_dir_all(&common).unwrap();

        let vdf = format!(
            "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n",
            home.join(".local/share/Steam").display()
        );
        fs::write(steamapps.join("libraryfolders.vdf"), vdf).unwrap();

        let mut ue_game_dir = PathBuf::new();

        // 10 registered Unreal Engine games with deep trees and hundreds of files each.
        for g in 0..10 {
            let app_id = 10_000 + g;
            let name = format!("SyntheticUnrealGame_{g:02}");
            let acf = format!(
                "\"AppState\"\n{{\n\t\"appid\"\t\t\"{app_id}\"\n\t\"name\"\t\t\"{name}\"\n\t\"installdir\"\t\t\"{name}\"\n}}\n"
            );
            fs::write(steamapps.join(format!("appmanifest_{app_id}.acf")), acf).unwrap();

            let game_dir = common.join(&name);
            let bin_dir = game_dir.join("Proj/Binaries/Win64");
            let ue_dlss = game_dir.join("Proj/Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64");
            let ue_sl = game_dir.join("Proj/Engine/Plugins/Runtime/Nvidia/StreamlineCore/Binaries/ThirdParty/Win64");
            let paks_dir = game_dir.join("Proj/Content/Paks");
            let engine_bin = game_dir.join("Engine/Binaries/Win64");

            fs::create_dir_all(&bin_dir).unwrap();
            fs::create_dir_all(&ue_dlss).unwrap();
            fs::create_dir_all(&ue_sl).unwrap();
            fs::create_dir_all(&paks_dir).unwrap();
            fs::create_dir_all(&engine_bin).unwrap();

            fs::write(game_dir.join(format!("{name}.exe")), vec![0u8; 32 * 1024]).unwrap();
            fs::write(bin_dir.join(format!("{name}-Win64-Shipping.exe")), vec![0u8; 64 * 1024]).unwrap();
            fs::write(bin_dir.join("CrashReportClient.exe"), b"MZ").unwrap();
            fs::write(engine_bin.join("UnrealCEFSubProcess.exe"), b"MZ").unwrap();
            fs::write(ue_dlss.join("nvngx_dlss.dll"), b"dlss").unwrap();
            fs::write(ue_sl.join("nvngx_dlssg.dll"), b"dlssg").unwrap();
            fs::write(ue_sl.join("sl.interposer.dll"), b"sl").unwrap();

            for f in 0..120 {
                fs::write(paks_dir.join(format!("pakchunk_{f:03}.pak")), b"pak").unwrap();
                fs::write(bin_dir.join(format!("helper_lib_{f:03}.dat")), b"dat").unwrap();
            }

            if g == 0 {
                ue_game_dir = game_dir;
            }
        }

        // 30 orphan folders with asset subtrees (3,600+ files total across the library).
        for i in 0..30 {
            let orphan = common.join(format!("OrphanLeftover_{i:02}"));
            let paks = orphan.join("GameProj/Content/Paks");
            let data = orphan.join(format!("OrphanLeftover_{i:02}_Data/Managed"));
            fs::create_dir_all(&paks).unwrap();
            fs::create_dir_all(&data).unwrap();
            for j in 0..40 {
                fs::write(paks.join(format!("asset_{j:03}.uasset")), b"uasset").unwrap();
                fs::write(data.join(format!("Assembly_{j:03}.dll")), b"dll").unwrap();
            }
        }

        SyntheticEnv { home, ue_game_dir }
    })
}

#[divan::bench(sample_count = 20)]
fn discover_synthetic_steam_library(bencher: divan::Bencher) {
    let env = synthetic_env();
    let cfg = Config {
        home: Some(env.home.clone()),
        ..Config::default()
    };
    bencher.bench_local(|| {
        let (games, skipped) = discover(divan::black_box(&cfg));
        assert_eq!(games.len(), 10);
        assert!(skipped.is_empty());
        divan::black_box(games)
    });
}

#[divan::bench]
fn find_targets_unreal_tree(bencher: divan::Bencher) {
    let env = synthetic_env();
    bencher.bench_local(|| {
        let t = targets(divan::black_box(&env.ue_game_dir));
        assert!(!t.is_empty());
        divan::black_box(t)
    });
}

#[divan::bench]
fn find_dlss_anywhere_unreal_tree(bencher: divan::Bencher) {
    let env = synthetic_env();
    let t = targets(&env.ue_game_dir);
    bencher.bench_local(|| {
        let a1 = find_dlss_anywhere_with_targets(divan::black_box(&env.ue_game_dir), divan::black_box(&t));
        let a2 = find_dlss_anywhere(divan::black_box(&env.ue_game_dir));
        divan::black_box((a1, a2))
    });
}

#[divan::bench]
fn steam_vdf_and_acf_parse(bencher: divan::Bencher) {
    let lf = "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"/opt/steam\"\n\t}\n\t\"1\"\n\t{\n\t\t\"path\"\t\t\"/srv/steam\"\n\t}\n}\n";
    let acf = "\"AppState\"\n{\n\t\"appid\"\t\t\"1001\"\n\t\"name\"\t\t\"Photon Blade\"\n\t\"installdir\"\t\t\"PhotonBlade\"\n\t\"UserConfig\"\n\t{\n\t\t\"platform_override_source\"\t\t\"windows\"\n\t}\n}\n";
    bencher.bench_local(|| {
        let a = parse_library_folders_vdf(divan::black_box(lf)).unwrap();
        let b = parse_appmanifest_acf(divan::black_box(acf)).unwrap();
        divan::black_box((a, b))
    });
}
