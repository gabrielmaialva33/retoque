//! Byte-preserving ini editing. Names compare without ASCII case; the last key wins.

use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
struct Splice {
    offset: usize,
    before: Vec<u8>,
    after: Vec<u8>,
}

/// Sparse provenance for one INI edit. Includes only replaced/inserted bytes, not an INI backup.
/// Deserialized provenance is validated by replaying the original edit before it can undo anything.
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct IniUndo {
    section: String,
    key: String,
    before_value: Option<String>,
    after_value: Option<String>,
    before_hash: String,
    after_hash: String,
    splices: Vec<Splice>,
    #[serde(default)]
    case_sensitive: bool,
}

impl IniUndo {
    pub(crate) fn matches_before(&self, bytes: &[u8]) -> bool {
        self.before_hash == format!("{:x}", Sha256::digest(bytes))
    }
    pub fn matches_after(&self, bytes: &[u8]) -> bool {
        self.after_hash == format!("{:x}", Sha256::digest(bytes))
    }

    pub(crate) fn matches_inverse(&self, section: &str, key: &str, value: Option<&str>) -> bool {
        self.section == section && self.key == key && self.before_value.as_deref() == value
    }
}

#[derive(Clone, Copy, Debug)]
struct Entry {
    section: (usize, usize),
    key: (usize, usize),
    value: (usize, usize),
}

#[derive(Clone, Debug, Default)]
pub struct Ini {
    bytes: Vec<u8>,
    entries: Vec<Entry>,
    case_sensitive: bool,
}

impl PartialEq for Ini {
    fn eq(&self, other: &Self) -> bool {
        self.bytes == other.bytes
    }
}

impl Eq for Ini {}

fn body(line: &[u8]) -> &[u8] {
    let line = line.strip_suffix(b"\n").unwrap_or(line);
    line.strip_suffix(b"\r").unwrap_or(line)
}

fn trim(s: &[u8]) -> &[u8] {
    s.trim_ascii()
}

fn subslice_range(base: &[u8], sub: &[u8]) -> (usize, usize) {
    let start = sub.as_ptr() as usize - base.as_ptr() as usize;
    (start, start + sub.len())
}

fn header(line: &[u8], case_sensitive: bool) -> Option<&[u8]> {
    let b = body(line);
    let text = trim(b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(b));
    let content = text.strip_prefix(b"[")?;
    let name = if case_sensitive {
        &content[..content.iter().position(|b| *b == b']')?]
    } else {
        content.strip_suffix(b"]")?
    };
    (!name.contains(&b']')).then(|| trim(name))
}

fn key_value(line: &[u8]) -> Option<(&[u8], &[u8])> {
    let text = trim(body(line));
    if matches!(text.first(), Some(b';' | b'#')) {
        return None;
    }
    let eq = text.iter().position(|b| *b == b'=')?;
    let key = trim(&text[..eq]);
    (!key.is_empty()).then(|| (key, trim(&text[eq + 1..])))
}

impl Ini {
    pub fn parse(bytes: &[u8]) -> Ini {
        let mut ini = Ini {
            bytes: bytes.to_vec(),
            entries: Vec::new(),
            case_sensitive: false,
        };
        ini.rebuild_entries();
        ini
    }

    /// ReShade uses case-sensitive section/key names. OptiScaler keeps the default behavior.
    pub fn parse_sensitive(bytes: &[u8]) -> Ini {
        let mut ini = Self::parse(bytes);
        ini.case_sensitive = true;
        ini.rebuild_entries();
        ini
    }

    fn names_match(&self, a: &[u8], b: &[u8]) -> bool {
        if self.case_sensitive {
            a == b
        } else {
            a.eq_ignore_ascii_case(b)
        }
    }

