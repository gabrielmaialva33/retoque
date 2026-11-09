use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use retoque_discovery::{
    Config, Runtime, Store, discover, discover_epic_from_dir, is_ignored_exe, is_steam_tool_or_runtime,
    parse_appmanifest_acf, parse_compat_tool_mapping_vdf, parse_epic_manifest_item, parse_heroic_game_config,
    parse_heroic_gog_installed, parse_heroic_legendary_installed, parse_heroic_nile_installed,
    parse_library_folders_vdf, parse_lutris_db, parse_lutris_yaml, targets,
};
use rusqlite::Connection;

struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new(label: &str) -> Self {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let id = COUNTER.fetch_add(1, Ordering::Relaxed);
        let pid = std::process::id();
        let base = std::env::temp_dir().join(format!("retoque-discovery-test-{pid}-{id}-{label}"));
        let _ = fs::remove_dir_all(&base);
        fs::create_dir_all(&base).expect("create temp test dir");
        Self { path: base }
    }

    fn path(&self) -> &Path {
        &self.path
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn fixture_str(name: &str) -> String {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests")
        .join("fixtures")
        .join(name);
    fs::read_to_string(&path).unwrap_or_else(|e| panic!("failed to read fixture {}: {e}", path.display()))
}

fn build_synthetic_lutris_sqlite(rows: &[(i64, &str, &str, &str, &str, i64, &str)]) -> Vec<u8> {
    let conn = Connection::open_in_memory().expect("open sqlite in memory");
    conn.execute_batch(
        "CREATE TABLE games (
            id INTEGER PRIMARY KEY,
            name TEXT,
            slug TEXT,
            runner TEXT,
            executable TEXT,
            directory TEXT,
            installed INTEGER,
            configpath TEXT
        );",
    )
    .expect("create games table");

    for &(id, name, slug, runner, dir, installed, configpath) in rows {
        conn.execute(
            "INSERT INTO games (id, name, slug, runner, directory, installed, configpath)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)",
            rusqlite::params![id, name, slug, runner, dir, installed, configpath],
        )
        .expect("insert lutris row");
    }

    conn.serialize(rusqlite::MAIN_DB).expect("serialize sqlite db").to_vec()
}

#[test]
fn parse_vdf_two_libraries_fixture() {
    let vdf = fixture_str("libraryfolders.vdf");
    let libs = parse_library_folders_vdf(&vdf).expect("parse libraryfolders.vdf");
    assert_eq!(
        libs,
        vec![
            PathBuf::from("/opt/steam-main"),
            PathBuf::from("/mnt/fast/SteamLibrary"),
        ]
    );
}

#[test]
fn parse_acf_fixtures_and_filter_tools() {
    let acf_1001 = parse_appmanifest_acf(&fixture_str("appmanifest_1001.acf")).expect("1001");
    assert_eq!(acf_1001.app_id, 1001);
    assert_eq!(acf_1001.name, "Photon Blade");
    assert_eq!(acf_1001.install_dir, "PhotonBlade");
    assert!(!is_steam_tool_or_runtime(
        acf_1001.app_id,
        &acf_1001.name,
        &acf_1001.install_dir
    ));

    let acf_1002 = parse_appmanifest_acf(&fixture_str("appmanifest_1002.acf")).expect("1002");
    assert_eq!(acf_1002.app_id, 1002);
    assert_eq!(acf_1002.name, "Tux Racer Extreme");
    assert!(!is_steam_tool_or_runtime(
        acf_1002.app_id,
        &acf_1002.name,
        &acf_1002.install_dir
    ));

    let acf_proton = parse_appmanifest_acf(&fixture_str("appmanifest_1493710.acf")).expect("proton");
    assert!(is_steam_tool_or_runtime(
        acf_proton.app_id,
        &acf_proton.name,
        &acf_proton.install_dir
    ));

    let acf_redist = parse_appmanifest_acf(&fixture_str("appmanifest_228980.acf")).expect("redist");
    assert!(is_steam_tool_or_runtime(
        acf_redist.app_id,
        &acf_redist.name,
        &acf_redist.install_dir
    ));
    assert!(is_steam_tool_or_runtime(
        1628350,
        "Steam Linux Runtime 3.0 (sniper)",
        "SteamLinuxRuntime_sniper"
    ));
}

#[test]
fn parse_steam_compat_tool_mapping_fixture() {
    let mapping = parse_compat_tool_mapping_vdf(&fixture_str("config.vdf")).expect("parse config.vdf");
    assert_eq!(mapping.get(&1001).map(String::as_str), Some("GE-Proton10-1"));
    assert_eq!(mapping.get(&1002).map(String::as_str), Some("SteamLinuxRuntime_sniper"));
}

#[test]
fn parse_heroic_fixtures() {
    let leg =
        parse_heroic_legendary_installed(&fixture_str("heroic_legendary_installed.json")).expect("parse legendary");
    assert_eq!(leg.len(), 2, "DLC entry should be skipped");
    assert_eq!(leg[0].app_name, "CypressApp");
    assert_eq!(leg[0].title, "Cypress Odyssey");
    assert_eq!(leg[0].install_path, PathBuf::from("/opt/games/heroic/CypressOdyssey"));
    assert_eq!(leg[1].app_name, "WinBackslashApp");
    assert_eq!(leg[1].executable, Some(PathBuf::from(r"bin\x64\Runner.exe")));

    let gog = parse_heroic_gog_installed(&fixture_str("heroic_gog_installed.json")).expect("parse heroic gog");
    assert_eq!(gog.len(), 1);
    assert_eq!(gog[0].app_name, "19990001");
    assert_eq!(gog[0].title, "Old Kingdom");

    let nile = parse_heroic_nile_installed(&fixture_str("heroic_nile_installed.json")).expect("parse heroic nile");
    assert_eq!(nile.len(), 1);
    assert_eq!(nile[0].app_name, "amzn1.adg.product.synthetic-01");
    assert_eq!(nile[0].title, "River Drift");
    assert_eq!(nile[0].install_path, PathBuf::from("/opt/games/nile/RiverDrift"));

    let pfx = parse_heroic_game_config(&fixture_str("heroic_game_config.json"), "CypressApp")
        .expect("parse heroic game config");
    assert_eq!(pfx, Some(PathBuf::from("/opt/prefixes/heroic/CypressOdyssey")));
}

#[test]
fn parse_lutris_sqlite_and_yaml_fixtures() {
    let sqlite_bytes = build_synthetic_lutris_sqlite(&[
        (
            1,
            "Star Runner",
            "star-runner",
            "wine",
            "/opt/games/lutris/StarRunner",
            1,
            "star-runner-1700000000",
        ),
        (
            2,
            "Uninstalled Relic",
            "uninstalled-relic",
            "wine",
            "/opt/games/lutris/Relic",
            0,
            "uninstalled-relic-1700000000",
        ),
    ]);

    let games = parse_lutris_db(&sqlite_bytes).expect("parse synthetic lutris db");
    assert_eq!(games.len(), 1);
    assert_eq!(games[0].name, "Star Runner");
    assert_eq!(games[0].slug, "star-runner");
    assert_eq!(games[0].configpath.as_deref(), Some("star-runner-1700000000"));

    let yaml = parse_lutris_yaml(&fixture_str("lutris_game.yml"));
    assert_eq!(
        yaml.exe,
        Some(PathBuf::from("/opt/games/lutris/StarRunner/bin/x64/StarRunner.exe"))
    );
    assert_eq!(yaml.prefix, Some(PathBuf::from("/opt/prefixes/lutris/star-runner")));
    assert_eq!(yaml.working_dir, Some(PathBuf::from("/opt/games/lutris/StarRunner")));
}

