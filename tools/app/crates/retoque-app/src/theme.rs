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
    visuals.extreme_bg_color = Color32::from_rgb(0x0E, 0x10, 0x13);
    visuals.faint_bg_color = Color32::from_rgb(0x19, 0x1C, 0x21);
    visuals.override_text_color = Some(TEXT);
    visuals.selection.bg_fill = ACCENT.gamma_multiply(0.35);
    visuals.selection.stroke.color = ACCENT;
    visuals.hyperlink_color = ACCENT;
    visuals.widgets.inactive.weak_bg_fill = Color32::from_rgb(0x25, 0x2A, 0x32);
    visuals.widgets.inactive.bg_fill = Color32::from_rgb(0x25, 0x2A, 0x32);
    visuals.widgets.hovered.weak_bg_fill = Color32::from_rgb(0x2E, 0x34, 0x3D);
    visuals.widgets.hovered.bg_fill = Color32::from_rgb(0x2E, 0x34, 0x3D);
    visuals.widgets.inactive.bg_stroke = egui::Stroke::new(1.0, Color32::from_rgb(0x57, 0x61, 0x70));
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
        style.spacing.interact_size.y = 30.0;
        style.text_styles.insert(TextStyle::Body, FontId::proportional(14.5));
        style.text_styles.insert(TextStyle::Button, FontId::proportional(14.5));
        style.text_styles.insert(TextStyle::Small, FontId::proportional(13.0));
        style.text_styles.insert(TextStyle::Heading, FontId::proportional(28.0));
        style.text_styles.insert(TextStyle::Monospace, FontId::monospace(13.0));
    });
}

/// The primary action button: accent fill, dark text.
pub fn primary(text: &str) -> egui::Button<'static> {
    egui::Button::new(
        egui::RichText::new(text.to_string())
            .strong()
            .color(Color32::from_rgb(0x1A, 0x14, 0x0A)),
    )
    .fill(ACCENT)
    .min_size(egui::vec2(120.0, 34.0))
}

#[cfg(test)]
mod tests {
    #[test]
    fn dark_theme_overrides_a_light_system_preference() {
        let ctx = egui::Context::default();
        ctx.set_theme(egui::ThemePreference::Light);
        super::apply(&ctx);
        assert_eq!(ctx.theme(), egui::Theme::Dark);
        assert!(ctx.style_of(egui::Theme::Dark).visuals.dark_mode);
    }
}
