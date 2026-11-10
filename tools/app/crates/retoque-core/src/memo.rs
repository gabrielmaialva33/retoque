//! Remembered answers about files that the app reads on every start (a proxy's hash, whether it is an OptiScaler
//! build), keyed by the file's identity: path, size and modification time, plus device, inode and change time on
//! Unix, which an ordinary write cannot restore. Any change to the file means a fresh read.
//!
//! Only status reporting uses this. Installs, updates and removals keep hashing the real bytes before they act.
//! Answers live in memory and, once the app calls [`persist_in`], in a small file beside its data, so a library
//! of games does not re-read hundreds of MB at every start. Tests and library users never write to disk.
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs,
    path::{Path, PathBuf},
    sync::Mutex,
};

const STORE: &str = "file-facts.json";
/// Bumped whenever what a remembered answer means changes, so older answers are not reused.
const SCHEMA: u32 = 1;
const LIMIT: usize = 1024;

#[derive(Default, Serialize, Deserialize)]
struct Store {
    schema: u32,
    entries: BTreeMap<String, String>,
}
struct State {
    store: Store,
    path: Option<PathBuf>,
}
static STATE: Mutex<Option<State>> = Mutex::new(None);

/// Loads and keeps writing the remembered answers in `dir`. A missing, damaged or older store starts empty.
pub fn persist_in(dir: &Path) {
    let path = dir.join(STORE);
    let store = fs::read(&path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Store>(&bytes).ok())
        .filter(|s| s.schema == SCHEMA)
        .unwrap_or_default();
    *STATE.lock().unwrap_or_else(|e| e.into_inner()) = Some(State {
        store,
        path: Some(path),
    });
}

/// The file's identity, or None for anything that is not a plain regular file (links are never remembered).
pub fn identity(path: &Path, meta: &fs::Metadata) -> Option<String> {
    if !meta.is_file() {
        return None;
    }
    let modified = meta
        .modified()
        .ok()?
        .duration_since(std::time::UNIX_EPOCH)
        .ok()?
        .as_nanos();
    #[cfg(unix)]
    let extra = {
        use std::os::unix::fs::MetadataExt;
        format!("{}:{}:{}.{}", meta.dev(), meta.ino(), meta.ctime(), meta.ctime_nsec())
    };
    #[cfg(not(unix))]
    let extra = meta
        .created()
        .ok()
        .and_then(|c| c.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|c| c.as_nanos().to_string())
        .unwrap_or_default();
    Some(format!("{}|{}|{modified}|{extra}", path.display(), meta.len()))
}

fn current(path: &Path) -> Option<String> {
    let link = fs::symlink_metadata(path).ok()?;
    if link.file_type().is_symlink() {
        return None;
    }
    identity(path, &link)
}

/// `compute`'s answer for the file at `path`, remembered under `kind` until the file changes. A file that
/// changed while it was read is answered but not remembered.
pub(crate) fn file<T: ToString + std::str::FromStr>(
    kind: &str,
    path: &Path,
    compute: impl FnOnce() -> Option<T>,
) -> Option<T> {
    let Some(id) = current(path) else { return compute() };
    let key = format!("{kind}|{id}");
    if let Some(hit) = STATE
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .as_ref()
        .and_then(|s| s.store.entries.get(&key))
        .and_then(|v| v.parse().ok())
    {
        return Some(hit);
    }
    let value = compute()?;
    if current(path).as_deref() == Some(id.as_str()) {
        remember(key, value.to_string());
    }
    Some(value)
}

fn remember(key: String, value: String) {
    let mut guard = STATE.lock().unwrap_or_else(|e| e.into_inner());
    let state = guard.get_or_insert_with(|| State {
        store: Store::default(),
        path: None,
    });
    state.store.schema = SCHEMA;
    while state.store.entries.len() >= LIMIT {
        let Some(first) = state.store.entries.keys().next().cloned() else {
            break;
        };
        state.store.entries.remove(&first);
    }
    state.store.entries.insert(key, value);
    if let Some(path) = &state.path
        && let Ok(bytes) = serde_json::to_vec(&state.store)
    {
        // Write beside, then rename: a crash leaves the old store or the new one, never half of one.
        let temp = path.with_extension("json.tmp");
        if fs::write(&temp, bytes).is_ok() && fs::rename(&temp, path).is_err() {
            let _ = fs::remove_file(&temp);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn answers_are_remembered_until_the_file_changes() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("dxgi.dll");
        fs::write(&path, b"first").unwrap();
        let calls = std::cell::Cell::new(0);
        let read = || {
            file("len", &path, || {
                calls.set(calls.get() + 1);
                Some(fs::read(&path).ok()?.len())
            })
        };
        assert_eq!(read(), Some(5));
        assert_eq!(read(), Some(5));
        assert_eq!(calls.get(), 1, "the second answer comes from memory");
        std::thread::sleep(std::time::Duration::from_millis(20));
        fs::OpenOptions::new()
            .append(true)
            .open(&path)
            .unwrap()
            .write_all(b" and more")
            .unwrap();
        assert_eq!(read(), Some(14));
        assert_eq!(calls.get(), 2, "a changed file is read again");
    }

    #[cfg(unix)]
    #[test]
    fn links_are_never_remembered() {
        let temp = tempfile::tempdir().unwrap();
        let real = temp.path().join("real.dll");
        let link = temp.path().join("dxgi.dll");
        fs::write(&real, b"x").unwrap();
        std::os::unix::fs::symlink(&real, &link).unwrap();
        let calls = std::cell::Cell::new(0);
        for _ in 0..2 {
            file("len", &link, || {
                calls.set(calls.get() + 1);
                Some(1u8)
            });
        }
        assert_eq!(calls.get(), 2);
    }
}
