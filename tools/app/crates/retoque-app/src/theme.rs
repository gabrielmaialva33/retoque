//! Colours, fonts and spacing. Dark and light palettes share semantic roles and one warm accent.

use std::sync::Arc;

use egui::{Color32, CornerRadius, FontData, FontDefinitions, FontFamily, FontId, TextStyle, Visuals};

/// Semantic roles shared by the two themes. Resolve from the local UI (including image-backed scopes),
/// never process-global state: independent windows and test contexts may use different themes.
#[derive(Clone, Copy, Debug)]
#[allow(non_snake_case)]
pub struct Palette {
    pub ACCENT: Color32,
    pub GOOD: Color32,
    pub WARN: Color32,
    pub BAD: Color32,
    pub MUTED: Color32,
    pub TEXT: Color32,
    pub TEXT_STRONG: Color32,
    pub BG: Color32,
    pub SIDE: Color32,
    pub CARD: Color32,
    pub ERROR_CARD: Color32,
    pub CARD_EDGE: Color32,
    pub ON_ACCENT: Color32,
    pub ON_IMAGE: Color32,
    pub CANVAS: Color32,
    pub FAINT: Color32,
    pub WIDGET: Color32,
    pub HOVER: Color32,
    pub ACTIVE: Color32,
    pub OPEN: Color32,
    pub CONTROL_EDGE: Color32,
}

pub const DARK: Palette = Palette {
    ACCENT: Color32::from_rgb(0xF0, 0xA8, 0x45),
    GOOD: Color32::from_rgb(0x6C, 0xCB, 0x8E),
    WARN: Color32::from_rgb(0xE6, 0xC2, 0x5C),
    BAD: Color32::from_rgb(0xEE, 0x70, 0x70),
    MUTED: Color32::from_rgb(0x8E, 0x97, 0xA4),
    TEXT: Color32::from_rgb(0xD6, 0xDB, 0xE2),
    TEXT_STRONG: Color32::from_rgb(0xF5, 0xF7, 0xFA),
    BG: Color32::from_rgb(0x12, 0x14, 0x18),
    SIDE: Color32::from_rgb(0x17, 0x1A, 0x1F),
    CARD: Color32::from_rgb(0x1C, 0x20, 0x26),
    ERROR_CARD: Color32::from_rgb(0x2D, 0x1D, 0x23),
    CARD_EDGE: Color32::from_rgb(0x2A, 0x2F, 0x37),
    ON_ACCENT: Color32::from_rgb(0x1A, 0x14, 0x0A),
    ON_IMAGE: Color32::from_gray(220),
    CANVAS: Color32::from_rgb(0x0E, 0x10, 0x13),
    FAINT: Color32::from_rgb(0x19, 0x1C, 0x21),
    WIDGET: Color32::from_rgb(0x25, 0x2A, 0x32),
    HOVER: Color32::from_rgb(0x2E, 0x34, 0x3D),
    ACTIVE: Color32::from_gray(55),
    OPEN: Color32::from_gray(27),
    CONTROL_EDGE: Color32::from_rgb(0x6C, 0x77, 0x88),
};

pub const LIGHT: Palette = Palette {
    ACCENT: Color32::from_rgb(0x80, 0x50, 0x0C),
    GOOD: Color32::from_rgb(0x24, 0x68, 0x3D),
    WARN: Color32::from_rgb(0x73, 0x54, 0x00),
    BAD: Color32::from_rgb(0xAD, 0x26, 0x36),
    MUTED: Color32::from_rgb(0x50, 0x5B, 0x68),
    TEXT: Color32::from_rgb(0x27, 0x31, 0x3D),
    TEXT_STRONG: Color32::from_rgb(0x13, 0x1B, 0x25),
    BG: Color32::from_rgb(0xF5, 0xF6, 0xF8),
    SIDE: Color32::from_rgb(0xEB, 0xEE, 0xF2),
    CARD: Color32::from_rgb(0xFF, 0xFF, 0xFF),
    ERROR_CARD: Color32::from_rgb(0xFF, 0xF1, 0xF2),
    CARD_EDGE: Color32::from_rgb(0xCF, 0xD5, 0xDD),
    ON_ACCENT: Color32::from_rgb(0xFF, 0xFF, 0xFF),
    ON_IMAGE: Color32::from_rgb(0xDC, 0xDC, 0xDC),
    CANVAS: Color32::from_rgb(0xF5, 0xF6, 0xF8),
    FAINT: Color32::from_rgb(0xEF, 0xF1, 0xF4),
    WIDGET: Color32::from_rgb(0xED, 0xF0, 0xF4),
    HOVER: Color32::from_rgb(0xE3, 0xE7, 0xED),
    ACTIVE: Color32::from_rgb(0xE8, 0xEB, 0xEF),
    OPEN: Color32::from_rgb(0xED, 0xF0, 0xF4),
    CONTROL_EDGE: Color32::from_rgb(0x72, 0x7D, 0x8C),
};

