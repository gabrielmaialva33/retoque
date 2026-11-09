//! Stdout failures are typed I/O errors, never formatting panics. A closed pipe stops output,
//! not an in-flight installer transaction. Other command failures retain their original exit codes.
use std::{
    fmt,
    io::{self, Write},
    sync::atomic::{AtomicBool, Ordering},
};
static CLOSED: AtomicBool = AtomicBool::new(false);
static FAILED: AtomicBool = AtomicBool::new(false);
pub fn line(args: fmt::Arguments<'_>) {
    if CLOSED.load(Ordering::Relaxed) || FAILED.load(Ordering::Relaxed) {
        return;
    }
    let mut stdout = io::stdout().lock();
    if let Err(error) = stdout.write_fmt(args).and_then(|_| stdout.write_all(b"\n")) {
        if error.kind() == io::ErrorKind::BrokenPipe {
            CLOSED.store(true, Ordering::Relaxed);
        } else {
            FAILED.store(true, Ordering::Relaxed);
            let _ = writeln!(io::stderr().lock(), "[-] Error writing stdout: {error}");
        }
    }
}
pub fn failed() -> bool {
    FAILED.load(Ordering::Relaxed)
}
pub fn broken_pipe() -> bool {
    CLOSED.load(Ordering::Relaxed)
}
