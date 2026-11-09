use std::borrow::Cow;
use std::collections::{BTreeMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

use rusqlite::Connection;
use yaml_rust2::parser::{Event, MarkedEventReceiver, Parser};
use yaml_rust2::{Yaml, YamlLoader};

use crate::io::{MAX_DB_BYTES, MAX_TEXT_BYTES, read_bounded_bytes, read_bounded_string};
use crate::steam::normalize_path_buf;
use crate::targets::{find_dlss_anywhere_with_targets, find_targets_with_hint, has_elf_executable, resolve_hint_path};
use crate::{Game, Runtime, Store};

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LutrisDbGame {
    pub id: i64,
    pub name: String,
    pub slug: String,
    pub runner: Option<String>,
    pub executable: Option<String>,
    pub directory: Option<String>,
    pub installed: bool,
    pub configpath: Option<String>,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LutrisYamlConfig {
    pub exe: Option<PathBuf>,
    pub prefix: Option<PathBuf>,
    pub working_dir: Option<PathBuf>,
}

/// Pure parser over the raw bytes of Lutris's `pga.db` SQLite database (`games` table).
pub fn parse_lutris_db(bytes: &[u8]) -> Result<Vec<LutrisDbGame>, String> {
    if bytes.len() > MAX_DB_BYTES {
        return Err(format!(
            "Lutris pga.db size ({}) exceeds limit ({MAX_DB_BYTES})",
            bytes.len()
        ));
    }
    if bytes.len() < 100 || &bytes[..16] != b"SQLite format 3\0" {
        return Err("invalid SQLite header in Lutris pga.db".to_string());
    }

    // SQLite's in-memory `memdb` VFS rejects images whose header marks WAL mode
    // (file format write/read version == 2 at offsets 18..20). Downgrading those two header
    // bytes to 1 in the deserialized buffer allows `sqlite3_deserialize` to read committed
    // pages from a WAL-mode database image in memory.
    let image: Cow<'_, [u8]> = if bytes[18] == 2 || bytes[19] == 2 {
        let mut patched = bytes.to_vec();
        if patched[18] == 2 {
            patched[18] = 1;
        }
        if patched[19] == 2 {
            patched[19] = 1;
        }
        Cow::Owned(patched)
    } else {
        Cow::Borrowed(bytes)
    };

    let mut conn = Connection::open_in_memory().map_err(|e| format!("sqlite open_in_memory: {e}"))?;
    conn.deserialize_read_exact(rusqlite::MAIN_DB, &image[..], image.len(), true)
        .map_err(|e| format!("sqlite deserialize pga.db: {e}"))?;

    query_games_from_conn(&conn)
}

const MAX_SNAPSHOT_RETRIES: usize = 5;

#[derive(Clone, Debug, PartialEq, Eq)]
struct FileStamp {
    exists: bool,
    len: u64,
    modified: Option<std::time::SystemTime>,
    #[cfg(unix)]
    ino: u64,
    #[cfg(unix)]
    dev: u64,
    #[cfg(unix)]
    ctime_nsec: i64,
}

fn file_stamp(path: &Path) -> FileStamp {
    match fs::symlink_metadata(path) {
        Ok(m) => {
            #[cfg(unix)]
            use std::os::unix::fs::MetadataExt;
            FileStamp {
                exists: true,
                len: m.len(),
                modified: m.modified().ok(),
                #[cfg(unix)]
                ino: m.ino(),
                #[cfg(unix)]
                dev: m.dev(),
                #[cfg(unix)]
                ctime_nsec: m.ctime_nsec(),
            }
        }
        Err(_) => FileStamp {
            exists: false,
            len: 0,
            modified: None,
            #[cfg(unix)]
            ino: 0,
            #[cfg(unix)]
            dev: 0,
            #[cfg(unix)]
            ctime_nsec: 0,
        },
    }
}

fn validate_wal_pair(db_bytes: &[u8], wal_bytes: Option<&[u8]>, shm_bytes: Option<&[u8]>) -> bool {
    if db_bytes.len() < 100 || &db_bytes[..16] != b"SQLite format 3\0" {
        return false;
    }
    let Some(wal) = wal_bytes.filter(|w| !w.is_empty()) else {
        return true;
    };
    if wal.len() < 32 {
        return false;
    }
    let magic = u32::from_be_bytes([wal[0], wal[1], wal[2], wal[3]]);
    if magic != 0x377f_0682 && magic != 0x377f_0683 {
        return false;
    }
    let raw_db_ps = u16::from_be_bytes([db_bytes[16], db_bytes[17]]) as u32;
    let db_ps = if raw_db_ps == 1 { 65536 } else { raw_db_ps };
    let wal_ps = u32::from_be_bytes([wal[8], wal[9], wal[10], wal[11]]);
    if db_ps != wal_ps {
        return false;
    }
    if let Some(shm) = shm_bytes.filter(|s| s.len() >= 96) {
        if shm[0..48] != shm[48..96] {
            return false;
        }
        if shm[8] != 0 && shm[32..40] != wal[16..24] {
            return false;
        }
    }
    true
}

/// Read a Lutris `pga.db` file from disk without ever opening or writing inside Lutris's folder.
/// Uses double-read bracketing (`stat -> db -> wal -> shm -> db -> wal -> shm -> stat`) with
/// bounded retries and WAL/SHM header validation so a concurrent checkpoint or WAL rotation can
/// never produce a mixed-generation snapshot.
pub fn read_lutris_db_file(path: &Path) -> Result<Vec<LutrisDbGame>, String> {
    read_lutris_db_file_with_hook(path, || {})
}

#[doc(hidden)]
pub fn read_lutris_db_file_with_hook(
    path: &Path,
    mut after_db_read: impl FnMut(),
) -> Result<Vec<LutrisDbGame>, String> {
    let mut wal_os = path.as_os_str().to_os_string();
    wal_os.push("-wal");
    let wal_path = PathBuf::from(wal_os);

    let mut shm_os = path.as_os_str().to_os_string();
    shm_os.push("-shm");
    let shm_path = PathBuf::from(shm_os);

    for _ in 0..MAX_SNAPSHOT_RETRIES {
        let stamp_before = (file_stamp(path), file_stamp(&wal_path), file_stamp(&shm_path));

        let db_1 = read_bounded_bytes(path, MAX_DB_BYTES)?;
        after_db_read();

        let wal_1 = if wal_path.exists() {
            Some(read_bounded_bytes(&wal_path, MAX_DB_BYTES)?)
        } else {
            None
        };
        let shm_1 = if shm_path.exists() {
            read_bounded_bytes(&shm_path, MAX_TEXT_BYTES).ok()
        } else {
            None
        };

        let db_2 = read_bounded_bytes(path, MAX_DB_BYTES)?;
        let wal_2 = if wal_path.exists() {
            Some(read_bounded_bytes(&wal_path, MAX_DB_BYTES)?)
        } else {
            None
        };
        let shm_2 = if shm_path.exists() {
            read_bounded_bytes(&shm_path, MAX_TEXT_BYTES).ok()
        } else {
            None
        };

        let stamp_after = (file_stamp(path), file_stamp(&wal_path), file_stamp(&shm_path));

        if stamp_before != stamp_after
            || db_1 != db_2
            || wal_1 != wal_2
            || shm_1 != shm_2
            || !validate_wal_pair(&db_1, wal_1.as_deref(), shm_1.as_deref())
        {
            continue;
        }

        if let Some(wal_bytes) = wal_1.filter(|w| !w.is_empty()) {
            let tmp = PrivateTempDir::new()?;
            let tmp_db = tmp.path.join("pga.db");
            let tmp_wal = tmp.path.join("pga.db-wal");
            fs::write(&tmp_db, &db_1).map_err(|e| format!("write temp pga.db: {e}"))?;
            fs::write(&tmp_wal, &wal_bytes).map_err(|e| format!("write temp pga.db-wal: {e}"))?;
            let conn = Connection::open(&tmp_db).map_err(|e| format!("sqlite open temp copy: {e}"))?;
            return query_games_from_conn(&conn);
        }

        return parse_lutris_db(&db_1);
    }

    Err("Lutris pga.db/WAL changed concurrently during read; could not obtain a consistent snapshot".to_string())
}

struct PrivateTempDir {
    path: PathBuf,
}

impl PrivateTempDir {
    fn new() -> Result<Self, String> {
        static COUNTER: AtomicU64 = AtomicU64::new(0);
        let pid = std::process::id();
        let base = std::env::temp_dir();
        for _ in 0..64 {
            let seq = COUNTER.fetch_add(1, Ordering::Relaxed);
            let candidate = base.join(format!("retoque-lutris-wal-{pid}-{seq}"));
            match fs::create_dir(&candidate) {
                Ok(()) => {
                    #[cfg(unix)]
                    {
                        use std::os::unix::fs::PermissionsExt;
                        let _ = fs::set_permissions(&candidate, fs::Permissions::from_mode(0o700));
                    }
                    return Ok(Self { path: candidate });
                }
                Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("create temp dir: {e}")),
            }
        }
        Err("could not allocate unique temp dir for Lutris WAL copy".to_string())
    }
}

