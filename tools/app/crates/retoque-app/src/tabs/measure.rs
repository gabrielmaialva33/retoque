//! "Measurement": how much of each real frame NR takes on the user's graphics card, from the runtime's confirmed GPU
//! timing samples in the newest log (`retoque_data::measure`). Turning the runtime's timing on or off is an ordinary
//! settings review of one key (`[DlssNr] GpuTiming`): refused while the game runs, recorded for Undo.

use crate::app::*;
use retoque_data::measure::{MIN_SAMPLES, NrTiming};

const SECTION: &str = "DlssNr";
const KEY: &str = "GpuTiming";

/// The one edit that turns measurement on (`true`) or back to the shipped default, off (`auto`).
fn edit(on: bool) -> Edit {
    (SECTION.into(), KEY.into(), if on { "true" } else { "auto" }.into())
}

/// Some(on) when a settings review was opened by this card: exactly its one edit.
pub(crate) fn measure_request(inputs: Option<&PlanInputs>) -> Option<bool> {
    match inputs {
        Some(PlanInputs::Settings(edits)) => match edits.as_slice() {
            [(section, key, value)] if section == SECTION && key == KEY => Some(value == "true"),
            _ => None,
        },
        _ => None,
    }
}

/// The action's label key ("…": its review opens) and the toast's key once it is written.
pub(crate) fn measure_keys(on: bool) -> (&'static str, &'static str) {
    if on {
        ("measure.start", "measure.started")
    } else {
        ("measure.stop", "measure.stopped")
    }
}

/// The review's title and confirm: the action's label without "…".
pub(crate) fn measure_verb(lang: Lang, on: bool) -> &'static str {
    t(lang, measure_keys(on).0).trim_end_matches('…').trim_end()
}

/// Milliseconds with the language's decimal separator; two places below 1 ms.
fn millis(lang: Lang, value: f64) -> String {
    let places = if value < 1.0 { 2 } else { 1 };
    format!("{} ms", crate::tabs::session::decimal(lang, value, places))
}

impl RetoqueApp {
    /// Whether the game's OptiScaler.ini has the runtime's GPU timing on; None before the ini is read.
    fn gpu_timing_on(&self) -> Option<bool> {
        match &self.view.ini {
            Some(Ok(Some(ini))) => Some(
                ini.get(SECTION, KEY)
                    .is_some_and(|v| v.trim().eq_ignore_ascii_case("true")),
            ),
            _ => None,
        }
    }

    pub(crate) fn measure_card(&mut self, ui: &mut egui::Ui) {
        let lang = self.lang;
        let summary = self.view.intelligence.latest_log.as_ref();
        let timing = summary
            .and_then(|s| s.nr_timing.clone())
            .filter(|t| t.samples >= MIN_SAMPLES);
        let fps = summary.and_then(|s| s.pacing.as_ref()).map(|p| p.base_fps_median);
        let on = self.gpu_timing_on();
        // The runtime times D3D12 only, and only while NR runs: no offer it cannot keep.
        let vulkan = summary.and_then(|s| s.api.as_deref()) == Some("Vulkan");
        let nr_off = matches!(&self.view.status, Some(Ok(s)) if s.nr_enabled == Some(false));
        let can_offer = on.is_some() && !vulkan && !nr_off;
        if timing.is_none() && on != Some(true) && !can_offer {
            return;
        }
        let busy = self.busy();
        let mut clicked = None;
        widgets::card(ui, |ui| {
            widgets::card_title(ui, t(lang, "measure.title"));
            if let Some(timing) = &timing {
                result(ui, lang, timing, fps);
                ui.add_space(6.0);
            }
            let note = match (on, timing.is_some()) {
                (Some(true), false) => Some("measure.waiting"),
                (Some(true), true) => Some("measure.still_on"),
                (_, false) => Some("measure.intro"),
                _ => None,
            };
            if let Some(note) = note {
                ui.add(egui::Label::new(RichText::new(t(lang, note)).color(theme::MUTED)).wrap());
                ui.add_space(6.0);
            }
            let action = match on {
                Some(true) => Some(false),
                Some(false) if can_offer => Some(true),
                _ => None,
            };
            if let Some(turn_on) = action
                && ui
                    .add_enabled(!busy, egui::Button::new(t(lang, measure_keys(turn_on).0)))
                    .clicked()
            {
                clicked = Some(turn_on);
            }
        });
        ui.add_space(theme::CARD_GAP);
        if let Some(turn_on) = clicked {
            self.request_plan(PlanKind::Settings, PlanInputs::Settings(vec![edit(turn_on)]));
        }
    }
}