    fn rebuild_entries(&mut self) {
        self.entries.clear();
        let mut current_section: Option<(usize, usize)> = None;
        for line in self.bytes.split_inclusive(|b| *b == b'\n') {
            if let Some(name) = header(line, self.case_sensitive) {
                current_section = Some(subslice_range(&self.bytes, name));
            } else if let Some(section) = current_section
                && let Some((k, v)) = key_value(line)
            {
                self.entries.push(Entry {
                    section,
                    key: subslice_range(&self.bytes, k),
                    value: subslice_range(&self.bytes, v),
                });
            }
        }
    }

    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        let sec_bytes = section.as_bytes();
        let key_bytes = key.as_bytes();
        self.entries
            .iter()
            .rev()
            .find(|e| {
                self.names_match(&self.bytes[e.section.0..e.section.1], sec_bytes)
                    && self.names_match(&self.bytes[e.key.0..e.key.1], key_bytes)
            })
            .and_then(|e| std::str::from_utf8(&self.bytes[e.value.0..e.value.1]).ok())
    }

    pub(crate) fn values(&self, section: &str, key: &str) -> Vec<&[u8]> {
        let sec_bytes = section.as_bytes();
        let key_bytes = key.as_bytes();
        self.entries
            .iter()
            .filter(|e| {
                self.names_match(&self.bytes[e.section.0..e.section.1], sec_bytes)
                    && self.names_match(&self.bytes[e.key.0..e.key.1], key_bytes)
            })
            .map(|e| &self.bytes[e.value.0..e.value.1])
            .collect()
    }

    /// Replaces every copy; a missing key goes directly below the first matching header, as in both scripts.
    pub fn set(&mut self, section: &str, key: &str, value: &str) -> bool {
        !self.edit(section, key, Some(value)).is_empty()
    }

    /// Removes every matching key line. All other bytes, including empty sections, are preserved.
    /// Exact reversal of inserted headers/newlines requires `edit_with_undo` provenance.
    pub fn unset(&mut self, section: &str, key: &str) -> bool {
        !self.edit(section, key, None).is_empty()
    }

    pub fn edit_with_undo(&mut self, section: &str, key: &str, value: Option<&str>) -> Option<IniUndo> {
        let before_hash = format!("{:x}", Sha256::digest(&self.bytes));
        let before_value = self.get(section, key).map(str::to_owned);
        let splices = self.edit(section, key, value);
        if splices.is_empty() {
            return None;
        }
        Some(IniUndo {
            section: section.into(),
            key: key.into(),
            before_value,
            after_value: value.map(str::to_owned),
            before_hash,
            after_hash: format!("{:x}", Sha256::digest(&self.bytes)),
            splices,
            case_sensitive: self.case_sensitive,
        })
    }

    /// Validates the current hash and the complete sparse edit by replay; failure leaves this INI intact.
    pub fn undo(&mut self, undo: &IniUndo) -> bool {
        if !undo.matches_after(&self.bytes) {
            return false;
        }
        let mut restored = self.bytes.clone();
        // Splices use original offsets and are applied from end to start. Undo from start to end.
        for splice in &undo.splices {
            let Some(end) = splice.offset.checked_add(splice.after.len()) else {
                return false;
            };
            if restored.get(splice.offset..end) != Some(splice.after.as_slice()) {
                return false;
            }
            restored.splice(splice.offset..end, splice.before.iter().copied());
        }
        let mut replay = Self::parse(&restored);
        replay.case_sensitive = undo.case_sensitive;
        replay.rebuild_entries();
        if replay
            .edit_with_undo(&undo.section, &undo.key, undo.after_value.as_deref())
            .as_ref()
            != Some(undo)
            || replay.bytes != self.bytes
        {
            return false;
        }
        self.bytes = restored;
        self.rebuild_entries();
        true
    }

    fn edit(&mut self, section: &str, key: &str, value: Option<&str>) -> Vec<Splice> {
        let sec_bytes = section.as_bytes();
        let key_bytes = key.as_bytes();
        let insert = value.is_some()
            && !self.entries.iter().any(|e| {
                self.names_match(&self.bytes[e.section.0..e.section.1], sec_bytes)
                    && self.names_match(&self.bytes[e.key.0..e.key.1], key_bytes)
            });
        let mut inside = false;
        let mut inserted = false;
        let mut splices = Vec::new();
        for line in self.bytes.split_inclusive(|b| *b == b'\n') {
            let start = subslice_range(&self.bytes, line).0;
            if let Some(name) = header(line, self.case_sensitive) {
                inside = self.names_match(name, sec_bytes);
                if inside && insert && !inserted {
                    let mut after = Vec::new();
                    if !line.ends_with(b"\n") {
                        after.push(b'\n');
                    }
                    let end: &[u8] = if line.ends_with(b"\r\n") { b"\r\n" } else { b"\n" };
                    after.extend_from_slice(key_bytes);
                    after.push(b'=');
                    after.extend_from_slice(value.unwrap().as_bytes());
                    after.extend_from_slice(end);
                    splices.push(Splice {
                        offset: start + line.len(),
                        before: Vec::new(),
                        after,
                    });
                    inserted = true;
                }
            } else if inside && key_value(line).is_some_and(|(name, _)| self.names_match(name, key_bytes)) {
                let (offset, before, after) = if let Some(value) = value {
                    let eq = line.iter().position(|b| *b == b'=').unwrap();
                    (start + eq + 1, &line[eq + 1..body(line).len()], value.as_bytes())
                } else {
                    (start, line, &b""[..])
                };
                if before != after {
                    splices.push(Splice {
                        offset,
                        before: before.to_vec(),
                        after: after.to_vec(),
                    });
                }
            }
        }
        if insert && !inserted {
            let mut after = Vec::new();
            if !self.bytes.is_empty() && !self.bytes.ends_with(b"\n") {
                after.push(b'\n');
            }
            after.extend_from_slice(b"\n[");
            after.extend_from_slice(sec_bytes);
            after.extend_from_slice(b"]\n");
            after.extend_from_slice(key_bytes);
            after.push(b'=');
            after.extend_from_slice(value.unwrap().as_bytes());
            after.push(b'\n');
            splices.push(Splice {
                offset: self.bytes.len(),
                before: Vec::new(),
                after,
            });
        }
        for splice in splices.iter().rev() {
            self.bytes.splice(
                splice.offset..splice.offset + splice.before.len(),
                splice.after.iter().copied(),
            );
        }
        if !splices.is_empty() {
            self.rebuild_entries();
        }
        splices
    }

    pub fn to_bytes(&self) -> Vec<u8> {
        self.bytes.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn duplicate_keys_crlf_comments_and_other_section() {
        let original = b";\xff untouched\r\n[ PlUgIns ]\r\n; LoadReshade=false\r\nloadreshade = auto\r\nLoadReShade=false\r\n[Other]\r\nLoadReshade=false\r\n";
        let mut ini = Ini::parse(original);
        assert_eq!(ini.get("plugins", "loadreshade"), Some("false"));
        assert!(ini.set("Plugins", "LoadReshade", "true"));
        assert_eq!(
            ini.to_bytes(),
            b";\xff untouched\r\n[ PlUgIns ]\r\n; LoadReshade=false\r\nloadreshade =true\r\nLoadReShade=true\r\n[Other]\r\nLoadReshade=false\r\n"
        );
        assert!(!ini.set("Plugins", "LoadReshade", "true"));
    }
    #[test]
    fn missing_key_and_section() {
        let mut ini = Ini::parse(b"[Plugins]\nLoadAsiPlugins=auto\n");
        ini.set("Plugins", "LoadReshade", "true");
        assert_eq!(ini.to_bytes(), b"[Plugins]\nLoadReshade=true\nLoadAsiPlugins=auto\n");
        let mut ini = Ini::parse(b"[Log]\nLogLevel=2");
        ini.set("Plugins", "LoadReshade", "true");
        assert_eq!(ini.to_bytes(), b"[Log]\nLogLevel=2\n\n[Plugins]\nLoadReshade=true\n");
    }
    #[test]
    fn repeated_section_and_unterminated_line_preserved() {
        let mut ini = Ini::parse(b"[A]\nk=1\n[A]\nk=2");
        ini.set("a", "k", "3");
        assert_eq!(ini.to_bytes(), b"[A]\nk=3\n[A]\nk=3");
    }
    #[test]
    fn bom_and_invalid_utf8_roundtrip() {
        let bytes = b"\xef\xbb\xbf[A]\r\nk=1\r\n;\xff";
        let mut ini = Ini::parse(bytes);
        assert_eq!(ini.to_bytes(), bytes);
        assert_eq!(ini.get("a", "k"), Some("1"));
        ini.set("a", "k", "2");
        assert_eq!(ini.to_bytes(), b"\xef\xbb\xbf[A]\r\nk=2\r\n;\xff");
    }
}