#[test]
fn parse_epic_manifest_fixture() {
    let item = parse_epic_manifest_item(&fixture_str("epic_manifest.item")).expect("epic item");
    assert_eq!(item.app_name, "NebulaNine");
    assert_eq!(item.display_name, "Nebula Nine");
    assert_eq!(item.install_location, PathBuf::from(r"C:\Games\Epic\NebulaNine"));
    assert!(!item.is_incomplete);
    assert!(!item.is_dlc);
}

#[test]
fn targets_ranks_unreal_shipping_and_filters_non_game_exes() {
    let tmp = TempDir::new("targets");
    let game_dir = tmp.path().join("PhotonBlade");

    // Root bootstrapper stub (small).
    fs::create_dir_all(&game_dir).unwrap();
    fs::write(game_dir.join("PhotonBlade.exe"), vec![0u8; 64 * 1024]).unwrap();
    fs::write(game_dir.join("UnityCrashHandler64.exe"), b"stub").unwrap();
    fs::write(game_dir.join("unins000.exe"), b"stub").unwrap();

    // Unreal Engine helpers and redistributables that must be ignored.
    let engine_bin = game_dir.join("Engine/Binaries/Win64");
    fs::create_dir_all(&engine_bin).unwrap();
    fs::write(engine_bin.join("CrashReportClient.exe"), b"stub").unwrap();
    fs::write(engine_bin.join("EpicWebHelper.exe"), b"stub").unwrap();

    let redist_dir = game_dir.join("Engine/Extras/Redist/en-us");
    fs::create_dir_all(&redist_dir).unwrap();
    fs::write(redist_dir.join("UEPrereqSetup_x64.exe"), b"stub").unwrap();
    fs::write(redist_dir.join("vc_redist.x64.exe"), b"stub").unwrap();

    // Real shipping directory with DLSS, receipt, and case-insensitive proxy DLL.
    let shipping_dir = game_dir.join("Photon/Binaries/Win64");
    fs::create_dir_all(shipping_dir.join(".retoque")).unwrap();
    fs::write(shipping_dir.join(".retoque/receipt.txt"), b"version\t1\n").unwrap();
    fs::write(
        shipping_dir.join("PhotonBlade-Win64-Shipping.exe"),
        vec![0u8; 2 * 1024 * 1024],
    )
    .unwrap();
    fs::write(shipping_dir.join("crs-handler.exe"), b"stub").unwrap();
    fs::write(shipping_dir.join("NVNGX_DLSS.DLL"), b"dlss").unwrap();
    fs::write(shipping_dir.join("sl.interposer.dll"), b"sl").unwrap();
    fs::write(shipping_dir.join("DXGI.DLL"), b"proxy").unwrap();
    fs::write(shipping_dir.join("WinMM.dll"), b"proxy2").unwrap();

    let found = targets(&game_dir);
    assert_eq!(found.len(), 2, "expected shipping dir and root dir only");

    let top = &found[0];
    assert_eq!(top.path, shipping_dir);
    assert_eq!(top.executables, vec!["PhotonBlade-Win64-Shipping.exe"]);
    assert_eq!(top.dlss, vec!["nvngx_dlss.dll", "sl.interposer.dll"]);
    assert!(top.has_receipt);
    assert_eq!(top.proxies_present, vec!["dxgi.dll", "winmm.dll"]);
    assert!(top.score > found[1].score);

    assert!(is_ignored_exe("UnityCrashHandler64.exe", Path::new("")));
    assert!(is_ignored_exe(
        "easyanticheat_eos_setup.exe",
        Path::new("Game/EasyAntiCheat")
    ));
    assert!(is_ignored_exe("BEService_x64.exe", Path::new("BattlEye")));
}