pub fn palette(ui: &egui::Ui) -> &'static Palette {
    for_theme(if ui.visuals().dark_mode {
        egui::Theme::Dark
    } else {
        egui::Theme::Light
    })
}

pub fn for_theme(theme: egui::Theme) -> &'static Palette {
    match theme {
        egui::Theme::Dark => &DARK,
        egui::Theme::Light => &LIGHT,
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
pub enum Appearance {
    System,
    #[default]
    Dark,
    Light,
}
impl Appearance {
    pub fn apply(self, ctx: &egui::Context) {
        ctx.set_theme(match self {
            Self::System => egui::ThemePreference::System,
            Self::Dark => egui::ThemePreference::Dark,
            Self::Light => egui::ThemePreference::Light,
        });
    }
    pub fn label(self) -> &'static str {
        match self {
            Self::System => "prefs.appearance_system",
            Self::Dark => "prefs.appearance_dark",
            Self::Light => "prefs.appearance_light",
        }
    }
}

// These are image/compositor roles, not UI surfaces. Image pixels and texture tints never change theme.
pub const IMAGE_TINT: Color32 = Color32::WHITE;
pub const IMAGE_BLACK: Color32 = Color32::BLACK;
pub const IMAGE_INK: Color32 = Color32::WHITE;
pub const IMAGE_SCRIM: Color32 = Color32::from_black_alpha(210);
pub const CLEAR: Color32 = Color32::TRANSPARENT;
pub const PLACEHOLDER: Color32 = Color32::PLACEHOLDER;
pub const CHIP_TINT: f32 = 0.10;
pub const NOTICE_TINT: f32 = 0.08;
pub const HERO_SHADE_MIN: u8 = 216;
pub const HERO_SHADE_STOPS: [(f32, f32); 3] = [(0.0, 232.0), (0.42, 224.0), (1.0, HERO_SHADE_MIN as f32)];
pub fn image_shade(alpha: u8) -> Color32 {
    Color32::from_black_alpha(alpha)
}

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

    for theme in [egui::Theme::Dark, egui::Theme::Light] {
        ctx.style_mut_of(theme, |style| style.visuals = visuals(theme));
    }
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

/// Complete visuals for one theme; both are installed once, so OS changes need no font reload or rebuild.
pub fn visuals(theme: egui::Theme) -> Visuals {
    let p = for_theme(theme);
    let mut v = match theme {
        egui::Theme::Dark => Visuals::dark(),
        egui::Theme::Light => Visuals::light(),
    };
    v.panel_fill = p.BG;
    v.window_fill = p.CARD;
    v.window_stroke.color = p.CARD_EDGE;
    v.extreme_bg_color = p.CANVAS;
    v.faint_bg_color = p.FAINT;
    v.code_bg_color = p.WIDGET;
    v.override_text_color = Some(p.TEXT);
    v.weak_text_color = Some(p.MUTED);
    v.selection.bg_fill = p.ACCENT.gamma_multiply(0.35);
    v.selection.stroke.color = p.TEXT_STRONG;
    v.hyperlink_color = p.ACCENT;
    v.warn_fg_color = p.WARN;
    v.error_fg_color = p.BAD;
    v.text_cursor.stroke.color = p.TEXT_STRONG;
    for (w, fill, edge, fg) in [
        (&mut v.widgets.noninteractive, p.CARD, p.CARD_EDGE, p.TEXT),
        (&mut v.widgets.inactive, p.WIDGET, p.CONTROL_EDGE, p.TEXT),
        (&mut v.widgets.hovered, p.HOVER, p.ACCENT, p.TEXT_STRONG),
        (&mut v.widgets.active, p.ACTIVE, p.TEXT_STRONG, p.TEXT_STRONG),
        (&mut v.widgets.open, p.OPEN, p.CONTROL_EDGE, p.TEXT),
    ] {
        w.weak_bg_fill = fill;
        w.bg_fill = fill;
        w.bg_stroke.color = edge;
        w.fg_stroke.color = fg;
        w.corner_radius = CornerRadius::same(8);
    }
    v.widgets.inactive.bg_stroke.width = 1.0;
    v.widgets.hovered.bg_stroke.width = 1.5;
    v.widgets.active.bg_stroke.width = 2.0;
    v
}

/// The primary action button. At most one per view.
pub fn primary(ui: &egui::Ui, text: &str) -> egui::Button<'static> {
    let p = palette(ui);
    egui::Button::new(egui::RichText::new(text.to_string()).strong().color(p.ON_ACCENT))
        .fill(p.ACCENT)
        .min_size(egui::vec2(120.0, 32.0))
}

