//! Colours, fonts and spacing. Dark, with one warm accent; status colours are kept apart from the accent so "on"
//! and "good" never read as the same thing.

use std::sync::Arc;

use egui::{Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, TextStyle, Visuals};

pub const ACCENT: Color32 = Color32::from_rgb(0xF0, 0xA8, 0x45);
pub const GOOD: Color32 = Color32::from_rgb(0x6C, 0xCB, 0x8E);
pub const WARN: Color32 = Color32::from_rgb(0xE6, 0xC2, 0x5C);
pub const BAD: Color32 = Color32::from_rgb(0xEE, 0x70, 0x70);
pub const MUTED: Color32 = Color32::from_rgb(0x8E, 0x97, 0xA4);
pub const TEXT: Color32 = Color32::from_rgb(0xD6, 0xDB, 0xE2);
pub const TEXT_STRONG: Color32 = Color32::from_rgb(0xF5, 0xF7, 0xFA);
pub const BG: Color32 = Color32::from_rgb(0x12, 0x14, 0x18);
pub const SIDE: Color32 = Color32::from_rgb(0x17, 0x1A, 0x1F);
pub const CARD: Color32 = Color32::from_rgb(0x1C, 0x20, 0x26);
pub const ERROR_CARD: Color32 = Color32::from_rgb(0x2D, 0x1D, 0x23);
pub const CARD_EDGE: Color32 = Color32::from_rgb(0x2A, 0x2F, 0x37);
/// Text drawn on an accent- or danger-filled button.
pub const ON_ACCENT: Color32 = Color32::from_rgb(0x1A, 0x14, 0x0A);
/// Text drawn over cover or hero art.
pub const ON_IMAGE: Color32 = Color32::from_gray(220);
/// Behind images and previews (darker than a card).
pub const CANVAS: Color32 = Color32::from_rgb(0x0E, 0x10, 0x13);

// Type scale, in points. Body text is 14.5 and small text 13 (set in `apply`); nothing else uses ad-hoc sizes.
/// A card's title.
pub const TITLE: f32 = 17.0;
/// A heading between cards that groups the ones below it.
pub const SECTION: f32 = 20.0;
/// A single large value (a model's version, a game's name without art).
pub const DISPLAY: f32 = 22.0;

/// The vertical gap between two cards, everywhere.
pub const CARD_GAP: f32 = 12.0;

/// A system UI font when there is one (Noto Sans on Linux, Segoe UI on Windows), else egui's own. Nothing is
/// bundled, so the binary carries no font licence.
fn system_font() -> Option<Vec<u8>> {
    let candidates: &[&str] = if cfg!(windows) {
        &[r"C:\Windows\Fonts\segoeui.ttf"]
    } else {
        &[
            "/usr/share/fonts/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
            "/usr/share/fonts/TTF/DejaVuSans.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ]
    };
    candidates.iter().find_map(|p| std::fs::read(p).ok())
}

pub fn apply(ctx: &egui::Context) {
    ctx.set_theme(egui::ThemePreference::Dark);
    if let Some(bytes) = system_font() {
        let mut fonts = FontDefinitions::default();
        fonts
            .font_data
            .insert("system".into(), Arc::new(FontData::from_owned(bytes)));
        if let Some(family) = fonts.families.get_mut(&FontFamily::Proportional) {
            family.insert(0, "system".into());
        }
        ctx.set_fonts(fonts);
    }

    let mut visuals = Visuals::dark();
    visuals.panel_fill = BG;
    visuals.window_fill = CARD;
    visuals.extreme_bg_color = CANVAS;
    visuals.faint_bg_color = Color32::from_rgb(0x19, 0x1C, 0x21);
    visuals.override_text_color = Some(TEXT);
    visuals.selection.bg_fill = ACCENT.gamma_multiply(0.35);
    visuals.selection.stroke.color = TEXT_STRONG;
    visuals.hyperlink_color = ACCENT;
    visuals.widgets.inactive.weak_bg_fill = Color32::from_rgb(0x25, 0x2A, 0x32);
    visuals.widgets.inactive.bg_fill = Color32::from_rgb(0x25, 0x2A, 0x32);
    visuals.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x2E, 0x34, 0x3D);
    visuals.widgets.hovered.bg_fill = Color32::from_rgb(0x2E, 0x34, 0x3D);
    visuals.widgets.inactive.bg_stroke = egui::Stroke::new(1.0, Color32::from_rgb(0x62, 0x6D, 0x7E));
    visuals.widgets.hovered.bg_stroke = egui::Stroke::new(1.5, ACCENT);
    visuals.widgets.active.bg_stroke = egui::Stroke::new(2.0, TEXT_STRONG);
    let radius = CornerRadius::same(8);
    for w in [
        &mut visuals.widgets.noninteractive,
        &mut visuals.widgets.inactive,
        &mut visuals.widgets.hovered,
        &mut visuals.widgets.active,
        &mut visuals.widgets.open,
    ] {
        w.corner_radius = radius;
    }
    ctx.set_visuals(visuals);
    ctx.all_styles_mut(|style| {
        style.animation_time = 0.15;
        style.wrap_mode = Some(egui::TextWrapMode::Wrap);
        style.spacing.item_spacing = egui::vec2(10.0, 8.0);
        style.spacing.button_padding = egui::vec2(12.0, 6.0);
        style.spacing.interact_size.y = 32.0;
        style.text_styles.insert(TextStyle::Body, FontId::proportional(14.5));
        style.text_styles.insert(TextStyle::Button, FontId::proportional(14.5));
        style.text_styles.insert(TextStyle::Small, FontId::proportional(13.0));
        style.text_styles.insert(TextStyle::Heading, FontId::proportional(28.0));
        style.text_styles.insert(TextStyle::Monospace, FontId::monospace(13.0));
    });
}

