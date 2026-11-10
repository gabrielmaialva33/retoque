//! A small diagnostic log beside the app's data: each error the interface shows, and each background job that
//! failed or took 100 ms or more. It is enabled by `main` only, so tests and library users never write to the
//! user's folder. The file stays under 1 MiB: at startup a larger log becomes `retoque-app.old.log`.
use std::{
    collections::VecDeque,
    fs::{File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    sync::Mutex,
    time::{Duration, SystemTime, UNIX_EPOCH},
};

const FILE: &str = "retoque-app.log";
const OLD: &str = "retoque-app.old.log";
const LIMIT: u64 = 1024 * 1024;
/// Errors repaint every frame; only the first showing of each recent one is written.
const RECENT: usize = 32;

struct Log {
    file: File,
    recent: VecDeque<String>,
}
static LOG: Mutex<Option<Log>> = Mutex::new(None);

/// Opens the log in `dir`, rotating an oversized one first. Failing to log never stops the app.
pub fn init(dir: &Path) -> Option<PathBuf> {
    std::fs::create_dir_all(dir).ok()?;
    let path = dir.join(FILE);
    if std::fs::symlink_metadata(&path).is_ok_and(|m| m.is_file() && m.len() > LIMIT) {
        let _ = std::fs::rename(&path, dir.join(OLD));
    }
    // Never follow a planted link out of the app's own folder.
    if std::fs::symlink_metadata(&path).is_ok_and(|m| !m.is_file()) {
        return None;
    }
    let file = OpenOptions::new().create(true).append(true).open(&path).ok()?;
    *LOG.lock().unwrap_or_else(|e| e.into_inner()) = Some(Log {
        file,
        recent: VecDeque::new(),
    });
    line(format_args!(
        "Retoque {} started on {}",
        env!("CARGO_PKG_VERSION"),
        std::env::consts::OS
    ));
    Some(path)
}

pub fn line(message: std::fmt::Arguments<'_>) {
    let mut guard = LOG.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(log) = guard.as_mut() {
        let thread = std::thread::current();
        let _ = writeln!(
            log.file,
            "{} [{}] {message}",
            timestamp(SystemTime::now()),
            thread.name().unwrap_or("?")
        );
    }
}

/// Records an error the first time it is shown; the same text again while it is recent is skipped.
pub fn shown_error(message: &str) {
    {
        let mut guard = LOG.lock().unwrap_or_else(|e| e.into_inner());
        let Some(log) = guard.as_mut() else { return };
        if log.recent.iter().any(|m| m == message) {
            return;
        }
        if log.recent.len() == RECENT {
            log.recent.pop_front();
        }
        log.recent.push_back(message.to_owned());
    }
    line(format_args!("error shown: {message}"));
}

/// Failures always; a success only when it took long enough to be felt (quick polls would flood the file).
pub fn job(name: &str, elapsed: Duration, failed: Option<&str>) {
    let ms = elapsed.as_millis();
    match failed {
        Some(message) => line(format_args!("job {name} failed after {ms} ms: {message}")),
        None if elapsed >= SLOW => line(format_args!("job {name} finished in {ms} ms")),
        None => {}
    }
}
const SLOW: Duration = Duration::from_millis(100);

/// UTC, so lines from the user's PC and a remote one compare without knowing either time zone.
fn timestamp(now: SystemTime) -> String {
    let since = now.duration_since(UNIX_EPOCH).unwrap_or_default();
    let secs = since.as_secs();
    let (days, rest) = (secs / 86_400, secs % 86_400);
    // Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
    let z = days as i64 + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}.{:03}Z",
        rest / 3600,
        rest % 3600 / 60,
        rest % 60,
        since.subsec_millis()
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn timestamps_are_utc_civil_dates() {
        assert_eq!(timestamp(UNIX_EPOCH), "1970-01-01T00:00:00.000Z");
        let t = UNIX_EPOCH + Duration::from_millis(1_794_355_200_123);
        assert_eq!(timestamp(t), "2026-11-11T00:00:00.123Z");
        let leap = UNIX_EPOCH + Duration::from_secs(951_782_400); // 2000-02-29
        assert_eq!(&timestamp(leap)[..10], "2000-02-29");
    }
}