#[test]
fn discover_end_to_end_across_stores_in_temp_home() {
    let tmp = TempDir::new("e2e");
    let home = tmp.path();

    // 1. Steam setup: two libraries, plus symlink ~/.steam/root -> ~/.local/share/Steam
    let steam_main = home.join(".local/share/Steam");
    let steam_lib2 = tmp.path().join("second-steam-lib");
    fs::create_dir_all(steam_main.join("steamapps/common")).unwrap();
    fs::create_dir_all(steam_lib2.join("steamapps/common")).unwrap();

    #[cfg(unix)]
    {
        fs::create_dir_all(home.join(".steam")).unwrap();
        std::os::unix::fs::symlink(&steam_main, home.join(".steam/root")).unwrap();
    }

    let vdf_content = format!(
        "\"libraryfolders\"\n{{\n\t\"0\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n\t\"1\"\n\t{{\n\t\t\"path\"\t\t\"{}\"\n\t}}\n}}\n",
        steam_main.display(),
        steam_lib2.display()
    );
    fs::write(steam_main.join("steamapps/libraryfolders.vdf"), vdf_content).unwrap();
    fs::write(
        steam_main.join("steamapps/appmanifest_1001.acf"),
        fixture_str("appmanifest_1001.acf"),
    )
    .unwrap();
    fs::write(
        steam_main.join("steamapps/appmanifest_1493710.acf"),
        fixture_str("appmanifest_1493710.acf"),
    )
    .unwrap();
    fs::write(
        steam_lib2.join("steamapps/appmanifest_1002.acf"),
        fixture_str("appmanifest_1002.acf"),
    )
    .unwrap();

    let photon_bin = steam_main.join("steamapps/common/PhotonBlade/Photon/Binaries/Win64");
    fs::create_dir_all(&photon_bin).unwrap();
    fs::write(photon_bin.join("PhotonBlade-Win64-Shipping.exe"), b"MZ-fake").unwrap();

    let photon_pfx = steam_main.join("steamapps/compatdata/1001/pfx");
    fs::create_dir_all(&photon_pfx).unwrap();

    // Native Linux ELF game in library 2 (ELF magic \x7fELF, no .exe).
    let tux_dir = steam_lib2.join("steamapps/common/TuxRacer");
    fs::create_dir_all(&tux_dir).unwrap();
    fs::write(tux_dir.join("tuxracer.x86_64"), b"\x7fELF\x02\x01\x01\x00").unwrap();

    // 2. Heroic setup (Legendary + GamesConfig)
    let heroic_root = home.join(".config/heroic");
    fs::create_dir_all(heroic_root.join("legendaryConfig/legendary")).unwrap();
    fs::create_dir_all(heroic_root.join("GamesConfig")).unwrap();
    let cypress_dir = tmp.path().join("heroic-games/CypressOdyssey");
    let cypress_bin = cypress_dir.join("Cypress/Binaries/Win64");
    fs::create_dir_all(&cypress_bin).unwrap();
    fs::write(cypress_bin.join("Cypress-Win64-Shipping.exe"), b"MZ-fake").unwrap();

    let cypress_pfx = tmp.path().join("prefixes/Cypress");
    let leg_json = serde_json::json!({
        "CypressApp": {
            "app_name": "CypressApp",
            "title": "Cypress Odyssey",
            "install_path": cypress_dir,
            "executable": "Cypress/Binaries/Win64/Cypress-Win64-Shipping.exe",
            "platform": "Windows",
            "is_dlc": false
        }
    });
    fs::write(
        heroic_root.join("legendaryConfig/legendary/installed.json"),
        leg_json.to_string(),
    )
    .unwrap();
    let cfg_json = serde_json::json!({
        "CypressApp": {
            "winePrefix": cypress_pfx
        }
    });
    fs::write(heroic_root.join("GamesConfig/CypressApp.json"), cfg_json.to_string()).unwrap();

    // 3. Lutris setup (pga.db + old ~/.config/lutris/games and new ~/.local/share/lutris/games)
    let lutris_data = home.join(".local/share/lutris");
    let lutris_new_games = lutris_data.join("games");
    let lutris_old_games = home.join(".config/lutris/games");
    fs::create_dir_all(&lutris_new_games).unwrap();
    fs::create_dir_all(&lutris_old_games).unwrap();

    let star_dir = tmp.path().join("lutris-games/StarRunner");
    let star_bin = star_dir.join("bin/x64");
    fs::create_dir_all(&star_bin).unwrap();
    fs::write(star_bin.join("StarRunner.exe"), b"MZ-fake").unwrap();
    let star_pfx = tmp.path().join("prefixes/star-runner");

    let old_yaml_game_dir = tmp.path().join("lutris-games/RetroQuest");
    fs::create_dir_all(&old_yaml_game_dir).unwrap();
    fs::write(old_yaml_game_dir.join("RetroQuest.exe"), b"MZ-fake").unwrap();
    let retro_pfx = tmp.path().join("prefixes/retro-quest");

    let db_bytes = build_synthetic_lutris_sqlite(&[
        (
            1,
            "Star Runner",
            "star-runner",
            "wine",
            &star_dir.to_string_lossy(),
            1,
            "star-runner-new",
        ),
        (
            2,
            "Retro Quest",
            "retro-quest",
            "wine",
            &old_yaml_game_dir.to_string_lossy(),
            1,
            "retro-quest-old",
        ),
    ]);
    fs::write(lutris_data.join("pga.db"), db_bytes).unwrap();
    fs::write(
        lutris_new_games.join("star-runner-new.yml"),
        format!(
            "game:\n  exe: '{}'\n  prefix: '{}'\n",
            star_bin.join("StarRunner.exe").display(),
            star_pfx.display()
        ),
    )
    .unwrap();
    fs::write(
        lutris_old_games.join("retro-quest-old.yml"),
        format!(
            "game:\n  exe: '{}'\n  prefix: '{}'\n",
            old_yaml_game_dir.join("RetroQuest.exe").display(),
            retro_pfx.display()
        ),
    )
    .unwrap();

    // 4. Manual game folder
    let manual_dir = tmp.path().join("manual-games/CustomModGame");
    fs::create_dir_all(&manual_dir).unwrap();
    fs::write(manual_dir.join("CustomModGame.exe"), b"MZ-fake").unwrap();

    let config = Config {
        manual: vec![manual_dir.clone()],
        home: Some(home.to_path_buf()),
        ..Config::default()
    };

    let (games, skipped) = discover(&config);
    assert!(skipped.is_empty(), "expected no skipped stores, got {skipped:?}");

    let names: Vec<&str> = games.iter().map(|g| g.name.as_str()).collect();
    assert_eq!(
        names,
        vec![
            "CustomModGame",
            "Cypress Odyssey",
            "Photon Blade",
            "Retro Quest",
            "Star Runner",
            "Tux Racer Extreme",
        ]
    );

    let photon = games.iter().find(|g| g.id == "steam:1001").unwrap();
    assert_eq!(photon.store, Store::Steam);
    assert_eq!(
        photon.runtime,
        Runtime::Proton {
            app_id: 1001,
            prefix: Some(photon_pfx),
        }
    );
    assert_eq!(photon.targets[0].path, photon_bin);

    let tux = games.iter().find(|g| g.id == "steam:1002").unwrap();
    assert_eq!(tux.runtime, Runtime::NativeLinux);
    assert!(tux.targets.is_empty());

    let cypress = games.iter().find(|g| g.id == "heroic:legendary:CypressApp").unwrap();
    assert_eq!(cypress.store, Store::Heroic);
    assert_eq!(
        cypress.runtime,
        Runtime::Wine {
            prefix: Some(cypress_pfx),
        }
    );
    assert_eq!(cypress.targets[0].path, cypress_bin);

    let star = games.iter().find(|g| g.id == "lutris:star-runner").unwrap();
    assert_eq!(star.store, Store::Lutris);
    assert_eq!(star.runtime, Runtime::Wine { prefix: Some(star_pfx) });
    assert_eq!(star.targets[0].path, star_bin);

    let retro = games.iter().find(|g| g.id == "lutris:retro-quest").unwrap();
    assert_eq!(
        retro.runtime,
        Runtime::Wine {
            prefix: Some(retro_pfx),
        }
    );

    // 5. Also verify Epic directory discovery on synthetic manifests
    let epic_dir = tmp.path().join("epic-manifests");
    fs::create_dir_all(&epic_dir).unwrap();
    let nebula_dir = tmp.path().join("epic-games/NebulaNine");
    let nebula_bin = nebula_dir.join("Nebula/Binaries/Win64");
    fs::create_dir_all(&nebula_bin).unwrap();
    fs::write(nebula_bin.join("NebulaNine-Win64-Shipping.exe"), b"MZ-fake").unwrap();
    let epic_item = serde_json::json!({
        "FormatVersion": 0,
        "bIsIncompleteInstall": false,
        "AppName": "NebulaNine",
        "DisplayName": "Nebula Nine",
        "InstallLocation": nebula_dir,
        "LaunchExecutable": "Nebula/Binaries/Win64/NebulaNine-Win64-Shipping.exe",
        "MainGameAppName": "NebulaNine"
    });
    fs::write(epic_dir.join("nebula.item"), epic_item.to_string()).unwrap();

    let (epic_games, epic_skipped) = discover_epic_from_dir(&epic_dir);
    assert!(epic_skipped.is_empty());
    assert_eq!(epic_games.len(), 1);
    assert_eq!(epic_games[0].id, "epic:NebulaNine");
    assert_eq!(epic_games[0].store, Store::Epic);
    assert_eq!(epic_games[0].runtime, Runtime::Windows);
    assert_eq!(epic_games[0].targets[0].path, nebula_bin);
}

