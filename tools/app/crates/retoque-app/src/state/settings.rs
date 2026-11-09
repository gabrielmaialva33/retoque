//! Effective settings values are captured only when an INI read completes.
use retoque_core::ini::Ini;
use std::collections::BTreeMap;
#[derive(Default)]
pub(crate) struct IniValues(BTreeMap<(&'static str, &'static str), String>);
impl IniValues {
    pub fn read(ini: &Ini) -> Self {
        Self(
            retoque_data::settings::all()
                .iter()
                .filter_map(|s| ini.get(s.section, s.key).map(|v| ((s.section, s.key), v.to_owned())))
                .collect(),
        )
    }
    pub fn get(&self, section: &'static str, key: &'static str) -> Option<&str> {
        self.0.get(&(section, key)).map(String::as_str)
    }
}

#[derive(Default)]
pub(crate) struct Filter {
    pub query: String,
    pub only_changed: bool,
    pub focus: bool,
    cached_query: String,
    lang: Option<crate::i18n::Lang>,
    matches: Vec<bool>,
}
impl Filter {
    pub fn refresh(&mut self, lang: crate::i18n::Lang) {
        if self.lang == Some(lang) && self.cached_query == self.query {
            return;
        }
        self.lang = Some(lang);
        self.cached_query.clone_from(&self.query);
        let mut search = crate::fuzzy::Search::new(&self.query);
        self.matches = retoque_data::settings::all()
            .iter()
            .map(|s| {
                let label = match lang {
                    crate::i18n::Lang::En => s.label_en,
                    crate::i18n::Lang::Pt => s.label_pt,
                };
                search.score(&format!("{label} {} {}", s.section, s.key)).is_some()
            })
            .collect();
    }
    pub fn includes(&self, index: usize, disk: &str, draft: Option<&str>) -> bool {
        self.matches.get(index).copied().unwrap_or(true)
            && (!self.only_changed
                || !disk.eq_ignore_ascii_case("auto")
                || draft.is_some_and(|v| !v.eq_ignore_ascii_case("auto")))
    }
    pub fn active(&self) -> bool {
        !self.query.is_empty() || self.only_changed
    }
}
