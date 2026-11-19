use crate::{
    app::RetoqueApp,
    i18n::{Lang, t},
    language_editor::{self as editor, Document, Pack, Target},
    theme, widgets,
};
use retoque_core::menu_pack::{self, Result};
use std::{collections::HashMap, path::PathBuf, sync::mpsc};

enum FileResult {
    Import(Vec<u8>),
    Export,
    Cancel,
}
pub(crate) struct State {
    pub open: bool,
    pub(crate) pack: Pack,
    pub(crate) document: Document,
    root: Option<PathBuf>,
    menus: HashMap<String, PathBuf>,
    search: String,
    untranslated: bool,
    changed: bool,
    pub(crate) selected: String,
    error: Option<&'static str>,
    notice: Option<&'static str>,
    unsaved: bool,
    reset: Option<Option<String>>,
    picker: Option<mpsc::Receiver<Result<FileResult>>>,
    focus: widgets::FocusRestore,
}
impl Default for State {
    fn default() -> Self {
        let pack = Pack::new(Target::App, Lang::De);
        Self {
            selected: pack.keys.first().cloned().unwrap_or_default(),
            pack,
            document: Document::default(),
            root: None,
            menus: HashMap::new(),
            open: false,
            search: String::new(),
            untranslated: false,
            changed: false,
            error: None,
            notice: None,
            unsaved: false,
            reset: None,
            picker: None,
            focus: Default::default(),
        }
    }
}
impl State {
    pub fn load() -> Self {
        let mut state = Self {
            root: retoque_data::lsfg_prepare::data_dir().ok().map(|p| p.join("languages")),
            ..Default::default()
        };
        if let Some(root) = &state.root {
            state.error = editor::load_active(root).err();
            for lang in Lang::ALL.into_iter().filter(|l| *l != Lang::En) {
                let pack = Pack::new(Target::Menu, lang);
                match Document::load(root, &pack) {
                    Ok(doc) if !doc.applied.is_empty() => {
                        // Staging is a generated app file. Recreate from validated corrections, never from imported paths.
                        let result = pack
                            .export(&doc.applied)
                            .and_then(|s| editor::write(&editor::menu_path(root, lang, &s), s.as_bytes()));
                        if let Err(e) = result {
                            state.error = Some(e);
                        } else {
                            state.menus.insert(
                                lang.code().to_owned(),
                                editor::menu_path(root, lang, &pack.export(&doc.applied).unwrap()),
                            );
                        }
                    }
                    Err(e) => state.error = Some(e),
                    _ => {}
                }
            }
            match Document::load(root, &state.pack) {
                Ok(doc) => state.document = doc,
                Err(e) => state.error = Some(e),
            }
        } else {
            state.error = Some("io");
        }
        state
    }
    pub fn menu_language(&self, choice: retoque_core::MenuLanguage) -> retoque_core::MenuLanguage {
        if let retoque_core::MenuLanguage::Pack(code) = &choice
            && let Some(path) = self.menus.get(code)
        {
            retoque_core::MenuLanguage::UserPack(path.clone())
        } else {
            choice
        }
    }
    fn save(&mut self) -> bool {
        let result = self
            .root
            .as_ref()
            .ok_or("io")
            .and_then(|root| self.document.save(root, &self.pack));
        match result {
            Ok(()) => {
                self.unsaved = false;
                self.error = None;
                self.notice = Some("languages.saved");
                true
            }
            Err(e) => {
                self.error = Some(e);
                false
            }
        }
    }
    fn select(&mut self, target: Target, lang: Lang) {
        if self.unsaved && !self.save() {
            return;
        }
        let pack = Pack::new(target, lang);
        let doc = self
            .root
            .as_ref()
            .map_or(Ok(Document::default()), |root| Document::load(root, &pack));
        match doc {
            Ok(doc) => {
                self.selected = pack.keys.first().cloned().unwrap_or_default();
                self.pack = pack;
                self.document = doc;
                self.error = None;
                self.notice = None;
            }
            Err(e) => self.error = Some(e),
        }
    }
    fn apply(&mut self) -> bool {
        let result = self
            .root
            .as_ref()
            .ok_or("io")
            .and_then(|root| self.document.apply(root, &self.pack));
        match result {
            Ok(()) => {
                if self.pack.target == Target::Menu {
                    let code = self.pack.lang.code().to_owned();
                    if self.document.applied.is_empty() {
                        self.menus.remove(&code);
                    } else {
                        self.menus.insert(
                            code,
                            editor::menu_path(
                                self.root.as_ref().unwrap(),
                                self.pack.lang,
                                &self.pack.export(&self.document.applied).unwrap(),
                            ),
                        );
                    }
                }
                self.unsaved = false;
                self.error = None;
                self.notice = Some(if self.pack.target == Target::App {
                    "languages.applied"
                } else {
                    "languages.menu_applied"
                });
                true
            }
            Err(e) => {
                self.error = Some(e);
                false
            }
        }
    }
    fn file(&mut self, import: bool, lang: Lang) {
        let export = if import {
            None
        } else {
            match self.pack.export(&self.document.changed(&self.pack)) {
                Ok(s) => Some(s),
                Err(e) => {
                    self.error = Some(e);
                    return;
                }
            }
        };
        let extension = self.pack.target.extension();
        let name = format!("{}.{}", self.pack.lang.code(), extension);
        let title = t(lang, if import { "languages.import" } else { "languages.export" }).to_owned();
        let (tx, rx) = mpsc::channel();
        self.picker = Some(rx);
        std::thread::spawn(move || {
            let dialog = rfd::FileDialog::new()
                .set_title(title)
                .set_file_name(name)
                .add_filter(extension, &[extension]);
            let result = if let Some(export) = export {
                dialog.save_file().map_or(Ok(FileResult::Cancel), |path| {
                    editor::write(&path, export.as_bytes()).map(|_| FileResult::Export)
                })
            } else {
                dialog.pick_file().map_or(Ok(FileResult::Cancel), |path| {
                    menu_pack::read(&path).map(FileResult::Import)
                })
            };
            let _ = tx.send(result);
        });
    }
    fn poll(&mut self) {
        let Some(rx) = &self.picker else {
            return;
        };
        match rx.try_recv() {
            Ok(result) => {
                self.picker = None;
                match result {
                    Ok(FileResult::Import(bytes)) => match self.pack.import(&bytes) {
                        Ok(entries) => {
                            // Sparse merge preserves unrelated local corrections; omitted keys are unchanged.
                            for (key, value) in entries {
                                if value == self.pack.base(&key) {
                                    self.document.drafts.remove(&key);
                                } else {
                                    self.document.drafts.insert(key, value);
                                }
                            }
                            self.unsaved = true;
                            if self.save() {
                                self.notice = Some("languages.imported");
                            }
                        }
                        Err(e) => self.error = Some(e),
                    },
                    Ok(FileResult::Export) => {
                        self.error = None;
                        self.notice = Some("languages.exported");
                    }
                    Ok(FileResult::Cancel) => {}
                    Err(e) => self.error = Some(e),
                }
            }
            Err(mpsc::TryRecvError::Disconnected) => {
                self.picker = None;
                self.error = Some("io");
            }
            Err(mpsc::TryRecvError::Empty) => {}
        }
    }
    fn body(&mut self, ui: &mut egui::Ui, lang: Lang) {
        let mut edited = false;
        let (mut target, mut code) = (self.pack.target, self.pack.lang);
        ui.horizontal(|ui| {
            for (value, label) in [(Target::App, "languages.app"), (Target::Menu, "languages.menu")] {
                ui.selectable_value(&mut target, value, t(lang, label));
            }
            if target == Target::Menu && code == Lang::En || target == Target::App && !code.is_draft() {
                code = Lang::De;
            }
            let label = ui.label(t(lang, "prefs.language"));
            egui::ComboBox::from_id_salt("correction_language")
                .selected_text(code.native_name())
                .show_ui(ui, |ui| {
                    for choice in Lang::ALL.into_iter().filter(|l| {
                        if target == Target::App {
                            l.is_draft()
                        } else {
                            *l != Lang::En
                        }
                    }) {
                        if crate::fonts::can_show(choice) {
                            ui.selectable_value(&mut code, choice, choice.native_name());
                        } else {
                            let name = crate::prefs_ui::language_name(lang, choice);
                            ui.add_enabled(false, egui::Button::new(name))
                                .on_disabled_hover_text(t(lang, "prefs.language_no_font").replace("{languages}", name));
                        }
                    }
                })
                .response
                .labelled_by(label.id);
            let response = ui.add(
                egui::TextEdit::singleline(&mut self.search)
                    .hint_text(t(lang, "languages.search"))
                    .desired_width(ui.available_width().max(140.0)),
            );
            response
                .widget_info(|| egui::WidgetInfo::labeled(egui::Role::TextInput, true, t(lang, "languages.search")));
        });
        if (target, code) != (self.pack.target, self.pack.lang) {
            self.select(target, code);
        }
        ui.horizontal_wrapped(|ui| {
            ui.checkbox(&mut self.untranslated, t(lang, "languages.untranslated"));
            ui.checkbox(&mut self.changed, t(lang, "languages.changed"));
        });
        let query = self.search.to_lowercase();
        let keys: Vec<_> = self
            .pack
            .keys
            .iter()
            .filter(|key| {
                let draft = self.document.draft(&self.pack, key);
                (!self.untranslated || draft == self.pack.english.get(*key).map_or("", String::as_str))
                    && (!self.changed || draft != self.pack.base(key))
                    && (query.is_empty()
                        || key.to_lowercase().contains(&query)
                        || self
                            .pack
                            .english
                            .get(*key)
                            .is_some_and(|s| s.to_lowercase().contains(&query))
                        || draft.to_lowercase().contains(&query))
            })
            .cloned()
            .collect();
        ui.label(
            t(lang, "languages.count")
                .replace("{n}", &keys.len().to_string())
                .replace("{changed}", &self.document.changed(&self.pack).len().to_string()),
        );
        let height = (ui.ctx().content_rect().height() - 310.0).clamp(130.0, 400.0);
        ui.columns(2, |columns| {
            egui::ScrollArea::vertical()
                .id_salt("language_keys")
                .max_height(height)
                .min_scrolled_height(height)
                .auto_shrink([false, false])
                .show_rows(&mut columns[0], 24.0, keys.len(), |ui, range| {
                    for key in &keys[range] {
                        let text = egui::RichText::new(key).small();
                        let response = ui.add(egui::Button::selectable(self.selected == *key, text).truncate());
                        if response.clicked() {
                            self.selected = key.clone();
                        }
                        response.on_hover_text(key);
                    }
                });
            egui::ScrollArea::vertical()
                .id_salt("language_entry")
                .max_height(height)
                .min_scrolled_height(height)
                .auto_shrink([false, false])
                .show(&mut columns[1], |ui| {
                    if keys.is_empty() {
                        self.selected.clear();
                        ui.label(t(lang, "languages.no_rows"));
                        return;
                    }
                    if !keys.contains(&self.selected) {
                        self.selected = keys[0].clone();
                    }
                    let key = &self.selected;
                    ui.columns(2, |columns| {
                        columns[0].strong(t(lang, "languages.source"));
                        columns[0].label(self.pack.english.get(key).map_or("", String::as_str));
                        columns[1].strong(t(lang, "languages.current"));
                        columns[1].label(
                            self.document
                                .applied
                                .get(key)
                                .map_or_else(|| self.pack.base(key), String::as_str),
                        );
                    });
                    let label = ui.strong(t(lang, "languages.draft"));
                    let mut value = self.document.draft(&self.pack, key).to_owned();
                    let response = ui
                        .add(
                            egui::TextEdit::multiline(&mut value)
                                .desired_rows(1)
                                .desired_width(f32::INFINITY)
                                .char_limit(menu_pack::MAX_LINE),
                        )
                        .labelled_by(label.id);
                    if response.changed() {
                        self.document.drafts.insert(key.clone(), value.clone());
                        self.unsaved = true;
                        edited = true;
                    }
                    match self.pack.validate(key, &value) {
                        Ok(()) => {}
                        Err(reason) => {
                            ui.colored_label(
                                theme::palette(ui).BAD,
                                t(lang, "languages.invalid").replace("{reason}", error_text(lang, reason)),
                            );
                        }
                    }
                    ui.strong(t(lang, "languages.preview"));
                    ui.label(editor::preview(&value, self.pack.target));
                });
        });
        if edited {
            self.save();
        }
    }
    fn actions(&mut self, ui: &mut egui::Ui, lang: Lang) -> bool {
        let mut applied_menu = false;
        let valid = self
            .document
            .drafts
            .iter()
            .all(|(key, value)| self.pack.validate(key, value).is_ok());
        if !valid {
            ui.label(t(lang, "languages.invalid_pack"));
        }
        ui.horizontal_wrapped(|ui| {
            if ui
                .add_enabled(valid, theme::primary(ui, t(lang, "languages.apply")))
                .clicked()
                && self.apply()
            {
                applied_menu = self.pack.target == Target::Menu;
            }
            if ui.button(t(lang, "languages.import")).clicked() {
                self.file(true, lang);
            }
            if ui
                .add_enabled(valid, egui::Button::new(t(lang, "languages.export")))
                .clicked()
            {
                self.file(false, lang);
            }
            if ui
                .add_enabled(!self.selected.is_empty(), egui::Button::new(t(lang, "languages.reset")))
                .clicked()
            {
                self.reset = Some(Some(self.selected.clone()));
            }
            if ui.button(t(lang, "languages.reset_all")).clicked() {
                self.reset = Some(None);
            }
        });
        applied_menu
    }
}
fn error_text(lang: Lang, reason: &str) -> &str {
    // Literal match keeps the app's i18n coverage scan aware of every error.
    match reason {
        "size" => t(lang, "languages.error.size"),
        "encoding" => t(lang, "languages.error.encoding"),
        "unknown" => t(lang, "languages.error.unknown"),
        "duplicate" => t(lang, "languages.error.duplicate"),
        "columns" => t(lang, "languages.error.columns"),
        "empty" => t(lang, "languages.error.empty"),
        "control" => t(lang, "languages.error.control"),
        "padding" => t(lang, "languages.error.padding"),
        "placeholder" => t(lang, "languages.error.placeholder"),
        "ellipsis" => t(lang, "languages.error.ellipsis"),
        "printf" => t(lang, "languages.error.printf"),
        "identity" => t(lang, "languages.error.identity"),
        "escape" => t(lang, "languages.error.escape"),
        "memory" => t(lang, "languages.error.memory"),
        _ => t(lang, "languages.error.io"),
    }
}
impl RetoqueApp {
    pub(crate) fn languages_dialog(&mut self, ctx: &egui::Context) {
        let state = &mut self.prefs.languages;
        state.focus.sync(ctx, state.open);
        if !state.open {
            return;
        }
        state.poll();
        let lang = self.lang;
        let mut close = false;
        let mut applied_menu = false;
        let modal = egui::Modal::new(egui::Id::unique("language_editor"))
            .frame(widgets::modal_frame(ctx))
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 72.0).clamp(320.0, 920.0));
                ui.heading(t(lang, "languages.title"));
                egui::ScrollArea::vertical()
                    .id_salt("language_editor_body")
                    .max_height((ctx.content_rect().height() - 190.0).max(200.0))
                    .show(ui, |ui| {
                        widgets::collapsible(
                            ui,
                            "language_help",
                            t(lang, "languages.help"),
                            widgets::Fold::Details,
                            |ui| {
                                ui.label(t(lang, "languages.scope"));
                                if state.pack.target == Target::Menu {
                                    ui.label(t(lang, "languages.menu_scope"));
                                }
                            },
                        );
                        ui.add_enabled_ui(state.picker.is_none(), |ui| {
                            state.body(ui, lang);
                        });
                        if let Some(error) = state.error {
                            ui.colored_label(theme::palette(ui).BAD, error_text(lang, error));
                        } else if let Some(notice) = state.notice {
                            ui.label(t(lang, notice));
                        }
                        if state.unsaved {
                            ui.label(t(lang, "languages.pending"));
                        }
                        if state.unsaved && ui.button(t(lang, "languages.retry")).clicked() {
                            state.save();
                        }
                        if state.picker.is_some() {
                            ui.label(t(lang, "languages.working"));
                            ctx.request_repaint_after(std::time::Duration::from_millis(100));
                        }
                    });
                ui.add_enabled_ui(state.picker.is_none(), |ui| {
                    applied_menu = state.actions(ui, lang);
                });
                close = ui.button(t(lang, "close")).clicked();
            });
        if applied_menu {
            self.saved.install_defaults.menu_language =
                crate::prefs_ui::MenuLanguagePref::Pack(state.pack.lang.code().to_owned());
        }
        if let Some(key) = state.reset.clone() {
            let full_item = key.as_deref().unwrap_or(state.pack.lang.native_name());
            let mut item: String = full_item.chars().take(120).collect();
            if item.len() < full_item.len() {
                item.push('…');
            }
            match widgets::delete_modal(
                ctx,
                lang,
                "language_reset",
                t(lang, "languages.reset_title"),
                &item,
                t(lang, "languages.reset_help"),
            ) {
                Some(widgets::Answer::Confirm) => {
                    // Reset only requested applied rows; unrelated pending drafts must never become active.
                    let mut next = state.document.clone();
                    if let Some(key) = key {
                        next.drafts.remove(&key);
                        next.applied.remove(&key);
                    } else {
                        next.drafts.clear();
                        next.applied.clear();
                    }
                    let result = state
                        .root
                        .as_ref()
                        .ok_or("io")
                        .and_then(|root| next.publish(root, &state.pack));
                    match result {
                        Ok(()) => {
                            state.document = next;
                            state.unsaved = false;
                            state.error = None;
                            if state.pack.target == Target::Menu {
                                let code = state.pack.lang.code().to_owned();
                                if state.document.applied.is_empty() {
                                    state.menus.remove(&code);
                                } else {
                                    state.menus.insert(
                                        code,
                                        editor::menu_path(
                                            state.root.as_ref().unwrap(),
                                            state.pack.lang,
                                            &state.pack.export(&state.document.applied).unwrap(),
                                        ),
                                    );
                                }
                            }
                            state.notice = Some("languages.reset_done");
                        }
                        Err(e) => state.error = Some(e),
                    }
                    state.reset = None;
                }
                Some(widgets::Answer::Cancel) => state.reset = None,
                None => {}
            }
        } else if close || modal.should_close() {
            // A storage failure must not trap the rest of the app behind this dialog. Keep unsaved drafts in memory.
            if state.unsaved {
                state.save();
            }
            state.open = false;
            state.focus.closed(ctx);
        }
    }
}

#[cfg(test)]
mod tests;
