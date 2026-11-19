//! Bounded worker lanes, shared by the shell and tabs. Selected-game reads carry an epoch;
//! writes and global discovery always finish even after the selection changes. Consented downloads each get
//! their own thread: they wait on the network for minutes and must not hold up local work or one another.
use crate::model::Outcome;
use std::path::PathBuf;
use std::sync::{
    Arc,
    atomic::{AtomicBool, AtomicU64, Ordering},
    mpsc::{Receiver, Sender, channel},
};
use std::time::{Duration, Instant};

/// A log that keeps changing is read again at most this often.
pub(crate) const LIVE_LOG_INTERVAL: Duration = Duration::from_secs(2);

/// The selected game's log, read again because the watcher saw it change. The watcher reports a running game's log
/// after every pause in its writing (`watcher::DEBOUNCE`): about every two seconds with the runtime's status lines,
/// more often with others between them. These reads run one at a time, the next no sooner than
/// `LIVE_LOG_INTERVAL` after the last began, and each one's answer is delivered. Asked for as ordinary "logs" jobs,
/// every report read the log again and superseded the read before it, so a log that takes longer to read than the
/// game pauses was read over and over beside the game and never shown.
struct LiveLog {
    wanted: Option<PathBuf>,
    reading: Arc<AtomicBool>,
    began: Option<Instant>,
    interval: Duration,
}
impl Default for LiveLog {
    fn default() -> Self {
        Self {
            wanted: None,
            reading: Default::default(),
            began: None,
            interval: LIVE_LOG_INTERVAL,
        }
    }
}
/// Clears the flag when the read ends, or when it never ran (no worker).
struct Reading(Arc<AtomicBool>);
impl Drop for Reading {
    fn drop(&mut self) {
        self.0.store(false, Ordering::Release);
    }
}

#[derive(Clone, Copy, Debug)]
pub enum Lane {
    Fast,
    Heavy,
    Net,
    Watcher,
}
type Work = Box<dyn FnOnce() + Send>;

/// Serialize catalog snapshots, not downloads. Publication order follows the reads, not thread delivery.
#[derive(Clone, Default)]
pub(crate) struct CatalogPublications(Arc<std::sync::Mutex<u64>>);
impl CatalogPublications {
    pub(crate) fn snapshot(&self, read: impl FnOnce() -> Outcome) -> Outcome {
        let mut revision = self.0.lock().unwrap_or_else(|e| e.into_inner());
        match read() {
            Outcome::Catalog { result } => {
                *revision += 1;
                Outcome::CatalogSnapshot {
                    revision: *revision,
                    result,
                }
            }
            other => other,
        }
    }
}

pub struct TaskHub {
    tx: Sender<Outcome>,
    watcher: crate::watcher::WatcherLane,
    watch_generation: u64,
    rx: Receiver<Outcome>,
    fast: Sender<Work>,
    heavy: Sender<Work>,
    ctx: egui::Context,
    running: usize,
    next_request: u64,
    requests: Arc<std::sync::Mutex<std::collections::HashMap<&'static str, u64>>>,
    epoch: Arc<AtomicU64>,
    pub(crate) catalog_publications: CatalogPublications,
    catalog_received: u64,
    live_log: LiveLog,
}