impl Drop for PrivateTempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

fn query_games_from_conn(conn: &Connection) -> Result<Vec<LutrisDbGame>, String> {
    let mut cols = HashSet::new();
    {
        let mut stmt = conn
            .prepare("PRAGMA table_info(games)")
            .map_err(|e| format!("sqlite pragma table_info(games): {e}"))?;
        let rows = stmt
            .query_map([], |row| row.get::<_, String>(1))
            .map_err(|e| format!("sqlite read table_info(games): {e}"))?;
        for col in rows.flatten() {
            cols.insert(col.to_ascii_lowercase());
        }
    }

    if cols.is_empty() {
        return Err("missing 'games' table in Lutris pga.db".to_string());
    }

    let col_expr = |name: &str, default_sql: &str| -> String {
        if cols.contains(name) {
            name.to_string()
        } else {
            default_sql.to_string()
        }
    };

    let sql = format!(
        "SELECT {}, {}, {}, {}, {}, {}, {}, {} FROM games",
        col_expr("id", "rowid"),
        col_expr("name", "''"),
        col_expr("slug", "''"),
        col_expr("runner", "NULL"),
        col_expr("executable", "NULL"),
        col_expr("directory", "NULL"),
        col_expr("installed", "1"),
        col_expr("configpath", "NULL"),
    );

    let mut stmt = conn.prepare(&sql).map_err(|e| format!("sqlite query games: {e}"))?;

    let rows = stmt
        .query_map([], |row| {
            let id: i64 = row.get(0).unwrap_or(0);
            let name: Option<String> = row.get(1).ok();
            let slug: Option<String> = row.get(2).ok();
            let runner: Option<String> = row.get(3).ok();
            let executable: Option<String> = row.get(4).ok();
            let directory: Option<String> = row.get(5).ok();
            let installed_int: i64 = row.get(6).unwrap_or(1);
            let configpath: Option<String> = row.get(7).ok();

            Ok(LutrisDbGame {
                id,
                name: name.unwrap_or_default(),
                slug: slug.unwrap_or_default(),
                runner: runner.filter(|s| !s.trim().is_empty()),
                executable: executable.filter(|s| !s.trim().is_empty()),
                directory: directory.filter(|s| !s.trim().is_empty()),
                installed: installed_int != 0,
                configpath: configpath.filter(|s| !s.trim().is_empty()),
            })
        })
        .map_err(|e| format!("sqlite map games: {e}"))?;

    let mut out = Vec::new();
    for item in rows {
        let g = item.map_err(|e| format!("sqlite row error: {e}"))?;
        if g.installed && (!g.name.trim().is_empty() || !g.slug.trim().is_empty()) {
            out.push(g);
        }
    }
    Ok(out)
}

