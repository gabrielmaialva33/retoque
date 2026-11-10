//! What a model file says about itself (PE version, CUDA targets), remembered by file identity so the catalog and
//! the selected game do not re-read hundreds of MB on every start. The identity is the path, size and
//! modification time, plus inode, device and change time on Unix (which an ordinary write cannot restore). Any
//! change re-reads the file. The facts are evidence for the interface and recommendations, never permission to
//! load anything: the game loads the file itself, and installs verify their own hashes.
//!
//! Remembered in memory always; written to disk only after the app calls [`persist_in`], so tests and library
//! users never write to the user's data folder.
use crate::model_compat::{self, ModelCompatibility};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeMap,
    fs::File,
    io::{self, Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::Mutex,
};

const STORE: &str = "model-facts.json";
/// Bumped whenever the inspectors change what they report, so older answers are not reused.
const SCHEMA: u32 = 1;
const LIMIT: usize = 64;

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct Facts {
    pub version: Option<String>,
    pub archs: Vec<u16>,
    pub known: bool,
}
impl Facts {
    pub fn compatibility(&self) -> ModelCompatibility {
        ModelCompatibility {
            archs: self.archs.clone(),
            known: self.known,
        }
    }
}

#[derive(Default, Serialize, Deserialize)]
struct Store {
    schema: u32,
    entries: BTreeMap<String, Facts>,
}
struct State {
    store: Store,
    path: Option<PathBuf>,
}
static STATE: Mutex<Option<State>> = Mutex::new(None);

/// Loads and keeps writing the remembered facts in `dir`. A missing, damaged or older store starts empty.
pub fn persist_in(dir: &Path) {
    let path = dir.join(STORE);
    let store = std::fs::read(&path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Store>(&bytes).ok())
        .filter(|s| s.schema == SCHEMA)
        .unwrap_or_default();
    *STATE.lock().unwrap_or_else(|e| e.into_inner()) = Some(State {
        store,
        path: Some(path),
    });
}

/// Facts for the open `file` found at `path`. Reads the file only when its identity is new.
pub fn read(path: &Path, file: &mut File) -> io::Result<Facts> {
    let key = identity(path, &file.metadata()?);
    if let Some(key) = &key
        && let Some(hit) = STATE
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .and_then(|s| s.store.entries.get(key).cloned())
    {
        return Ok(hit);
    }
    let compatibility = model_compat::inspect(file)?;
    file.seek(SeekFrom::Start(0))?;
    let facts = Facts {
        version: scan_version(&mut Read::by_ref(file).take(model_compat::MAX_MODEL_BYTES))?,
        archs: compatibility.archs,
        known: compatibility.known,
    };
    // The file must not have changed while it was read; otherwise answer without remembering.
    let unchanged = |k: &String| file.metadata().ok().and_then(|m| identity(path, &m)).as_ref() == Some(k);
    if let Some(key) = key.filter(unchanged) {
        remember(key, facts.clone());
    }
    Ok(facts)
}

fn remember(key: String, facts: Facts) {
    let mut guard = STATE.lock().unwrap_or_else(|e| e.into_inner());
    let state = guard.get_or_insert_with(|| State {
        store: Store::default(),
        path: None,
    });
    state.store.schema = SCHEMA;
    // Entries for files that moved or changed are never looked up again; drop the oldest-sorted ones at the bound.
    while state.store.entries.len() >= LIMIT {
        let Some(first) = state.store.entries.keys().next().cloned() else {
            break;
        };
        state.store.entries.remove(&first);
    }
    state.store.entries.insert(key, facts);
    if let Some(path) = &state.path
        && let Ok(bytes) = serde_json::to_vec(&state.store)
    {
        // Write beside, then rename: a crash leaves the old store or the new one, never half of one.
        let temp = path.with_extension("json.tmp");
        if std::fs::write(&temp, bytes).is_ok() && std::fs::rename(&temp, path).is_err() {
            let _ = std::fs::remove_file(&temp);
        }
    }
}

fn identity(path: &Path, meta: &std::fs::Metadata) -> Option<String> {
    retoque_core::memo::identity(path, meta)
}

/// The PE file version, streamed in 64 KiB blocks with enough overlap to find a record across a boundary.
pub fn scan_version<R: Read>(r: &mut R) -> io::Result<Option<String>> {
    const SIGNATURE: [u8; 8] = [0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0];
    let mut block = [0; 65536];
    let mut tail = Vec::new();
    let mut version = None;
    loop {
        let count = r.read(&mut block)?;
        if count == 0 {
            return Ok(version);
        }
        tail.extend_from_slice(&block[..count]);
        if tail.windows(8).any(|s| s == SIGNATURE) {
            version = crate::models::pe_file_version(&tail);
        }
        if tail.len() > 15 {
            tail.drain(..tail.len() - 15);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn version_record(ms: u32, ls: u32) -> Vec<u8> {
        let mut b = vec![0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0];
        b.extend(ms.to_le_bytes());
        b.extend(ls.to_le_bytes());
        b
    }

    #[test]
    fn remembered_until_the_file_changes() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("nvngx_dlssnr.dll");
        let mut bytes = vec![0u8; 70_000];
        bytes.extend(version_record(310 << 16 | 9, 16 << 16));
        std::fs::write(&path, &bytes).unwrap();
        let first = read(&path, &mut File::open(&path).unwrap()).unwrap();
        assert_eq!(first.version.as_deref(), Some("310.9.16.0"));
        let key = identity(&path, &std::fs::metadata(&path).unwrap()).unwrap();
        assert_eq!(
            STATE.lock().unwrap().as_ref().unwrap().store.entries.get(&key),
            Some(&first)
        );

        // A rewrite changes the identity, so the new content is read instead of the remembered answer.
        std::thread::sleep(std::time::Duration::from_millis(20));
        let mut file = std::fs::OpenOptions::new().append(true).open(&path).unwrap();
        file.write_all(&version_record(310 << 16 | 10, 0)).unwrap();
        drop(file);
        let second = read(&path, &mut File::open(&path).unwrap()).unwrap();
        assert_eq!(second.version.as_deref(), Some("310.10.0.0"));
    }

    #[test]
    fn a_damaged_or_older_store_starts_empty() {
        let temp = tempfile::tempdir().unwrap();
        std::fs::write(temp.path().join(STORE), b"{not json").unwrap();
        let parsed = std::fs::read(temp.path().join(STORE))
            .ok()
            .and_then(|b| serde_json::from_slice::<Store>(&b).ok())
            .filter(|s| s.schema == SCHEMA);
        assert!(parsed.is_none());
        let old = serde_json::to_vec(&Store {
            schema: SCHEMA + 1,
            entries: BTreeMap::new(),
        })
        .unwrap();
        assert!(
            serde_json::from_slice::<Store>(&old)
                .ok()
                .filter(|s| s.schema == SCHEMA)
                .is_none()
        );
    }
}
