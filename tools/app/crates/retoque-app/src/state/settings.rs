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