#[test]
fn finding3_lutris_wal_checkpointed_pending_and_concurrent_writer() {
    let tmp = TempDir::new("wal");
    let home = tmp.path();
    let lutris_dir = home.join(".local/share/lutris");
    fs::create_dir_all(&lutris_dir).unwrap();

    let game1_dir = tmp.path().join("wal-game-1");
    let game2_dir = tmp.path().join("wal-game-2");
    fs::create_dir_all(&game1_dir).unwrap();
    fs::create_dir_all(&game2_dir).unwrap();
    fs::write(game1_dir.join("game1.exe"), b"MZ").unwrap();
    fs::write(game2_dir.join("game2.exe"), b"MZ").unwrap();

    let db_path = lutris_dir.join("pga.db");
    {
        let conn = Connection::open(&db_path).unwrap();
        conn.execute_batch(&format!(
            "PRAGMA journal_mode=WAL;
             CREATE TABLE games (
                 id INTEGER PRIMARY KEY,
                 name TEXT,
                 slug TEXT,
                 runner TEXT,
                 directory TEXT,
                 installed INTEGER,
                 configpath TEXT
             );
             INSERT INTO games VALUES (1, 'WAL Checkpointed', 'wal-ckpt', 'wine', '{}', 1, '');
             PRAGMA wal_checkpoint(TRUNCATE);",
            game1_dir.display()
        ))
        .unwrap();
    }

    let snapshot_dir = |dir: &Path| -> std::collections::BTreeMap<String, Vec<u8>> {
        let mut map = std::collections::BTreeMap::new();
        for entry in fs::read_dir(dir).unwrap().flatten() {
            let name = entry.file_name().to_string_lossy().into_owned();
            let bytes = fs::read(entry.path()).unwrap_or_default();
            map.insert(name, bytes);
        }
        map
    };

    // Case A: WAL-mode pga.db with no -wal/-shm on disk (after clean close).
    let before_closed = snapshot_dir(&lutris_dir);
    assert!(
        !lutris_dir.join("pga.db-wal").exists() && !lutris_dir.join("pga.db-shm").exists(),
        "cleanly closed WAL DB should have no -wal or -shm before discovery"
    );
    let parsed_ckpt = parse_lutris_db(&before_closed["pga.db"]).expect("parse_lutris_db on checkpointed WAL file");
    assert_eq!(parsed_ckpt.len(), 1);
    assert_eq!(parsed_ckpt[0].slug, "wal-ckpt");

    let (games_closed, skipped_closed) = discover(&Config {
        home: Some(home.to_path_buf()),
        ..Config::default()
    });
    assert!(skipped_closed.is_empty(), "unexpected skipped: {skipped_closed:?}");
    assert_eq!(games_closed.len(), 1);
    assert_eq!(games_closed[0].id, "lutris:wal-ckpt");
    let after_closed = snapshot_dir(&lutris_dir);
    assert_eq!(
        before_closed.keys().collect::<Vec<_>>(),
        after_closed.keys().collect::<Vec<_>>(),
        "discovery must not create pga.db-wal or pga.db-shm in Lutris folder"
    );
    assert_eq!(before_closed, after_closed, "Lutris folder bytes must not change");

    // Case B: Open writer connection with uncheckpointed frames in pga.db-wal (autocheckpoint off).
    let writer_conn = Connection::open(&db_path).unwrap();
    writer_conn.execute_batch("PRAGMA wal_autocheckpoint = 0;").unwrap();
    writer_conn
        .execute(
            "INSERT INTO games VALUES (2, 'WAL Pending', 'wal-pending', 'wine', ?1, 1, '')",
            rusqlite::params![game2_dir.to_string_lossy()],
        )
        .unwrap();
    assert!(
        lutris_dir.join("pga.db-wal").exists(),
        "expected pending pga.db-wal while writer_conn is open"
    );

    let before_pending = snapshot_dir(&lutris_dir);
    let (games, skipped) = discover(&Config {
        home: Some(home.to_path_buf()),
        ..Config::default()
    });
    let after_pending = snapshot_dir(&lutris_dir);
    assert!(skipped.is_empty(), "unexpected skipped: {skipped:?}");
    let slugs: Vec<&str> = games.iter().map(|g| g.id.as_str()).collect();
    assert_eq!(slugs, vec!["lutris:wal-ckpt", "lutris:wal-pending"]);
    assert_eq!(
        before_pending, after_pending,
        "Lutris folder listing and file bytes must remain unchanged after reading pending WAL"
    );
}

#[test]
fn finding3_lutris_wal_race_does_not_return_mixed_snapshot() {
    let tmp = TempDir::new("wal-race");
    let db_path = tmp.path().join("pga.db");
    let conn = Connection::open(&db_path).unwrap();
    conn.execute_batch(
        "PRAGMA page_size=512;
         PRAGMA journal_mode=WAL;
         PRAGMA wal_autocheckpoint=0;
         CREATE TABLE games (
             id INTEGER PRIMARY KEY,
             name TEXT,
             slug TEXT,
             installed INTEGER
         );",
    )
    .unwrap();
    {
        let tx = conn.unchecked_transaction().unwrap();
        let slug = "x".repeat(100);
        for i in 1..=2000 {
            tx.execute(
                "INSERT INTO games VALUES (?1, 'old', ?2, 1)",
                rusqlite::params![i, slug],
            )
            .unwrap();
        }
        tx.commit().unwrap();
    }
    conn.execute_batch(
        "PRAGMA wal_checkpoint(TRUNCATE);
         UPDATE games SET name='old' WHERE id=2000;",
    )
    .unwrap();

    // 1. Interleave a full update + checkpoint + new WAL generation once right after the first pga.db read.
    let mut fired = false;
    let rows = retoque_discovery::lutris::read_lutris_db_file_with_hook(&db_path, || {
        if !fired {
            fired = true;
            conn.execute_batch(
                "UPDATE games SET name='new';
                 PRAGMA wal_checkpoint(TRUNCATE);
                 UPDATE games SET name='last' WHERE id=2000;",
            )
            .unwrap();
        }
    })
    .expect("retry should recover a consistent post-checkpoint snapshot");

    assert_eq!(rows.len(), 2000);
    let first = &rows.first().unwrap().name;
    let last = &rows.last().unwrap().name;
    assert_ne!(
        (first.as_str(), last.as_str()),
        ("old", "last"),
        "must never return mixed generation snapshot (first=old, last=last)"
    );
    assert_eq!((first.as_str(), last.as_str()), ("new", "last"));

    // 2. If a writer mutates across every retry so no consistent pair can be read, return Err.
    let mut counter = 0i64;
    let res = retoque_discovery::lutris::read_lutris_db_file_with_hook(&db_path, || {
        counter += 1;
        conn.execute(
            "UPDATE games SET name=?1 WHERE id=1",
            rusqlite::params![format!("mut-{counter}")],
        )
        .unwrap();
        conn.execute_batch("PRAGMA wal_checkpoint(TRUNCATE);").unwrap();
        conn.execute(
            "UPDATE games SET name=?1 WHERE id=2000",
            rusqlite::params![format!("wal-{counter}")],
        )
        .unwrap();
    });
    assert!(
        res.is_err(),
        "persistent concurrent mutation across all retries must fail instead of returning mixed data"
    );
}

