//! Bounded pipes and asynchronous lifecycle. No I/O or waits on the drawing thread.
use super::protocol::{self, Command, Event, Phase, Problem, State};
use std::{
    io::{self, BufReader, Read, Write},
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};

pub(super) trait Child: Send {
    fn try_wait(&mut self) -> io::Result<Option<i32>>;
    fn kill_wait(&mut self) -> io::Result<i32>;
}
pub(super) struct Spawned {
    pub child: Box<dyn Child>,
    pub input: Box<dyn Write + Send>,
    pub output: Box<dyn Read + Send>,
    // Kept until the process has been reaped, before removing the private files.
    pub folder: Option<tempfile::TempDir>,
}
#[derive(Clone, Copy)]
pub(super) struct Limits {
    pub start: Duration,
    pub stop: Duration,
}
impl Default for Limits {
    fn default() -> Self {
        Self {
            start: Duration::from_secs(80),
            stop: Duration::from_secs(3),
        }
    }
}
enum Message {
    Event(Event),
    Failure(Problem, String),
    Exit(Option<i32>, bool),
}
pub(crate) struct Session {
    stop: Arc<AtomicBool>,
    done: Arc<AtomicBool>,
    overflow: Arc<AtomicBool>,
    rx: mpsc::Receiver<Message>,
}
impl Drop for Session {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
    }
}
impl Session {
    pub(super) fn start(
        factory: impl FnOnce(&AtomicBool) -> io::Result<Spawned> + Send + 'static,
        revision: u64,
        limits: Limits,
    ) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let done = Arc::new(AtomicBool::new(false));
        let overflow = Arc::new(AtomicBool::new(false));
        let (tx, rx) = mpsc::sync_channel(256);
        let (s, d, o) = (stop.clone(), done.clone(), overflow.clone());
        std::thread::spawn(move || {
            struct Done(Arc<AtomicBool>);
            impl Drop for Done {
                fn drop(&mut self) {
                    self.0.store(true, Ordering::Release);
                }
            }
            let _done = Done(d);
            let send = |m| {
                if tx.try_send(m).is_err() {
                    o.store(true, Ordering::Release);
                }
            };
            if s.load(Ordering::Acquire) {
                send(Message::Exit(Some(0), true));
                return;
            }
            let spawned = match factory(&s) {
                Ok(p) => p,
                Err(e) => {
                    if s.load(Ordering::Acquire) {
                        send(Message::Exit(Some(0), true));
                    } else {
                        send(Message::Failure(Problem::Files, e.to_string()));
                    }
                    return;
                }
            };
            let Spawned {
                mut child,
                mut input,
                output,
                folder,
            } = spawned;
            let (lines_tx, lines_rx) = mpsc::sync_channel(64);
            let pipe_overflow = Arc::new(AtomicBool::new(false));
            let po = pipe_overflow.clone();
            let reader = std::thread::spawn(move || {
                let mut reader = BufReader::new(output);
                loop {
                    let event = protocol::read_event(&mut reader);
                    let terminal = !matches!(event, Ok(Some(_)));
                    if lines_tx.try_send(event).is_err() {
                        po.store(true, Ordering::Release);
                        break;
                    }
                    if terminal {
                        break;
                    }
                }
            });
            let started = Instant::now();
            let mut ready = false;
            let mut stopping = None;
            let mut fatal = false;
            let mut eof = false;
            let mut status_sent = false;
            let mut exit_seen = None;
            let exit = loop {
                // Bound each iteration so a flooding child cannot starve cancellation or timeout handling.
                for _ in 0..64 {
                    match lines_rx.try_recv() {
                        Ok(Ok(Some(e))) => {
                            if matches!(e, Event::Started { .. } | Event::Catalog { .. }) {
                                ready = true;
                            }
                            if matches!(e, Event::RestartRequired { .. } | Event::Refused { .. }) {
                                fatal = true;
                            }
                            send(Message::Event(e));
                        }
                        Ok(Ok(None)) => {
                            eof = true;
                            break;
                        }
                        Ok(Err(e)) => {
                            send(Message::Failure(Problem::Protocol, e.to_string()));
                            fatal = true;
                            break;
                        }
                        Err(_) => break,
                    }
                }
                if pipe_overflow.load(Ordering::Acquire) || o.load(Ordering::Acquire) {
                    send(Message::Failure(Problem::Protocol, "host event queue overflow".into()));
                    fatal = true;
                }
                if s.load(Ordering::Acquire) && stopping.is_none() {
                    stopping = Some(Instant::now());
                    // Just one short command fits in an empty OS pipe. There is no unbounded command queue.
                    let _ = input.write_all(protocol::command(Command::Stop, revision).as_bytes());
                }
                if ready && !status_sent && stopping.is_none() {
                    status_sent = true;
                    let _ = input.write_all(protocol::command(Command::Status, revision).as_bytes());
                }
                if !ready && stopping.is_none() && started.elapsed() >= limits.start {
                    send(Message::Failure(Problem::Slow, "host startup deadline".into()));
                    fatal = true;
                }
                if fatal || stopping.is_some_and(|t| !ready || t.elapsed() >= limits.stop) {
                    match child.kill_wait() {
                        Ok(code) => {
                            break Some(code);
                        }
                        Err(e) => {
                            send(Message::Failure(Problem::Crash, e.to_string()));
                            std::thread::park_timeout(Duration::from_millis(100));
                            continue; // Keep ownership and the update gate until exit is proved.
                        }
                    }
                }
                match child.try_wait() {
                    Ok(Some(code)) => {
                        let seen = exit_seen.get_or_insert_with(Instant::now);
                        // Drain events already buffered in the pipe before interpreting the exit code.
                        if eof {
                            break Some(code);
                        }
                        if seen.elapsed() > Duration::from_secs(1) {
                            send(Message::Failure(Problem::Protocol, "host pipe did not close".into()));
                            break Some(code);
                        }
                    }
                    Ok(None) => {}
                    Err(e) => {
                        send(Message::Failure(Problem::Crash, e.to_string()));
                        fatal = true; // Retry termination through the ownership-preserving path above.
                    }
                }
                std::thread::park_timeout(Duration::from_millis(10));
            };
            drop(input);
            // Drop closes the Windows job, including any descendants holding a pipe.
            drop(child);
            drop(lines_rx);
            if reader.is_finished() {
                let _ = reader.join();
            }
            drop(folder);
            send(Message::Exit(exit, s.load(Ordering::Acquire)));
        });
        Self {
            stop,
            done,
            overflow,
            rx,
        }
    }
    pub fn stop(&self) {
        self.stop.store(true, Ordering::Release);
    }
    pub fn done(&self) -> bool {
        self.done.load(Ordering::Acquire)
    }
    pub fn poll(&self, state: &mut State) {
        for m in self.rx.try_iter().take(256) {
            match m {
                Message::Event(e) => state.accept(e),
                Message::Failure(p, e) => state.fail(p, e),
                Message::Exit(code, stop) => state.exited(code, stop),
            }
        }
        if self.overflow.load(Ordering::Acquire) {
            state.fail(Problem::Protocol, "host event queue overflow");
        }
        if matches!(state.phase, Phase::Failed | Phase::RestartRequired) {
            self.stop();
        }
    }
}
