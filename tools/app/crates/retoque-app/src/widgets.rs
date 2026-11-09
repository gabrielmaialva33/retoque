//! The app's own pieces of UI: library rows with cover art, the game's banner, underlined tabs, cards and chips.
//! Painted by hand where egui's stock widgets would not give the layout (a cover beside two lines of text, a cropped
//! banner fading into the panel).

use std::path::Path;

use egui::{Color32, CornerRadius, FontId, Rect, RichText, Sense, Stroke, Vec2, pos2, vec2};

use crate::theme;

/// `file://` URI egui's image loaders read from disk.
pub fn file_uri(path: &Path) -> String {
    let text = path.display().to_string();
    if cfg!(windows) {
        // egui's file loader reads `file:///C:/x` as a drive path and `file://host/share` as UNC; `file://C:\x`
        // would be taken for a server named "C:" and no image would load.
        if let Some(unc) = text.strip_prefix("\\\\") {
            return format!("file://{}", unc.replace('\\', "/"));
        }
        return format!("file:///{}", text.replace('\\', "/"));
    }
    format!("file://{text}")
}

/// A rounded panel that groups one subject.
pub fn card<R>(ui: &mut egui::Ui, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    tinted_card(ui, theme::CARD, add)
}

pub fn tinted_card<R>(ui: &mut egui::Ui, fill: Color32, add: impl FnOnce(&mut egui::Ui) -> R) -> R {
    egui::Frame::new()
        .fill(fill)
        .stroke(Stroke::new(1.0, theme::CARD_EDGE))
        .corner_radius(12.0)
        .inner_margin(egui::Margin::same(16))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            add(ui)
        })
        .inner
}

/// A card's title line.
pub fn card_title(ui: &mut egui::Ui, text: &str) {
    ui.label(RichText::new(text).size(17.0).strong());
    ui.add_space(4.0);
}

pub fn chip(ui: &mut egui::Ui, text: &str, color: Color32) {
    // Allocate the measured width as one widget so horizontal_wrapped can move the whole badge to the next row.
    // Letting a Frame discover its width from a Label first makes it wrap inside the last few pixels of a row.
    let galley = ui.painter().layout(
        text.to_owned(),
        FontId::proportional(12.5),
        color,
        (ui.max_rect().width() - 20.0).max(30.0),
    );
    let (rect, response) = ui.allocate_exact_size(galley.size() + vec2(20.0, 8.0), Sense::hover());
    ui.painter().rect_filled(rect, 12.0, color.gamma_multiply(0.16));
    ui.painter().rect_stroke(
        rect,
        12.0,
        Stroke::new(1.0, color.gamma_multiply(0.45)),
        egui::StrokeKind::Inside,
    );
    ui.painter().galley(rect.min + vec2(10.0, 4.0), galley, color);
    response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Label, true, text));
}

/// Tabs drawn as text with an accent underline under the current one. Returns the index clicked, if any.
pub fn tabs(ui: &mut egui::Ui, labels: &[&str], current: usize) -> Option<usize> {
    let mut clicked = None;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 22.0;
        for (i, label) in labels.iter().enumerate() {
            let selected = i == current;
            let color = if selected { theme::TEXT_STRONG } else { theme::MUTED };
            let galley = ui
                .painter()
                .layout_no_wrap(label.to_string(), FontId::proportional(16.0), color);
            let (rect, response) = ui.allocate_exact_size(galley.size() + vec2(0.0, 12.0), Sense::click());
            let color = if response.hovered() && !selected {
                theme::TEXT
            } else {
                color
            };
            ui.painter().galley(rect.min, galley, color);
            if selected {
                let left = ui
                    .ctx()
                    .animate_value_with_time(ui.scope_id().with("tab_left"), rect.left(), 0.15);
                let right = ui
                    .ctx()
                    .animate_value_with_time(ui.scope_id().with("tab_right"), rect.right(), 0.15);
                let y = rect.bottom() - 2.0;
                ui.painter()
                    .line_segment([pos2(left, y), pos2(right, y)], Stroke::new(3.0, theme::ACCENT));
            }
            if response.has_focus() {
                ui.painter().rect_stroke(
                    rect.expand(2.0),
                    4,
                    Stroke::new(2.0, theme::TEXT_STRONG),
                    egui::StrokeKind::Outside,
                );
            }
            response.widget_info(|| egui::WidgetInfo::selected(egui::Role::Tab, true, selected, *label));
            if let Some((_, _, shortcut)) = crate::modals::palette::TABS.get(i) {
                response.clone().on_hover_text(*shortcut);
            }
            if response.clicked() {
                clicked = Some(i);
            }
        }
    });
    let y = ui.cursor().top();
    let full = ui.max_rect();
    ui.painter().line_segment(
        [pos2(full.left(), y), pos2(full.right(), y)],
        Stroke::new(1.0, theme::CARD_EDGE),
    );
    ui.add_space(12.0);
    clicked
}