#[test]
fn finding4_lutris_yaml_unicode_escapes_flow_mapping_and_block_scalars() {
    let unicode_yaml = "game:\n  exe: \"/games/Caf\\xE9/game.exe\"\n  prefix: \"/games/Jos\\u00E9/pfx\"\n";
    let parsed_u = parse_lutris_yaml(unicode_yaml);
    assert_eq!(parsed_u.exe, Some(PathBuf::from("/games/Café/game.exe")));
    assert_eq!(parsed_u.prefix, Some(PathBuf::from("/games/José/pfx")));

    let flow_yaml = "game: {exe: /games/test/game.exe, prefix: /games/test/pfx, working_dir: /games/test}\n";
    let parsed_f = parse_lutris_yaml(flow_yaml);
    assert_eq!(parsed_f.exe, Some(PathBuf::from("/games/test/game.exe")));
    assert_eq!(parsed_f.prefix, Some(PathBuf::from("/games/test/pfx")));
    assert_eq!(parsed_f.working_dir, Some(PathBuf::from("/games/test")));

    let folded_yaml = "game:\n  exe: >-\n    /games/test/game.exe\n  prefix: |-\n    /games/test/pfx\n";
    let parsed_b = parse_lutris_yaml(folded_yaml);
    assert_eq!(parsed_b.exe, Some(PathBuf::from("/games/test/game.exe")));
    assert_eq!(parsed_b.prefix, Some(PathBuf::from("/games/test/pfx")));

    assert!(retoque_discovery::try_parse_lutris_yaml("game: [unclosed").is_err());
}

#[test]
fn finding5_windows_separator_hints_on_linux_and_heroic() {
    let tmp = TempDir::new("hint-sep");
    let hintgame = tmp.path().join("hintgame");
    fs::create_dir_all(hintgame.join("bin")).unwrap();
    fs::create_dir_all(hintgame.join("alt")).unwrap();
    fs::write(hintgame.join("bin/game.exe"), b"MZ").unwrap();
    fs::write(hintgame.join("alt/other.exe"), vec![0u8; 20 * 1024 * 1024]).unwrap();
    fs::write(hintgame.join("alt/nvngx_dlss.dll"), b"MZ").unwrap();

    for hint in ["bin/game.exe", r"bin\game.exe"] {
        let ts = retoque_discovery::targets::find_targets_with_hint(&hintgame, Some(Path::new(hint)));
        assert_eq!(
            ts[0].path,
            hintgame.join("bin"),
            "hint {hint} should rank bin first over alt with DLSS"
        );
    }
}

#[test]
fn finding6_fifo_and_deep_vdf_do_not_hang_or_crash() {
    let deep_vdf = "node { ".repeat(20_000) + &"}".repeat(20_000);
    // Run in a thread with a 2 MiB stack to prove parse_vdf does not recurse deeply on the stack.
    let res = std::thread::Builder::new()
        .stack_size(2 * 1024 * 1024)
        .spawn(move || retoque_discovery::parse_vdf(&deep_vdf))
        .unwrap()
        .join()
        .expect("parse_vdf must not overflow stack");
    assert!(res.is_err(), "deeply nested VDF must return Err");

    #[cfg(unix)]
    {
        let tmp = TempDir::new("fifo");
        let apps = tmp.path().join(".local/share/Steam/steamapps");
        fs::create_dir_all(&apps).unwrap();
        let fifo_path = apps.join("appmanifest_42.acf");
        let status = std::process::Command::new("mkfifo")
            .arg(&fifo_path)
            .status()
            .expect("mkfifo");
        assert!(status.success());

        let home = tmp.path().to_path_buf();
        let (tx, rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || {
            let out = discover(&Config {
                home: Some(home),
                ..Config::default()
            });
            let _ = tx.send(out);
        });
        let (games, skipped) = rx
            .recv_timeout(std::time::Duration::from_secs(2))
            .expect("discover blocked on FIFO appmanifest_42.acf");
        assert!(games.is_empty());
        assert!(!skipped.is_empty(), "FIFO manifest should be reported in skipped list");
    }
}

#[cfg(unix)]
#[test]
fn finding8_non_utf8_directory_and_executable_names() {
    use std::ffi::OsString;
    use std::os::unix::ffi::OsStringExt;

    let tmp = TempDir::new("nonutf8");
    let game = tmp.path().join("game");
    let nonutf_dir = game.join(OsString::from_vec(b"bin-\xff".to_vec()));
    fs::create_dir_all(&nonutf_dir).unwrap();
    fs::write(nonutf_dir.join("game.exe"), b"MZ").unwrap();
    fs::write(nonutf_dir.join(OsString::from_vec(b"extra-\xfe.exe".to_vec())), b"MZ").unwrap();

    let found = targets(&game);
    assert_eq!(found.len(), 1, "non-UTF-8 directory must still be traversed");
    assert_eq!(found[0].path, nonutf_dir);
    assert_eq!(found[0].executables.len(), 2);

    // Also test non-UTF-8 directory for ELF executable detection.
    let elf_game = tmp.path().join("elf-game");
    let nonutf_elf_dir = elf_game.join(OsString::from_vec(b"linux-\xff".to_vec()));
    fs::create_dir_all(&nonutf_elf_dir).unwrap();
    fs::write(
        nonutf_elf_dir.join(OsString::from_vec(b"runner-\xfe".to_vec())),
        b"\x7fELF\x02\x01\x01\x00",
    )
    .unwrap();
    assert!(retoque_discovery::has_elf_executable(&elf_game));
}

#[test]
fn winfix1_ue_plugin_dlss_anywhere_and_skips_loader_subfolders() {
    let tmp = TempDir::new("winfix1-ue-dlss");
    let install_dir = tmp.path().join("MortalShell2");
    let exe_dir = install_dir.join("Sparta/Binaries/Win64");
    fs::create_dir_all(&exe_dir).unwrap();
    fs::write(exe_dir.join("Sparta-Win64-Shipping.exe"), b"MZ-sparta").unwrap();

    // Loader's bundled runtime under dlssg_sm86\runtime must be ignored by dlss_anywhere.
    let sm86_rt = exe_dir.join("dlssg_sm86/runtime");
    fs::create_dir_all(&sm86_rt).unwrap();
    fs::write(sm86_rt.join("nvngx_dlssg.dll"), b"loader-copy").unwrap();
    assert!(
        retoque_discovery::find_dlss_anywhere(&install_dir).is_empty(),
        "dlssg_sm86/runtime/nvngx_dlssg.dll must not count as game DLSS"
    );

    // Unreal Engine plugin paths (Mortal Shell II layout).
    let ue_dlss = install_dir.join("Sparta/Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64");
    let ue_sl = install_dir.join("Sparta/Engine/Plugins/Runtime/Nvidia/StreamlineCore/Binaries/ThirdParty/Win64");
    fs::create_dir_all(&ue_dlss).unwrap();
    fs::create_dir_all(&ue_sl).unwrap();
    fs::write(ue_dlss.join("nvngx_dlss.dll"), b"dlss").unwrap();
    fs::write(ue_sl.join("nvngx_dlssg.dll"), b"dlssg").unwrap();
    fs::write(ue_sl.join("sl.interposer.dll"), b"sl").unwrap();

    let ts = targets(&install_dir);
    assert_eq!(ts.len(), 1);
    assert!(ts[0].dlss.is_empty(), "no DLSS DLLs sits directly beside the exe");

    let anywhere = retoque_discovery::find_dlss_anywhere_with_targets(&install_dir, &ts);
    assert_eq!(
        anywhere,
        vec![
            "nvngx_dlss.dll".to_string(),
            "nvngx_dlssg.dll".to_string(),
            "sl.interposer.dll".to_string(),
        ]
    );
}

