//! One selected target, three non-recursive watches. No polling of game/library trees.
use crate::model::Outcome;
use notify::{EventKind, RecursiveMode};
use notify_debouncer_full::{DebounceEventResult, new_debouncer};
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    sync::{
        Arc, Mutex,
        mpsc::{self, Sender},
    },
    time::{Duration, Instant},
};

pub(crate) const DEBOUNCE: Duration = Duration::from_millis(350);
const COOLDOWN: Duration = Duration::from_millis(250);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Changes {
    pub status: bool,
    pub ini: bool,
    pub logs: bool,
    pub captures: bool,
}
impl Changes {
    fn path(&mut self, target: &Path, path: &Path) {
        let Ok(relative) = path.strip_prefix(target) else {
            return;
        };
        let name = relative.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
        if name.is_empty() {
            *self = Self {
                status: true,
                ini: true,
                logs: true,
                captures: true,
            };
        } else if name == ".retoque" || name.starts_with(".retoque/") || name.ends_with(".dll") {
            self.status = true;
        } else if name == "optiscaler.ini" {
            self.ini = true;
        } else if name == "retoque-captures" || name.starts_with("retoque-captures/") {
            self.captures = true;
        } else if name.ends_with(".log") {
            self.logs = true;
        }
    }
}
#[derive(Clone, Copy)]
struct Window {
    start: Instant,
    end: Option<Instant>,
}
#[derive(Default)]
struct Suppression(HashMap<PathBuf, Vec<Window>>);
impl Suppression {
    fn begin(&mut self, target: &Path, now: Instant) {
        let windows = self.0.entry(target.to_owned()).or_default();
        windows.retain(|w| {
            w.end
                .is_none_or(|end| now.saturating_duration_since(end) < Duration::from_secs(5))
        });
        windows.push(Window { start: now, end: None });
    }
    fn end(&mut self, target: &Path, now: Instant) {
        if let Some(w) = self.0.get_mut(target).and_then(|w| w.last_mut()) {
            w.end = Some(now + COOLDOWN);
        }
    }
    fn contains(&self, target: &Path, at: Instant) -> bool {
        self.0.get(target).is_some_and(|windows| {
            windows
                .iter()
                .any(|w| at >= w.start && w.end.is_none_or(|end| at <= end))
        })
    }
}
enum Message {
    Select(u64, Option<PathBuf>),
    Events(u64, DebounceEventResult),
    Stop,
}
pub(crate) struct WatcherLane {
    tx: Sender<Message>,
    suppression: Arc<Mutex<Suppression>>,
}
impl WatcherLane {
    pub(crate) fn new(out: Sender<Outcome>, ctx: egui::Context) -> Self {
        let (tx, rx) = mpsc::channel();
        let events = tx.clone();
        let suppression = Arc::new(Mutex::new(Suppression::default()));
        let suppress = suppression.clone();
        let _ = std::thread::Builder::new()
            .name("retoque-watcher".into())
            .spawn(move || {
                let mut current: Option<(u64, PathBuf)> = None;
                let mut watcher = None;
                let mut watched = Vec::<PathBuf>::new();
                while let Ok(message) = rx.recv() {
                    match message {
                        Message::Stop => break,
                        Message::Select(generation, target) => {
                            watcher = None; // drop closes native handles before acknowledging the new selection
                            watched.clear();
                            current = target.map(|t| (generation, t));
                            if let Some((generation, target)) = &current {
                                let generation = *generation;
                                let events = events.clone();
                                let result = new_debouncer(DEBOUNCE, None, move |event| {
                                    let _ = events.send(Message::Events(generation, event));
                                })
                                .and_then(|mut w| {
                                    sync_watches(&mut w, target, &mut watched)?;
                                    Ok(w)
                                });
                                let result = match result {
                                    Ok(w) => {
                                        watcher = Some(w);
                                        Ok(Changes::default())
                                    }
                                    Err(e) => Err(e.to_string()),
                                };
                                let _ = out.send(Outcome::Watch {
                                    generation,
                                    target: target.clone(),
                                    result,
                                });
                                ctx.request_repaint();
                            }
                        }
                        Message::Events(generation, result) => {
                            let Some((active, target)) = &current else { continue };
                            if *active != generation || watcher.is_none() {
                                continue;
                            }
                            let result = result
                                .map_err(|errors| errors.iter().map(ToString::to_string).collect::<Vec<_>>().join("; "))
                                .and_then(|events| {
                                    sync_watches(watcher.as_mut().unwrap(), target, &mut watched)
                                        .map_err(|e| e.to_string())?;
                                    let mut changes = Changes::default();
                                    let guard = suppress.lock().unwrap_or_else(|e| e.into_inner());
                                    for event in events {
                                        // Reads by our own loaders must not cause feedback loops. Use event time,
                                        // not delivery time: the debouncer can deliver after apply has finished.
                                        if matches!(event.kind, EventKind::Access(_))
                                            || guard.contains(target, event.time)
                                        {
                                            continue;
                                        }
                                        for path in &event.paths {
                                            changes.path(target, path);
                                        }
                                    }
                                    Ok(changes)
                                });
                            if result.is_err() {
                                watcher = None;
                                watched.clear();
                            }
                            if !result.as_ref().is_ok_and(|c| *c == Changes::default()) {
                                let _ = out.send(Outcome::Watch {
                                    generation,
                                    target: target.clone(),
                                    result,
                                });
                                ctx.request_repaint();
                            }
                        }
                    }
                }
            });
        Self { tx, suppression }
    }
    pub(crate) fn select(&self, generation: u64, target: Option<PathBuf>) -> bool {
        self.tx.send(Message::Select(generation, target)).is_ok()
    }
    pub(crate) fn begin_write(&self, target: &Path) {
        self.suppression
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .begin(target, Instant::now());
    }
    pub(crate) fn end_write(&self, target: &Path) {
        self.suppression
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .end(target, Instant::now());
    }
}
impl Drop for WatcherLane {
    fn drop(&mut self) {
        let _ = self.tx.send(Message::Stop);
    }
}
fn sync_watches(
    watcher: &mut notify_debouncer_full::Debouncer<notify::RecommendedWatcher, notify_debouncer_full::RecommendedCache>,
    target: &Path,
    watched: &mut Vec<PathBuf>,
) -> notify::Result<()> {
    // A missing root means live coverage is lost. Do not silently claim to watch a deleted folder.
    if !target.is_dir() {
        return Err(notify::Error::path_not_found().add_path(target.to_owned()));
    }
    let candidates = [
        target.to_owned(),
        target.join(".retoque"),
        target.join("retoque-captures"),
    ];
    for path in candidates {
        let exists = std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_dir() && !m.file_type().is_symlink());
        if exists && !watched.contains(&path) {
            watcher.watch(&path, RecursiveMode::NonRecursive)?;
            watched.push(path);
        } else if !exists && watched.contains(&path) {
            let _ = watcher.unwatch(&path);
            watched.retain(|p| p != &path);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn classification_is_scoped_and_specific() {
        let target = Path::new("/synthetic/game");
        let mut c = Changes::default();
        c.path(target, &target.join("OptiScaler.ini"));
        assert_eq!(
            c,
            Changes {
                ini: true,
                ..Default::default()
            }
        );
        c.path(target, Path::new("/synthetic/another/OptiScaler.log"));
        c.path(target, &target.join("unrelated.txt"));
        assert!(!c.logs && !c.status && !c.captures);
        c.path(target, &target.join(".retoque/receipt.txt"));
        c.path(target, &target.join("retoque-captures/frame.json"));
        c.path(target, &target.join("OptiScaler.1.log"));
        assert!(c.status && c.logs && c.captures);
    }
    #[test]
    fn suppression_uses_event_time_and_includes_cooldown() {
        let temp = tempfile::tempdir().unwrap();
        let mut guard = Suppression::default();
        let start = Instant::now();
        let end = start + Duration::from_millis(20);
        guard.begin(temp.path(), start);
        assert!(guard.contains(temp.path(), start + Duration::from_millis(1)));
        guard.end(temp.path(), end);
        assert!(guard.contains(temp.path(), end + COOLDOWN));
        assert!(!guard.contains(temp.path(), end + COOLDOWN + Duration::from_nanos(1)));
        assert!(!guard.contains(temp.path(), start - Duration::from_nanos(1)));
        assert!(!guard.contains(&temp.path().join("other"), end));
        // A callback delivered 350 ms later still filters the timestamp from the self-write.
        assert!(guard.contains(temp.path(), start + Duration::from_millis(10)));
    }
    fn next(rx: &mpsc::Receiver<Outcome>) -> (u64, PathBuf, Result<Changes, String>) {
        match rx.recv_timeout(Duration::from_secs(5)).unwrap() {
            Outcome::Watch {
                generation,
                target,
                result,
            } => (generation, target, result),
            _ => panic!("unexpected watcher event"),
        }
    }
    #[test]
    fn temp_directory_debounce_self_writes_and_deselect() {
        let temp = tempfile::tempdir().unwrap();
        let (tx, rx) = mpsc::channel();
        let lane = WatcherLane::new(tx, egui::Context::default());
        lane.select(1, Some(temp.path().into()));
        assert_eq!(next(&rx).2.unwrap(), Changes::default());
        let start = Instant::now();
        for value in ["one", "two", "three"] {
            std::fs::write(temp.path().join("OptiScaler.ini"), value).unwrap();
        }
        let (generation, _, result) = next(&rx);
        assert_eq!(generation, 1);
        assert!(result.unwrap().ini);
        assert!(start.elapsed() >= DEBOUNCE);
        assert!(rx.recv_timeout(DEBOUNCE * 2).is_err(), "one batch for the burst");
        lane.begin_write(temp.path());
        std::fs::write(temp.path().join("OptiScaler.ini"), "ours").unwrap();
        lane.end_write(temp.path());
        assert!(
            rx.recv_timeout(DEBOUNCE * 3).is_err(),
            "self-write must stay suppressed after delivery delay"
        );
        std::fs::create_dir(temp.path().join("retoque-captures")).unwrap();
        assert!(next(&rx).2.unwrap().captures);
        std::fs::write(temp.path().join("retoque-captures/pair.json"), "{}").unwrap();
        assert!(next(&rx).2.unwrap().captures, "attach a child watch when it appears");
        lane.select(2, None);
        // A following selection's acknowledgement fences the deselect message without sleeping.
        let other = tempfile::tempdir().unwrap();
        lane.select(3, Some(other.path().into()));
        assert_eq!(next(&rx).0, 3);
        std::fs::write(temp.path().join("OptiScaler.ini"), "old target").unwrap();
        assert!(rx.recv_timeout(DEBOUNCE * 2).is_err());
        lane.select(4, Some(other.path().join("missing")));
        assert!(
            next(&rx).2.is_err(),
            "registration failures require a visible manual fallback"
        );
    }
}
