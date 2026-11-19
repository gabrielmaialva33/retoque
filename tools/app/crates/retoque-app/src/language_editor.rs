//! Independent correction workflow inspired by the NeuRotic Hub editor screenshot.
//! Drafts and applied corrections are separate. No editor operation accepts a game directory.
use crate::i18n::{self, Lang};
use retoque_core::menu_pack::{self, Entries, Result};
use serde::{Deserialize, Serialize};
use std::{
    cell::RefCell,
    collections::HashMap,
    io::Write,
    path::{Path, PathBuf},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Target {
    App,
    Menu,
}
impl Target {
    pub fn extension(self) -> &'static str {
        if self == Self::App { "tsv" } else { "lang" }
    }
    pub fn folder(self) -> &'static str {
        if self == Self::App { "app" } else { "menu" }
    }
}

// The existing translation API lends strings. Intern only validated applied text, never keystrokes.
// Retained strings are deduplicated and capped process-wide at 16 MiB; further applies require a restart.
// Lookup maps are UI-thread local so independent windows/test harnesses never share active translations.
thread_local! { static ACTIVE: RefCell<HashMap<Lang, HashMap<String, &'static str>>> = RefCell::default(); }
#[derive(Default)]
struct Interner {
    bytes: usize,
    values: HashMap<&'static str, &'static str>,
}
static INTERN: std::sync::Mutex<Option<Interner>> = std::sync::Mutex::new(None);
pub(crate) fn lookup(lang: Lang, key: &str) -> Option<&'static str> {
    ACTIVE.with(|active| active.borrow().get(&lang).and_then(|p| p.get(key)).copied())
}
fn prepare_active(entries: &Entries) -> Result<HashMap<String, &'static str>> {
    let mut lock = INTERN.lock().map_err(|_| "memory")?;
    let pool = lock.get_or_insert_with(Default::default);
    let additional: usize = entries
        .values()
        .filter(|v| !pool.values.contains_key(v.as_str()))
        .map(String::len)
        .sum();
    if pool.bytes + additional > 16 * 1024 * 1024 || pool.values.len() + entries.len() > 65_536 {
        return Err("memory");
    }
    Ok(entries
        .iter()
        .map(|(key, value)| {
            let value = if let Some(found) = pool.values.get(value.as_str()) {
                *found
            } else {
                pool.bytes += value.len();
                let stored: &'static str = Box::leak(value.clone().into_boxed_str());
                pool.values.insert(stored, stored);
                stored
            };
            (key.clone(), value)
        })
        .collect())
}

fn menu_source(lang: Lang) -> &'static str {
    macro_rules! pack {
        ($name:literal) => {
            include_str!(concat!(
                "../../../../../retoque/translations/optional/",
                $name,
                ".lang"
            ))
        };
    }
    match lang {
        Lang::Pt => pack!("pt-BR"),
        Lang::De => pack!("de"),
        Lang::Es => pack!("es"),
        Lang::Fr => pack!("fr"),
        Lang::Ja => pack!("ja"),
        Lang::Ko => pack!("ko"),
        Lang::Pl => pack!("pl"),
        Lang::Ru => pack!("ru"),
        Lang::ZhCn => pack!("zh-CN"),
        Lang::En => "",
    }
}

