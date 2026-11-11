//! "Measure this game": the NR GPU time per frame, from the runtime's fence-confirmed timing samples
//! (`[DlssNr] GpuTiming=true`, docs/dlssnr/design/gpu-timing.md). Fed one line at a time by `logs::summarize`, in
//! bounded memory.
//!
//! With timing on, the runtime samples one eligible NR evaluation in `interval` (default 30) and, once the GPU has
//! finished it, writes (retoque/dlssnr/DlssNr_GpuTiming.cpp, `PollLocked`):
//!
//! `DLSS-NR gpu timing confirmed: run=… sample=… evaluation=… present=… stage=after … model_reset=false
//! capture_active=false cadence=1 carried=false passes=2/2 dispatch_ms=3.412000 model_ms=2.950000
//! outside_model_ms=0.462000 … unit=eligible-evaluations … proof=sealed-single-execution-fence`
//!
//! `dispatch_ms` is everything NR recorded for that evaluation (the model calls and the work around them);
//! `model_ms` is the sum of the model calls of all passes. The runtime does not split time per pass: only the pass
//! count (`passes=done/requested`) is known. HDR conversion samples (`conversion_sample=`) and the Present-route
//! regions (`gpu timing region confirmed`) are other lines and are not counted here.

use std::collections::VecDeque;

/// About two hours of samples at the default interval and 60 evaluations per second; the oldest go first.
const MAX_SAMPLES: usize = 16 * 1024;
/// Fewer samples than this (a few seconds of play) is not a measurement yet.
pub const MIN_SAMPLES: usize = 10;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct NrTiming {
    /// Samples behind the numbers: evaluations where the model ran, outside a reset or a capture.
    pub samples: usize,
    /// NR GPU time of one evaluation (`dispatch_ms`): the median and the time the worst 5% stayed above.
    pub median_ms: f64,
    pub worst_ms: f64,
    /// The model calls' share of it (`model_ms`), median.
    pub model_median_ms: f64,
    /// NR passes per evaluation (the most seen). The log gives their sum, never a time per pass.
    pub passes: u32,
    /// The model runs on one evaluation in `cadence` (model cadence); 1 when it runs on every one.
    pub cadence: u32,
    /// Carried evaluations (cadence on: no model call) seen and left out of the numbers.
    pub carried: usize,
}

impl NrTiming {
    /// NR's share of the real frame time at `real_fps`, when the model runs on every frame. With model cadence the
    /// samples cannot say how often it ran, so there is no share.
    pub fn frame_share(&self, real_fps: f64) -> Option<f64> {
        (self.cadence <= 1 && real_fps.is_finite() && real_fps > 0.0 && self.samples >= MIN_SAMPLES)
            .then(|| self.median_ms * real_fps / 1000.0)
    }
}

#[derive(Clone, Copy)]
struct Sample {
    dispatch: f64,
    model: f64,
    passes: u32,
}

#[derive(Default)]
pub struct TimingBuilder {
    samples: VecDeque<Sample>,
    cadence: u32,
    carried: usize,
}

fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split_whitespace()
        .find_map(|s| s.split_once('=').filter(|(k, _)| *k == key).map(|(_, v)| v))
}

fn flag(line: &str, key: &str) -> Option<bool> {
    match field(line, key)? {
        "true" | "1" => Some(true),
        "false" | "0" => Some(false),
        _ => None,
    }
}

fn millis(line: &str, key: &str) -> Option<f64> {
    field(line, key)?
        .parse::<f64>()
        .ok()
        .filter(|v| v.is_finite() && *v >= 0.0)
}

impl TimingBuilder {
    pub fn observe(&mut self, line: &str) {
        let Some((_, rest)) = line.split_once("DLSS-NR gpu timing confirmed: ") else {
            return;
        };
        // Evaluation samples only: the HDR conversion's line has no dispatch_ms and another unit.
        if field(rest, "unit").is_some_and(|u| u != "eligible-evaluations") {
            return;
        }
        let (Some(dispatch), Some(model), Some(carried)) = (
            millis(rest, "dispatch_ms"),
            millis(rest, "model_ms"),
            flag(rest, "carried"),
        ) else {
            return;
        };
        if let Some(cadence) = field(rest, "cadence").and_then(|c| c.parse::<u32>().ok()) {
            self.cadence = self.cadence.max(cadence);
        }
        if carried {
            self.carried += 1;
            return;
        }
        // A model reset or a capture costs more for a frame; neither is the game's usual frame.
        if flag(rest, "model_reset") == Some(true) || flag(rest, "capture_active") == Some(true) {
            return;
        }
        let passes = field(rest, "passes")
            .and_then(|p| p.split('/').next())
            .and_then(|p| p.parse::<u32>().ok())
            .unwrap_or(1);
        if self.samples.len() == MAX_SAMPLES {
            self.samples.pop_front();
        }
        self.samples.push_back(Sample {
            dispatch,
            model,
            passes,
        });
    }

    pub fn finish(self) -> Option<NrTiming> {
        if self.samples.is_empty() {
            return None;
        }
        let dispatch: Vec<f64> = self.samples.iter().map(|s| s.dispatch).collect();
        let model: Vec<f64> = self.samples.iter().map(|s| s.model).collect();
        let mut counts = std::collections::BTreeMap::<u32, usize>::new();
        for sample in &self.samples {
            *counts.entry(sample.passes).or_default() += 1;
        }
        let passes = counts
            .iter()
            .max_by_key(|(passes, count)| (**count, **passes))
            .map(|(passes, _)| *passes)
            .unwrap_or(1);
        Some(NrTiming {
            samples: dispatch.len(),
            median_ms: percentile(&dispatch, 0.5),
            worst_ms: percentile(&dispatch, 0.95),
            model_median_ms: percentile(&model, 0.5),
            passes,
            cadence: self.cadence.max(1),
            carried: self.carried,
        })
    }
}

