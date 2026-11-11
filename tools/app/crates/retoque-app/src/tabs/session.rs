//! "Last session": how the newest log's session went, read from the summary the game page already loaded
//! (`retoque_data::session`). Numbers, the real frame rate over time, and what to change. No disk access here.

use crate::app::*;
use egui::{Color32, Sense, Stroke, pos2, vec2};
use retoque_data::session::{self, Advice, Level, Pacing};

/// Decimal with the language's separator (pt-BR writes 4,0).
fn decimal(lang: Lang, value: f64, places: usize) -> String {
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

/// A change the card offers under an advice, through the same reviewed settings plan as the Settings tab (refused while
/// the game runs, recorded for undo). `label` is the button ("Use x3…"); the review's title and confirm drop the "…",
/// and `done` is the toast that repeats it.
#[derive(Clone, Debug, PartialEq)]
pub(crate) struct SessionFix {
    pub edit: Edit,
    pub label: &'static str,
    pub done: &'static str,
}

/// The change an advice maps to, offered only while the game's OptiScaler.ini holds the value the advice is about.
/// x4 -> x3 for frame generation: with LSFG on, its multiplier (4 -> 3, `recommend::lsfg_edits`); otherwise the DLSS-G
/// count sent through sm86 or the game's own DLSS-G (`[DLSSG] OverrideInterpolationCount`, three generated frames ->
/// two, as `recommend::SM86_MFG` asks). An automatic count is the game's own choice and gets no button, and advice
/// about the game's DLSS mode stays text: the app does not change the game's settings.
pub(crate) fn fix_for(advice: &Advice, ini: &Ini) -> Option<SessionFix> {
    let Advice::LowBaseForFrameGeneration { x4: true, .. } = advice else {
        return None;
    };
    let number = |section: &str, key: &str| ini.get(section, key).and_then(|v| v.trim().parse::<f64>().ok());
    let lsfg = ini
        .get("LSFG", "Enabled")
        .is_some_and(|v| matches!(v.trim().to_ascii_lowercase().as_str(), "true" | "1"));
    let (section, key, value) = if lsfg {
        (number("LSFG", "Multiplier") == Some(4.0)).then_some(("LSFG", "Multiplier", "3"))?
    } else {
        (number("DLSSG", "OverrideInterpolationCount") == Some(3.0)).then_some((
            "DLSSG",
            "OverrideInterpolationCount",
            "2",
        ))?
    };
    Some(SessionFix {
        edit: (section.into(), key.into(), value.into()),
        label: "sess.use_x3",
        done: "sess.use_x3_done",
    })
}

impl RetoqueApp {
    /// The session change the review with this request id was opened for, if any.
    pub(crate) fn session_fix_for(&self, request: u64) -> Option<&SessionFix> {
        self.view
            .session_fix
            .as_ref()
            .filter(|(id, _)| *id == request)
            .map(|(_, fix)| fix)
    }

    /// Opens the reviewed settings plan for a session change; the review and the toast then repeat its verb.
    pub(crate) fn start_session_fix(&mut self, target: PathBuf, fix: SessionFix) {
        if self.view.target.as_ref() != Some(&target) || self.busy() {
            return;
        }
        self.start_plan(PlanKind::Settings, PlanInputs::Settings(vec![fix.edit.clone()]));
        self.view.session_fix = Some((self.view.request, fix));
    }

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
        let ini = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(Option::as_ref);
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
                if let (Some(fix), Some(target)) = (ini.and_then(|ini| fix_for(&advice, ini)), &self.view.target) {
                    let button = ui.add_enabled(!self.busy(), egui::Button::new(t(lang, fix.label)));
                    if button.clicked() {
                        self.cmd_tx.send(AppCommand::SessionFix(target.clone(), fix));
                    }
                }
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

    fn x4_pacing() -> session::Pacing {
        let mut builder = retoque_data::session::PacingBuilder::default();
        for i in 0..30 {
            builder.observe(&format!(
                "[15:46:{:02}.000000] [I] DlssNr::ReportRuntimeStatus DLSS-NR status: upscaled: active on the \
                 upscaler's output | 184 presents : 46 renders = 4.00x on screen",
                i * 2
            ));
        }
        builder.finish().unwrap()
    }

    #[test]
    fn x3_is_offered_only_for_x4_advice_and_the_x4_value_of_the_provider_in_use() {
        let x4 = Advice::LowBaseForFrameGeneration { base: 23, x4: true };
        let fix = |text: &str, advice: &Advice| fix_for(advice, &Ini::parse(text.as_bytes())).map(|f| f.edit);
        let edit = |s: &str, k: &str, v: &str| Some((s.to_owned(), k.to_owned(), v.to_owned()));
        // DLSS-G through sm86 or the game: three generated frames become two.
        assert_eq!(
            fix("[DLSSG]\nOverrideInterpolationCount=3\n", &x4),
            edit("DLSSG", "OverrideInterpolationCount", "2")
        );
        // LSFG on: its multiplier, and the DLSS-G count is not touched.
        assert_eq!(
            fix(
                "[LSFG]\nEnabled=true\nMultiplier=4\n[DLSSG]\nOverrideInterpolationCount=3\n",
                &x4
            ),
            edit("LSFG", "Multiplier", "3")
        );
        for text in [
            "[DLSSG]\nOverrideInterpolationCount=auto\n",
            "[DLSSG]\nOverrideInterpolationCount=2\n",
            "[DLSSG]\n",
            "",
            "[LSFG]\nEnabled=false\nMultiplier=4\n",
            "[LSFG]\nEnabled=true\nMultiplier=3\n",
            "[LSFG]\nEnabled=true\nMultiplier=auto\n[DLSSG]\nOverrideInterpolationCount=3\n",
        ] {
            assert_eq!(fix(text, &x4), None, "{text}");
        }
        let count3 = "[DLSSG]\nOverrideInterpolationCount=3\n";
        for advice in [
            Advice::LowBaseForFrameGeneration { base: 23, x4: false },
            Advice::Drops { percent: 12 },
            Advice::Steady,
            Advice::Short,
            Advice::FakeNvapi,
            Advice::NrTurnedOff,
        ] {
            assert_eq!(fix(count3, &advice), None, "{advice:?}");
        }
    }

    #[test]
    fn use_x3_reviews_exactly_that_key_and_the_confirm_and_toast_repeat_it() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let game = temp.path().join("game");
            std::fs::create_dir(&game).unwrap();
            let before = "; keep\n[DLSSG]\nOverrideInterpolationCount=3\n[LSFG]\nEnabled=false\nMultiplier=4\n";
            std::fs::write(game.join("OptiScaler.ini"), before).unwrap();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.intuitive.history_root = Some(temp.path().join("history"));
            app.view.target = Some(game.clone());
            app.view.ini = Some(Ok(Some(Ini::parse(before.as_bytes()))));
            app.view.intelligence.latest_log = Some(Summary {
                pacing: Some(x4_pacing()),
                ..Default::default()
            });
            let mut h = Harness::builder().with_size([1100., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.session_card(ui);
                    app.review_sheet(ui.ctx());
                    app.handle_commands();
                },
                app,
            );
            let label = t(lang, "sess.use_x3");
            assert!(label.ends_with('…'));
            h.get_by_role_and_label(egui::Role::Button, label).click();
            for _ in 0..400 {
                h.step();
                if h.state().review.is_some() {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            let verb = label.trim_end_matches('…');
            {
                let review = h.state().review.as_ref().unwrap().as_ref().unwrap();
                assert!(review.title.starts_with(verb), "{}", review.title);
                let steps: Vec<_> = review.plans.iter().flat_map(|p| p.steps()).collect();
                assert_eq!(steps.len(), 1, "{steps:?}");
                assert!(matches!(
                    steps[0],
                    Step::SetIni { section, key, old, value, .. }
                        if section == "DLSSG" && key == "OverrideInterpolationCount"
                            && old.as_deref() == Some("3") && value == "2"
                ));
            }
            assert_eq!(std::fs::read_to_string(game.join("OptiScaler.ini")).unwrap(), before);
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, verb).click();
            for _ in 0..400 {
                h.step();
                if !h.state().toasts.is_empty() {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            let toast = h.state().toasts.last().unwrap();
            assert!(!toast.failed, "{}", toast.message);
            assert!(
                toast.message.starts_with(t(lang, "sess.use_x3_done")),
                "{}",
                toast.message
            );
            let after = std::fs::read_to_string(game.join("OptiScaler.ini")).unwrap();
            assert_eq!(
                after,
                before.replace("OverrideInterpolationCount=3", "OverrideInterpolationCount=2")
            );
        }
    }

    #[test]
    fn no_button_when_the_game_does_not_hold_the_x4_value() {
        for text in [
            "[DLSSG]\nOverrideInterpolationCount=auto\n",
            "[DLSSG]\nOverrideInterpolationCount=2\n",
        ] {
            let mut app = crate::app_tests::shell_tests::app();
            app.view.target = Some("/synthetic/game".into());
            app.view.ini = Some(Ok(Some(Ini::parse(text.as_bytes()))));
            app.view.intelligence.latest_log = Some(Summary {
                pacing: Some(x4_pacing()),
                ..Default::default()
            });
            let h = Harness::builder()
                .with_size([1100., 700.])
                .build_ui_state(|ui, app: &mut RetoqueApp| app.session_card(ui), app);
            // The advice is still there, as text.
            h.get_by_label_contains(t(Lang::En, "sess.low_base_x4"));
            assert!(h.query_by_label(t(Lang::En, "sess.use_x3")).is_none(), "{text}");
        }
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
