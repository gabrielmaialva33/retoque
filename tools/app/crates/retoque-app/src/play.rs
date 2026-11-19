//! Launcher requests, favorites and read-only shortcuts. Inspired by the Hub comparison in the task brief;
//! independently implemented from launcher documentation/source, with no NeuRotic code or data.
use crate::app::*;

/// Pure: only registered entries with a verified protocol and usable identifier get Play.
/// `windows` makes the host constraint testable without starting any launcher.
pub(crate) fn launch_uri(game: &Game, windows: bool) -> Option<String> {
    if !game.registered {
        return None;
    }
    match game.store {
        // Valve's protocol reference: https://developer.valvesoftware.com/wiki/Steam_browser_protocol
        // Its generated shortcuts also use this URI (Valve's client tracker):
        // https://github.com/ValveSoftware/steam-for-linux/issues/12387
        Store::Steam => {
            let id = game.id.strip_prefix("steam:")?;
            let app_id = id.parse::<u32>().ok().filter(|id| *id > 0)?;
            if !id.bytes().all(|b| b.is_ascii_digit()) {
                return None;
            }
            if let Runtime::Proton { app_id: runtime_id, .. } = game.runtime
                && runtime_id != app_id
            {
                return None;
            }
            Some(format!("steam://rungameid/{app_id}"))
        }
        // Heroic generates this exact query form for its desktop/Windows shortcuts:
        // https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher/blob/main/src/backend/shortcuts/shortcuts/shortcuts.ts
        Store::Heroic => {
            let (runner, id) = game.id.strip_prefix("heroic:")?.split_once(':')?;
            if !matches!(runner, "legendary" | "gog" | "nile") || id.is_empty() {
                return None;
            }
            Some(format!("heroic://launch?appName={}&runner={runner}", component(id)))
        }
        // Lutris rungame explicitly looks up the slug, whereas rungameid looks up its database row ID:
        // https://github.com/lutris/lutris/blob/master/lutris/gui/application.py
        Store::Lutris if !windows => {
            let slug = game.id.strip_prefix("lutris:")?;
            // Discovery uses the numeric DB id when the slug is absent. It cannot distinguish that fallback
            // from a genuinely numeric slug: omit the action rather than launch another installed entry.
            if slug.is_empty()
                || slug.bytes().all(|b| b.is_ascii_digit())
                || !slug
                    .bytes()
                    .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
            {
                return None;
            }
            Some(format!("lutris:rungame/{slug}"))
        }
        // Epic documents the artifact-only (AppName) form as supported for backwards compatibility:
        // https://dev.epicgames.com/docs/epic-games-store/protocol-activation
        // https://assets-unreal2-epic-prod-us2.s3.dualstack.us-east-1.amazonaws.com/original/4X/f/c/4/fc4342c50e1a0abf34943e019ee90bacccc62950.pdf
        Store::Epic if windows && game.runtime == Runtime::Windows => {
            let id = game.id.strip_prefix("epic:").filter(|id| !id.is_empty())?;
            Some(format!(
                "com.epicgames.launcher://apps/{}?action=launch&silent=true",
                component(id)
            ))
        }
        // No Galaxy launch URI verified in GOG's own documentation. Xbox discovery holds a directory,
        // not the Store/Title ID required by Microsoft's deep-linking protocol. Manual folders have no launcher.
        _ => None,
    }
}

fn component(value: &str) -> String {
    let mut out = String::new();
    for b in value.bytes() {
        if b.is_ascii_alphanumeric() || b"-._~".contains(&b) {
            out.push(b as char);
        } else {
            use std::fmt::Write;
            write!(out, "%{b:02X}").unwrap();
        }
    }
    out
}

impl RetoqueApp {
    pub(crate) fn game_write_in_flight(&self, game: &Game) -> bool {
        self.writing.keys().any(|p| {
            p.starts_with(&game.install_dir)
                || game.install_dir.starts_with(p)
                || game
                    .targets
                    .iter()
                    .any(|t| p.starts_with(&t.path) || t.path.starts_with(p))
        })
    }

    pub(crate) fn play_game(&mut self, game: &Game) {
        self.play_game_with(game, |uri| model::open_system(std::ffi::OsStr::new(uri)));
    }

