//! Bounded worker lanes, shared by the shell and tabs. Selected-game reads carry an epoch;
//! writes and global discovery always finish even after the selection changes. Consented downloads each get
//! their own thread: they wait on the network for minutes and must not hold up local work or one another.
use crate::model::Outcome;
use std::path::PathBuf;
use std::sync::{
    Arc,
    atomic::{AtomicU64, Ordering},
    mpsc::{Receiver, Sender, channel},
};

#[derive(Clone, Copy, Debug)]
pub enum Lane {
    Fast,
    Heavy,
    Net,
    Watcher,
}
type Work = Box<dyn FnOnce() + Send>;

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
        }
    }
    pub(crate) fn watch(&mut self, target: Option<PathBuf>) {
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
        let job = Box::new(move || {
            let newest =
                !tracked || current_requests.lock().unwrap_or_else(|e| e.into_inner()).get(name) == Some(&request);
            if newest && (!scoped || current.load(Ordering::Relaxed) == epoch) {
                let outcome =
                    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&tx))).unwrap_or_else(|panic| {
                        Outcome::Failed {
                            job: name,
                            target: failed_target,
                            message: panic_text(panic.as_ref()),
                        }
                    });
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
                        out.push(*outcome);
                    }
                }
                Outcome::Watch { generation, .. } if generation != self.watch_generation => {}
                other => out.push(other),
            }
        }
        out
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
