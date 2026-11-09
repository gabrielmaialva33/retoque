//! Byte-preserving ini editing. Names compare without ASCII case; the last key wins.

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

fn header(line: &[u8]) -> Option<&[u8]> {
    let b = body(line);
    let text = trim(b.strip_prefix(b"\xef\xbb\xbf").unwrap_or(b));
    let name = text.strip_prefix(b"[")?.strip_suffix(b"]")?;
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
        };
        ini.rebuild_entries();
        ini
    }

    fn rebuild_entries(&mut self) {
        self.entries.clear();
        let mut current_section: Option<(usize, usize)> = None;
        for line in self.bytes.split_inclusive(|b| *b == b'\n') {
            if let Some(name) = header(line) {
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
                self.bytes[e.section.0..e.section.1].eq_ignore_ascii_case(sec_bytes)
                    && self.bytes[e.key.0..e.key.1].eq_ignore_ascii_case(key_bytes)
            })
            .and_then(|e| std::str::from_utf8(&self.bytes[e.value.0..e.value.1]).ok())
    }

    pub(crate) fn values(&self, section: &str, key: &str) -> Vec<&[u8]> {
        let sec_bytes = section.as_bytes();
        let key_bytes = key.as_bytes();
        self.entries
            .iter()
            .filter(|e| {
                self.bytes[e.section.0..e.section.1].eq_ignore_ascii_case(sec_bytes)
                    && self.bytes[e.key.0..e.key.1].eq_ignore_ascii_case(key_bytes)
            })
            .map(|e| &self.bytes[e.value.0..e.value.1])
            .collect()
    }

    /// Replaces every copy; a missing key goes directly below the first matching header, as in both scripts.
    pub fn set(&mut self, section: &str, key: &str, value: &str) -> bool {
        let sec_bytes = section.as_bytes();
        let key_bytes = key.as_bytes();
        let insert = !self.entries.iter().any(|e| {
            self.bytes[e.section.0..e.section.1].eq_ignore_ascii_case(sec_bytes)
                && self.bytes[e.key.0..e.key.1].eq_ignore_ascii_case(key_bytes)
        });
        let mut inside = false;
        let mut inserted = false;
        let mut out = Vec::with_capacity(self.bytes.len() + section.len() + key.len() + value.len() + 8);
        for line in self.bytes.split_inclusive(|b| *b == b'\n') {
            if let Some(name) = header(line) {
                inside = name.eq_ignore_ascii_case(sec_bytes);
                out.extend_from_slice(line);
                if inside && insert && !inserted {
                    if !line.ends_with(b"\n") {
                        out.push(b'\n');
                    }
                    let end: &[u8] = if line.ends_with(b"\r\n") { b"\r\n" } else { b"\n" };
                    out.extend_from_slice(key_bytes);
                    out.push(b'=');
                    out.extend_from_slice(value.as_bytes());
                    out.extend_from_slice(end);
                    inserted = true;
                }
            } else if inside && key_value(line).is_some_and(|(name, _)| name.eq_ignore_ascii_case(key_bytes)) {
                let eq = line.iter().position(|b| *b == b'=').unwrap();
                out.extend_from_slice(&line[..=eq]);
                out.extend_from_slice(value.as_bytes());
                out.extend_from_slice(&line[body(line).len()..]);
            } else {
                out.extend_from_slice(line);
            }
        }
        if insert && !inserted {
            if !out.is_empty() && !out.ends_with(b"\n") {
                out.push(b'\n');
            }
            out.extend_from_slice(b"\n[");
            out.extend_from_slice(sec_bytes);
            out.extend_from_slice(b"]\n");
            out.extend_from_slice(key_bytes);
            out.push(b'=');
            out.extend_from_slice(value.as_bytes());
            out.push(b'\n');
        }
        let changed = out != self.bytes;
        if changed {
            self.bytes = out;
            self.rebuild_entries();
        }
        changed
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
