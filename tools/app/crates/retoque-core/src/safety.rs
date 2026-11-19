//! Static evidence only: no loading binaries, following links, or compatibility verdicts.
use crate::pe_version::{self, Machine};
use std::{
    fs,
    path::{Path, PathBuf},
};

pub const ENTRY_BUDGET: usize = 4096;
pub const MAX_DEPTH: usize = 4;

pub(crate) fn unlinked(path: &Path) -> bool {
    path.ancestors().all(|p| {
        if p.as_os_str().is_empty() {
            return true;
        }
        fs::symlink_metadata(p).is_ok_and(|m| {
            #[cfg(windows)]
            {
                use std::os::windows::fs::MetadataExt;
                m.file_attributes() & 0x400 == 0
            }
            #[cfg(not(windows))]
            {
                !m.file_type().is_symlink()
            }
        })
    })
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Executables {
    pub files: Vec<(String, Machine)>,
    /// False on an enumeration error or budget exhaustion. Never infer x86-only from a partial list.
    pub complete: bool,
}
impl Executables {
    pub fn x86_only(&self) -> bool {
        self.complete && !self.files.is_empty() && self.files.iter().all(|(_, m)| *m == Machine::X86)
    }
    pub fn names(&self, machine: Machine) -> Vec<String> {
        self.files
            .iter()
            .filter(|(_, m)| *m == machine)
            .map(|(n, _)| n.clone())
            .collect()
    }
}
pub fn executables(root: &Path) -> Executables {
    executables_with_budget(root, ENTRY_BUDGET)
}
fn executables_with_budget(root: &Path, budget: usize) -> Executables {
    let mut result = Executables::default();
    if !unlinked(root) {
        return result;
    }
    let Ok(entries) = fs::read_dir(root) else { return result };
    for (i, entry) in entries.enumerate() {
        if i >= budget {
            return result;
        }
        let Ok(entry) = entry else { return result };
        let path = entry.path();
        if path.extension().is_some_and(|e| e.eq_ignore_ascii_case("exe"))
            && let Some(header) = pe_version::file_header(&path)
            && !header.dll
        {
            result
                .files
                .push((entry.file_name().to_string_lossy().into_owned(), header.machine));
        }
    }
    result.files.sort();
    result.complete = true;
    result
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AntiCheatFinding {
    pub product: &'static str,
    /// Relative to the scanned game installation, never a system-wide search.
    pub path: PathBuf,
}
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AntiCheatScan {
    pub findings: Vec<AntiCheatFinding>,
    /// False means some entries were unreadable, linked, beyond the depth limit or the scan budget.
    /// Even true with no findings is not evidence that anti-cheat is absent.
    pub complete: bool,
}
// Independently authored filename evidence, not copied from any fork's detection rules.
fn product(name: &str, directory: bool) -> Option<&'static str> {
    Some(match (name, directory) {
        ("easyanticheat" | "easyanticheat_eos", true)
        | (
            "easyanticheat_setup.exe"
            | "easyanticheat_eos_setup.exe"
            | "easyanticheat_x64.dll"
            | "easyanticheat_x86.dll",
            false,
        ) => "Easy Anti-Cheat",
        ("battleye", true) | ("beservice.exe" | "beservice_x64.exe" | "beclient.dll" | "beclient_x64.dll", false) => {
            "BattlEye"
        }
        ("eaanticheat", true)
        | (
            "eaanticheat.gameservicelauncher.exe" | "eaanticheat.gameservice.exe" | "eaanticheat.installer.exe",
            false,
        ) => "EA AntiCheat / Javelin",
        ("gameguard", true) | ("gameguard.des" | "gamemon.des" | "gamemon64.des", false) => "nProtect GameGuard",
        ("xigncode" | "xigncode3", true) | ("xhunter1.sys" | "x3.xem" | "xcoronahost.xem", false) => "XIGNCODE3",
        ("pb", true) | ("pbcl.dll" | "pbcl64.dll" | "pbsvc.exe" | "pnkbstra.exe" | "pnkbstrb.exe", false) => {
            "PunkBuster"
        }
        ("anticheatexpert", true) | ("aceservice.exe" | "ace-base.sys" | "ace-base64.sys", false) => {
            "Anti-Cheat Expert"
        }
        _ => return None,
    })
}
pub fn anti_cheat(root: &Path) -> AntiCheatScan {
    anti_cheat_with_budget(root, ENTRY_BUDGET)
}
fn anti_cheat_with_budget(root: &Path, budget: usize) -> AntiCheatScan {
    let mut result = AntiCheatScan::default();
    if !unlinked(root) {
        return result;
    }
    result.complete = true;
    let mut pending = vec![(root.to_owned(), 0)];
    let mut visited = 0;
    while let Some((dir, depth)) = pending.pop() {
        if !unlinked(&dir) {
            result.complete = false;
            continue;
        }
        let Ok(entries) = fs::read_dir(&dir) else {
            result.complete = false;
            continue;
        };
        for entry in entries {
            if visited == budget {
                result.complete = false;
                return result;
            }
            visited += 1;
            let Ok(entry) = entry else {
                result.complete = false;
                continue;
            };
            let path = entry.path();
            if !unlinked(&path) {
                result.complete = false;
                continue;
            }
            let Ok(kind) = entry.file_type() else {
                result.complete = false;
                continue;
            };
            if !kind.is_dir() && !kind.is_file() {
                continue;
            }
            if let Some(product) = product(&entry.file_name().to_string_lossy().to_ascii_lowercase(), kind.is_dir()) {
                result.findings.push(AntiCheatFinding {
                    product,
                    path: path.strip_prefix(root).unwrap().to_owned(),
                });
            }
            if kind.is_dir() {
                if depth < MAX_DEPTH {
                    pending.push((path, depth + 1));
                } else {
                    result.complete = false;
                }
            }
        }
    }
    result.findings.sort_by(|a, b| a.path.cmp(&b.path));
    result
}
pub const ANTI_CHEAT_NOTE: &str = "Anti-cheat evidence";
pub fn anti_cheat_note(f: &AntiCheatFinding) -> String {
    format!(
        "{ANTI_CHEAT_NOTE}: {} ({}). A proxy DLL may trigger anti-cheat and an account ban in online play. Retoque cannot tell whether this game's anti-cheat allows it.",
        f.product,
        f.path.display()
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    fn pe(machine: u16) -> Vec<u8> {
        let mut b = vec![0u8; 90];
        b[..2].copy_from_slice(b"MZ");
        b[60] = 64;
        b[64..68].copy_from_slice(b"PE\0\0");
        b[68..70].copy_from_slice(&machine.to_le_bytes());
        b[84] = 2;
        b[88..90].copy_from_slice(&(if machine == 0x14c { 0x10bu16 } else { 0x20b }).to_le_bytes());
        b
    }
    #[test]
    fn architecture_is_evidence_not_a_guess() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path();
        assert!(!executables(p).x86_only());
        fs::write(p.join("junk.exe"), b"MZ unknown").unwrap();
        assert!(executables(p).files.is_empty());
        fs::write(p.join("a.EXE"), pe(0x14c)).unwrap();
        assert!(executables(p).x86_only());
        fs::write(p.join("b.exe"), pe(0x8664)).unwrap();
        assert!(!executables(p).x86_only());
        assert_eq!(executables(p).names(Machine::X86), ["a.EXE"]);
        fs::write(p.join("b.exe"), pe(0xaa64)).unwrap();
        assert!(!executables(p).x86_only());
        fs::write(p.join("b.exe"), pe(0x9999)).unwrap();
        assert!(!executables(p).x86_only());
        assert!(!executables_with_budget(p, 1).x86_only());
    }
    #[test]
    fn malformed_headers_and_large_files_have_bounded_reads() {
        use std::io::{Cursor, Read, Seek, SeekFrom};
        struct Count {
            inner: Cursor<Vec<u8>>,
            read: usize,
        }
        impl Read for Count {
            fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
                let n = self.inner.read(out)?;
                self.read += n;
                Ok(n)
            }
        }
        impl Seek for Count {
            fn seek(&mut self, p: SeekFrom) -> std::io::Result<u64> {
                self.inner.seek(p)
            }
        }
        let mut r = Count {
            inner: Cursor::new(pe(0x8664)),
            read: 0,
        };
        assert_eq!(pe_version::read_header(&mut r).unwrap().machine, Machine::X64);
        assert_eq!(r.read, 90);
        let mut b = pe(0x14c);
        b[60..64].copy_from_slice(&u32::MAX.to_le_bytes());
        let mut r = Count {
            inner: Cursor::new(b),
            read: 0,
        };
        assert!(pe_version::read_header(&mut r).is_none());
        assert_eq!(r.read, 64);
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("large.exe");
        fs::write(&path, pe(0x8664)).unwrap();
        fs::OpenOptions::new()
            .write(true)
            .open(&path)
            .unwrap()
            .set_len(1024 * 1024 * 1024)
            .unwrap();
        assert_eq!(pe_version::file_header(&path).unwrap().machine, Machine::X64);
        let mut dll = pe(0x14c);
        dll[87] = 0x20;
        fs::write(&path, dll).unwrap();
        assert!(executables(dir.path()).files.is_empty());
    }
    #[cfg(unix)]
    #[test]
    fn unreadable_evidence_is_unknown_and_unreadable_directory_is_incomplete() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("game.exe");
        fs::write(&path, pe(0x14c)).unwrap();
        fs::set_permissions(&path, fs::Permissions::from_mode(0o0)).unwrap();
        // Privileged test runners may still read it; exercise the permission failure on ordinary hosts.
        if fs::File::open(&path).is_err() {
            assert!(!executables(dir.path()).x86_only());
        }
        fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
        let hidden = dir.path().join("unreadable");
        fs::create_dir(&hidden).unwrap();
        fs::set_permissions(&hidden, fs::Permissions::from_mode(0o0)).unwrap();
        if fs::read_dir(&hidden).is_err() {
            assert!(!anti_cheat(dir.path()).complete);
        }
        fs::set_permissions(&hidden, fs::Permissions::from_mode(0o700)).unwrap();
    }
    #[test]
    fn each_family_has_named_relative_evidence() {
        for (name, family) in [
            ("EasyAntiCheat", "Easy Anti-Cheat"),
            ("BattlEye", "BattlEye"),
            ("EAAntiCheat", "EA AntiCheat / Javelin"),
            ("GameGuard", "nProtect GameGuard"),
            ("XIGNCODE3", "XIGNCODE3"),
            ("pb", "PunkBuster"),
            ("AntiCheatExpert", "Anti-Cheat Expert"),
        ] {
            let d = tempfile::tempdir().unwrap();
            fs::create_dir(d.path().join(name)).unwrap();
            let scan = anti_cheat(d.path());
            assert!(scan.complete);
            assert_eq!(
                scan.findings,
                [AntiCheatFinding {
                    product: family,
                    path: name.into()
                }]
            );
        }
        let d = tempfile::tempdir().unwrap();
        fs::write(d.path().join("BEClient_x64.dll"), b"").unwrap();
        assert_eq!(anti_cheat(d.path()).findings[0].product, "BattlEye");
        assert!(!anti_cheat_with_budget(d.path(), 0).complete);
        let deep = d.path().join("a/b/c/d/e/f");
        fs::create_dir_all(&deep).unwrap();
        fs::write(deep.join("pbcl.dll"), b"").unwrap();
        assert!(!anti_cheat(d.path()).complete);
        assert_eq!(anti_cheat(d.path()).findings.len(), 1);
    }
    #[cfg(unix)]
    #[test]
    fn links_never_supply_architecture_or_anti_cheat_evidence() {
        let d = tempfile::tempdir().unwrap();
        let outside = tempfile::tempdir().unwrap();
        fs::write(outside.path().join("real.exe"), pe(0x14c)).unwrap();
        std::os::unix::fs::symlink(outside.path().join("real.exe"), d.path().join("game.exe")).unwrap();
        std::os::unix::fs::symlink(outside.path(), d.path().join("BattlEye")).unwrap();
        assert!(executables(d.path()).files.is_empty());
        let scan = anti_cheat(d.path());
        assert!(!scan.complete);
        assert!(scan.findings.is_empty());
    }
}