/// One library row: cover art (or a lettered placeholder), the name, and a second line with the store and state.
pub fn game_row(
    ui: &mut egui::Ui,
    name: &str,
    detail: &str,
    installed: Option<&str>,
    capsule: Option<&Path>,
    selected: bool,
    hint: Option<&str>,
) -> egui::Response {
    let height = if hint.is_some() { 82.0 } else { 64.0 };
    let (rect, response) = ui.allocate_exact_size(vec2(ui.available_width(), height), Sense::click());
    let painter = ui.painter_at(rect);
    if selected {
        painter.rect_filled(rect, 8.0, theme::ACCENT.gamma_multiply(0.22));
        painter.rect_stroke(
            rect,
            8.0,
            Stroke::new(1.0, theme::ACCENT.gamma_multiply(0.6)),
            egui::StrokeKind::Inside,
        );
    } else if response.hovered() {
        painter.rect_filled(rect, 8.0, theme::CARD);
    }
    // Steam covers are 2:3.
    let cover = Rect::from_min_size(pos2(rect.left() + 8.0, rect.top() + 6.0), vec2(35.0, 52.0));
    match capsule {
        Some(path) => {
            egui::Image::new(file_uri(path))
                .corner_radius(CornerRadius::same(4))
                .paint_at(ui, cover);
        }
        None => {
            painter.rect_filled(cover, 4.0, theme::CARD_EDGE);
            let letter = name
                .chars()
                .find(|c| c.is_alphanumeric())
                .unwrap_or('?')
                .to_uppercase()
                .to_string();
            painter.text(
                cover.center(),
                egui::Align2::CENTER_CENTER,
                letter,
                FontId::proportional(18.0),
                theme::MUTED,
            );
        }
    }
    let left = cover.right() + 12.0;
    let width = (rect.right() - 8.0 - left).max(20.0);
    let name_color = if selected { theme::TEXT_STRONG } else { theme::TEXT };
    let mut job = egui::text::LayoutJob::simple_singleline(name.to_string(), FontId::proportional(15.0), name_color);
    job.wrap = egui::text::TextWrapping::truncate_at_width(width);
    let name_galley = ui.painter().layout_job(job);
    painter.galley(pos2(left, rect.top() + 13.0), name_galley, name_color);
    let mut x = left;
    let y = rect.top() + 37.0;
    if let Some(state) = installed {
        painter.circle_filled(pos2(x + 4.0, y + 7.0), 3.5, theme::GOOD);
        x += 13.0;
        let g = ui
            .painter()
            .layout_no_wrap(state.to_string(), FontId::proportional(12.5), theme::GOOD);
        let w = g.size().x;
        painter.galley(pos2(x, y), g, theme::GOOD);
        x += w + 8.0;
    }
    let mut job =
        egui::text::LayoutJob::simple_singleline(detail.to_string(), FontId::proportional(12.5), theme::MUTED);
    job.wrap = egui::text::TextWrapping::truncate_at_width((rect.right() - 8.0 - x).max(10.0));
    let g = ui.painter().layout_job(job);
    painter.galley(pos2(x, y), g, theme::MUTED);
    if response.has_focus() {
        painter.rect_stroke(
            rect.shrink(1.0),
            8,
            Stroke::new(2.0, theme::TEXT_STRONG),
            egui::StrokeKind::Inside,
        );
    }
    if let Some(hint) = hint {
        ui.put(
            Rect::from_min_size(pos2(left, rect.top() + 58.0), vec2(width, 18.0)),
            egui::Label::new(RichText::new(hint).size(12.0).color(theme::MUTED)).truncate(),
        );
    }
    // Painted by hand, so tell accessibility (and the UI tests) what this row is.
    response.widget_info(|| egui::WidgetInfo::selected(egui::Role::Button, true, selected, name));
    response.on_hover_text(
        hint.map(|hint| format!("{name}\n{hint}"))
            .unwrap_or_else(|| name.into()),
    )
}