#[test]
fn winfix2_third_party_loaders_sm86_modern_legacy_reshade_specialk() {
    let tmp = TempDir::new("winfix2-loaders");

    // 1. Modern dlssg_for_sm86 0.4.x (dlssg_sm86.ini + version.dll + dlssg_sm86/runtime).
    let mod_dir = tmp.path().join("sm86-04");
    fs::create_dir_all(mod_dir.join("dlssg_sm86/runtime")).unwrap();
    fs::write(mod_dir.join("Game.exe"), b"MZ").unwrap();
    fs::write(mod_dir.join("dlssg_sm86.ini"), b"[DLSSG]\nPreset=A\n").unwrap();
    fs::write(mod_dir.join("version.dll"), b"MZ-DlssgProxy_Role-0.4").unwrap();
    let t_mod = targets(&mod_dir);
    assert_eq!(t_mod[0].loaders, vec!["dlssg_for_sm86"]);

    // 2. Legacy dlssg_for_sm86 0.3.x (monolithic version.dll + dlssg_sm86.ini without dlssg_sm86/runtime).
    let leg_dir = tmp.path().join("sm86-03");
    fs::create_dir_all(&leg_dir).unwrap();
    fs::write(leg_dir.join("Game.exe"), b"MZ").unwrap();
    fs::write(leg_dir.join("dlssg_sm86.ini"), b"[FrameGeneration]\nOptimized=1\n").unwrap();
    fs::write(leg_dir.join("version.dll"), b"MZ-DlssgMod_GetInfo-0.3").unwrap();
    let t_leg = targets(&leg_dir);
    assert_eq!(t_leg[0].loaders, vec!["dlssg_for_sm86-legacy"]);

    // 3. ReShade + Special K, while OptiScaler proxy carrying "OptiScaler working as ... ReShade" is not ReShade.
    let rs_dir = tmp.path().join("reshade-sk");
    fs::create_dir_all(&rs_dir).unwrap();
    fs::write(rs_dir.join("Game.exe"), b"MZ").unwrap();
    fs::write(rs_dir.join("dxgi.dll"), b"MZ OptiScaler working as dxgi ReShade64.dll").unwrap();
    assert!(targets(&rs_dir)[0].loaders.is_empty());

    fs::write(rs_dir.join("ReShade64.dll"), b"MZ ReShade").unwrap();
    fs::write(rs_dir.join("SpecialK64.dll"), b"MZ Special K").unwrap();
    let t_rs = targets(&rs_dir);
    assert_eq!(t_rs[0].loaders, vec!["reshade", "special_k"]);
}

#[test]
fn winfix3_orphan_steam_folders_without_appmanifest() {
    let tmp = TempDir::new("winfix3-orphan");
    let home = tmp.path();
    let steam_main = home.join(".local/share/Steam");
    let common = steam_main.join("steamapps/common");
    fs::create_dir_all(&common).unwrap();

    // Manifest-backed game (registered = true).
    fs::write(
        steam_main.join("steamapps/appmanifest_1001.acf"),
        fixture_str("appmanifest_1001.acf"),
    )
    .unwrap();
    let photon_bin = common.join("PhotonBlade/Photon/Binaries/Win64");
    fs::create_dir_all(&photon_bin).unwrap();
    fs::write(photon_bin.join("PhotonBlade-Win64-Shipping.exe"), b"MZ").unwrap();

    // Unregistered orphan game folder with an exe (registered = false).
    let subnautica_bin = common.join("Subnautica2/Subnautica2/Binaries/Win64");
    fs::create_dir_all(&subnautica_bin).unwrap();
    fs::write(subnautica_bin.join("Subnautica2-Win64-Shipping.exe"), b"MZ").unwrap();

    // Empty leftover folder without exe and Steamworks Shared must be skipped.
    fs::create_dir_all(common.join("LeftoverNoExe/.retoque")).unwrap();
    fs::write(common.join("LeftoverNoExe/.retoque/receipt.txt"), b"version\t1\n").unwrap();
    fs::create_dir_all(common.join("Steamworks Shared/_CommonRedist")).unwrap();
    fs::write(common.join("Steamworks Shared/vc_redist.x64.exe"), b"MZ").unwrap();

    let (games, skipped) = discover(&Config {
        home: Some(home.to_path_buf()),
        ..Config::default()
    });
    assert!(skipped.is_empty(), "unexpected skipped: {skipped:?}");
    assert_eq!(games.len(), 2);

    let photon = games.iter().find(|g| g.name == "Photon Blade").unwrap();
    assert!(photon.registered);
    assert_eq!(photon.id, "steam:1001");

    let sub = games.iter().find(|g| g.name == "Subnautica2").unwrap();
    assert!(!sub.registered);
    assert_eq!(sub.store, Store::Steam);
    assert!(sub.id.starts_with("steam-folder:"));
    assert!(sub.id.ends_with("/Subnautica2"));
}

#[test]
fn winfix4_missing_install_dir_skipped_with_note() {
    let tmp = TempDir::new("winfix4-missing");
    let epic_dir = tmp.path().join("epic-manifests");
    fs::create_dir_all(&epic_dir).unwrap();

    let missing_path = tmp.path().join("deleted-games/HandOfFate2");
    let epic_item = serde_json::json!({
        "FormatVersion": 0,
        "bIsIncompleteInstall": false,
        "AppName": "HandOfFate2",
        "DisplayName": "Hand of Fate 2",
        "InstallLocation": missing_path,
        "LaunchExecutable": "Hand of Fate 2.exe",
        "MainGameAppName": "HandOfFate2"
    });
    fs::write(epic_dir.join("hof2.item"), epic_item.to_string()).unwrap();

    let (games, skipped) = discover_epic_from_dir(&epic_dir);
    assert!(games.is_empty(), "deleted install_dir must not be returned as a game");
    assert_eq!(skipped.len(), 1);
    assert!(skipped[0].contains("Hand of Fate 2"));
    assert!(skipped[0].contains("install directory not found"));
}

#[test]
fn winfix5_normalize_windows_paths_mixed_slashes_and_drive_case() {
    assert_eq!(
        retoque_discovery::normalize_windows_path(r"c:/program files (x86)/steam\steamapps\common\Lossless Scaling"),
        r"C:\program files (x86)\steam\steamapps\common\Lossless Scaling"
    );
    assert_eq!(
        retoque_discovery::normalize_windows_path("f:/SteamLibrary//steamapps/"),
        r"F:\SteamLibrary\steamapps"
    );

    let vdf = r#""libraryfolders"
{
    "0"
    {
        "path" "c:/program files (x86)/steam"
    }
    "1"
    {
        "path" "f:\\SteamLibrary/Games"
    }
}"#;
    let libs = parse_library_folders_vdf(vdf).unwrap();
    assert_eq!(
        libs,
        vec![
            PathBuf::from(r"C:\program files (x86)\steam"),
            PathBuf::from(r"F:\SteamLibrary\Games"),
        ]
    );
}

