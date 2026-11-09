//! `<game>/.retoque/receipt.txt`, version 1, byte-compatible with tools/deploy-to-game.sh and
//! tools/install-windows.ps1. A receipt is plain text in a game folder and is not trusted: every path must be
//! relative, without `.` or `..` components, tabs or newlines.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReceiptStatus {
    Complete,
    Partial,
    UninstallIncomplete,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Record {
    Proxy { name: String },
    File { sha256: String, path: String },
    Backup { sha256: String, path: String, copy: String },
    Rename { sha256: String, from: String, to: String },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Receipt {
    pub status: ReceiptStatus,
    pub updated: String,
    pub records: Vec<Record>,
}

pub(crate) const HEADER: &str = "# Retoque deploy receipt: what tools/deploy-to-game.sh wrote, set aside or renamed here.\n# --uninstall reads it. Tab-separated; paths are relative to the game directory.\n";

/// Uses the stricter Windows rules on both platforms, so receipts remain portable.
pub(crate) fn safe_rel(path: &str) -> bool {
    !path.is_empty()
        && !path
            .chars()
            .any(|c| c.is_control() || matches!(c, '\\' | ':' | '<' | '>' | '\"' | '|' | '?' | '*'))
        && path
            .split('/')
            .all(|part| !part.is_empty() && part != "." && part != ".." && !part.ends_with(['.', ' ']))
}
impl Record {
    pub(crate) fn paths(&self) -> Vec<&str> {
        match self {
            Self::Proxy { name } => vec![name],
            Self::File { path, .. } => vec![path],
            Self::Backup { path, copy, .. } => vec![path, copy],
            Self::Rename { from, to, .. } => vec![from, to],
        }
    }
}
impl Receipt {
    pub fn parse(text: &str) -> Result<Receipt, String> {
        let mut receipt = Receipt {
            status: ReceiptStatus::Partial,
            updated: String::new(),
            records: Vec::new(),
        };
        let mut version = false;
        for line in text.trim_start_matches('\u{feff}').lines() {
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let fields: Vec<_> = line.split('\t').collect();
            let record = match fields.as_slice() {
                ["version", "1"] => {
                    version = true;
                    continue;
                }
                ["status", status] => {
                    receipt.status = match *status {
                        "complete" => ReceiptStatus::Complete,
                        "partial" => ReceiptStatus::Partial,
                        "uninstall-incomplete" => ReceiptStatus::UninstallIncomplete,
                        _ => return Err(format!("unknown receipt status: {status}")),
                    };
                    continue;
                }
                ["updated", updated] => {
                    receipt.updated = updated.to_string();
                    continue;
                }
                ["proxy", name] => Record::Proxy { name: name.to_string() },
                ["file", sha, path] => Record::File {
                    sha256: sha.to_string(),
                    path: path.to_string(),
                },
                ["backup", sha, path, copy] => Record::Backup {
                    sha256: sha.to_string(),
                    path: path.to_string(),
                    copy: copy.to_string(),
                },
                ["rename", sha, from, to] => Record::Rename {
                    sha256: sha.to_string(),
                    from: from.to_string(),
                    to: to.to_string(),
                },
                _ => return Err(format!("unrecognised or invalid receipt record: {}", fields[0])),
            };
            if record.paths().iter().any(|p| !safe_rel(p)) {
                return Err("unsafe path in receipt".into());
            }
            match &record {
                Record::File { path, .. } => {
                    if let Some(old) = receipt
                        .records
                        .iter_mut()
                        .find(|r| matches!(r, Record::File { path: p, .. } if p == path))
                    {
                        *old = record;
                        continue;
                    }
                }
                Record::Proxy { .. } if receipt.records.contains(&record) => continue,
                _ => {}
            }
            receipt.records.push(record);
        }
        if !version {
            return Err("receipt must declare version 1".into());
        }
        Ok(receipt)
    }
    pub fn to_text(&self) -> String {
        let status = match self.status {
            ReceiptStatus::Complete => "complete",
            ReceiptStatus::Partial => "partial",
            ReceiptStatus::UninstallIncomplete => "uninstall-incomplete",
        };
        let mut text = format!("{HEADER}version\t1\nstatus\t{status}\nupdated\t{}\n", self.updated);
        // Both scripts group record kinds, preserving first-write order within each kind.
        for kind in 0..4 {
            for record in &self.records {
                match record {
                    Record::Proxy { name } if kind == 0 => text.push_str(&format!("proxy\t{name}\n")),
                    Record::File { sha256, path } if kind == 1 => text.push_str(&format!("file\t{sha256}\t{path}\n")),
                    Record::Backup { sha256, path, copy } if kind == 2 => {
                        text.push_str(&format!("backup\t{sha256}\t{path}\t{copy}\n"))
                    }
                    Record::Rename { sha256, from, to } if kind == 3 => {
                        text.push_str(&format!("rename\t{sha256}\t{from}\t{to}\n"))
                    }
                    _ => {}
                }
            }
        }
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn script_receipts_and_roundtrip() {
        let records = "version\t1\nstatus\tcomplete\nupdated\t2026-11-05T12:34:56Z\nproxy\tdxgi.dll\nfile\tabc\tdxgi.dll\nbackup\tdef\tdxgi.dll\t.retoque/backup/20261105T123456Z/dxgi.dll\nrename\t123\tlib.dll\tlib.disabled\n";
        let bash = format!("{HEADER}{records}");
        let windows = format!(
            "# Retoque install receipt: what install-windows.ps1 or tools/deploy-to-game.sh wrote, set aside or renamed here.\n# Their uninstall reads it. Tab-separated; paths are relative to the game directory.\n{records}"
        );
        let a = Receipt::parse(&bash).unwrap();
        assert_eq!(a, Receipt::parse(&windows).unwrap());
        assert_eq!(a.to_text(), bash);
        assert_eq!(a, Receipt::parse(&a.to_text()).unwrap());
        assert_eq!(a, Receipt::parse(&windows.replace('\n', "\r\n")).unwrap());
    }
    #[test]
    fn unsafe_paths_and_unknown_records_refused() {
        for path in [
            "../x", "..\\x", "/x", "C:/x", "a/.. /x", "a/./x", "x:stream", "a//b", "a/", "a\tb", "a\rb",
        ] {
            assert!(
                Receipt::parse(&format!("version\t1\nfile\tabc\t{path}\n")).is_err(),
                "{path}"
            );
        }
        for text in ["version\t2\n", "version\t1\nwhatever\tx\n", "file\tabc\tx\n"] {
            assert!(Receipt::parse(text).is_err());
        }
    }
}