const MAX_YAML_DEPTH: usize = 64;
const MAX_YAML_EVENTS: usize = 16_384;
const MAX_YAML_ALIASES: usize = 64;

/// Fallible YAML parser for a Lutris game YAML configuration file (`<configpath>.yml`).
pub fn try_parse_lutris_yaml(text: &str) -> Result<LutrisYamlConfig, String> {
    if text.len() > MAX_TEXT_BYTES {
        return Err(format!(
            "Lutris YAML size ({}) exceeds limit ({MAX_TEXT_BYTES})",
            text.len()
        ));
    }

    let mut parser = Parser::new_from_str(text);
    let mut loader = YamlLoader::default();
    let mut depth = 0usize;
    let mut events = 0usize;
    let mut aliases = 0usize;
    let mut expected_docs = 0usize;

    loop {
        let (ev, mark) = parser.next_token().map_err(|e| format!("invalid Lutris YAML: {e}"))?;
        events += 1;
        if events > MAX_YAML_EVENTS {
            return Err(format!("Lutris YAML event count exceeds limit ({MAX_YAML_EVENTS})"));
        }

        let forwarded = match ev {
            Event::StreamEnd => {
                loader.on_event(Event::StreamEnd, mark);
                break;
            }
            Event::SequenceStart(_, tag) => {
                depth += 1;
                if depth > MAX_YAML_DEPTH {
                    return Err(format!("Lutris YAML nesting depth exceeds limit ({MAX_YAML_DEPTH})"));
                }
                // Strip collection anchors so alias expansion cannot trigger exponential Billion Laughs cloning.
                Event::SequenceStart(0, tag)
            }
            Event::MappingStart(_, tag) => {
                depth += 1;
                if depth > MAX_YAML_DEPTH {
                    return Err(format!("Lutris YAML nesting depth exceeds limit ({MAX_YAML_DEPTH})"));
                }
                Event::MappingStart(0, tag)
            }
            Event::SequenceEnd | Event::MappingEnd => {
                depth = depth.saturating_sub(1);
                ev
            }
            Event::Alias(_) => {
                aliases += 1;
                if aliases > MAX_YAML_ALIASES {
                    return Err(format!("Lutris YAML alias count exceeds limit ({MAX_YAML_ALIASES})"));
                }
                ev
            }
            Event::DocumentEnd => {
                expected_docs += 1;
                loader.on_event(Event::DocumentEnd, mark);
                if loader.documents().len() != expected_docs {
                    return Err("invalid Lutris YAML: duplicate key in mapping".to_string());
                }
                continue;
            }
            other => other,
        };

        loader.on_event(forwarded, mark);
    }

    let mut cfg = LutrisYamlConfig::default();
    for doc in loader.documents() {
        if let Some(game_sec) = yaml_get_case_insensitive(doc, "game") {
            extract_lutris_yaml_fields(game_sec, &mut cfg);
        }
        extract_lutris_yaml_fields(doc, &mut cfg);
    }

    Ok(cfg)
}