/// A small heading between library groups.
pub fn group_label(ui: &mut egui::Ui, text: &str, count: usize) {
    ui.add_space(8.0);
    ui.label(
        RichText::new(format!("{}  {count}", text.to_uppercase()))
            .size(11.5)
            .color(theme::MUTED)
            .strong(),
    );
    ui.add_space(2.0);
}

/// Steam heroes are 3840x1240.
const HERO_ASPECT: f32 = 3840.0 / 1240.0;

/// The game's banner: its hero image cropped to a strip, fading into the panel, with the logo (or the name) on it.
/// Without a hero image the name is drawn as a plain heading.
pub fn banner(ui: &mut egui::Ui, name: &str, subtitle: &str, hero: Option<&Path>, logo: Option<&Path>) {
    let Some(hero) = hero else {
        ui.add_space(4.0);
        ui.label(RichText::new(name).size(28.0).strong().color(theme::TEXT_STRONG));
        ui.label(RichText::new(subtitle).color(theme::MUTED));
        ui.add_space(6.0);
        return;
    };
    let width = ui.available_width();
    let natural = width / HERO_ASPECT;
    let height = natural.clamp(150.0, 230.0);
    let (rect, _) = ui.allocate_exact_size(vec2(width, height), Sense::hover());
    // Show the middle band of the image at its own aspect: crop top and bottom, never stretch.
    let shown = (height / natural).min(1.0);
    let uv = Rect::from_min_max(pos2(0.0, (1.0 - shown) / 2.0), pos2(1.0, (1.0 + shown) / 2.0));
    egui::Image::new(file_uri(hero))
        .uv(uv)
        .corner_radius(CornerRadius::same(12))
        .paint_at(ui, rect);
    // Darken the lower part so the logo and text read on any picture.
    let fade_top = rect.top() + rect.height() * 0.35;
    let mut mesh = egui::Mesh::default();
    let clear = Color32::from_black_alpha(0);
    let dark = Color32::from_black_alpha(200);
    mesh.colored_vertex(pos2(rect.left(), fade_top), clear);
    mesh.colored_vertex(pos2(rect.right(), fade_top), clear);
    mesh.colored_vertex(pos2(rect.right(), rect.bottom()), dark);
    mesh.colored_vertex(pos2(rect.left(), rect.bottom()), dark);
    mesh.add_triangle(0, 1, 2);
    mesh.add_triangle(0, 2, 3);
    ui.painter().add(mesh);
    let pad = 20.0;
    let sub = ui.painter().layout_no_wrap(
        subtitle.to_string(),
        FontId::proportional(13.0),
        Color32::from_gray(220),
    );
    let sub_pos = pos2(rect.left() + pad, rect.bottom() - pad - sub.size().y);
    match logo {
        Some(logo) => {
            let max = Vec2::new((width * 0.45).min(380.0), (height * 0.45).min(100.0));
            let area = Rect::from_min_size(pos2(rect.left() + pad, sub_pos.y - 8.0 - max.y), max);
            // Bottom-left of the band, at the logo's own aspect.
            // A child Ui does not move the parent's cursor, so the content below the banner stays below it.
            let mut child = ui.new_child(
                egui::UiBuilder::new()
                    .max_rect(area)
                    .layout(egui::Layout::left_to_right(egui::Align::Max)),
            );
            child.add(
                egui::Image::new(file_uri(logo))
                    .max_size(max)
                    .maintain_aspect_ratio(true),
            );
        }
        None => {
            let title = ui
                .painter()
                .layout_no_wrap(name.to_string(), FontId::proportional(30.0), Color32::WHITE);
            let pos = pos2(rect.left() + pad, sub_pos.y - 6.0 - title.size().y);
            ui.painter().galley(pos, title, Color32::WHITE);
        }
    }
    ui.painter().galley(sub_pos, sub, Color32::from_gray(220));
    ui.add_space(10.0);
}

/// Compact, fixed-width facts wrap as a group at narrow widths; the value never competes with its caption.
pub fn stat_tile(ui: &mut egui::Ui, label: &str, value: &str, color: Color32) {
    let width = 160.0_f32.min(ui.max_rect().width());
    let (rect, response) = ui.allocate_exact_size(vec2(width, 90.0), Sense::hover());
    ui.painter().rect_filled(rect, 8.0, theme::CARD);
    ui.painter()
        .rect_stroke(rect, 8.0, Stroke::new(1.0, theme::CARD_EDGE), egui::StrokeKind::Inside);
    let caption = ui
        .painter()
        .layout(label.to_owned(), FontId::proportional(12.0), theme::MUTED, width - 24.0);
    ui.painter().galley(rect.min + vec2(12.0, 10.0), caption, theme::MUTED);
    let mut job = egui::text::LayoutJob::simple_singleline(value.to_owned(), FontId::proportional(21.0), color);
    job.wrap = egui::text::TextWrapping::truncate_at_width(width - 24.0);
    let galley = ui.painter().layout_job(job);
    ui.painter().galley(rect.min + vec2(12.0, 46.0), galley, color);
    response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Label, true, format!("{label}: {value}")));
}