/// A confirm button for something that deletes or uninstalls.
pub fn danger(ui: &egui::Ui, text: &str) -> egui::Button<'static> {
    let p = palette(ui);
    egui::Button::new(egui::RichText::new(text.to_string()).strong().color(p.ON_ACCENT))
        .fill(p.BAD)
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
        for theme in [egui::Theme::Dark, egui::Theme::Light] {
            let p = for_theme(theme);
            let v = visuals(theme);
            for bg in [p.BG, p.SIDE, p.CARD, p.ERROR_CARD, p.CANVAS, p.FAINT] {
                for (name, fg) in [
                    ("TEXT", p.TEXT),
                    ("TEXT_STRONG", p.TEXT_STRONG),
                    ("MUTED", p.MUTED),
                    ("ACCENT", p.ACCENT),
                    ("GOOD", p.GOOD),
                    ("WARN", p.WARN),
                    ("BAD", p.BAD),
                ] {
                    assert!(
                        contrast(fg, bg) >= 4.5,
                        "{theme:?}: {name} on {bg:?}: {:.2}",
                        contrast(fg, bg)
                    );
                }
                for w in [v.widgets.inactive, v.widgets.hovered, v.widgets.active, v.widgets.open] {
                    assert!(contrast(w.bg_stroke.color, bg) >= 3.0, "{theme:?}: border on {bg:?}");
                    assert!(
                        contrast(w.bg_stroke.color, w.bg_fill) >= 3.0,
                        "{theme:?}: inner control border"
                    );
                    assert!(contrast(p.TEXT, w.bg_fill) >= 4.5, "{theme:?}: widget text");
                    assert!(contrast(w.fg_stroke.color, w.bg_fill) >= 4.5, "{theme:?}: widget icon");
                }
                let selected = over(v.selection.bg_fill, bg);
                assert!(
                    contrast(v.selection.stroke.color, selected) >= 4.5,
                    "{theme:?}: selection"
                );
                assert!(contrast(p.TEXT_STRONG, bg) >= 3.0, "{theme:?}: focus ring/star");
                assert!(contrast(p.ACCENT, bg) >= 3.0, "{theme:?}: chart");
            }
            assert!(contrast(p.ON_ACCENT, p.ACCENT) >= 4.5, "{theme:?}: primary");
            assert!(contrast(p.ON_ACCENT, p.BAD) >= 4.5, "{theme:?}: danger");
            assert_eq!(v.widgets.active.bg_stroke, egui::Stroke::new(2.0, p.TEXT_STRONG));
            assert!(contrast(p.ON_IMAGE, over(IMAGE_SCRIM, IMAGE_TINT)) >= 4.5);
            assert!(contrast(p.MUTED, p.CARD_EDGE) >= 4.5, "{theme:?}: placeholder initials");
            for bg in [p.BG, p.SIDE, p.CARD] {
                for fg in [p.ACCENT, p.GOOD, p.WARN, p.BAD, p.MUTED] {
                    assert!(
                        contrast(fg, over(fg.gamma_multiply(CHIP_TINT), bg)) >= 4.5,
                        "{theme:?}: badge {fg:?}"
                    );
                }
                for fg in [p.TEXT, p.TEXT_STRONG, p.MUTED] {
                    assert!(
                        contrast(fg, over(p.ACCENT.gamma_multiply(NOTICE_TINT), bg)) >= 4.5,
                        "{theme:?}: tinted card"
                    );
                }
                assert!(
                    contrast(p.TEXT_STRONG, over(p.ACCENT.gamma_multiply(0.22), bg)) >= 4.5,
                    "{theme:?}: selected row"
                );
            }
            let hero = over(image_shade(HERO_SHADE_MIN), IMAGE_TINT);
            for fg in [DARK.TEXT, DARK.TEXT_STRONG, DARK.MUTED, DARK.WARN] {
                assert!(contrast(fg, hero) >= 4.5, "hero on white art");
            }
            // The opposite ink under image focus outlines and wipe rules cannot disappear on white/black art.
            assert!(contrast(IMAGE_INK, IMAGE_BLACK) >= 3.0);
        }
    }

    #[test]
    fn appearance_follows_live_system_changes_and_explicit_choices_override_it() {
        let ctx = egui::Context::default();
        apply(&ctx);
        let frame = |theme| {
            let mut output = ctx.run_ui(
                egui::RawInput {
                    system_theme: Some(theme),
                    ..Default::default()
                },
                |_| {},
            );
            output.textures_delta.clear();
        };
        frame(egui::Theme::Light);
        assert_eq!(ctx.theme(), egui::Theme::Dark);
        Appearance::System.apply(&ctx);
        assert_eq!(ctx.theme(), egui::Theme::Light);
        frame(egui::Theme::Dark);
        assert_eq!(ctx.theme(), egui::Theme::Dark);
        Appearance::Light.apply(&ctx);
        frame(egui::Theme::Dark);
        assert_eq!(ctx.theme(), egui::Theme::Light);
        assert_eq!(ctx.style_of(ctx.theme()).visuals.panel_fill, LIGHT.BG);
        Appearance::Dark.apply(&ctx);
        frame(egui::Theme::Light);
        assert_eq!(ctx.theme(), egui::Theme::Dark);
        assert_eq!(ctx.style_of(ctx.theme()).visuals.panel_fill, DARK.BG);
    }
}
