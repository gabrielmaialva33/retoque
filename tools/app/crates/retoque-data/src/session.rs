//! How the last session went, from the runtime's periodic status lines: real (rendered) and on-screen frame rates,
//! the frame-generation multiplier, how often the real rate dropped, and a few signals that explain a bad session.
//! Fed one line at a time by `logs::summarize`, in bounded memory. Advice is pure and testable; the GUI words it.
//!
//! The runtime reports `DLSS-NR status: ... | <presents> presents : <renders> renders = <x>x on screen` about every
//! two seconds. A window's rate is its count divided by the time since the previous status line, measured from the
//! timestamps, never assumed: reading a two-second window as one second doubles every rate.

use std::collections::VecDeque;

/// About nine hours of two-second windows; older windows are dropped first.
const MAX_WINDOWS: usize = 16 * 1024;
/// Points kept for the curve the GUI draws.
const CURVE_POINTS: usize = 240;
/// A window shorter than this or longer than this is not a measurement (a stall, a pause, a loading screen).
const MIN_WINDOW_S: f64 = 0.5;
const MAX_WINDOW_S: f64 = 10.0;
/// A drop: a window whose real rate is below this share of the session's median.
const DROP_SHARE_OF_MEDIAN: f64 = 0.8;
/// On-screen frames per rendered frame from which a window counts as frame generation.
const GENERATION_RATIO: f32 = 1.5;
/// Frame generation is judged when it ran in at least this share of the session.
const GENERATION_MIN_SHARE: f64 = 0.2;
/// Fewer windows than this (about half a minute) is too short to judge.
const MIN_WINDOWS: usize = 15;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Pacing {
    /// First and last measured window, as the log writes the time (HH:MM:SS).
    pub start: String,
    pub end: String,
    pub active_seconds: f64,
    pub windows: usize,
    pub base_fps_median: f64,
    /// The rate the worst 5% of windows stayed under.
    pub base_fps_low: f64,
    pub screen_fps_median: f64,
    /// Share of windows with frame generation (at least 1.5 on-screen frames per rendered frame).
    pub generation_share: f64,
    /// Over those windows only: the median multiplier and the median real rate it multiplied (0 without any).
    pub generation_multiplier: f64,
    pub generation_base_fps: f64,
    /// Share of windows whose real rate dropped below 80% of the median.
    pub drop_share: f64,
    /// (seconds since start, real fps), downsampled for drawing.
    pub curve: Vec<(f32, f32)>,
    pub nr_turned_off: bool,
    /// The runtime gave the game its own NVAPI stand-in (fakenvapi), which hides DLSS frame generation.
    pub fake_nvapi: bool,
    pub driver: Option<String>,
}

#[derive(Default)]
pub struct PacingBuilder {
    previous: Option<f64>,
    day_offset: f64,
    last_raw: Option<f64>,
    windows: VecDeque<(f64, f32, f32)>,
    start: Option<String>,
    end: Option<String>,
    nr_turned_off: bool,
    fake_nvapi: bool,
    driver: Option<String>,
}

/// "[15:45:32.123456] ..." -> seconds since midnight.
fn timestamp(line: &str) -> Option<(f64, &str)> {
    let inner = line.strip_prefix('[')?;
    let stamp = inner.get(..15)?;
    let bytes = stamp.as_bytes();
    if bytes.get(2) != Some(&b':') || bytes.get(5) != Some(&b':') {
        return None;
    }
    let hours: f64 = stamp.get(0..2)?.parse().ok()?;
    let minutes: f64 = stamp.get(3..5)?.parse().ok()?;
    let seconds: f64 = stamp.get(6..)?.trim_end_matches(']').parse().ok()?;
    Some((hours * 3600.0 + minutes * 60.0 + seconds, stamp.get(0..8)?))
}

/// "| 192 presents : 48 renders" -> (192, 48).
fn counts(line: &str) -> Option<(u64, u64)> {
    let (_, tail) = line.rsplit_once(" | ")?;
    let (presents, rest) = tail.split_once(" presents : ")?;
    let renders = rest.split_once(" renders")?.0;
    Some((presents.trim().parse().ok()?, renders.trim().parse().ok()?))
}

