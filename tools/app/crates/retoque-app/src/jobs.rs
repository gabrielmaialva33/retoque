//! Work that touches the disk runs on its own thread so the window never stalls; its result comes back through a
//! channel and wakes the UI. A panic in a job, or a thread the OS refuses, becomes a `Failed` outcome naming the
//! folder it was about, so the app can release that folder and reload it instead of staying busy.

use std::path::PathBuf;
use std::sync::mpsc::{Receiver, Sender, channel};

use crate::model::Outcome;

pub struct Jobs {
    tx: Sender<Outcome>,
    rx: Receiver<Outcome>,
    ctx: egui::Context,
    running: usize,
}

impl Jobs {
    pub fn new(ctx: egui::Context) -> Jobs {
        let (tx, rx) = channel();
        Jobs {
            tx,
            rx,
            ctx,
            running: 0,
        }
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
        self.running += 1;
        let tx = self.tx.clone();
        let ctx = self.ctx.clone();
        let failed_target = target.clone();
        let spawned = std::thread::Builder::new()
            .name(format!("retoque-{name}"))
            .spawn(move || {
                let progress = tx.clone();
                let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| work(&progress)))
                    .unwrap_or_else(|panic| Outcome::Failed {
                        job: name,
                        target: failed_target,
                        message: panic_text(panic.as_ref()),
                    });
                let _ = tx.send(outcome);
                let _ = tx.send(Outcome::Finished);
                ctx.request_repaint();
            });
        if let Err(e) = spawned {
            let _ = self.tx.send(Outcome::Failed {
                job: name,
                target,
                message: format!("no worker thread: {e}"),
            });
            let _ = self.tx.send(Outcome::Finished);
        }
    }

    /// Everything that arrived since the last frame. `Finished` only balances the running count.
    pub fn drain(&mut self) -> Vec<Outcome> {
        let mut out = Vec::new();
        while let Ok(o) = self.rx.try_recv() {
            if matches!(o, Outcome::Finished) {
                self.running = self.running.saturating_sub(1);
            } else {
                out.push(o);
            }
        }
        out
    }
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