/// One cover is one focusable button; wrapping moves the whole card, including its title and state.
pub fn cover_card(
    ui: &mut egui::Ui,
    name: &str,
    capsule: Option<&Path>,
    state: &str,
    color: Color32,
    hint: Option<&str>,
) -> egui::Response {
    let width = 150.0_f32.min(ui.max_rect().width());
    let (rect, response) = ui.allocate_exact_size(
        vec2(width, width * 1.5 + if hint.is_some() { 86.0 } else { 66.0 }),
        Sense::click(),
    );
    let cover = Rect::from_min_size(rect.min, vec2(width, width * 1.5));
    let hover = ui
        .ctx()
        .animate_bool_with_time(response.id.with("hover"), response.hovered(), 0.15);
    ui.painter().rect_filled(cover, 12, theme::CARD);
    if let Some(path) = capsule {
        egui::Image::new(file_uri(path)).corner_radius(12).paint_at(ui, cover);
    } else {
        let title = ui.painter().layout(
            name.to_owned(),
            FontId::proportional(20.0),
            theme::TEXT_STRONG,
            width - 24.0,
        );
        ui.painter()
            .galley(cover.min + vec2(12.0, 24.0), title, theme::TEXT_STRONG);
    }
    if hover > 0.0 {
        ui.painter().rect_stroke(
            cover,
            12,
            Stroke::new(1.5, theme::ACCENT.gamma_multiply(hover)),
            egui::StrokeKind::Inside,
        );
    }
    if response.has_focus() {
        ui.painter().rect_stroke(
            rect.expand(3.0),
            12,
            Stroke::new(2.0, theme::TEXT_STRONG),
            egui::StrokeKind::Outside,
        );
    }
    let mut title = egui::text::LayoutJob::simple_singleline(name.into(), FontId::proportional(14.5), theme::TEXT);
    title.wrap = egui::text::TextWrapping::truncate_at_width(width);
    let title = ui.painter().layout_job(title);
    ui.painter()
        .galley(pos2(rect.left(), cover.bottom() + 8.0), title, theme::TEXT);
    ui.painter()
        .circle_filled(pos2(rect.left() + 4.0, cover.bottom() + 39.0), 3.5, color);
    let mut status = egui::text::LayoutJob::simple_singleline(state.into(), FontId::proportional(12.5), color);
    status.wrap = egui::text::TextWrapping::truncate_at_width(width - 14.0);
    let status = ui.painter().layout_job(status);
    ui.painter()
        .galley(pos2(rect.left() + 14.0, cover.bottom() + 30.0), status, color);
    if let Some(hint) = hint {
        ui.put(
            Rect::from_min_size(pos2(rect.left(), cover.bottom() + 52.), vec2(width, 18.)),
            egui::Label::new(RichText::new(hint).size(12.).color(theme::MUTED)).truncate(),
        );
    }
    response.widget_info(|| egui::WidgetInfo::labeled(egui::Role::Button, true, name));
    response.on_hover_text(format!(
        "{name}\n{state}{}",
        hint.map(|h| format!("\n{h}")).unwrap_or_default()
    ))
}

#[cfg(test)]
mod uri_tests {
    use super::*;

    #[cfg(windows)]
    #[test]
    fn windows_drive_and_unc_paths_become_loader_uris() {
        assert_eq!(
            file_uri(Path::new(r"C:\Program Files (x86)\Steam\a.jpg")),
            "file:///C:/Program Files (x86)/Steam/a.jpg"
        );
        assert_eq!(file_uri(Path::new(r"\\nas\games\a.jpg")), "file://nas/games/a.jpg");
    }

    #[cfg(not(windows))]
    #[test]
    fn unix_paths_keep_their_slashes() {
        assert_eq!(file_uri(Path::new("/home/u/a b.jpg")), "file:///home/u/a b.jpg"); // privacy: allow (synthetic test path)
    }
}