impl PacingBuilder {
    pub fn observe(&mut self, line: &str) {
        if line.contains("nvapi64.dll is loaded when Nvidia is not the primary GPU") {
            self.fake_nvapi = true;
        }
        if line.contains("DLSS-NR did not run: it is switched off") {
            self.nr_turned_off = true;
        }
        if let Some((_, rest)) = line.split_once("DLSS-NR: NVIDIA driver ") {
            self.driver = rest.split_whitespace().next().map(str::to_owned);
        }
        if !line.contains("DLSS-NR status:") {
            return;
        }
        let Some((raw, clock)) = timestamp(line) else { return };
        // The log's clock wraps at midnight; keep time monotonic across it.
        if self.last_raw.is_some_and(|last| raw + 3600.0 < last) {
            self.day_offset += 86_400.0;
        }
        self.last_raw = Some(raw);
        let now = raw + self.day_offset;
        let previous = self.previous.replace(now);
        let (Some(previous), Some((presents, renders))) = (previous, counts(line)) else {
            return;
        };
        let dt = now - previous;
        if !(MIN_WINDOW_S..=MAX_WINDOW_S).contains(&dt) || renders == 0 {
            return;
        }
        if self.windows.len() == MAX_WINDOWS {
            self.windows.pop_front();
        }
        self.windows
            .push_back((now, (renders as f64 / dt) as f32, (presents as f64 / dt) as f32));
        if self.start.is_none() {
            self.start = Some(clock.to_owned());
        }
        self.end = Some(clock.to_owned());
    }

    pub fn finish(self) -> Option<Pacing> {
        if self.windows.is_empty() {
            return None;
        }
        let first = self.windows.front()?.0;
        let base: Vec<f32> = self.windows.iter().map(|w| w.1).collect();
        let screen: Vec<f32> = self.windows.iter().map(|w| w.2).collect();
        // Frame generation can be on for part of a session only (switched in the game's menu): judge it over the
        // windows where it ran, not over a median that the windows without it decide.
        let generated: Vec<(f32, f32)> = self
            .windows
            .iter()
            .map(|w| (w.1, w.2 / w.1.max(f32::EPSILON)))
            .filter(|(_, ratio)| *ratio >= GENERATION_RATIO)
            .collect();
        let generated_base: Vec<f32> = generated.iter().map(|g| g.0).collect();
        let generated_ratio: Vec<f32> = generated.iter().map(|g| g.1).collect();
        let median = percentile(&base, 0.5);
        let drops = base
            .iter()
            .filter(|&&b| f64::from(b) < DROP_SHARE_OF_MEDIAN * median)
            .count();
        let active_seconds = self
            .windows
            .iter()
            .zip(self.windows.iter().skip(1))
            .map(|(a, b)| b.0 - a.0)
            .sum();
        Some(Pacing {
            start: self.start.unwrap_or_default(),
            end: self.end.unwrap_or_default(),
            active_seconds,
            windows: base.len(),
            base_fps_median: median,
            base_fps_low: percentile(&base, 0.05),
            screen_fps_median: percentile(&screen, 0.5),
            generation_share: generated.len() as f64 / base.len() as f64,
            generation_multiplier: if generated.is_empty() {
                0.0
            } else {
                percentile(&generated_ratio, 0.5)
            },
            generation_base_fps: if generated.is_empty() {
                0.0
            } else {
                percentile(&generated_base, 0.5)
            },
            drop_share: drops as f64 / base.len() as f64,
            curve: downsample(&self.windows, first),
            nr_turned_off: self.nr_turned_off,
            fake_nvapi: self.fake_nvapi,
            driver: self.driver,
        })
    }
}

fn percentile(values: &[f32], q: f64) -> f64 {
    let mut sorted = values.to_vec();
    sorted.sort_by(f32::total_cmp);
    let index = ((sorted.len() - 1) as f64 * q).round() as usize;
    f64::from(sorted[index.min(sorted.len() - 1)])
}

