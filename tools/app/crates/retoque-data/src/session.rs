//! How the last session went, from the runtime's periodic status lines: real (rendered) and on-screen frame rates,
//! the frame-generation multiplier, how often the real rate dropped, and a few signals that explain a bad session.
//! Fed one line at a time by `logs::summarize`, in bounded memory. Advice is pure and testable; the GUI words it.
//!
//! The runtime reports `DLSS-NR status: ... | <presents> presents : <renders> renders = <x>x on screen` about every
//! two seconds. A window's rate is its count divided by the time since the previous status line, measured from the
//! timestamps, never assumed: reading a two-second window as one second doubles every rate. Pure present-route
//! sessions (`0 renders`) measure their rate from `presents` on active present windows.

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

#[derive(Clone, Default)]
pub struct PacingBuilder {
    previous: Option<f64>,
    day_offset: f64,
    last_raw: Option<f64>,
    saw_renders: bool,
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

/// Whether a status line names an active present-route pass, where `renders` stays zero when no upscaler runs.
/// Waiting, skipping, declined and upscaled windows are excluded.
fn present_route_active(line: &str) -> bool {
    let Some((_, rest)) = line.split_once("DLSS-NR status: ") else {
        return false;
    };
    let Some((route, _)) = rest.split_once(" | ") else {
        return false;
    };
    route.starts_with("present: active")
        || route.starts_with("present: at the game's own Present")
        || (route.starts_with("auto: present, ")
            && !route.starts_with("auto: present, no new frame")
            && !route.starts_with("auto: present, waiting"))
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
        let Some((presents, renders)) = counts(line) else {
            return;
        };
        if renders > 0 && !self.saw_renders {
            self.saw_renders = true;
            self.windows.clear();
            self.start = None;
            self.end = None;
        }
        let Some(previous) = previous else {
            return;
        };
        let dt = now - previous;
        if !(MIN_WINDOW_S..=MAX_WINDOW_S).contains(&dt) {
            return;
        }
        let (base, screen) = if renders > 0 {
            ((renders as f64 / dt) as f32, (presents as f64 / dt) as f32)
        } else if !self.saw_renders && presents > 0 && present_route_active(line) {
            let fps = (presents as f64 / dt) as f32;
            (fps, fps)
        } else {
            return;
        };
        if self.windows.len() == MAX_WINDOWS {
            self.windows.pop_front();
        }
        self.windows.push_back((now, base, screen));
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

    #[test]
    fn present_route_sessions_without_upscaler_renders_record_pacing_from_presents() {
        // Sanitized PCSX2 present-route session (2026-11-18): no upscaler runs, so `renders` stays 0 across every
        // window while `presents` carries the real base rate. Warm-up and settling windows without a pass still anchor
        // the clock for the first active window without being counted themselves.
        const PCSX2_SANITIZED: &[&str] = &[
            "[12:09:12.566823] [I] DlssNr::ApplyDriverGate DLSS-NR: NVIDIA driver 621.14 meets Neural Rendering's minimum (610.88)",
            "[12:09:17.025766] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: no pass this window (startup deferred (stabilizing swapchain)) | 1 presents : 0 renders",
            "[12:09:28.516965] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: no pass this window (startup deferred (stabilizing swapchain)) | 1 presents : 0 renders",
            "[12:09:30.732168] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: no pass this window (chain working resolution is settling (500 ms); no model reconstruction) | 35 presents : 0 renders",
            "[12:09:32.761598] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 53 presents : 0 renders = 0.00x on screen",
            "[12:09:34.771472] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 88 presents : 0 renders = 0.00x on screen",
            "[12:09:36.793684] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 78 presents : 0 renders = 0.00x on screen",
            "[12:09:38.819895] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 61 presents : 0 renders = 0.00x on screen",
            "[12:09:40.854621] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:09:42.867383] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:09:44.898752] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 61 presents : 0 renders = 0.00x on screen",
            "[12:09:46.909983] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:09:48.921009] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 49 presents : 0 renders = 0.00x on screen",
            "[12:09:50.938047] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 61 presents : 0 renders = 0.00x on screen",
            "[12:09:52.948565] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 59 presents : 0 renders = 0.00x on screen",
            "[12:09:54.960334] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:09:56.989148] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:09:59.022729] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 59 presents : 0 renders = 0.00x on screen",
            "[12:10:01.025133] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 58 presents : 0 renders = 0.00x on screen",
            "[12:10:03.057659] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 60 presents : 0 renders = 0.00x on screen",
            "[12:10:05.064709] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 59 presents : 0 renders = 0.00x on screen",
            "[12:10:07.097318] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 58 presents : 0 renders = 0.00x on screen",
            "[12:10:09.103583] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 54 presents : 0 renders = 0.00x on screen",
            "[12:10:11.141364] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 53 presents : 0 renders = 0.00x on screen",
            "[12:10:13.175447] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 48 presents : 0 renders = 0.00x on screen",
            "[12:10:15.200285] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 59 presents : 0 renders = 0.00x on screen",
            "[12:10:17.215633] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides (no motion) | 59 presents : 0 renders = 0.00x on screen",
        ];

        let mut b = PacingBuilder::default();
        for line in PCSX2_SANITIZED {
            b.observe(line);
        }
        let p = b.finish().expect("present-route session must produce pacing");
        assert_eq!(p.windows, 23);
        assert_eq!(p.start, "12:09:32");
        assert_eq!(p.end, "12:10:17");
        assert_eq!(p.driver.as_deref(), Some("621.14"));
        assert!((p.base_fps_median - p.screen_fps_median).abs() < 1e-6);
        assert!((p.base_fps_median - 29.4).abs() < 0.6, "{}", p.base_fps_median);
        assert!((p.base_fps_low - 24.3).abs() < 1.0, "{}", p.base_fps_low);
        assert_eq!(p.generation_share, 0.0);
        assert_eq!(p.drop_share, 0.0);
        assert_eq!(advice(&p), [Advice::Steady]);

        // Active auto-present and boundary-present variants count when renders == 0; waiting/skipping/declined do not.
        assert!(present_route_active(
            "[12:00:02.000000] [I] DlssNr status: DLSS-NR status: auto: present, synthesized motion | 120 presents : 0 renders = 0.00x on screen"
        ));
        assert!(present_route_active(
            "[12:00:02.000000] [I] DlssNr status: DLSS-NR status: present: at the game's own Present, ahead of its frame generation | 120 presents : 0 renders = 0.00x on screen"
        ));
        for inactive in [
            "present: waiting for the game's depth and motion",
            "present: no new frame from the upscaler (shown as the game drew it)",
            "present: declined, the game's DLSS-G interpolates before this pass",
            "auto: present, waiting for the game's depth and motion",
            "auto: present, no new frame from the upscaler (shown as the game drew it)",
            "auto: upscaled, the upscaler has handed over no temporal inputs yet",
            "off",
        ] {
            let line = format!(
                "[12:00:02.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: {inactive} | 120 presents : 0 renders = 0.00x on screen"
            );
            assert!(!present_route_active(&line), "{inactive}");
        }
    }

    #[test]
    fn upscaler_sessions_ignore_present_only_menus_and_transient_zero_render_windows() {
        let mut b = PacingBuilder::default();
        // Intro/menu before DLSS starts: HookMethod=0 runs auto: present at 120 fps with 0 renders.
        for i in 0..6 {
            b.observe(&format!(
                "[10:00:{:02}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: auto: present, zero guides \
                 (no motion) | 240 presents : 0 renders = 0.00x on screen",
                i * 2
            ));
        }
        // Gameplay begins on DLSS at 60 fps (120 renders per 2 s); tentative intro windows are cleared.
        for i in 6..26 {
            b.observe(&format!(
                "[10:{:02}:{:02}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, the game's \
                 depth and motion | 120 presents : 120 renders = 1.00x on screen",
                (i * 2) / 60,
                (i * 2) % 60
            ));
        }
        // Transient pause/loading window with 0 renders on HookMethod=2 must not count as a 10 fps drop.
        b.observe(
            "[10:00:52.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, zero guides \
             (no motion) | 20 presents : 0 renders = 0.00x on screen",
        );
        b.observe(
            "[10:00:54.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: present: active, the game's \
             depth and motion | 120 presents : 120 renders = 1.00x on screen",
        );

        let p = b.finish().unwrap();
        assert_eq!(p.windows, 21);
        assert_eq!(p.start, "10:00:12");
        assert_eq!(p.end, "10:00:54");
        assert!((p.base_fps_median - 60.0).abs() < 0.01);
        assert_eq!(p.drop_share, 0.0);
        assert_eq!(advice(&p), [Advice::Steady]);
    }
}
