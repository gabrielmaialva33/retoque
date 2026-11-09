//! Read-only adapter inventory. Detection never interprets a GPU name as proof that a game route works.

use serde::{Deserialize, Serialize};
use std::io::{Read, Seek, SeekFrom};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Vendor {
    Nvidia,
    Amd,
    Intel,
    Other,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub enum Arch {
    Turing,
    Ampere,
    Ada,
    Blackwell,
    OlderNvidia,
    NotNvidia,
    Unknown,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
pub struct Gpu {
    pub name: String,
    pub vendor: Vendor,
    pub arch: Arch,
    pub compute_cap: Option<(u32, u32)>,
    pub driver: Option<String>,
    pub vram_mib: Option<u64>,
}

fn architecture(cap: Option<(u32, u32)>) -> Arch {
    match cap {
        Some((7, 5)) => Arch::Turing,
        Some((8, 0 | 6 | 7)) => Arch::Ampere,
        Some((8, 9)) => Arch::Ada,
        Some((10, _) | (12, _)) => Arch::Blackwell,
        Some((major, _)) if major < 8 => Arch::OlderNvidia,
        _ => Arch::Unknown,
    }
}

// nvidia-smi uses CSV quoting for names containing a comma. Do not shift columns on malformed rows.
fn csv_row(line: &str) -> Option<Vec<String>> {
    let mut values = Vec::new();
    let mut value = String::new();
    let mut quoted = false;
    let mut chars = line.chars().peekable();
    while let Some(ch) = chars.next() {
        match ch {
            '"' if quoted && chars.peek() == Some(&'"') => {
                value.push('"');
                chars.next();
            }
            '"' => quoted = !quoted,
            ',' if !quoted => {
                values.push(value.trim().to_owned());
                value.clear();
            }
            _ => value.push(ch),
        }
    }
    if quoted {
        return None;
    }
    values.push(value.trim().to_owned());
    Some(values)
}

/// Parse `name,compute_cap,driver_version,memory.total`, queried with `csv,noheader,nounits`.
/// Unavailable numeric fields remain None, never a made-up architecture or memory size.
pub fn parse_nvidia_smi_csv(text: &str) -> Vec<Gpu> {
    text.lines()
        .take(64)
        .filter_map(|line| {
            if line.len() > 4096 {
                return None;
            }
            let row = csv_row(line)?;
            if row.len() != 4 || row[0].is_empty() || row[0].eq_ignore_ascii_case("name") {
                return None;
            }
            let compute_cap = row[1]
                .split_once('.')
                .and_then(|(major, minor)| Some((major.parse().ok()?, minor.parse().ok()?)));
            let driver =
                (!row[2].is_empty() && row[2].bytes().all(|c| c.is_ascii_digit() || c == b'.')).then(|| row[2].clone());
            Some(Gpu {
                name: row[0].clone(),
                vendor: Vendor::Nvidia,
                arch: architecture(compute_cap),
                compute_cap,
                driver,
                vram_mib: row[3].strip_suffix(" MiB").unwrap_or(&row[3]).trim().parse().ok(),
            })
        })
        .collect()
}

fn run_smi(program: &std::path::Path, timeout: Duration) -> Option<String> {
    // A file avoids blocking on a full stdout pipe. Only a bounded result is read; no shell is involved.
    let mut output = tempfile::tempfile().ok()?;
    let mut command = Command::new(program);
    command
        .args([
            "--query-gpu=name,compute_cap,driver_version,memory.total",
            "--format=csv,noheader,nounits",
        ])
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .stdout(output.try_clone().ok()?);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }
    let mut child = command.spawn().ok()?;
    let deadline = Instant::now() + timeout;
    loop {
        match child.try_wait() {
            Ok(Some(status)) => {
                if !status.success() {
                    return None;
                }
                break;
            }
            Ok(None) if Instant::now() < deadline => std::thread::sleep(Duration::from_millis(10)),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        }
    }
    output.seek(SeekFrom::Start(0)).ok()?;
    let mut text = String::new();
    output.take(262145).read_to_string(&mut text).ok()?;
    (text.len() <= 262144).then_some(text)
}

pub fn detect() -> Vec<Gpu> {
    let program = {
        #[cfg(windows)]
        {
            let system = std::path::PathBuf::from(r"C:\Windows\System32\nvidia-smi.exe");
            if system.is_file() { system } else { "nvidia-smi".into() }
        }
        #[cfg(not(windows))]
        {
            std::path::PathBuf::from("nvidia-smi")
        }
    };
    let mut gpus = run_smi(&program, Duration::from_secs(3))
        .map(|s| parse_nvidia_smi_csv(&s))
        .unwrap_or_default();
    for gpu in platform_adapters() {
        // NVIDIA's richer inventory wins. PCI/registry names are only a fallback.
        if gpu.vendor == Vendor::Nvidia && gpus.iter().any(|g| g.vendor == Vendor::Nvidia) {
            continue;
        }
        if !gpus.iter().any(|g| g.name == gpu.name && g.vendor == gpu.vendor) {
            gpus.push(gpu);
        }
    }
    gpus.sort_by_key(|g| g.vendor != Vendor::Nvidia);
    gpus
}

fn fallback(name: String, vendor: Vendor) -> Gpu {
    Gpu {
        name,
        vendor,
        arch: if vendor == Vendor::Nvidia {
            Arch::Unknown
        } else {
            Arch::NotNvidia
        },
        compute_cap: None,
        driver: None,
        vram_mib: None,
    }
}

#[cfg(target_os = "linux")]
fn platform_adapters() -> Vec<Gpu> {
    let Ok(entries) = std::fs::read_dir("/sys/class/drm") else {
        return Vec::new();
    };
    entries
        .flatten()
        .filter_map(|entry| {
            let name = entry.file_name();
            let suffix = name.to_str()?.strip_prefix("card")?;
            if suffix.is_empty() || !suffix.bytes().all(|c| c.is_ascii_digit()) {
                return None;
            }
            let vendor = std::fs::read_to_string(entry.path().join("device/vendor")).ok()?;
            let (vendor, name) = match vendor.trim() {
                "0x10de" => (Vendor::Nvidia, "NVIDIA GPU"),
                "0x1002" => (Vendor::Amd, "AMD GPU"),
                "0x8086" => (Vendor::Intel, "Intel GPU"),
                _ => (Vendor::Other, "Display adapter"),
            };
            Some(fallback(name.into(), vendor))
        })
        .collect()
}
#[cfg(windows)]
fn platform_adapters() -> Vec<Gpu> {
    use winreg::{RegKey, enums::HKEY_LOCAL_MACHINE};
    let Ok(video) = RegKey::predef(HKEY_LOCAL_MACHINE).open_subkey(r"SYSTEM\CurrentControlSet\Control\Video") else {
        return Vec::new();
    };
    video
        .enum_keys()
        .flatten()
        .filter_map(|key| {
            let adapter = video.open_subkey(format!(r"{key}\0000")).ok()?;
            let name: String = adapter.get_value("DriverDesc").ok()?;
            let id: String = adapter.get_value("MatchingDeviceId").unwrap_or_default();
            let id = id.to_ascii_lowercase();
            let vendor = if id.contains("ven_10de") {
                Vendor::Nvidia
            } else if id.contains("ven_1002") {
                Vendor::Amd
            } else if id.contains("ven_8086") {
                Vendor::Intel
            } else {
                Vendor::Other
            };
            Some(fallback(name, vendor))
        })
        .collect()
}
#[cfg(not(any(target_os = "linux", windows)))]
fn platform_adapters() -> Vec<Gpu> {
    Vec::new()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn csv_adapters_unavailable_fields_and_architectures() {
        for (cap, arch) in [
            ("7.5", Arch::Turing),
            ("8.6", Arch::Ampere),
            ("8.9", Arch::Ada),
            ("12.0", Arch::Blackwell),
            ("6.1", Arch::OlderNvidia),
            ("9.0", Arch::Unknown),
            ("[N/A]", Arch::Unknown),
        ] {
            let g = parse_nvidia_smi_csv(&format!("\"NVIDIA, Synthetic\", {cap}, 617.14, 12288\n"));
            assert_eq!(g[0].arch, arch);
            assert_eq!(g[0].name, "NVIDIA, Synthetic");
            assert_eq!(g[0].vram_mib, Some(12288));
        }
        assert_eq!(parse_nvidia_smi_csv("GPU, N/A, N/A, N/A")[0].vram_mib, None);
        assert!(
            parse_nvidia_smi_csv("broken\n\"unclosed, 8.6, 1, 2\nname, compute_cap, driver_version, memory.total")
                .is_empty()
        );
        assert_eq!(
            parse_nvidia_smi_csv("GPU A,8.6,617.14,12288\nGPU B,8.9,617.14,24576").len(),
            2
        );
    }
    #[cfg(unix)]
    #[test]
    fn timeout_reaps_a_nonresponding_query() {
        use std::os::unix::fs::PermissionsExt;
        let dir = tempfile::tempdir().unwrap();
        let script = dir.path().join("fake-smi");
        std::fs::write(&script, "#!/bin/sh\nexec sleep 10\n").unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o700)).unwrap();
        let start = Instant::now();
        assert!(run_smi(&script, Duration::from_millis(50)).is_none());
        assert!(start.elapsed() < Duration::from_secs(1));
    }
}