/// The primary action button: accent fill, dark text. At most one per view.
pub fn primary(text: &str) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.to_string()).strong().color(ON_ACCENT))
        .fill(ACCENT)
        .min_size(egui::vec2(120.0, 32.0))
}

/// A confirm button for something that deletes or uninstalls: the primary's shape with a danger fill.
pub fn danger(text: &str) -> egui::Button<'static> {
    egui::Button::new(egui::RichText::new(text.to_string()).strong().color(ON_ACCENT))
        .fill(BAD)
        .min_size(egui::vec2(120.0, 32.0))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn relative_luminance(c: Color32) -> f64 {
        let ch = |u: u8| {
            let s = f64::from(u) / 255.0;
            if s <= 0.04045 {
                s / 12.92
            } else {
                ((s + 0.055) / 1.055).powf(2.4)
            }
        };
        0.2126 * ch(c.r()) + 0.7152 * ch(c.g()) + 0.0722 * ch(c.b())
    }

    fn contrast(a: Color32, b: Color32) -> f64 {
        let (la, lb) = (relative_luminance(a), relative_luminance(b));
        (la.max(lb) + 0.05) / (la.min(lb) + 0.05)
    }

    fn over(premul_fg: Color32, opaque_bg: Color32) -> Color32 {
        let inv = 1.0 - f64::from(premul_fg.a()) / 255.0;
        let mix = |f: u8, b: u8| (f64::from(f) + f64::from(b) * inv).round().clamp(0.0, 255.0) as u8;
        Color32::from_rgb(
            mix(premul_fg.r(), opaque_bg.r()),
            mix(premul_fg.g(), opaque_bg.g()),
            mix(premul_fg.b(), opaque_bg.b()),
        )
    }

    #[test]
    fn dark_theme_overrides_a_light_system_preference() {
        let ctx = egui::Context::default();
        ctx.set_theme(egui::ThemePreference::Light);
        super::apply(&ctx);
        assert_eq!(ctx.theme(), egui::Theme::Dark);
        assert!(ctx.style_of(egui::Theme::Dark).visuals.dark_mode);
    }

    #[test]
    fn wcag_contrast_ratios_meet_aa_for_text_selection_and_controls() {
        let ctx = egui::Context::default();
        super::apply(&ctx);
        let visuals = &ctx.style_of(egui::Theme::Dark).visuals;

        for bg in [BG, SIDE, CARD, ERROR_CARD] {
            for (name, fg) in [
                ("TEXT", TEXT),
                ("TEXT_STRONG", TEXT_STRONG),
                ("MUTED", MUTED),
                ("ACCENT", ACCENT),
                ("GOOD", GOOD),
                ("WARN", WARN),
                ("BAD", BAD),
            ] {
                let r = contrast(fg, bg);
                assert!(r >= 4.5, "{name} on {bg:?} contrast {r:.2}:1 < 4.5:1");
            }
        }
        assert!(contrast(ON_ACCENT, ACCENT) >= 4.5);
        assert!(contrast(ON_ACCENT, BAD) >= 4.5);

        // Selected items (ComboBox options, Command Palette row, selected text) must remain readable.
        let selected_bg = over(visuals.selection.bg_fill, CARD);
        let sel_ratio = contrast(visuals.selection.stroke.color, selected_bg);
        assert!(sel_ratio >= 4.5, "selected text contrast {sel_ratio:.2}:1 < 4.5:1");

        // Control boundaries on cards meet WCAG 1.4.11 (3:1 non-text contrast), and focus uses TEXT_STRONG.
        let border_ratio = contrast(visuals.widgets.inactive.bg_stroke.color, CARD);
        assert!(
            border_ratio >= 3.0,
            "control border contrast {border_ratio:.2}:1 < 3.0:1"
        );
        assert_eq!(visuals.widgets.active.bg_stroke, egui::Stroke::new(2.0, TEXT_STRONG));
    }
}