/// Pure parser for a Lutris game YAML configuration file (`<configpath>.yml`).
pub fn parse_lutris_yaml(text: &str) -> LutrisYamlConfig {
    try_parse_lutris_yaml(text).unwrap_or_default()
}

fn yaml_get_case_insensitive<'a>(node: &'a Yaml, target_key: &str) -> Option<&'a Yaml> {
    let hash = node.as_hash()?;
    for (k, v) in hash {
        if let Some(ks) = k.as_str()
            && ks.eq_ignore_ascii_case(target_key)
        {
            return Some(v);
        }
    }
    None
}

fn extract_lutris_yaml_fields(node: &Yaml, cfg: &mut LutrisYamlConfig) {
    let Some(hash) = node.as_hash() else {
        return;
    };
    for (k, v) in hash {
        let Some(key) = k.as_str() else {
            continue;
        };
        let Some(val) = v.as_str().map(str::trim).filter(|s| !s.is_empty()) else {
            continue;
        };
        match key {
            "exe" if cfg.exe.is_none() => cfg.exe = Some(PathBuf::from(val)),
            "prefix" if cfg.prefix.is_none() => cfg.prefix = Some(PathBuf::from(val)),
            "working_dir" if cfg.working_dir.is_none() => {
                cfg.working_dir = Some(PathBuf::from(val));
            }
            _ => {}
        }
    }
}