#[cfg(test)]
mod unset_tests {
    use super::*;

    #[test]
    fn unset_removes_all_copies_and_preserves_every_other_byte() {
        let mut ini = Ini::parse(b"\xef\xbb\xbf[ A ]\r\n K =\xff\r\n;K=comment\n#K=comment\r\nq=untouched\n[a]\nk=two\r\n[B]\nk=other\n[A]\nk=end");
        assert!(ini.unset("a", "K"));
        assert_eq!(
            ini.to_bytes(),
            b"\xef\xbb\xbf[ A ]\r\n;K=comment\n#K=comment\r\nq=untouched\n[a]\n[B]\nk=other\n[A]\n"
        );
        assert!(!ini.unset("a", "k"));
        assert_eq!(ini.get("B", "k"), Some("other"));
    }

    #[test]
    fn exact_insert_undo_distinguishes_created_headers_and_newlines_after_reparse() {
        for original in [
            b"".as_slice(),
            b"[A]",
            b"[A]\n",
            b"\n[A]\n",
            b"[A]\r",
            b"\xef\xbb\xbf[A]\r\n",
            b";\xff\n[B]\nk=1",
        ] {
            let mut ini = Ini::parse(original);
            let undo = ini.edit_with_undo("A", "k", Some("value")).unwrap();
            let mut reloaded = Ini::parse(&ini.to_bytes());
            assert!(reloaded.undo(&undo));
            assert_eq!(reloaded.to_bytes(), original);
        }
    }