fn downsample(windows: &VecDeque<(f64, f32, f32)>, first: f64) -> Vec<(f32, f32)> {
    let size = windows.len().div_ceil(CURVE_POINTS).max(1);
    let all: Vec<_> = windows.iter().collect();
    all.chunks(size)
        .map(|chunk| {
            let t = chunk.iter().map(|w| w.0 - first).sum::<f64>() / chunk.len() as f64;
            let b = chunk.iter().map(|w| f64::from(w.1)).sum::<f64>() / chunk.len() as f64;
            (t as f32, b as f32)
        })
        .collect()
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Level {
    Good,
    Info,
    Warn,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Advice {
    /// Too short to judge.
    Short,
    /// Nothing to change.
    Steady,
    /// The runtime gave the game fakenvapi: DLSS frame generation can vanish from the game's menu.
    FakeNvapi,
    /// Frame generation multiplies too few real frames; `x4` when it multiplies by about four.
    LowBaseForFrameGeneration { base: u32, x4: bool },
    /// The real rate dropped below 80% of its median in this share of the session.
    Drops { percent: u32 },
    /// NR was switched off during the session (the toggle key).
    NrTurnedOff,
}

impl Advice {
    pub fn level(&self) -> Level {
        match self {
            Advice::Steady => Level::Good,
            Advice::Short | Advice::NrTurnedOff => Level::Info,
            Advice::FakeNvapi | Advice::LowBaseForFrameGeneration { .. } | Advice::Drops { .. } => Level::Warn,
        }
    }
}

/// Frame generation feels heavy and stutters below about this many real frames per second.
pub const FRAME_GENERATION_MIN_BASE: f64 = 40.0;

pub fn advice(pacing: &Pacing) -> Vec<Advice> {
    let mut out = Vec::new();
    if pacing.fake_nvapi {
        out.push(Advice::FakeNvapi);
    }
    if pacing.windows < MIN_WINDOWS {
        out.push(Advice::Short);
        return out;
    }
    if pacing.generation_share >= GENERATION_MIN_SHARE && pacing.generation_base_fps < FRAME_GENERATION_MIN_BASE {
        out.push(Advice::LowBaseForFrameGeneration {
            base: pacing.generation_base_fps.round() as u32,
            x4: pacing.generation_multiplier >= 3.5,
        });
    }
    if pacing.drop_share >= 0.05 {
        out.push(Advice::Drops {
            percent: (pacing.drop_share * 100.0).round() as u32,
        });
    }
    if pacing.nr_turned_off {
        out.push(Advice::NrTurnedOff);
    }
    if !out.iter().any(|a| a.level() == Level::Warn) {
        out.insert(0, Advice::Steady);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn status(clock: &str, presents: u64, renders: u64) -> String {
        format!(
            "[{clock}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: upscaled: active on the upscaler's \
             output | {presents} presents : {renders} renders = {:.2}x on screen",
            presents as f64 / renders.max(1) as f64
        )
    }

    fn session(lines: &[String]) -> Pacing {
        let mut b = PacingBuilder::default();
        for line in lines {
            b.observe(line);
        }
        b.finish().unwrap()
    }

    #[test]
    fn rates_come_from_the_time_between_status_lines_not_from_one_second_windows() {
        // Dying Light: The Beast on an RTX 3060, 2026-11-10: 184 presents and 46 renders every two seconds.
        let lines: Vec<String> = (0..30)
            .map(|i| status(&format!("15:46:{:02}", i * 2 % 60), 184, 46))
            .collect();
        let p = session(&lines[..30]);
        assert!((p.base_fps_median - 23.0).abs() < 0.01, "{}", p.base_fps_median);
        assert!((p.screen_fps_median - 92.0).abs() < 0.01);
        assert!((p.generation_multiplier - 4.0).abs() < 0.01);
        assert!((p.generation_share - 1.0).abs() < 1e-9);
        assert_eq!(p.drop_share, 0.0);
        assert_eq!(advice(&p), [Advice::LowBaseForFrameGeneration { base: 23, x4: true }]);
    }

    #[test]
    fn frame_generation_switched_on_mid_session_is_judged_over_its_own_windows() {
        // 15:38 session: 1.0x until frame generation was switched on, then x4 over ~23 real fps.
        let mut lines: Vec<String> = (0..20)
            .map(|i| status(&format!("15:40:{:02}", i * 2), 54, 54))
            .collect();
        lines.extend((0..15).map(|i| status(&format!("15:41:{:02}", i * 2), 184, 46)));
        let p = session(&lines);
        assert!(
            p.generation_share > 0.4 && p.generation_share < 0.5,
            "{}",
            p.generation_share
        );
        assert!((p.generation_base_fps - 23.0).abs() < 0.01);
        assert!((p.generation_multiplier - 4.0).abs() < 0.01);
        assert!(advice(&p).contains(&Advice::LowBaseForFrameGeneration { base: 23, x4: true }));
    }

    #[test]
    fn drops_menus_and_long_gaps() {
        let mut lines = Vec::new();
        for i in 0..20 {
            lines.push(status(&format!("10:00:{:02}", i * 2), 120, 120));
        }
        // A menu window (no renders) and a 30-second loading gap measure nothing.
        lines.push(status("10:00:42", 300, 0));
        lines.push(status("10:01:12", 120, 120));
        // Two drops to half the rate.
        lines.push(status("10:01:14", 60, 60));
        lines.push(status("10:01:16", 60, 60));
        let p = session(&lines);
        assert_eq!(p.windows, 21);
        assert!((p.base_fps_median - 60.0).abs() < 0.01);
        assert!((p.drop_share - 2.0 / 21.0).abs() < 1e-9);
        assert_eq!(advice(&p), [Advice::Drops { percent: 10 }]);
        assert_eq!(p.start, "10:00:02");
        assert_eq!(p.end, "10:01:16");
    }

    #[test]
    fn signals_short_sessions_midnight_and_a_steady_one() {
        let mut b = PacingBuilder::default();
        b.observe("[13:24:29.345832] [W] LibraryLoadHooks::LoadNvApi nvapi64.dll is loaded when Nvidia is not the primary GPU");
        b.observe("[13:24:29.598371] [I] DlssNr::ApplyDriverGate DLSS-NR: NVIDIA driver 617.14 meets Neural Rendering's minimum (610.88)");
        b.observe(&status("23:59:58", 100, 100));
        b.observe(&status("00:00:00", 100, 100));
        let p = b.finish().unwrap();
        assert_eq!(p.windows, 1, "a window across midnight still measures two seconds");
        assert!((p.base_fps_median - 50.0).abs() < 0.01);
        assert_eq!(p.driver.as_deref(), Some("617.14"));
        assert_eq!(advice(&p), [Advice::FakeNvapi, Advice::Short]);

        let steady: Vec<String> = (0..20)
            .map(|i| status(&format!("20:00:{:02}", i * 2), 140, 140))
            .collect();
        assert_eq!(advice(&session(&steady)), [Advice::Steady]);
        assert!(PacingBuilder::default().finish().is_none());
    }

    #[test]
    fn memory_stays_bounded_and_the_curve_is_small() {
        let mut b = PacingBuilder::default();
        let mut t = 0.0_f64;
        for _ in 0..(MAX_WINDOWS + 500) {
            t += 2.0;
            let s = t % 86_400.0;
            let clock = format!(
                "{:02}:{:02}:{:02}",
                (s / 3600.0) as u32,
                (s % 3600.0 / 60.0) as u32,
                (s % 60.0) as u32
            );
            b.observe(&status(&clock, 100, 50));
        }
        assert_eq!(b.windows.len(), MAX_WINDOWS);
        let p = b.finish().unwrap();
        assert!(p.curve.len() <= CURVE_POINTS);
        assert!((p.base_fps_median - 25.0).abs() < 0.01);
    }
}
