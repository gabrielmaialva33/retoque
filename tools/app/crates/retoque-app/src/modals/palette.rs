//! Keyboard-first command dialog. The catalog and matching results are generation/query cached.
use crate::{
    app::Tab,
    commands::PaletteAction,
    fuzzy::Search,
    i18n::{Lang, t},
};

pub(crate) const TABS: [(Tab, &str, &str); 5] = [
    (Tab::Install, "tab.install", "Ctrl+1"),
    (Tab::Settings, "tab.settings", "Ctrl+2"),
    (Tab::Model, "tab.model", "Ctrl+3"),
    (Tab::Compare, "tab.compare", "Ctrl+4"),
    (Tab::Logs, "tab.logs", "Ctrl+5"),
];
#[derive(Clone)]
pub(crate) enum Destination {
    Game(String),
    Tab(Tab),
    Setting(&'static str),
    Action(PaletteAction),
    FocusSearch,
}
pub(crate) struct Entry {
    pub label: String,
    pub destination: Destination,
}
#[derive(Default)]
pub(crate) struct Palette {
    pub open: bool,
    pub query: String,
    pub entries: Vec<Entry>,
    pub results: Vec<usize>,
    pub catalog_key: Option<(u64, Lang)>,
    query_key: Option<String>,
    selected: usize,
    focus: bool,
    origin: Option<egui::Id>,
    restore_frames: u8,
}
impl Palette {
    pub fn open(&mut self, ctx: &egui::Context) {
        self.open = true;
        self.query.clear();
        self.query_key = None;
        self.selected = 0;
        self.focus = true;
        self.origin = ctx.memory(|m| m.focused());
    }
    pub fn restore_focus(&mut self, ctx: &egui::Context) {
        if self.restore_frames > 0 {
            if let Some(id) = self.origin {
                ctx.memory_mut(|m| m.request_focus(id));
            }
            self.restore_frames -= 1;
            ctx.request_repaint();
        }
    }
    pub fn invalidate(&mut self) {
        self.query_key = None;
    }
    pub fn refresh(&mut self) {
        if self.query_key.as_ref() == Some(&self.query) {
            return;
        }
        self.query_key = Some(self.query.clone());
        let mut search = Search::new(&self.query);
        let mut ranked: Vec<_> = self
            .entries
            .iter()
            .enumerate()
            .filter_map(|(i, e)| search.score(&e.label).map(|score| (i, score)))
            .collect();
        ranked.sort_by(|(a, sa), (b, sb)| {
            sb.cmp(sa)
                .then_with(|| self.entries[*a].label.cmp(&self.entries[*b].label))
        });
        self.results = ranked.into_iter().map(|(i, _)| i).collect();
        self.selected = 0;
    }
    pub fn show(&mut self, ctx: &egui::Context, lang: Lang) -> Option<Destination> {
        if !self.open {
            return None;
        }
        let mut chosen = None;
        let mut close = false;
        let modal = egui::Modal::new(egui::Id::unique("command_palette")).show(ctx, |ui| {
            ui.set_width((ctx.content_rect().width() - 64.).clamp(260., 660.));
            ui.response()
                .widget_info(|| egui::WidgetInfo::labeled(egui::Role::Dialog, true, t(lang, "palette.title")));
            ui.heading(t(lang, "palette.title"));
            let (down, up, enter) = ui.input_mut(|i| {
                (
                    i.consume_key(egui::Modifiers::NONE, egui::Key::ArrowDown),
                    i.consume_key(egui::Modifiers::NONE, egui::Key::ArrowUp),
                    i.consume_key(egui::Modifiers::NONE, egui::Key::Enter),
                )
            });
            let label = ui.label(t(lang, "palette.search"));
            let input = ui
                .add(
                    egui::TextEdit::singleline(&mut self.query)
                        .id(egui::Id::unique("palette_query"))
                        .desired_width(f32::INFINITY),
                )
                .labelled_by(label.id);
            if self.focus {
                input.request_focus();
                self.focus = false;
            }
            self.refresh();
            if down {
                self.selected = (self.selected + 1).min(self.results.len().saturating_sub(1));
            }
            if up {
                self.selected = self.selected.saturating_sub(1);
            }
            if enter {
                chosen = self
                    .results
                    .get(self.selected)
                    .map(|&i| self.entries[i].destination.clone());
            }
            ui.label(t(lang, "palette.keys"));
            if self.results.is_empty() {
                ui.label(t(lang, "palette.empty"));
            }
            egui::ScrollArea::vertical().max_height(320.).show(ui, |ui| {
                for (row, &index) in self.results.iter().enumerate() {
                    let response = ui.selectable_label(self.selected == row, &self.entries[index].label);
                    if response.clicked() {
                        chosen = Some(self.entries[index].destination.clone());
                    }
                    if self.selected == row && (up || down) {
                        response.scroll_to_me(None);
                    }
                }
            });
            if ui.button(t(lang, "cancel")).clicked() {
                close = true;
            }
        });
        if modal.should_close() || close || chosen.is_some() {
            self.open = false;
            self.restore_frames = if matches!(&chosen, Some(Destination::Setting(_) | Destination::FocusSearch)) {
                0
            } else {
                2
            };
            ctx.request_repaint();
        }
        chosen
    }
}