impl TaskHub {
    pub fn new(ctx: egui::Context) -> Self {
        let (tx, rx) = channel();
        Self {
            watcher: crate::watcher::WatcherLane::new(tx.clone(), ctx.clone()),
            watch_generation: 0,
            tx,
            rx,
            fast: lane("fast"),
            heavy: lane("heavy"),
            ctx,
            running: 0,
            next_request: 0,
            requests: Default::default(),
            epoch: Arc::new(AtomicU64::new(0)),
            catalog_publications: Default::default(),
            catalog_received: 0,
            live_log: Default::default(),
        }
    }
    /// The watcher saw the selected game's log change: read it again, now or when the last read allows.
    pub(crate) fn log_changed(&mut self, target: PathBuf) {
        self.live_log.wanted = Some(target);
        self.read_live_log();
    }
    fn read_live_log(&mut self) {
        if self.live_log.wanted.is_none() || self.live_log.reading.load(Ordering::Acquire) {
            // A read that is running repaints when it ends, and `drain` comes back here.
            return;
        }
        if let Some(wait) = self
            .live_log
            .began
            .and_then(|began| self.live_log.interval.checked_sub(began.elapsed()))
            .filter(|wait| !wait.is_zero())
        {
            self.ctx.request_repaint_after(wait);
            return;
        }
        let Some(target) = self.live_log.wanted.take() else {
            return;
        };
        self.live_log.began = Some(Instant::now());
        self.live_log.reading.store(true, Ordering::Release);
        let reading = Reading(self.live_log.reading.clone());
        // Not a tracked "logs" request: the Fast lane runs in order, so this answer can only come before that of a
        // read asked for later, and it is never dropped for one. The view takes it only for the selected folder.
        self.submit(Lane::Fast, false, "logs-live", None, move |_| {
            let _reading = reading;
            crate::model::logs(target)
        });
    }
    pub(crate) fn watch(&mut self, target: Option<PathBuf>) {
        // Another selection: a read still waiting for its turn was for the folder the user left.
        self.live_log.wanted = None;
        self.watch_generation += 1;
        if !self.watcher.select(self.watch_generation, target.clone())
            && let Some(target) = target
        {
            let _ = self.tx.send(Outcome::Watch {
                generation: self.watch_generation,
                target,
                result: Err("watcher worker unavailable".into()),
            });
        }
    }
    pub(crate) fn begin_write(&self, target: &std::path::Path) {
        self.watcher.begin_write(target);
    }
    pub(crate) fn end_write(&self, target: &std::path::Path) {
        self.watcher.end_write(target);
    }
    pub fn advance_epoch(&mut self) {
        self.epoch.fetch_add(1, Ordering::Relaxed);
    }
    pub fn busy(&self) -> bool {
        self.running > 0
    }
    pub fn spawn(
        &mut self,
        name: &'static str,
        target: Option<PathBuf>,
        work: impl FnOnce(&Sender<Outcome>) -> Outcome + Send + 'static,
    ) {
        let lane = match name {
            "status" | "ini" | "logs" | "model-usage" | "doctor" => Lane::Fast,
            "dlss-download" | "nr-model-fetch" | "sm86-download" | "payload-fetch" | "reshade-download" => Lane::Net,
            _ => Lane::Heavy,
        };
        let scoped = matches!(
            name,
            "doctor" | "status" | "ini" | "logs" | "plan" | "autotune-plan" | "autotune-facts" | "model-inspection"
        );
        self.submit(lane, scoped, name, target, work);
    }
    /// Short background work. Native watches live in the dedicated watcher lane.
    pub fn submit(
        &mut self,
        lane: Lane,
        scoped: bool,
        name: &'static str,
        target: Option<PathBuf>,
        work: impl FnOnce(&Sender<Outcome>) -> Outcome + Send + 'static,
    ) {
        self.running += 1;
        self.next_request += 1;
        let request = self.next_request;
        let tracked = scoped || matches!(name, "payload" | "model-usage" | "library-status" | "catalog");
        if tracked {
            self.requests
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .insert(name, request);
        }
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        let epoch = self.epoch.load(Ordering::Relaxed);
        let current = self.epoch.clone();
        let current_requests = self.requests.clone();
        let failed_target = target.clone();
        let catalog_publications = self.catalog_publications.clone();
        let job = Box::new(move || {
            let started = std::time::Instant::now();
            let newest =
                !tracked || current_requests.lock().unwrap_or_else(|e| e.into_inner()).get(name) == Some(&request);
            if newest && (!scoped || current.load(Ordering::Relaxed) == epoch) {
                let run = || {
                    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&tx))).unwrap_or_else(|panic| {
                        Outcome::Failed {
                            job: name,
                            target: failed_target,
                            message: panic_text(panic.as_ref()),
                        }
                    })
                };
                // Mutations must execute even when another read was requested after them.
                let outcome = if matches!(name, "catalog" | "import" | "remove") {
                    catalog_publications.snapshot(run)
                } else {
                    run()
                };
                let failed = match &outcome {
                    Outcome::Failed { message, .. } => Some(message.as_str()),
                    _ => None,
                };
                crate::applog::job(name, started.elapsed(), failed);
                let outcome = if tracked {
                    Outcome::Scoped {
                        epoch: scoped.then_some(epoch),
                        request,
                        name,
                        outcome: Box::new(outcome),
                    }
                } else {
                    outcome
                };
                let _ = tx.send(outcome);
            }
            let _ = tx.send(Outcome::Finished);
            ctx.request_repaint();
        });
        let sent = match lane {
            Lane::Fast | Lane::Watcher => self.fast.send(job).is_ok(),
            Lane::Heavy => self.heavy.send(job).is_ok(),
            // Each user-confirmed download is its own short-lived thread; a busy flag per kind prevents repeats.
            Lane::Net => std::thread::Builder::new()
                .name(format!("retoque-net-{name}"))
                .spawn(job)
                .is_ok(),
        };
        if !sent {
            let _ = self.tx.send(Outcome::Failed {
                job: name,
                target,
                message: "worker unavailable".into(),
            });
            let _ = self.tx.send(Outcome::Finished);
            self.ctx.request_repaint();
        }
    }
    pub fn drain(&mut self) -> Vec<Outcome> {
        let mut out = Vec::new();
        while let Ok(o) = self.rx.try_recv() {
            match o {
                Outcome::Finished => self.running = self.running.saturating_sub(1),
                Outcome::Scoped {
                    epoch,
                    request,
                    name,
                    outcome,
                } => {
                    if epoch.is_none_or(|epoch| epoch == self.epoch.load(Ordering::Relaxed))
                        && self.requests.lock().unwrap_or_else(|e| e.into_inner()).get(name) == Some(&request)
                    {
                        out.extend(self.accept_catalog(*outcome));
                    }
                }
                Outcome::Watch { generation, .. } if generation != self.watch_generation => {}
                other => out.extend(self.accept_catalog(other)),
            }
        }
        self.read_live_log();
        out
    }
    fn accept_catalog(&mut self, outcome: Outcome) -> Option<Outcome> {
        match outcome {
            Outcome::CatalogSnapshot { revision, result } => {
                // Compare with delivered lists: a newer snapshot still in transit must not hide a completion's list.
                if revision <= self.catalog_received {
                    return None;
                }
                self.catalog_received = revision;
                Some(Outcome::Catalog { result })
            }
            other => Some(other),
        }
    }
}
fn lane(name: &str) -> Sender<Work> {
    let (tx, rx) = channel::<Work>();
    // Spawn failure drops rx: every submitted operation becomes a visible Failed event.
    let _ = std::thread::Builder::new()
        .name(format!("retoque-{name}"))
        .spawn(move || {
            while let Ok(job) = rx.recv() {
                job();
            }
        });
    tx
}
fn panic_text(panic: &(dyn std::any::Any + Send)) -> String {
    if let Some(s) = panic.downcast_ref::<&str>() {
        (*s).to_string()
    } else if let Some(s) = panic.downcast_ref::<String>() {
        s.clone()
    } else {
        "the job stopped unexpectedly".to_string()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{Duration, Instant};
    fn until(hub: &mut TaskHub, predicate: impl Fn(&[Outcome]) -> bool) -> Vec<Outcome> {
        let start = Instant::now();
        let mut events = Vec::new();
        loop {
            events.extend(hub.drain());
            if predicate(&events) {
                return events;
            }
            assert!(start.elapsed() < Duration::from_secs(3));
            std::thread::yield_now();
        }
    }
    fn log_reads(events: &[Outcome]) -> Vec<usize> {
        events
            .iter()
            .filter_map(|event| match event {
                Outcome::Logs {
                    result: Ok(Some(summary)),
                    ..
                } => Some(summary.error_count),
                _ => None,
            })
            .collect()
    }
    #[test]
    fn a_log_that_keeps_changing_is_read_one_at_a_time_and_each_answer_arrives() {
        use std::io::Write;
        let temp = tempfile::tempdir().unwrap();
        let log = temp.path().join("OptiScaler.log");
        std::fs::write(&log, "[12:00:00.000000] [E] first\n").unwrap();
        let mut hub = TaskHub::new(egui::Context::default());
        hub.live_log.interval = Duration::from_millis(60);
        // The game writes many times a second: every event asks, one read starts, and the game writes on.
        for _ in 0..20 {
            hub.log_changed(temp.path().into());
        }
        std::fs::OpenOptions::new()
            .append(true)
            .open(&log)
            .unwrap()
            .write_all(b"[12:00:01.000000] [E] second\n")
            .unwrap();
        // The read that was running answers (it is not dropped for the requests that came after it), and those
        // requests are one more read, after the interval, with what was added.
        let mut reads = Vec::new();
        while reads.len() < 2 {
            reads.extend(log_reads(&until(&mut hub, |e| !log_reads(e).is_empty())));
        }
        assert!(matches!(reads[..], [1 | 2, 2]), "{reads:?}");
        // And then nothing: twenty events were two reads.
        let quiet = Instant::now();
        while quiet.elapsed() < Duration::from_millis(200) {
            assert!(log_reads(&hub.drain()).is_empty(), "a read nobody asked for");
            std::thread::sleep(Duration::from_millis(5));
        }
        assert!(hub.live_log.wanted.is_none());
    }
    #[test]
    fn a_log_read_waiting_for_its_turn_is_dropped_when_the_selection_changes() {
        let temp = tempfile::tempdir().unwrap();
        std::fs::write(temp.path().join("OptiScaler.log"), "[12:00:00.000000] [E] first\n").unwrap();
        let mut hub = TaskHub::new(egui::Context::default());
        hub.live_log.interval = Duration::from_millis(60);
        hub.log_changed(temp.path().into());
        hub.log_changed(temp.path().into());
        assert_eq!(log_reads(&until(&mut hub, |e| !log_reads(e).is_empty())), [1]);
        hub.watch(None);
        let began = Instant::now();
        while began.elapsed() < Duration::from_millis(200) {
            assert!(log_reads(&hub.drain()).is_empty(), "read for a folder the user left");
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    #[test]
    fn heavy_work_does_not_block_fast_reads_and_panics_are_visible() {
        let mut hub = TaskHub::new(egui::Context::default());
        let (release, blocked) = channel();
        hub.spawn("hash", None, move |_| {
            blocked.recv().unwrap();
            Outcome::Catalog { result: Ok(vec![]) }
        });
        hub.spawn("status", None, |_| panic!("synthetic fast failure"));
        let events = until(&mut hub, |e| !e.is_empty());
        assert!(matches!(&events[0], Outcome::Failed { message, .. } if message == "synthetic fast failure"));
        release.send(()).unwrap();
        until(&mut hub, |e| e.iter().any(|e| matches!(e, Outcome::Catalog { .. })));
    }
    #[test]
    fn catalog_snapshot_in_transit_does_not_suppress_an_earlier_delivery() {
        let mut hub = TaskHub::new(egui::Context::default());
        let first = hub
            .catalog_publications
            .snapshot(|| Outcome::Catalog { result: Ok(vec![]) });
        let later = hub.catalog_publications.snapshot(|| Outcome::Catalog {
            result: Err("later read failed".into()),
        });
        hub.tx.send(first).unwrap();
        assert!(matches!(hub.drain().as_slice(), [Outcome::Catalog { result: Ok(_) }]));
        hub.tx.send(later).unwrap();
        assert!(matches!(hub.drain().as_slice(), [Outcome::Catalog { result: Err(_) }]));
    }
    #[test]
    fn local_catalog_mutations_supersede_late_net_lists_but_keep_completion() {
        for name in ["import", "remove", "catalog"] {
            let mut hub = TaskHub::new(egui::Context::default());
            let stale = hub.catalog_publications.snapshot(|| Outcome::Catalog {
                result: Err("stale catalog".into()),
            });
            let (started, ready) = channel();
            let (release, blocked) = channel();
            hub.spawn("nr-model-fetch", None, move |tx| {
                started.send(()).unwrap();
                blocked.recv_timeout(Duration::from_secs(3)).unwrap();
                tx.send(stale).unwrap();
                Outcome::NrFetch(crate::tabs::model::NrFetchEvent::Downloaded(Err(
                    "download completion".into()
                )))
            });
            ready.recv_timeout(Duration::from_secs(3)).unwrap();
            hub.spawn(name, None, |_| Outcome::Catalog { result: Ok(vec![]) });
            until(&mut hub, |events| {
                events.iter().any(|o| matches!(o, Outcome::Catalog { result: Ok(_) }))
            });
            release.send(()).unwrap();
            let events = until(&mut hub, |events| {
                events.iter().any(|o| matches!(o, Outcome::NrFetch(_)))
            });
            assert!(
                events.iter().any(|o| matches!(o, Outcome::NrFetch(_))),
                "completion must survive {name}"
            );
            assert!(
                !events.iter().any(|o| matches!(o, Outcome::Catalog { .. })),
                "late list survived {name}"
            );
        }
    }
    #[test]
    fn a_slow_download_holds_up_neither_heavy_work_nor_another_download() {
        let mut hub = TaskHub::new(egui::Context::default());
        let (release_dlss, dlss_blocked) = channel();
        let (release_model, model_blocked) = channel();
        hub.spawn("dlss-download", None, move |_| {
            dlss_blocked.recv().unwrap();
            Outcome::Catalog { result: Ok(vec![]) }
        });
        hub.spawn("nr-model-fetch", None, move |_| {
            model_blocked.recv().unwrap();
            Outcome::Catalog { result: Ok(vec![]) }
        });
        hub.spawn("hash", None, |_| panic!("heavy work ran"));
        let events = until(&mut hub, |e| !e.is_empty());
        assert!(matches!(&events[0], Outcome::Failed { job: "hash", .. }));
        assert!(hub.busy());
        release_model.send(()).unwrap();
        until(&mut hub, |e| e.iter().any(|e| matches!(e, Outcome::Catalog { .. })));
        release_dlss.send(()).unwrap();
        until(&mut hub, |e| e.iter().any(|e| matches!(e, Outcome::Catalog { .. })));
    }
    #[test]
    fn old_selection_result_is_dropped_but_write_completion_survives() {
        let mut hub = TaskHub::new(egui::Context::default());
        let (ready, started) = channel();
        let (release, blocked) = channel();
        hub.spawn("status", None, move |_| {
            ready.send(()).unwrap();
            blocked.recv().unwrap();
            Outcome::Catalog { result: Ok(vec![]) }
        });
        started.recv_timeout(Duration::from_secs(3)).unwrap();
        hub.advance_epoch();
        hub.advance_epoch(); // A -> B -> A is still a new selection.
        release.send(()).unwrap();
        hub.spawn("apply", None, |_| Outcome::Failed {
            job: "apply",
            target: None,
            message: "write completion".into(),
        });
        let events = until(&mut hub, |e| {
            e.iter().any(|e| matches!(e, Outcome::Failed { job: "apply", .. }))
        });
        assert!(!events.iter().any(|e| matches!(e, Outcome::Catalog { .. })));
    }
    #[test]
    fn newer_request_on_same_selection_supersedes_older_read() {
        let mut hub = TaskHub::new(egui::Context::default());
        let (ready, started) = channel();
        let (release, blocked) = channel();
        hub.spawn("logs", None, move |_| {
            ready.send(()).unwrap();
            blocked.recv().unwrap();
            Outcome::Failed {
                job: "old",
                target: None,
                message: "old".into(),
            }
        });
        started.recv_timeout(Duration::from_secs(3)).unwrap();
        hub.spawn("logs", None, |_| Outcome::Failed {
            job: "new",
            target: None,
            message: "new".into(),
        });
        release.send(()).unwrap();
        let events = until(&mut hub, |e| {
            e.iter().any(|e| matches!(e, Outcome::Failed { job: "new", .. }))
        });
        assert!(!events.iter().any(|e| matches!(e, Outcome::Failed { job: "old", .. })));
    }
}