    pub(crate) fn play_game_with(&mut self, game: &Game, open: impl FnOnce(&str) -> std::io::Result<()>) {
        let Some(uri) = launch_uri(game, cfg!(windows)) else {
            return;
        };
        if self.game_write_in_flight(game) {
            self.toast(self.tr("play.wait_write").into(), None, false);
            return;
        }
        match open(&uri) {
            Ok(()) => self.toast(
                self.tr("play.asked")
                    .replace("{launcher}", store_name(self.lang, game.store))
                    .replace("{game}", &game.name),
                None,
                false,
            ),
            Err(error) => self.toast(self.tr("play.failed").into(), Some(error.into()), true),
        }
    }

    pub(crate) fn play_button(&mut self, ui: &mut egui::Ui, game: &Game) {
        if launch_uri(game, cfg!(windows)).is_none() {
            return;
        }
        let writing = self.game_write_in_flight(game);
        if ui
            .add_enabled(!writing, egui::Button::new(self.tr("play.action")))
            .clicked()
        {
            self.play_game(game);
        }
        if writing {
            ui.label(RichText::new(self.tr("play.wait_write")).small().color(theme::MUTED));
        }
    }

    pub(crate) fn toggle_favorite(&mut self, id: &str) {
        if !self.saved.favorites.remove(id) {
            self.saved.favorites.insert(id.into());
        }
    }

    /// Preserve filtering and each existing group's search order; favorites precede both install groups.
    pub(crate) fn library_groups(&self) -> [(&'static str, Vec<usize>); 3] {
        let favorite = |i: &usize| self.saved.favorites.contains(&self.library_state.games[*i].id);
        let d = &self.library_state.derived;
        [
            (
                "favorites.title",
                d.installed.iter().chain(&d.others).copied().filter(favorite).collect(),
            ),
            (
                "lib.group_installed",
                d.installed.iter().copied().filter(|i| !favorite(i)).collect(),
            ),
            (
                "lib.group_other",
                d.others.iter().copied().filter(|i| !favorite(i)).collect(),
            ),
        ]
    }

    pub(crate) fn quick_actions(&self, ui: &mut egui::Ui) {
        let Some(target) = self.view.target.as_ref() else {
            return;
        };
        for (key, path, exists) in [
            (
                "play.open_settings",
                target.join("OptiScaler.ini"),
                target.join("OptiScaler.ini").is_file(),
            ),
            (
                "play.open_captures",
                target.join("retoque-captures"),
                target.join("retoque-captures").is_dir(),
            ),
        ] {
            if exists && ui.button(self.tr(key)).clicked() {
                model::open_folder(&path);
            }
        }
    }
}

/// A separate keyboard-focusable star, with a spoken verb and state; its outline/fill survives monochrome.
pub(crate) fn favorite_button(ui: &mut egui::Ui, game: &Game, selected: bool, lang: Lang) -> egui::Response {
    let label = t(lang, if selected { "favorites.remove" } else { "favorites.add" }).replace("{game}", &game.name);
    let response = ui
        .push_id((&game.id, "favorite"), |ui| {
            let (rect, response) = ui.allocate_exact_size(egui::vec2(32., 32.), egui::Sense::click());
            let points: Vec<_> = (0..10)
                .map(|i| {
                    let angle = std::f32::consts::PI * (i as f32 / 5. - 0.5);
                    rect.center() + egui::vec2(angle.cos(), angle.sin()) * if i % 2 == 0 { 11. } else { 5. }
                })
                .collect();
            // One mesh avoids antialiased seams between the convex wedges of the star.
            if selected {
                let mut mesh = egui::Mesh::default();
                mesh.colored_vertex(rect.center(), theme::TEXT_STRONG);
                for point in &points {
                    mesh.colored_vertex(*point, theme::TEXT_STRONG);
                }
                for i in 0..points.len() as u32 {
                    mesh.add_triangle(0, i + 1, (i + 1) % points.len() as u32 + 1);
                }
                ui.painter().add(egui::Shape::mesh(mesh));
            }
            ui.painter().add(egui::Shape::closed_line(
                points,
                egui::Stroke::new(1.5, theme::TEXT_STRONG),
            ));
            if response.has_focus() || response.hovered() {
                ui.painter().rect_stroke(
                    rect.shrink(1.),
                    4,
                    egui::Stroke::new(2., theme::TEXT_STRONG),
                    egui::StrokeKind::Inside,
                );
            }
            response.widget_info(|| egui::WidgetInfo::selected(egui::Role::Button, ui.is_enabled(), selected, &label));
            response
        })
        .inner;
    response.on_hover_text(label)
}