pub(crate) struct Pack {
    pub target: Target,
    pub lang: Lang,
    pub keys: Vec<String>,
    pub english: Entries,
    pub embedded: Entries,
    template: String,
}
impl Pack {
    pub fn new(target: Target, lang: Lang) -> Self {
        let template = match target {
            Target::App => i18n::pack_source(lang).unwrap_or(""),
            Target::Menu => menu_source(lang),
        }
        .to_owned();
        let mut keys = Vec::new();
        let mut embedded = Entries::new();
        for line in template.lines().filter(|l| !l.is_empty() && !l.starts_with('#')) {
            let (key, value) = line.split_once('\t').expect("shipped pack");
            let (key, value) = if target == Target::Menu {
                (menu_pack::decode(key).unwrap(), menu_pack::decode(value).unwrap())
            } else {
                (key.to_owned(), value.to_owned())
            };
            keys.push(key.clone());
            embedded.insert(key, value);
        }
        let mut english: Entries = if target == Target::Menu {
            keys.iter().map(|k| (k.clone(), k.clone())).collect()
        } else {
            i18n::english().into_iter().map(|(k, v)| (k, v.to_owned())).collect()
        };
        if target == Target::App {
            for key in &keys {
                if english.contains_key(key) {
                    continue;
                }
                if let Some(base) = key.strip_suffix("_few").or_else(|| key.strip_suffix("_many"))
                    && english.contains_key(&format!("{base}_one"))
                    && let Some(value) = english
                        .get(&format!("{base}_other"))
                        .or_else(|| english.get(base))
                        .cloned()
                {
                    english.insert(key.clone(), value);
                }
            }
            for key in english.keys() {
                if !embedded.contains_key(key) {
                    keys.push(key.clone());
                }
            }
        }
        Self {
            target,
            lang,
            keys,
            english,
            embedded,
            template,
        }
    }
    pub fn base(&self, key: &str) -> &str {
        self.embedded
            .get(key)
            .or_else(|| self.english.get(key))
            .map_or("", String::as_str)
    }
    pub fn validate(&self, key: &str, value: &str) -> Result<()> {
        let source = self.english.get(key).ok_or("unknown")?;
        if value.trim().is_empty() {
            return Err("empty");
        }
        if menu_pack::controls(value, self.target == Target::Menu) {
            return Err("control");
        }
        if value.len() + key.len() + 1 > menu_pack::MAX_LINE {
            return Err("size");
        }
        if self.target == Target::Menu {
            menu_pack::compatible(source, value)?;
            if source.ends_with('…') != value.ends_with('…') {
                return Err("ellipsis");
            }
            if menu_pack::encode(key).len() + menu_pack::encode(value).len() + 1 > menu_pack::MAX_LINE {
                return Err("size");
            }
        } else {
            if value.trim() != value {
                return Err("padding");
            }
            if placeholders(source) != placeholders(value) {
                return Err("placeholder");
            }
            if value.contains("...") || source.ends_with('…') != value.ends_with('…') {
                return Err("ellipsis");
            }
        }
        Ok(())
    }
    pub fn import(&self, bytes: &[u8]) -> Result<Entries> {
        if bytes.len() > menu_pack::MAX_BYTES {
            return Err("size");
        }
        let text = std::str::from_utf8(bytes)
            .map_err(|_| "encoding")?
            .trim_start_matches('\u{feff}');
        let mut entries = Entries::new();
        for line in text.split('\n') {
            if line.len() > menu_pack::MAX_LINE {
                return Err("size");
            }
            let line = line.strip_suffix('\r').unwrap_or(line);
            if menu_pack::controls(line, true) || line.contains('\r') {
                return Err("control");
            }
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let (key, value) = line.split_once('\t').ok_or("columns")?;
            if value.contains('\t') {
                return Err("columns");
            }
            let (key, value) = if self.target == Target::Menu {
                (menu_pack::decode(key)?, menu_pack::decode(value)?)
            } else {
                (key.to_owned(), value.to_owned())
            };
            self.validate(&key, &value)?;
            if entries.insert(key, value).is_some() {
                return Err("duplicate");
            }
        }
        Ok(entries)
    }
    /// Preserve every shipped header/comment/row and append newly translated fallback keys in stable order.
    pub fn export(&self, reviewed: &Entries) -> Result<String> {
        for (key, value) in reviewed {
            self.validate(key, value)?;
        }
        let mut out = String::new();
        for line in self.template.lines() {
            if let Some((key, _)) = line.split_once('\t').filter(|_| !line.starts_with('#')) {
                let decoded = if self.target == Target::Menu {
                    menu_pack::decode(key)?
                } else {
                    key.to_owned()
                };
                if let Some(value) = reviewed.get(&decoded) {
                    out.push_str(&self.row(&decoded, value));
                } else {
                    out.push_str(line);
                }
            } else {
                out.push_str(line);
            }
            out.push('\n');
        }
        for key in &self.keys {
            if !self.embedded.contains_key(key)
                && let Some(value) = reviewed.get(key)
            {
                out.push_str(&self.row(key, value));
                out.push('\n');
            }
        }
        if out.len() > menu_pack::MAX_BYTES {
            return Err("size");
        }
        self.import(out.as_bytes())?;
        Ok(out)
    }
    fn row(&self, key: &str, value: &str) -> String {
        if self.target == Target::Menu {
            format!("{}\t{}", menu_pack::encode(key), menu_pack::encode(value))
        } else {
            format!("{key}\t{value}")
        }
    }
}
fn placeholders(text: &str) -> Vec<&str> {
    let mut found: Vec<_> = text
        .match_indices('{')
        .filter_map(|(at, _)| {
            let end = at + text[at..].find('}')?;
            text[at + 1..end]
                .bytes()
                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_')
                .then(|| &text[at..=end])
        })
        .collect();
    found.sort_unstable();
    found
}
pub(crate) fn preview(text: &str, target: Target) -> String {
    let mut result = text.to_owned();
    if target == Target::App {
        for name in placeholders(text) {
            result = result.replace(
                name,
                if matches!(name, "{n}" | "{count}") {
                    "3"
                } else {
                    "Retoque"
                },
            );
        }
    } else if let Ok(formats) = menu_pack::printf(text) {
        // Never call printf on imported data. Substitute plain sample text only, from left to right.
        let mut offset = 0;
        result.clear();
        for format in formats {
            let at = offset + text[offset..].find(format).unwrap();
            result.push_str(&text[offset..at]);
            result.push_str(if format == "%%" {
                "%"
            } else if format.ends_with('s') {
                "Retoque"
            } else {
                "3"
            });
            offset = at + format.len();
        }
        result.push_str(&text[offset..]);
    }
    result
}
#[derive(Clone, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub(crate) struct Document {
    pub drafts: Entries,
    pub applied: Entries,
}
impl Document {
    pub fn draft<'a>(&'a self, pack: &'a Pack, key: &str) -> &'a str {
        self.drafts.get(key).map_or_else(|| pack.base(key), String::as_str)
    }
    pub fn changed(&self, pack: &Pack) -> Entries {
        self.drafts
            .iter()
            .filter(|(k, v)| v.as_str() != pack.base(k))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect()
    }
    pub fn load(root: &Path, pack: &Pack) -> Result<Self> {
        let path = document_path(root, pack);
        if !path.try_exists().map_err(|_| "io")? {
            return Ok(Self::default());
        }
        let doc: Self = serde_json::from_slice(&menu_pack::read(&path)?).map_err(|_| "encoding")?;
        for (key, value) in &doc.drafts {
            if !pack.english.contains_key(key) {
                return Err("unknown");
            }
            if value.len() > menu_pack::MAX_LINE {
                return Err("size");
            }
        }
        pack.export(&doc.applied)?;
        Ok(doc)
    }
    pub fn save(&self, root: &Path, pack: &Pack) -> Result<()> {
        let bytes = serde_json::to_vec(self).map_err(|_| "encoding")?;
        if bytes.len() > menu_pack::MAX_BYTES {
            return Err("size");
        }
        write(&document_path(root, pack), &bytes)
    }
    pub fn apply(&mut self, root: &Path, pack: &Pack) -> Result<()> {
        let mut next = self.clone();
        next.applied = self.changed(pack);
        next.publish(root, pack)?;
        *self = next;
        Ok(())
    }
    pub fn publish(&self, root: &Path, pack: &Pack) -> Result<()> {
        let exported = pack.export(&self.applied)?;
        let active = if pack.target == Target::App {
            Some(prepare_active(&self.applied)?)
        } else {
            None
        };
        // Immutable revision filenames preserve the source of previously reviewed plans even if saving fails.
        if pack.target == Target::Menu && !self.applied.is_empty() {
            write(&menu_path(root, pack.lang, &exported), exported.as_bytes())?;
        }
        self.save(root, pack)?;
        if let Some(active) = active {
            ACTIVE.with(|all| all.borrow_mut().insert(pack.lang, active));
        }
        Ok(())
    }
}
fn document_path(root: &Path, pack: &Pack) -> PathBuf {
    root.join(pack.target.folder())
        .join(format!("{}.json", pack.lang.code()))
}
pub(crate) fn menu_path(root: &Path, lang: Lang, text: &str) -> PathBuf {
    use sha2::{Digest, Sha256};
    root.join("menu")
        .join(format!("{}-{:x}.lang", lang.code(), Sha256::digest(text.as_bytes())))
}
pub(crate) fn write(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path.parent().ok_or("io")?;
    std::fs::create_dir_all(parent).map_err(|_| "io")?;
    // Atomic replacement never follows a linked destination. Do not clobber directories or links.
    if let Ok(meta) = std::fs::symlink_metadata(path) {
        if !meta.is_file() || meta.file_type().is_symlink() {
            return Err("io");
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if meta.file_attributes() & 0x400 != 0 {
                return Err("io");
            }
        }
    }
    let mut temp = tempfile::NamedTempFile::new_in(parent).map_err(|_| "io")?;
    temp.write_all(bytes).map_err(|_| "io")?;
    temp.as_file().sync_all().map_err(|_| "io")?;
    temp.persist(path).map_err(|_| "io")?;
    Ok(())
}

pub(crate) fn load_active(root: &Path) -> Result<()> {
    ACTIVE.with(|active| active.borrow_mut().clear());
    let mut failure = None;
    for lang in Lang::ALL.into_iter().filter(|l| l.is_draft()) {
        let pack = Pack::new(Target::App, lang);
        match Document::load(root, &pack).and_then(|doc| prepare_active(&doc.applied)) {
            Ok(active) => {
                ACTIVE.with(|all| all.borrow_mut().insert(lang, active));
            }
            Err(e) => failure = Some(e),
        }
    }
    failure.map_or(Ok(()), Err)
}

#[cfg(test)]
mod tests;