#[test]
fn roots1_library_folders_discover_child_games() {
    let tmp = TempDir::new("roots1-libs");
    let home = tmp.path().join("empty-home");
    fs::create_dir_all(&home).unwrap();

    let games_nosteam = tmp.path().join("Games noSteam");
    let gh3 = games_nosteam.join("GuitarHero3");
    let pt_deep = games_nosteam.join("Silent Hill PT/P.T.For.PC.v0.9.2/P.T.For.PC.v0.9.2/PT For PC");
    let empty_dir = games_nosteam.join("EmptyFolder");
    fs::create_dir_all(&gh3).unwrap();
    fs::create_dir_all(&pt_deep).unwrap();
    fs::create_dir_all(&empty_dir).unwrap();
    fs::write(gh3.join("GH3.exe"), b"MZ").unwrap();
    fs::write(pt_deep.join("PT.exe"), b"MZ").unwrap();

    let (games, skipped) = discover(&Config {
        libraries: vec![games_nosteam.clone()],
        home: Some(home),
        ..Config::default()
    });
    assert!(skipped.is_empty(), "unexpected skipped: {skipped:?}");
    assert_eq!(games.len(), 2);

    assert_eq!(games[0].name, "GuitarHero3");
    assert_eq!(games[0].store, Store::Manual);
    assert!(games[0].registered);
    assert_eq!(games[0].id, format!("folder:{}", gh3.display()));

    assert_eq!(games[1].name, "Silent Hill PT");
    assert_eq!(games[1].store, Store::Manual);
    assert!(games[1].registered);
    assert_eq!(games[1].targets[0].path, pt_deep);
}

#[test]
fn roots2_xbox_gaming_root_unclaimed_epic_and_extra_steam_libraries() {
    let tmp = TempDir::new("roots2-winroots");
    let drive_c = tmp.path().join("drive_c");
    let drive_f = tmp.path().join("drive_f");
    fs::create_dir_all(drive_c.join("XboxGames")).unwrap();
    fs::create_dir_all(drive_c.join("Games/SteamLibrary")).unwrap();
    fs::create_dir_all(&drive_f).unwrap();

    // Write a synthetic binary .GamingRoot ("RGBX" + u32(1) + UTF-16LE "\XboxGames\0") on drive_f.
    let mut gr_bytes = b"RGBX\x01\x00\x00\x00".to_vec();
    for unit in "\\XboxGames\0".encode_utf16() {
        gr_bytes.extend_from_slice(&unit.to_le_bytes());
    }
    fs::write(drive_f.join(".GamingRoot"), &gr_bytes).unwrap();
    assert_eq!(
        retoque_discovery::parse_gaming_root(&gr_bytes).as_deref(),
        Some("XboxGames")
    );

    let halo_content = drive_f.join("XboxGames/HaloInfinite/Content");
    fs::create_dir_all(&halo_content).unwrap();
    fs::write(halo_content.join("HaloInfinite.exe"), b"MZ").unwrap();

    let xbox_games = retoque_discovery::discover_xbox_from_drives(&[drive_c.clone(), drive_f.clone()]);
    assert_eq!(xbox_games.len(), 1);
    assert_eq!(xbox_games[0].name, "HaloInfinite");
    assert_eq!(xbox_games[0].store, Store::Xbox);
    assert!(xbox_games[0].registered);
    assert_eq!(xbox_games[0].install_dir, halo_content);

    // Unclaimed Epic Games folder + empty Epic folder.
    let epic_manifests = tmp.path().join("epic-manifests");
    fs::create_dir_all(&epic_manifests).unwrap();
    let epic_root = drive_f.join("Epic Games");
    let breach_dir = epic_root.join("IntoTheBreach");
    let evil_empty = epic_root.join("TheEvilWithin2");
    fs::create_dir_all(&breach_dir).unwrap();
    fs::create_dir_all(&evil_empty).unwrap();
    fs::write(breach_dir.join("Breach.exe"), b"MZ").unwrap();

    let (epic_games, epic_skipped) = retoque_discovery::discover_epic_with_roots(&epic_manifests, &[epic_root]);
    assert!(epic_skipped.is_empty());
    assert_eq!(epic_games.len(), 1);
    assert_eq!(epic_games[0].name, "IntoTheBreach");
    assert_eq!(epic_games[0].store, Store::Epic);
    assert!(!epic_games[0].registered);
    assert_eq!(epic_games[0].id, format!("epic-folder:{}", breach_dir.display()));

    // Extra Steam library discovery.
    let extra_steam = retoque_discovery::extra_steam_libraries_from_drives(&[drive_c.clone(), drive_f.clone()]);
    assert_eq!(extra_steam, vec![drive_c.join("Games/SteamLibrary")]);
}

#[test]
fn roots3_emulators_pcsx2_and_skip_games_iso_folder() {
    let tmp = TempDir::new("roots3-emu");
    let drive_f = tmp.path().join("drive_f");
    let pcsx2_dir = drive_f.join("PCSX2");
    let iso_dir = pcsx2_dir.join("games");
    fs::create_dir_all(&iso_dir).unwrap();
    fs::write(pcsx2_dir.join("pcsx2-qt.exe"), b"MZ-pcsx2").unwrap();
    // Place a helper exe inside `games/` to prove `games/` is not walked when pcsx2-qt.exe is present.
    fs::write(iso_dir.join("iso_tool.exe"), b"MZ-tool").unwrap();

    let emu_games = retoque_discovery::discover_emulators_from_drives(&[drive_f]);
    assert_eq!(emu_games.len(), 1);
    assert_eq!(emu_games[0].name, "PCSX2");
    assert_eq!(emu_games[0].store, Store::Manual);
    assert!(emu_games[0].registered);
    assert_eq!(emu_games[0].targets.len(), 1, "must not walk into PCSX2/games/");
    assert_eq!(emu_games[0].targets[0].path, pcsx2_dir);
    assert_eq!(emu_games[0].targets[0].executables, vec!["pcsx2-qt.exe"]);
}

#[test]
fn roots_fix1_xbox_ignores_appdata_root_and_non_content_children() {
    let tmp = TempDir::new("roots-fix1-xbox");
    let drive_c = tmp.path().join("drive_c");
    let drive_f = tmp.path().join("drive_f");

    // C:\.GamingRoot pointing at Users\SyntheticUser\AppData must be ignored completely.
    let appdata_local_discord = drive_c.join("Users/SyntheticUser/AppData/Local/Discord/app-1.0");
    let appdata_roaming = drive_c.join("Users/SyntheticUser/AppData/Roaming/Overwolf");
    fs::create_dir_all(&appdata_local_discord).unwrap();
    fs::create_dir_all(&appdata_roaming).unwrap();
    fs::write(appdata_local_discord.join("Discord.exe"), b"MZ").unwrap();
    fs::write(appdata_roaming.join("moba-cap64.exe"), b"MZ").unwrap();

    let mut gr_c = b"RGBX\x01\x00\x00\x00".to_vec();
    for unit in "Users\\SyntheticUser\\AppData\0".encode_utf16() {
        gr_c.extend_from_slice(&unit.to_le_bytes());
    }
    fs::write(drive_c.join(".GamingRoot"), &gr_c).unwrap();

    // F:\XboxGames with one child lacking Content/ and one valid game with Content/.
    let no_content_child = drive_f.join("XboxGames/LooseFolder");
    let valid_content = drive_f.join("XboxGames/ForzaMotorsport/Content");
    fs::create_dir_all(&no_content_child).unwrap();
    fs::create_dir_all(&valid_content).unwrap();
    fs::write(no_content_child.join("LooseTool.exe"), b"MZ").unwrap();
    fs::write(valid_content.join("ForzaMotorsport.exe"), b"MZ").unwrap();
    fs::write(valid_content.join("MicrosoftGame.config"), b"<Game/>").unwrap();

    let xbox_games = retoque_discovery::discover_xbox_from_drives(&[drive_c, drive_f]);
    assert_eq!(xbox_games.len(), 1, "must not include Local, Roaming, or LooseFolder");
    assert_eq!(xbox_games[0].name, "ForzaMotorsport");
    assert_eq!(xbox_games[0].install_dir, valid_content);
}