/// The numbers: NR time per frame (median and the worst 5%), the session's real frame rate and NR's share of it.
fn result(ui: &mut egui::Ui, lang: Lang, timing: &NrTiming, fps: Option<f64>) {
    let share = fps.and_then(|fps| timing.frame_share(fps));
    ui.horizontal_wrapped(|ui| {
        ui.spacing_mut().item_spacing = egui::vec2(10.0, 10.0);
        let shown = 2 + usize::from(fps.is_some()) + usize::from(share.is_some());
        let w = widgets::row_width(shown, ui.available_width(), 130.0, 10.0);
        widgets::stat_tile_width(
            ui,
            w,
            t(lang, "measure.per_frame"),
            &millis(lang, timing.median_ms),
            theme::TEXT_STRONG,
        );
        widgets::stat_tile_width(
            ui,
            w,
            t(lang, "sess.low"),
            &millis(lang, timing.worst_ms),
            theme::TEXT_STRONG,
        );
        if let Some(fps) = fps {
            let value = format!("{} fps", fps.round() as u32);
            widgets::stat_tile_width(ui, w, t(lang, "sess.base"), &value, theme::TEXT_STRONG);
        }
        if let Some(share) = share {
            let value = format!("{}%", (share * 100.0).round() as u32);
            widgets::stat_tile_width(ui, w, t(lang, "measure.share"), &value, theme::TEXT_STRONG);
        }
    });
    let detail = t(lang, "measure.detail")
        .replace("{model}", &millis(lang, timing.model_median_ms))
        .replace("{passes}", &timing.passes.to_string())
        .replace("{samples}", &timing.samples.to_string());
    ui.add_space(4.0);
    ui.add(egui::Label::new(RichText::new(detail).small().color(theme::MUTED)).wrap());
    if timing.cadence > 1 {
        let text = t(lang, "measure.cadence").replace("{n}", &timing.cadence.to_string());
        ui.add(egui::Label::new(RichText::new(text).small().color(theme::MUTED)).wrap());
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};

    fn timing() -> NrTiming {
        NrTiming {
            samples: 120,
            median_ms: 2.0,
            worst_ms: 3.25,
            model_median_ms: 1.6,
            passes: 2,
            cadence: 1,
            carried: 0,
        }
    }

    fn app(lang: Lang, ini: &str, timing: Option<NrTiming>) -> RetoqueApp {
        let mut builder = retoque_data::session::PacingBuilder::default();
        for i in 0..30 {
            builder.observe(&format!(
                "[15:46:{:02}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: upscaled: active on the \
                 upscaler's output | 120 presents : 120 renders = 1.00x on screen",
                i * 2
            ));
        }
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        app.view.target = Some("/synthetic/game".into());
        app.view.ini = Some(Ok(Some(Ini::parse(ini.as_bytes()))));
        app.view.intelligence.latest_log = Some(Summary {
            pacing: builder.finish(),
            nr_timing: timing,
            ..Default::default()
        });
        app
    }

    fn card(app: RetoqueApp) -> Harness<'static, RetoqueApp> {
        let mut h = Harness::builder()
            .with_size([1100., 600.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.measure_card(ui), app);
        h.run_steps(2);
        h
    }

    #[test]
    fn off_offers_to_measure_through_a_review_of_the_one_key() {
        for lang in [Lang::En, Lang::Pt] {
            let mut h = card(app(lang, "[DlssNr]\nGpuTiming=auto\n", None));
            h.get_by_label_contains(t(lang, "measure.intro"));
            assert!(h.query_by_label(t(lang, "measure.stop")).is_none());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "measure.start"))
                .click();
            h.run_steps(2);
            let sent: Vec<_> = h.state().cmd_rx.try_iter().collect();
            let inputs = sent.iter().find_map(|c| match c {
                AppCommand::RequestPlan(_, PlanKind::Settings, inputs) => Some(inputs.clone()),
                _ => None,
            });
            assert_eq!(measure_request(inputs.as_ref()), Some(true), "{lang:?}");
            // The review is titled and confirmed with the label's verb, without "…".
            let verb = measure_verb(lang, true);
            assert!(!verb.ends_with('…') && t(lang, "measure.start").starts_with(verb));
        }
    }

    #[test]
    fn on_without_data_says_what_to_do_and_offers_to_turn_it_off() {
        let lang = Lang::Pt;
        let mut h = card(app(lang, "[DlssNr]\nGpuTiming=true\n", None));
        h.get_by_label_contains(t(lang, "measure.waiting"));
        h.get_by_role_and_label(egui::Role::Button, t(lang, "measure.stop"))
            .click();
        h.run_steps(2);
        let sent: Vec<_> = h.state().cmd_rx.try_iter().collect();
        assert!(sent.iter().any(|c| matches!(c,
            AppCommand::RequestPlan(_, PlanKind::Settings, PlanInputs::Settings(e))
                if e == &vec![edit(false)])));
        assert_eq!(
            measure_request(Some(&PlanInputs::Settings(vec![edit(false)]))),
            Some(false)
        );
        // Any other settings review is not a measurement.
        let other = PlanInputs::Settings(vec![edit(true), ("DlssNr".into(), "Stage".into(), "0".into())]);
        assert_eq!(measure_request(Some(&other)), None);
    }

    #[test]
    fn a_measured_session_shows_time_per_frame_frame_rate_and_share() {
        for lang in [Lang::En, Lang::Pt] {
            let h = card(app(lang, "[DlssNr]\nGpuTiming=true\n", Some(timing())));
            // 2 ms of a 60 fps frame: 12%.
            h.get_by_label_contains(&format!("{}: {}", t(lang, "measure.per_frame"), millis(lang, 2.0)));
            h.get_by_label_contains(&format!("{}: {}", t(lang, "sess.low"), millis(lang, 3.25)));
            h.get_by_label_contains("60 fps");
            h.get_by_label_contains(&format!("{}: 12%", t(lang, "measure.share")));
            h.get_by_label_contains(&millis(lang, 1.6));
            h.get_by_label_contains(t(lang, "measure.still_on"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "measure.stop"));
        }
        // Model cadence: the times stay, the share does not.
        let mut cadence = timing();
        cadence.cadence = 2;
        let h = card(app(Lang::En, "[DlssNr]\nGpuTiming=auto\n", Some(cadence)));
        assert!(h.query_by_label_contains(t(Lang::En, "measure.share")).is_none());
        h.get_by_label_contains(&t(Lang::En, "measure.cadence").replace("{n}", "2"));
        h.get_by_role_and_label(egui::Role::Button, t(Lang::En, "measure.start"));
    }

    #[test]
    fn no_offer_on_vulkan_or_with_nr_off_and_none_before_the_ini_is_read() {
        let mut vulkan = app(Lang::En, "[DlssNr]\nGpuTiming=auto\n", None);
        if let Some(s) = vulkan.view.intelligence.latest_log.as_mut() {
            s.api = Some("Vulkan".into());
        }
        let h = card(vulkan);
        assert!(h.query_by_label_contains(t(Lang::En, "measure.title")).is_none());
        let mut unread = app(Lang::En, "", None);
        unread.view.ini = None;
        let h = card(unread);
        assert!(h.query_by_label_contains(t(Lang::En, "measure.title")).is_none());
        let mut off = app(Lang::En, "[DlssNr]\nGpuTiming=auto\n", None);
        off.view.status = Some(Ok(Status {
            nr_enabled: Some(false),
            ..Default::default()
        }));
        let h = card(off);
        assert!(h.query_by_label_contains(t(Lang::En, "measure.title")).is_none());
    }
}
