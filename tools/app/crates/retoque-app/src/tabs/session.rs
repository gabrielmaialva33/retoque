//! "Last session": how the newest log's session went, read from the summary the game page already loaded
//! (`retoque_data::session`). Numbers, the real frame rate over time, and what to change. No disk access here.

use crate::app::*;
use egui::{Color32, Sense, Stroke, pos2, vec2};
use retoque_data::session::{self, Advice, Level, Pacing};

/// Decimal with the language's separator (pt-BR writes 4,0).
pub(crate) fn decimal(lang: Lang, value: f64, places: usize) -> String {
    let text = format!("{value:.places$}");
    if lang.decimal_comma() {
        text.replace('.', ",")
    } else {
        text
    }
}

fn advice_text(lang: Lang, advice: &Advice) -> String {
    match advice {
        Advice::Short => t(lang, "sess.short").into(),
        Advice::Steady => t(lang, "sess.steady").into(),
        Advice::FakeNvapi => t(lang, "sess.fake_nvapi").into(),
        Advice::LowBaseForFrameGeneration { base, x4 } => {
            let mut text = t(lang, "sess.low_base").replace("{base}", &base.to_string());
            if *x4 {
                text.push(' ');
                text.push_str(t(lang, "sess.low_base_x4"));
            }
            text
        }
        Advice::Drops { percent } => t(lang, "sess.drops_advice").replace("{percent}", &percent.to_string()),
        Advice::NrTurnedOff => t(lang, "sess.nr_off").into(),
    }
}

impl RetoqueApp {
    pub(crate) fn session_card(&self, ui: &mut egui::Ui) {
        let Some(pacing) = self
            .view
            .intelligence
            .latest_log
            .as_ref()
            .and_then(|s| s.pacing.as_ref())
        else {
            return;
        };
        let lang = self.lang;
        widgets::card(ui, |ui| {
            ui.horizontal_wrapped(|ui| {
                widgets::card_title(ui, t(lang, "sess.title"));
                let minutes = (pacing.active_seconds / 60.0).round().max(1.0) as u32;
                let mut period = format!("{}–{} · {minutes} min", pacing.start, pacing.end);
                if let Some(driver) = &pacing.driver {
                    period.push_str(&format!(" · {} {driver}", t(lang, "sess.driver")));
                }
                ui.label(RichText::new(period).small().color(theme::MUTED));
            });
            ui.horizontal_wrapped(|ui| {
                ui.spacing_mut().item_spacing = vec2(10.0, 10.0);
                let fps = |v: f64| format!("{} fps", v.round() as u32);
                let shown = 4 + usize::from(pacing.generation_share >= 0.05);
                let w = widgets::row_width(shown, ui.available_width(), 130.0, 10.0);
                widgets::stat_tile_width(
                    ui,
                    w,
                    t(lang, "sess.base"),
                    &fps(pacing.base_fps_median),
                    theme::TEXT_STRONG,
                );
                widgets::stat_tile_width(
                    ui,
                    w,
                    t(lang, "sess.low"),
                    &fps(pacing.base_fps_low),
                    theme::TEXT_STRONG,
                );
                widgets::stat_tile_width(
                    ui,
                    w,
                    t(lang, "sess.screen"),
                    &fps(pacing.screen_fps_median),
                    theme::TEXT_STRONG,
                );
                if pacing.generation_share >= 0.05 {
                    let value = t(lang, "sess.generation_value")
                        .replace("{x}", &decimal(lang, pacing.generation_multiplier, 1))
                        .replace(
                            "{percent}",
                            &((pacing.generation_share * 100.0).round() as u32).to_string(),
                        );
                    widgets::stat_tile_width(ui, w, t(lang, "sess.generation"), &value, theme::TEXT_STRONG);
                }
                let drops = format!("{}%", (pacing.drop_share * 100.0).round() as u32);
                widgets::stat_tile_width(ui, w, t(lang, "sess.drops"), &drops, theme::TEXT_STRONG);
            });
            ui.add_space(6.0);
            curve(ui, lang, pacing);
            ui.add_space(6.0);
            for advice in session::advice(pacing) {
                let (mark, color) = match advice.level() {
                    Level::Good => ("✓", theme::GOOD),
                    Level::Info => ("i", theme::MUTED),
                    Level::Warn => ("!", theme::WARN),
                };
                ui.horizontal_top(|ui| {
                    ui.label(RichText::new(mark).strong().color(color));
                    ui.add(egui::Label::new(advice_text(lang, &advice)).wrap());
                });
            }
        });
        ui.add_space(theme::CARD_GAP);
    }
}