#[test]
fn roots_fix2_orphan_steam_mod_tools_ignored_and_dropped() {
    let tmp = TempDir::new("roots-fix2-modtools");
    let home = tmp.path();
    let common = home.join(".local/share/Steam/steamapps/common");

    // 1. Uninstalled DARK SOULS III with leftover mod launchers/loaders.
    let ds3 = common.join("DARK SOULS III");
    fs::create_dir_all(ds3.join("Loader")).unwrap();
    fs::create_dir_all(ds3.join("Server")).unwrap();
    fs::create_dir_all(ds3.join("Game")).unwrap();
    fs::create_dir_all(ds3.join("ME2/modengine2/tools/scyllahide")).unwrap();
    fs::write(ds3.join("Loader/Loader.exe"), b"MZ").unwrap();
    fs::write(ds3.join("Server/Server.exe"), b"MZ").unwrap();
    fs::write(ds3.join("Game/ds3sc_launcher.exe"), b"MZ").unwrap();
    fs::write(ds3.join("ME2/modengine2_launcher.exe"), b"MZ").unwrap();
    fs::write(ds3.join("ME2/modengine2/tools/scyllahide/InjectorCLIx64.exe"), b"MZ").unwrap();

    // 2. Uninstalled ELDEN RING with leftover ModEngine2, randomizer, me3, downloader, seamless coop.
    let er = common.join("ELDEN RING");
    fs::create_dir_all(er.join("Game/ModEngine2/randomizer")).unwrap();
    fs::create_dir_all(er.join("ConvergenceER/ConvergenceER 3.0/me3/Windows")).unwrap();
    fs::create_dir_all(er.join("mods instalados")).unwrap();
    fs::write(er.join("Convergence ER Downloader-3419-1-0.exe"), b"MZ").unwrap();
    fs::write(er.join("Game/ersc_launcher.exe"), b"MZ").unwrap();
    fs::write(er.join("Game/ModEngine2/EldenRingRandomizer.exe"), b"MZ").unwrap();
    fs::write(er.join("Game/ModEngine2/modengine2_launcher.exe"), b"MZ").unwrap();
    fs::write(er.join("ConvergenceER/ConvergenceER 3.0/me3/Windows/me3.exe"), b"MZ").unwrap();
    fs::write(
        er.join("ConvergenceER/ConvergenceER 3.0/me3/Windows/me3-launcher.exe"),
        b"MZ",
    )
    .unwrap();
    fs::write(er.join("mods instalados/modengine2_launcher.exe"), b"MZ").unwrap();

    // 3. Uninstalled ELDEN RING NIGHTREIGN with leftover me3, nrsc_launcher, custom server launcher.
    let ern = common.join("ELDEN RING NIGHTREIGN");
    fs::create_dir_all(ern.join("me3/bin")).unwrap();
    fs::create_dir_all(ern.join("Game")).unwrap();
    fs::create_dir_all(ern.join("More Map Variations 2.1.7/mod/Server Redirector")).unwrap();
    fs::write(ern.join("me3/bin/me3.exe"), b"MZ").unwrap();
    fs::write(ern.join("me3/bin/me3-launcher.exe"), b"MZ").unwrap();
    fs::write(ern.join("Game/nrsc_launcher.exe"), b"MZ").unwrap();
    fs::write(
        ern.join("More Map Variations 2.1.7/mod/Server Redirector/NightreignCustomServerLauncher.exe"),
        b"MZ",
    )
    .unwrap();
    fs::write(ern.join("uninstall.exe"), b"MZ").unwrap();
    fs::write(ern.join("crashpad_handler.exe"), b"MZ").unwrap();

    let (games, skipped) = discover(&Config {
        home: Some(home.to_path_buf()),
        ..Config::default()
    });
    assert!(skipped.is_empty());
    assert!(
        games.is_empty(),
        "uninstalled games containing only mod tools must produce 0 games, got: {games:?}"
    );
}

#[test]
fn roots_fix3_synthetic_big_tree_performance() {
    let tmp = TempDir::new("roots-fix3-perf");
    let home = tmp.path();
    let common = home.join(".local/share/Steam/steamapps/common");
    fs::create_dir_all(&common).unwrap();

    // Create 55 orphan folders with heavy Content/Paks, *_Data/Managed, and *.pak subtrees.
    for i in 0..55 {
        let orphan = common.join(format!("UninstalledGame_{i:02}"));
        let paks = orphan.join("GameProj/Content/Paks/pakchunk0.pak");
        let unity_data = orphan.join(format!("UninstalledGame_{i:02}_Data/Managed"));
        fs::create_dir_all(&paks).unwrap();
        fs::create_dir_all(&unity_data).unwrap();
        for j in 0..30 {
            fs::write(paks.join(format!("asset_{j}.uasset")), b"data").unwrap();
            fs::write(unity_data.join(format!("Assembly_{j}.dll")), b"dll").unwrap();
        }
    }

    // Create 5 real Unreal games with Engine/Plugins/Runtime/Nvidia/...
    for i in 0..5 {
        let game = common.join(format!("RealUEGame_{i:02}"));
        let bin = game.join("Proj/Binaries/Win64");
        let ue_dlss = game.join("Proj/Engine/Plugins/Runtime/Nvidia/DLSS/Binaries/ThirdParty/Win64");
        let paks = game.join("Proj/Content/Paks");
        fs::create_dir_all(&bin).unwrap();
        fs::create_dir_all(&ue_dlss).unwrap();
        fs::create_dir_all(&paks).unwrap();
        fs::write(bin.join(format!("RealUEGame_{i:02}-Win64-Shipping.exe")), b"MZ").unwrap();
        fs::write(ue_dlss.join("nvngx_dlss.dll"), b"dlss").unwrap();
        fs::write(ue_dlss.join("nvngx_dlssg.dll"), b"dlssg").unwrap();
        for j in 0..100 {
            fs::write(paks.join(format!("pakchunk_{j}.pak")), b"pak").unwrap();
        }
    }

    let start = std::time::Instant::now();
    let (games, skipped) = discover(&Config {
        home: Some(home.to_path_buf()),
        ..Config::default()
    });
    let elapsed = start.elapsed();

    assert!(skipped.is_empty());
    assert_eq!(games.len(), 5);
    for g in &games {
        assert_eq!(g.dlss_anywhere, vec!["nvngx_dlss.dll", "nvngx_dlssg.dll"]);
    }
    assert!(
        elapsed.as_millis() < 500,
        "synthetic big tree discovery took {} ms (expected < 500 ms)",
        elapsed.as_millis()
    );
}
