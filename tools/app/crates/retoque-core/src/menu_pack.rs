//! Data-only menu dictionaries, matching `retoque/misc/Localization.h`'s wire contract.
use std::{collections::BTreeMap, io::Read, path::Path};

pub const MAX_BYTES: usize = 1024 * 1024;
pub const MAX_LINE: usize = 16384;
pub type Entries = BTreeMap<String, String>;
pub type Result<T> = std::result::Result<T, &'static str>;

pub fn read(path: &Path) -> Result<Vec<u8>> {
    let meta = std::fs::symlink_metadata(path).map_err(|_| "io")?;
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
    let mut bytes = Vec::new();
    std::fs::File::open(path)
        .map_err(|_| "io")?
        .take(MAX_BYTES as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|_| "io")?;
    if bytes.len() > MAX_BYTES {
        return Err("size");
    }
    Ok(bytes)
}

pub fn decode(text: &str) -> Result<String> {
    let mut out = String::new();
    let mut chars = text.chars();
    while let Some(c) = chars.next() {
        out.push(if c == '\\' {
            match chars.next() {
                Some('n') => '\n',
                Some('r') => '\r',
                Some('t') => '\t',
                Some('\\') => '\\',
                _ => return Err("escape"),
            }
        } else {
            c
        });
    }
    Ok(out)
}
pub fn encode(text: &str) -> String {
    text.replace('\\', "\\\\")
        .replace('\n', "\\n")
        .replace('\r', "\\r")
        .replace('\t', "\\t")
}
pub fn controls(text: &str, multiline: bool) -> bool {
    text.chars().any(|c| {
        (c.is_control() && !(multiline && matches!(c, '\n' | '\r' | '\t')))
            || matches!(c, '\u{202a}'..='\u{202e}' | '\u{2066}'..='\u{2069}')
    })
}
/// Exact directives, including flags, width, precision, length and literal percent, in call order.
pub fn printf(text: &str) -> Result<Vec<&str>> {
    let b = text.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] != b'%' {
            i += 1;
            continue;
        }
        let start = i;
        i += 1;
        if b.get(i) == Some(&b'%') {
            i += 1;
            out.push(&text[start..i]);
            continue;
        }
        while b.get(i).is_some_and(|c| b"-+ #0'".contains(c)) {
            i += 1;
        }
        if b.get(i) == Some(&b'*') {
            i += 1;
        } else {
            while b.get(i).is_some_and(u8::is_ascii_digit) {
                i += 1;
            }
        }
        if b.get(i) == Some(&b'.') {
            i += 1;
            if b.get(i) == Some(&b'*') {
                i += 1;
            } else {
                while b.get(i).is_some_and(u8::is_ascii_digit) {
                    i += 1;
                }
            }
        }
        if b[i..].starts_with(b"I64") || b[i..].starts_with(b"I32") {
            i += 3;
        } else if b[i..].starts_with(b"ll") || b[i..].starts_with(b"hh") {
            i += 2;
        } else if b.get(i).is_some_and(|c| b"hljztLw".contains(c)) {
            i += 1;
        }
        if !b.get(i).is_some_and(|c| b"diuoxXfFeEgGaAcCsSp".contains(c)) {
            return Err("printf");
        }
        i += 1;
        out.push(&text[start..i]);
    }
    Ok(out)
}
pub fn compatible(source: &str, value: &str) -> Result<()> {
    if value.is_empty() {
        return Err("empty");
    }
    if controls(source, true) || controls(value, true) {
        return Err("control");
    }
    if value.contains("##") {
        return Err("identity");
    }
    if printf(source)? != printf(value)? {
        return Err("printf");
    }
    Ok(())
}
pub fn parse(bytes: &[u8]) -> Result<Entries> {
    if bytes.len() > MAX_BYTES {
        return Err("size");
    }
    let text = std::str::from_utf8(bytes)
        .map_err(|_| "encoding")?
        .trim_start_matches('\u{feff}');
    let mut entries = Entries::new();
    for line in text.split('\n') {
        if line.len() > MAX_LINE {
            return Err("size");
        }
        let line = line.strip_suffix('\r').unwrap_or(line);
        if controls(line, true) || line.contains('\r') {
            return Err("control");
        }
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, value) = line.split_once('\t').ok_or("columns")?;
        if value.contains('\t') {
            return Err("columns");
        }
        let (key, value) = (decode(key)?, decode(value)?);
        if key.is_empty() {
            return Err("unknown");
        }
        compatible(&key, &value)?;
        if entries.insert(key, value).is_some() {
            return Err("duplicate");
        }
        if entries.len() > 4096 {
            return Err("size");
        }
    }
    Ok(entries)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn hostile_formats_and_files() {
        for text in ["%n", "%2$s", "%", "%I64n", "%*2$d"] {
            assert!(printf(text).is_err(), "{text}");
        }
        assert!(compatible("%s %d", "%d %s").is_err());
        assert!(compatible("%.2f", "%.3f").is_err());
        assert!(compatible("%*.*llu %%", "%*.*llu %%").is_ok());
        for data in [
            b"a\tb\na\tc\n".as_slice(),
            b"a\tb\0",
            b"a\tb\tc",
            b"a\t\\x",
            b"a\t##x",
            b"a\t\xff",
        ] {
            assert!(parse(data).is_err());
        }
        assert_eq!(parse(&vec![b'x'; MAX_BYTES + 1]), Err("size"));
        assert_eq!(parse(b"a\tb\na\tc"), Err("duplicate"));
        assert_eq!(decode(&encode("a\n\t\r\\b")).unwrap(), "a\n\t\r\\b");
    }
    #[test]
    fn all_shipped_packs_match_runtime_contract() {
        for code in crate::MENU_PACKS {
            let path = Path::new(env!("CARGO_MANIFEST_DIR"))
                .join("../../../../retoque/translations/optional")
                .join(format!("{code}.lang"));
            assert!(!parse(&read(&path).unwrap()).unwrap().is_empty(), "{code}");
        }
    }
}
