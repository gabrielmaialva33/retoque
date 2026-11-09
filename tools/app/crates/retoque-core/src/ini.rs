//! Byte-preserving ini editing. Names compare without ASCII case; the last key wins.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Ini {
    lines: Vec<Vec<u8>>,
}

fn body(line: &[u8]) -> &[u8] {
    let line = line.strip_suffix(b"\n").unwrap_or(line);
    line.strip_suffix(b"\r").unwrap_or(line)
}
fn trim(s: &[u8]) -> &[u8] {
    s.trim_ascii()
}
fn header(line: &[u8]) -> Option<&[u8]> {
    let text = trim(body(line).strip_prefix(b"\xef\xbb\xbf").unwrap_or(body(line)));
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
        Ini {
            lines: bytes.split_inclusive(|b| *b == b'\n').map(Vec::from).collect(),
        }
    }
    pub fn get(&self, section: &str, key: &str) -> Option<&str> {
        self.values(section, key)
            .last()
            .and_then(|s| std::str::from_utf8(s).ok())
    }
    pub(crate) fn values(&self, section: &str, key: &str) -> Vec<&[u8]> {
        let mut inside = false;
        let mut values = Vec::new();
        for line in &self.lines {
            if let Some(name) = header(line) {
                inside = name.eq_ignore_ascii_case(section.as_bytes());
            } else if inside
                && let Some((name, value)) = key_value(line)
                && name.eq_ignore_ascii_case(key.as_bytes())
            {
                values.push(value);
            }
        }
        values
    }
    /// Replaces every copy; a missing key goes directly below the first matching header, as in both scripts.
    pub fn set(&mut self, section: &str, key: &str, value: &str) -> bool {
        let insert = self.values(section, key).is_empty();
        let mut inside = false;
        let mut inserted = false;
        let mut out = Vec::new();
        for line in &self.lines {
            if let Some(name) = header(line) {
                inside = name.eq_ignore_ascii_case(section.as_bytes());
                out.extend(line);
                if inside && insert && !inserted {
                    if !line.ends_with(b"\n") {
                        out.push(b'\n');
                    }
                    let end = if line.ends_with(b"\r\n") { "\r\n" } else { "\n" };
                    out.extend(format!("{key}={value}{end}").bytes());
                    inserted = true;
                }
            } else if inside && key_value(line).is_some_and(|(name, _)| name.eq_ignore_ascii_case(key.as_bytes())) {
                let eq = line.iter().position(|b| *b == b'=').unwrap();
                out.extend(&line[..=eq]);
                out.extend(value.bytes());
                out.extend(&line[body(line).len()..]);
            } else {
                out.extend(line);
            }
        }
        if insert && !inserted {
            if !out.is_empty() && !out.ends_with(b"\n") {
                out.push(b'\n');
            }
            out.extend(format!("\n[{section}]\n{key}={value}\n").bytes());
        }
        let changed = out != self.to_bytes();
        *self = Ini::parse(&out);
        changed
    }
    pub fn to_bytes(&self) -> Vec<u8> {
        self.lines.concat()
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
        assert_eq!(ini.to_bytes(), b";\xff untouched\r\n[ PlUgIns ]\r\n; LoadReshade=false\r\nloadreshade =true\r\nLoadReShade=true\r\n[Other]\r\nLoadReshade=false\r\n");
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