    #[test]
    fn exact_undo_preserves_duplicate_values_and_whitespace() {
        let original = b"[A]\nk= one \r\n;keep\n[A]\nk=two\n";
        let mut ini = Ini::parse(original);
        let undo = ini.edit_with_undo("A", "k", Some("new")).unwrap();
        assert!(ini.undo(&undo));
        assert_eq!(ini.to_bytes(), original);
        let undo = ini.edit_with_undo("A", "k", None).unwrap();
        assert!(ini.undo(&undo));
        assert_eq!(ini.to_bytes(), original);
    }

    #[test]
    fn stale_and_forged_provenance_leave_bytes_unchanged() {
        let mut ini = Ini::parse(b"[A]\n");
        let mut undo = ini.edit_with_undo("A", "k", Some("v")).unwrap();
        let after = ini.to_bytes();
        undo.splices[0].before = b"unreviewed=change\n".to_vec();
        assert!(!ini.undo(&undo));
        assert_eq!(ini.to_bytes(), after);
        undo.splices[0].offset = usize::MAX;
        assert!(!ini.undo(&undo));
        assert_eq!(ini.to_bytes(), after);
        ini.set("A", "other", "external");
        let changed = ini.to_bytes();
        assert!(!ini.undo(&undo));
        assert_eq!(ini.to_bytes(), changed);
    }
}