pub fn discover_lutris(home: Option<&Path>) -> (Vec<Game>, Vec<String>) {
    let Some(h) = home else {
        return (Vec::new(), Vec::new());
    };

    let instances = [
        (
            h.join(".local/share/lutris/pga.db"),
            vec![h.join(".local/share/lutris/games"), h.join(".config/lutris/games")],
        ),
        (
            h.join(".var/app/net.lutris.Lutris/data/lutris/pga.db"),
            vec![
                h.join(".var/app/net.lutris.Lutris/data/lutris/games"),
                h.join(".var/app/net.lutris.Lutris/config/lutris/games"),
            ],
        ),
    ];

    let mut seen_dbs: HashSet<PathBuf> = HashSet::new();
    let mut games: BTreeMap<String, Game> = BTreeMap::new();
    let mut skipped = Vec::new();

    for (db_path, yaml_dirs) in instances {
        if !db_path.exists() {
            continue;
        }
        let canon = fs::canonicalize(&db_path).unwrap_or_else(|_| db_path.clone());
        if !seen_dbs.insert(canon) {
            continue;
        }

        let db_games = match read_lutris_db_file(&db_path) {
            Ok(g) => g,
            Err(e) => {
                skipped.push(format!("lutris ({}): {e}", db_path.display()));
                continue;
            }
        };

        for db_game in db_games {
            let yaml_cfg = load_lutris_yaml(&db_game, &yaml_dirs, &mut skipped);

            let prefix = yaml_cfg.prefix.clone();
            let raw_exe = yaml_cfg
                .exe
                .clone()
                .or_else(|| db_game.executable.as_deref().map(PathBuf::from));

            let base_dir_for_exe = db_game
                .directory
                .as_deref()
                .map(PathBuf::from)
                .or_else(|| yaml_cfg.working_dir.clone())
                .or_else(|| prefix.clone());

            let resolved_exe = raw_exe.as_ref().map(|exe| {
                if let Some(base) = &base_dir_for_exe {
                    resolve_hint_path(base, prefix.as_deref(), exe)
                } else {
                    exe.clone()
                }
            });

            let install_dir = if let Some(dir) = &db_game.directory {
                PathBuf::from(dir)
            } else if let Some(wd) = &yaml_cfg.working_dir {
                wd.clone()
            } else if let Some(exe) = &resolved_exe
                && let Some(parent) = exe.parent()
            {
                parent.to_path_buf()
            } else if let Some(pfx) = &prefix {
                pfx.clone()
            } else {
                continue;
            };
            let install_dir = normalize_path_buf(&install_dir);

            let slug = if db_game.slug.trim().is_empty() {
                db_game.id.to_string()
            } else {
                db_game.slug.clone()
            };
            let name = if db_game.name.trim().is_empty() {
                slug.clone()
            } else {
                db_game.name.clone()
            };

            if !install_dir.is_dir() {
                skipped.push(format!(
                    "lutris ({name}): install directory not found: {}",
                    install_dir.display()
                ));
                continue;
            }

            let game_targets = find_targets_with_hint(&install_dir, resolved_exe.as_deref());
            let dlss_anywhere = find_dlss_anywhere_with_targets(&install_dir, &game_targets);

            let is_linux_runner = db_game
                .runner
                .as_deref()
                .is_some_and(|r| r.eq_ignore_ascii_case("linux"));

            let runtime = if is_linux_runner
                || (prefix.is_none()
                    && db_game
                        .runner
                        .as_deref()
                        .is_none_or(|r| !r.eq_ignore_ascii_case("wine"))
                    && game_targets.is_empty()
                    && has_elf_executable(&install_dir))
            {
                Runtime::NativeLinux
            } else {
                Runtime::Wine { prefix }
            };

            let id = format!("lutris:{slug}");
            games.entry(id.clone()).or_insert(Game {
                id,
                name,
                store: Store::Lutris,
                install_dir,
                runtime,
                targets: game_targets,
                dlss_anywhere,
                registered: true,
            });
        }
    }

    (games.into_values().collect(), skipped)
}

fn load_lutris_yaml(db_game: &LutrisDbGame, yaml_dirs: &[PathBuf], skipped: &mut Vec<String>) -> LutrisYamlConfig {
    let mut keys = Vec::new();
    if let Some(cp) = &db_game.configpath {
        keys.push(cp.clone());
    }
    if !db_game.slug.is_empty() && !keys.iter().any(|k| k == &db_game.slug) {
        keys.push(db_game.slug.clone());
    }

    for key in keys {
        let stem = key
            .strip_suffix(".yml")
            .or_else(|| key.strip_suffix(".yaml"))
            .unwrap_or(&key);
        let candidates = [format!("{stem}.yml"), format!("{stem}.yaml")];

        for dir in yaml_dirs {
            for file_name in &candidates {
                let path = dir.join(file_name);
                if path.exists() {
                    match read_bounded_string(&path, MAX_TEXT_BYTES) {
                        Ok(text) => match try_parse_lutris_yaml(&text) {
                            Ok(cfg) => return cfg,
                            Err(e) => skipped.push(format!("lutris ({}): {e}", path.display())),
                        },
                        Err(e) => {
                            skipped.push(format!("lutris ({}): {e}", path.display()));
                        }
                    }
                }
            }
        }
    }

    LutrisYamlConfig::default()
}