fn percentile(values: &[f64], q: f64) -> f64 {
    let mut sorted = values.to_vec();
    sorted.sort_by(f64::total_cmp);
    let index = ((sorted.len() - 1) as f64 * q).round() as usize;
    sorted[index.min(sorted.len() - 1)]
}

#[cfg(test)]
mod tests {
    use super::*;

    /// One evaluation sample exactly as `DlssNr_GpuTiming.cpp` formats it (`[%H:%M:%S.%f] [%L] %v`).
    fn confirmed(dispatch: f64, model: f64, passes: &str, carried: bool, cadence: u32, reset: bool) -> String {
        format!(
            "[20:14:03.512345] [I] DLSS-NR gpu timing confirmed: run=1 sample=31 evaluation=901 present=899 \
             stage=after contract=12345678901234 generation=4 \
             model_identity=file-bytes=91234567;mtime=133700000;version=1.0.0.0 render=1280x720 compose=2560x1440 \
             model=1280x720 working_scale=0.5 model_reset={reset} capture_active=false cadence={cadence} \
             carried={carried} passes={passes} dispatch_ms={dispatch:.6} model_ms={model:.6} \
             outside_model_ms={:.6} queue=2 fence=1771 frequency_hz=1000000000 interval=30 \
             unit=eligible-evaluations accepted=31 discarded=0 proof=sealed-single-execution-fence",
            dispatch - model
        )
    }

    #[test]
    fn median_worst_five_percent_and_passes_from_the_runtime_lines() {
        let mut builder = TimingBuilder::default();
        // 1.00 .. 2.99 ms; the model is 80% of each.
        for i in 0..200 {
            let dispatch = 1.0 + f64::from(i) / 100.0;
            builder.observe(&confirmed(dispatch, dispatch * 0.8, "2/2", false, 1, false));
        }
        let timing = builder.finish().unwrap();
        assert_eq!(timing.samples, 200);
        assert!((timing.median_ms - 2.0).abs() < 0.011, "{timing:?}");
        assert!((timing.worst_ms - 2.89).abs() < 0.011, "{timing:?}");
        assert!((timing.model_median_ms - 1.6).abs() < 0.011, "{timing:?}");
        assert_eq!((timing.passes, timing.cadence, timing.carried), (2, 1, 0));
        // 2 ms of a 60 fps frame (16.7 ms) is 12%.
        assert!((timing.frame_share(60.0).unwrap() - 0.12).abs() < 0.001);
        assert_eq!(timing.frame_share(0.0), None);
    }

    #[test]
    fn other_lines_resets_and_malformed_values_are_not_samples() {
        let mut builder = TimingBuilder::default();
        for line in [
            // The HDR conversion's sample, a Present-route region, the coverage summary and a discard.
            "[20:14:03.000001] [I] DLSS-NR gpu timing confirmed: run=1 conversion_sample=3 hdr_conversion_ms=0.400000 \
             decode_ms=0.200000 encode_ms=0.200000 queue=2 fence=9 frequency_hz=1000000000 interval=30 \
             unit=eligible-conversions proof=sealed-single-execution-fence",
            "[20:14:03.000002] [I] DLSS-NR gpu timing region confirmed: run=1 region=present sample=2 total_ms=5.000000 \
             dispatch_marked=true dispatch_call_ms=3.000000 outside_dispatch_ms=2.000000 unit=eligible-regions",
            "[20:14:03.000003] [I] DLSS-NR gpu timing coverage: run=1 eligible=90 attempted=3 recorded=3 accepted=3 \
             discarded=0 pending=0 waiting_reset=0 waiting_gpu=0 waiting_notification=0 retained=0 interval=30 \
             unit=eligible-evaluations enabled=true",
            "[20:14:03.000004] [I] DLSS-NR gpu timing discard: run=1 sample=4 reason=incomplete-evaluation",
        ] {
            builder.observe(line);
        }
        builder.observe(&confirmed(9.0, 8.0, "1/1", false, 1, true));
        builder
            .observe(&confirmed(1.0, 0.8, "1/1", false, 1, false).replace("dispatch_ms=1.000000", "dispatch_ms=NaN"));
        builder.observe(&confirmed(1.0, 0.8, "1/1", false, 1, false).replace("carried=false", "carried=maybe"));
        assert_eq!(builder.finish(), None);
    }

    #[test]
    fn carried_frames_are_counted_apart_and_cadence_has_no_share() {
        let mut builder = TimingBuilder::default();
        for _ in 0..20 {
            builder.observe(&confirmed(4.0, 3.5, "1/1", false, 2, false));
            builder.observe(&confirmed(0.3, 0.0, "0/1", true, 2, false));
        }
        let timing = builder.finish().unwrap();
        assert_eq!((timing.samples, timing.carried, timing.cadence), (20, 20, 2));
        assert_eq!(timing.median_ms, 4.0);
        assert_eq!(timing.frame_share(60.0), None);
    }

    #[test]
    fn memory_stays_bounded_and_keeps_the_newest_samples() {
        let mut builder = TimingBuilder::default();
        for _ in 0..MAX_SAMPLES {
            builder.observe(&confirmed(9.0, 8.0, "1/1", false, 1, false));
        }
        for _ in 0..MAX_SAMPLES {
            builder.observe(&confirmed(1.0, 0.5, "1/1", false, 1, false));
        }
        let timing = builder.finish().unwrap();
        assert_eq!(timing.samples, MAX_SAMPLES);
        assert_eq!(timing.worst_ms, 1.0);
    }
}