/// The real frame rate over the session: one thin line, the median as a recessive reference, and the value under
/// the pointer. One series, so the card's caption names it instead of a legend.
fn curve(ui: &mut egui::Ui, lang: Lang, pacing: &Pacing) {
    if pacing.curve.len() < 2 {
        return;
    }
    ui.label(RichText::new(t(lang, "sess.curve")).small().color(theme::MUTED));
    let (rect, response) = ui.allocate_exact_size(vec2(ui.available_width(), 72.0), Sense::hover());
    let painter = ui.painter_at(rect);
    // A loading-screen spike would decide the scale and flatten the session; the top follows the bulk of the
    // windows (the 95th percentile, or twice the median when that is higher) and taller points touch the edge.
    let mut sorted: Vec<f32> = pacing.curve.iter().map(|p| p.1).collect();
    sorted.sort_by(f32::total_cmp);
    let p95 = sorted[((sorted.len() - 1) as f32 * 0.95).round() as usize];
    let top = (p95 * 1.15).max(pacing.base_fps_median as f32 * 2.0).max(1.0);
    let end = pacing.curve.last().map(|p| p.0).unwrap_or(1.0).max(1.0);
    let at = |(t, fps): (f32, f32)| {
        pos2(
            rect.left() + rect.width() * (t / end),
            rect.bottom() - rect.height() * (fps / top).min(1.0),
        )
    };
    let median_y = rect.bottom() - rect.height() * (pacing.base_fps_median as f32 / top);
    painter.line_segment(
        [pos2(rect.left(), median_y), pos2(rect.right(), median_y)],
        Stroke::new(1.0, theme::CARD_EDGE),
    );
    painter.text(
        pos2(rect.right() - 2.0, median_y - 2.0),
        egui::Align2::RIGHT_BOTTOM,
        format!(
            "{} {} fps",
            t(lang, "sess.median"),
            pacing.base_fps_median.round() as u32
        ),
        egui::FontId::proportional(11.0),
        theme::MUTED,
    );
    let points: Vec<_> = pacing.curve.iter().copied().map(at).collect();
    painter.add(egui::Shape::line(points, Stroke::new(2.0, theme::ACCENT)));
    if let Some(pointer) = response.hover_pos() {
        let t = (pointer.x - rect.left()) / rect.width() * end;
        if let Some(nearest) = pacing
            .curve
            .iter()
            .min_by(|a, b| (a.0 - t).abs().total_cmp(&(b.0 - t).abs()))
        {
            let p = at(*nearest);
            painter.line_segment(
                [pos2(p.x, rect.top()), pos2(p.x, rect.bottom())],
                Stroke::new(1.0, Color32::from_white_alpha(40)),
            );
            painter.circle(p, 4.0, theme::ACCENT, Stroke::new(2.0, theme::CARD));
            let minute = (nearest.0 / 60.0).floor() as u32;
            let second = (nearest.0 % 60.0).floor() as u32;
            response.on_hover_text_at_pointer(format!("+{minute}:{second:02} · {} fps", nearest.1.round() as u32));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use egui_kittest::{Harness, kittest::Queryable};

    #[test]
    fn last_session_card_shows_numbers_and_advice_in_both_languages() {
        let mut builder = retoque_data::session::PacingBuilder::default();
        for i in 0..30 {
            builder.observe(&format!(
                "[15:46:{:02}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: upscaled: active on the \
                 upscaler's output | 184 presents : 46 renders = 4.00x on screen",
                i * 2
            ));
        }
        let pacing = builder.finish().unwrap();
        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.intelligence.latest_log = Some(Summary {
                pacing: Some(pacing.clone()),
                ..Default::default()
            });
            let h = Harness::builder()
                .with_size([1100., 700.])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.session_card(ui), app);
            h.get_by_label_contains(t(lang, "sess.title"));
            // Real frames and the worst 5% are both 23 in this steady synthetic session.
            assert_eq!(h.query_all_by_label_contains("23 fps").count(), 2);
            h.get_by_label_contains("92 fps");
            h.get_by_label_contains(&decimal(lang, 4.0, 1));
            h.get_by_label_contains(&advice_text(
                lang,
                &Advice::LowBaseForFrameGeneration { base: 23, x4: true },
            ));
        }
        // No log with status lines: no card at all.
        let app = crate::app_tests::shell_tests::app();
        let h = Harness::builder()
            .with_size([1100., 300.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.session_card(ui), app);
        assert!(h.query_by_label_contains(t(Lang::Pt, "sess.title")).is_none());
        assert!(h.query_by_label_contains(t(Lang::En, "sess.title")).is_none());
    }

    #[test]
    fn decimals_follow_the_language_and_advice_fills_its_numbers() {
        assert_eq!(decimal(Lang::Pt, 4.0, 1), "4,0");
        assert_eq!(decimal(Lang::En, 4.0, 1), "4.0");
        for lang in [Lang::En, Lang::Pt] {
            let text = advice_text(lang, &Advice::LowBaseForFrameGeneration { base: 23, x4: true });
            assert!(text.contains("23") && !text.contains('{'), "{text}");
            let text = advice_text(lang, &Advice::Drops { percent: 12 });
            assert!(text.contains("12") && !text.contains('{'), "{text}");
        }
    }
}
