//! Bounded-memory summaries and manually shared diagnostic archives. Coverage is a CPU observation, not proof
//! of GPU completion; only the explicitly confirmed timing records contribute to the timing mean.

use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::time::SystemTime;

const MAX_LINE: usize = 16 * 1024;
const MAX_HIGHLIGHTS: usize = 200;
const MAX_ERRORS: usize = 100;

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Coverage {
    pub stage: String,
    pub calls: u64,
    pub model_ok: u64,
    pub model_failed: u64,
    pub skipped: u64,
    pub fallback: u64,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Summary {
    pub path: PathBuf,
    pub dlss_version: Option<String>,
    /// An API explicitly named in an NGX init/evaluate or API record; hooks alone do not identify the game API.
    pub api: Option<String>,
    /// Latest NR route/stage observation, preserving the log's wording (including refusal).
    pub nr_route: Option<String>,
    /// At least one successful model initialization or evaluate observed; not current running state.
    pub nr_initialized: bool,
    /// Successful evaluate observed on CPU; not proof of GPU completion.
    pub nr_evaluated: bool,
    /// First evidence per diagnostic rule, independent of highlight/error quotas.
    pub diagnostic_lines: Vec<String>,
    /// First 100 actionable problem lines; routine Streamline callbacks do not consume this quota.
    pub errors: Vec<String>,
    pub warnings: usize,
    /// Mean dispatch_ms over all confirmed samples, including carried frames (Python comparison semantics).
    pub nr_gpu_ms: Option<f64>,
    /// First 200 interesting lines, in file order. Latest coverage/route remain available separately.
    pub highlights: Vec<String>,
    pub coverage: Option<Coverage>,
    pub error_count: usize,
    /// Routine error-level Streamline callbacks, counted separately from actionable errors.
    pub streamline_noise_count: usize,
    pub timing_samples: u64,
    pub highlights_omitted: usize,
    /// Lines longer than 16 KiB are drained without retaining the tail; their prefix is still parsed.
    pub truncated_lines: usize,
    /// Frame rates and signals of the session this log covers (`session.rs`); None without status lines.
    pub pacing: Option<crate::session::Pacing>,
}

fn log_name(name: &str, stem: &str) -> bool {
    let name = name.to_ascii_lowercase();
    let stem = stem.to_ascii_lowercase();
    if name == format!("{stem}.log") || name == format!("{stem}.previous.log") {
        return true;
    }
    let Some(rest) = name.strip_prefix(&format!("{stem}_")) else {
        return false;
    };
    let Some(number) = rest.strip_suffix(".previous.log").or_else(|| rest.strip_suffix(".log")) else {
        return false;
    };
    !number.is_empty() && number.bytes().all(|b| b.is_ascii_digit())
}

fn configured_log(target: &Path) -> Option<PathBuf> {
    let mut ini = String::new();
    File::open(target.join("OptiScaler.ini"))
        .ok()?
        .take(1024 * 1024)
        .read_to_string(&mut ini)
        .ok()?;
    let mut in_log = false;
    let mut value = None;
    for line in ini.lines().map(|s| s.trim().trim_start_matches('\u{feff}')) {
        if line.starts_with(';') || line.starts_with('#') {
            continue;
        }
        if let Some(section) = line.strip_prefix('[').and_then(|s| s.strip_suffix(']')) {
            in_log = section.eq_ignore_ascii_case("Log");
        } else if in_log
            && let Some((key, v)) = line.split_once('=')
            && key.trim().eq_ignore_ascii_case("LogFileName")
        {
            value = Some(v.trim().trim_matches('"'));
        }
    }
    let value = value.filter(|s| !s.is_empty() && !s.eq_ignore_ascii_case("auto"))?;
    let normalized = value.replace('\\', "/");
    let path = if cfg!(windows) {
        PathBuf::from(&normalized)
    } else if normalized.get(..3).is_some_and(|s| s.eq_ignore_ascii_case("z:/")) {
        PathBuf::from(&normalized[2..])
    } else if normalized.starts_with("//") || normalized.as_bytes().get(1) == Some(&b':') {
        // Other Wine drives need a prefix/dosdevices mapping that this API does not receive.
        return None;
    } else {
        PathBuf::from(&normalized)
    };
    let stem = path.file_stem()?.to_str()?;
    // Config.cpp intentionally drops the directory of a relative LogFileName.
    let dir = if path.is_absolute() { path.parent()? } else { target };
    Some(dir.join(format!("{stem}.log")))
}

/// Default and configured logs, including .previous and numeric session suffixes, newest first.
pub fn find(target_dir: &Path) -> Vec<PathBuf> {
    let mut bases = vec![target_dir.join("OptiScaler.log")];
    if let Some(path) = configured_log(target_dir) {
        bases.push(path);
    }
    let mut files = Vec::new();
    for base in bases {
        let (Some(dir), Some(stem)) = (base.parent(), base.file_stem().and_then(|s| s.to_str())) else {
            continue;
        };
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        for entry in entries.flatten() {
            if !entry.file_name().to_str().is_some_and(|name| log_name(name, stem)) {
                continue;
            }
            let Ok(metadata) = entry.path().symlink_metadata() else {
                continue;
            };
            if metadata.is_file() {
                files.push((metadata.modified().unwrap_or(SystemTime::UNIX_EPOCH), entry.path()));
            }
        }
    }
    files.sort_by(|a, b| b.0.cmp(&a.0).then_with(|| a.1.cmp(&b.1)));
    files.dedup_by(|a, b| a.1 == b.1);
    files.into_iter().map(|(_, path)| path).collect()
}

// Unlike BufRead::lines, this cannot allocate the size of a malformed unbroken log line.
fn bounded_line(reader: &mut impl BufRead, line: &mut Vec<u8>) -> io::Result<Option<bool>> {
    line.clear();
    let mut seen = false;
    let mut truncated = false;
    loop {
        let bytes = reader.fill_buf()?;
        if bytes.is_empty() {
            return Ok(seen.then_some(truncated));
        }
        seen = true;
        let newline = bytes.iter().position(|b| *b == b'\n');
        let consumed = newline.map_or(bytes.len(), |i| i + 1);
        let retained = consumed.min(MAX_LINE - line.len());
        line.extend_from_slice(&bytes[..retained]);
        truncated |= retained < consumed;
        reader.consume(consumed);
        if newline.is_some() {
            return Ok(Some(truncated));
        }
    }
}

fn field<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    line.split_whitespace()
        .find_map(|s| s.split_once('=').filter(|(k, _)| *k == key).map(|(_, v)| v))
}

fn coverage(line: &str) -> Option<Coverage> {
    Some(Coverage {
        stage: field(line, "stage")?.into(),
        calls: field(line, "calls")?.parse().ok()?,
        model_ok: field(line, "model_ok")?.parse().ok()?,
        model_failed: field(line, "model_failed")?.parse().ok()?,
        skipped: field(line, "skipped")?.parse().ok()?,
        fallback: field(line, "fallback")?.parse().ok()?,
    })
}

fn api(line: &str) -> Option<&'static str> {
    for (name, markers) in [
        (
            "D3D11",
            [
                "NVSDK_NGX_D3D11_Init",
                "NVSDK_NGX_D3D11_Evaluate",
                "NVNGXProxy::D3D11_Init",
                "API: D3D11",
            ],
        ),
        (
            "D3D12",
            [
                "NVSDK_NGX_D3D12_Init",
                "NVSDK_NGX_D3D12_Evaluate",
                "NVNGXProxy::D3D12_Init",
                "API: D3D12",
            ],
        ),
        (
            "Vulkan",
            [
                "NVSDK_NGX_VULKAN_Init",
                "NVSDK_NGX_VULKAN_Evaluate",
                "NVNGXProxy::VULKAN_Init",
                "API: Vulkan",
            ],
        ),
    ] {
        if markers.iter().any(|m| line.contains(m)) {
            return Some(name);
        }
    }
    None
}

pub fn summarize(log: &Path) -> io::Result<Summary> {
    let mut result = Summary {
        path: log.to_owned(),
        ..Summary::default()
    };
    let mut reader = BufReader::new(File::open(log)?);
    let mut bytes = Vec::new();
    let mut mean = 0.0;
    let mut pacing = crate::session::PacingBuilder::default();
    while let Some(truncated) = bounded_line(&mut reader, &mut bytes)? {
        result.truncated_lines += usize::from(truncated);
        let decoded = String::from_utf8_lossy(&bytes);
        let line = decoded.trim();
        pacing.observe(line);
        for kind in signal_kinds(line) {
            if !result
                .diagnostic_lines
                .iter()
                .any(|old| signal_kinds(old).contains(&kind))
            {
                result.diagnostic_lines.push(line.to_owned());
            }
        }
        let lower = line.to_ascii_lowercase();
        let severe = ["faulted", "device removed", "device_removed", "hung", "0x887a"]
            .iter()
            .any(|s| lower.contains(s));
        let error_level = line.contains("[E]") || line.contains("\tE\t");
        let noise = error_level && line.contains("streamlineLogCallback") && !severe;
        result.streamline_noise_count += usize::from(noise);
        let is_error = severe || (error_level && !noise);
        if is_error {
            result.error_count += 1;
            if result.errors.len() < MAX_ERRORS {
                result.errors.push(line.to_owned());
            }
        }
        if line.contains("[W]") || line.contains("\tW\t") {
            result.warnings += 1;
        }
        let mut interesting = is_error;
        if let Some((_, rest)) = line.split_once("DLSS v") {
            let version: String = rest.chars().take_while(|c| c.is_ascii_digit() || *c == '.').collect();
            if version.split('.').count() >= 3 && version.split('.').all(|part| !part.is_empty()) {
                result.dlss_version = Some(version);
                interesting = true;
            }
        }
        if let Some(name) = api(line) {
            result.api = Some(name.into());
            interesting = true;
        }
        if let Some((_, nr)) = line.split_once("DLSS-NR") {
            if nr.starts_with(" coverage stage=") {
                if let Some(cov) = coverage(nr) {
                    result.nr_initialized |= cov.model_ok > 0;
                    result.nr_evaluated |= cov.model_ok > 0;
                    result.coverage = Some(cov);
                }
                result.nr_route = Some(format!("DLSS-NR{nr}"));
                interesting = true;
            }
            if nr.contains("the model initialised on this device")
                || nr.contains("the model initialised on device ")
                || nr.starts_with(" (proxy): feature created at ")
                || nr.starts_with(" running after ")
                || nr.starts_with(" running before upscale:")
                || nr.starts_with(" Vulkan: feature up at ")
                || nr.starts_with(" Vulkan: running natively at ")
            {
                result.nr_initialized = true;
                interesting = true;
            }
            if nr.starts_with(" gpu timing confirmed:") {
                if let (Some(stage), Some(carried), Some(dispatch), Some(model)) = (
                    field(nr, "stage"),
                    field(nr, "carried"),
                    field(nr, "dispatch_ms"),
                    field(nr, "model_ms"),
                ) && !stage.is_empty()
                    && ["true", "false", "0", "1"].contains(&carried)
                    && let (Ok(dispatch), Ok(model)) = (dispatch.parse::<f64>(), model.parse::<f64>())
                    && dispatch.is_finite()
                    && model.is_finite()
                    && dispatch >= 0.0
                    && model >= 0.0
                {
                    result.nr_evaluated = true;
                    result.timing_samples += 1;
                    mean += (dispatch - mean) / result.timing_samples as f64;
                    result.nr_initialized = true;
                }
            } else if [
                "route",
                "stage",
                "present host",
                "before the upscaler",
                "HookMethod",
                "native D3D11 guides",
                " running ",
                "status:",
                "present: first pass",
                "present: the pass runs",
            ]
            .iter()
            .any(|s| nr.contains(s))
            {
                result.nr_route = Some(format!("DLSS-NR{nr}"));
                interesting = true;
            }
            if [
                "composition:",
                "cadence:",
                "periphery:",
                "enlargement:",
                "model initialised",
                "model loader",
                "forwarder loaded",
                "game profile:",
            ]
            .iter()
            .any(|s| nr.contains(s))
            {
                interesting = true;
            }
        }
        interesting |=
            line.contains("DLSSG override state:") || line.contains("BuildEnlarger") || line.contains("EnlargeEdit");
        if interesting {
            if result.highlights.len() < MAX_HIGHLIGHTS {
                result.highlights.push(line.to_owned());
            } else {
                result.highlights_omitted += 1;
            }
        }
    }
    result.nr_gpu_ms = (result.timing_samples > 0).then_some(mean);
    result.pacing = pacing.finish();
    Ok(result)
}

struct Redactor {
    replacements: Vec<(Vec<u8>, &'static [u8])>,
    overlap: usize,
    markers: Vec<(Vec<u8>, bool)>,
}

impl Redactor {
    fn new(homes: &[String], users: &[String]) -> Self {
        let mut replacements = Vec::new();
        for home in homes.iter().filter(|s| !s.is_empty() && s.len() > 1) {
            let slash = home.replace('\\', "/").trim_end_matches('/').to_owned();
            for spelling in [home.clone(), slash.clone(), slash.replace('/', "\\")] {
                replacements.push((spelling.as_bytes().to_vec(), b"~".as_slice()));
                replacements.push((spelling.replace('\\', "\\\\").into_bytes(), b"~".as_slice()));
                if slash.starts_with('/') {
                    replacements.push((format!("Z:{spelling}").into_bytes(), b"~".as_slice()));
                    replacements.push((
                        format!("Z:{spelling}").replace('\\', "\\\\").into_bytes(),
                        b"~".as_slice(),
                    ));
                }
            }
        }
        for user in users.iter().filter(|s| !s.is_empty()) {
            replacements.push((user.as_bytes().to_vec(), b"<user>".as_slice()));
        }
        replacements.sort_by_key(|(s, _)| std::cmp::Reverse(s.len()));
        replacements.dedup();
        let mut markers = Vec::new();
        for separator in ["/", r"\/", r"\", r"\\", r"\\\\"] {
            for component in ["home", "Users", "userdata"] {
                markers.push((
                    format!("{separator}{component}{separator}").into_bytes(),
                    component == "userdata",
                ));
            }
        }
        markers.sort_by_key(|(marker, _)| std::cmp::Reverse(marker.len()));
        let overlap = replacements
            .iter()
            .map(|(s, _)| s.len() + 1)
            .chain(markers.iter().map(|(s, _)| s.len() + 1))
            .max()
            .unwrap_or(1);
        Self {
            replacements,
            overlap,
            markers,
        }
    }

    fn environment() -> Self {
        let homes = ["HOME", "USERPROFILE"]
            .iter()
            .filter_map(|name| std::env::var(name).ok())
            .collect::<Vec<_>>();
        let users = ["USER", "USERNAME", "LOGNAME"]
            .iter()
            .filter_map(|name| std::env::var(name).ok())
            .collect::<Vec<_>>();
        Self::new(&homes, &users)
    }

    // Preserve enough unprocessed input that a personal path split at a read boundary still matches.
    fn copy(&self, mut input: impl Read, output: &mut impl Write) -> io::Result<()> {
        let mut block = [0; 65536];
        let mut pending = Vec::new();
        let mut previous = None;
        let mut hiding: Option<bool> = None;
        loop {
            let count = input.read(&mut block)?;
            pending.extend_from_slice(&block[..count]);
            let limit = if count == 0 {
                pending.len()
            } else {
                pending.len().saturating_sub(self.overlap)
            };
            let mut at = 0;
            let mut clean = Vec::with_capacity(limit);
            while at < limit {
                let byte = pending[at];
                if let Some(steam) = hiding {
                    if !component_end(byte) && (!steam || byte.is_ascii_digit()) {
                        previous = Some(byte);
                        at += 1;
                        continue;
                    }
                    hiding = None;
                }
                if let Some((source, replacement)) = self.replacements.iter().find(|(source, replacement)| {
                    let end = at + source.len();
                    let matches = pending
                        .get(at..end)
                        .is_some_and(|candidate| candidate.eq_ignore_ascii_case(source));
                    let after = pending.get(end).copied();
                    // Standalone usernames must not corrupt longer names or words. A home must end at a path
                    // component boundary, not match the prefix of another user's home.
                    matches
                        && if *replacement == b"<user>" {
                            previous.is_none_or(|b| !word_byte(b)) && after.is_none_or(|b| !word_byte(b))
                        } else {
                            after.is_none_or(component_end)
                        }
                }) {
                    clean.extend_from_slice(replacement);
                    at += source.len();
                    previous = pending.get(at - 1).copied();
                } else if let Some((marker, steam)) = self.markers.iter().find(|(marker, steam)| {
                    let end = at + marker.len();
                    pending.get(at..end).is_some_and(|s| s.eq_ignore_ascii_case(marker))
                        && pending
                            .get(end)
                            .is_some_and(|b| !component_end(*b) && (!*steam || b.is_ascii_digit()))
                }) {
                    clean.extend_from_slice(&pending[at..at + marker.len()]);
                    clean.extend_from_slice(if *steam { b"<steam-id>" } else { b"<user>" });
                    at += marker.len();
                    previous = pending.get(at - 1).copied();
                    hiding = Some(*steam);
                } else {
                    clean.push(byte);
                    previous = Some(byte);
                    at += 1;
                }
            }
            output.write_all(&clean)?;
            pending.drain(..at);
            if count == 0 {
                return Ok(());
            }
        }
    }
}

fn word_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte >= 128 || matches!(byte, b'_' | b'-' | b'.')
}

fn component_end(byte: u8) -> bool {
    matches!(
        byte,
        b'/' | b'\\' | b'"' | b'\'' | b'\r' | b'\n' | b'\t' | b'<' | b'>' | b'{' | b'}'
    )
}

/// Writes only known diagnostic members to a unique zip. Missing inputs are omitted, read errors abort.
/// Redacts local identities and recognized Unix/Windows/Wine home and Steam userdata components in every member.
/// This is not a general secret detector.
pub fn bundle(target_dir: &Path, status: &str, out_dir: &Path) -> io::Result<PathBuf> {
    bundle_with_redactor(target_dir, status, out_dir, &Redactor::environment())
}

fn bundle_with_redactor(target: &Path, status: &str, out: &Path, redactor: &Redactor) -> io::Result<PathBuf> {
    fs::create_dir_all(out)?;
    let mut temporary = tempfile::Builder::new()
        .prefix("retoque-diagnostics-")
        .suffix(".zip")
        .tempfile_in(out)?;
    let mut archive = zip::ZipWriter::new(temporary.as_file_mut());
    let options = zip::write::SimpleFileOptions::default()
        .compression_method(zip::CompressionMethod::Deflated)
        .large_file(true)
        .unix_permissions(0o600);
    let mut members = vec![
        (target.join("OptiScaler.ini"), "OptiScaler.ini"),
        (target.join(".retoque/receipt.txt"), ".retoque/receipt.txt"),
    ];
    if let Some(log) = find(target).into_iter().next() {
        members.insert(0, (log, "OptiScaler.log"));
    }
    for (path, name) in members {
        let metadata = match fs::symlink_metadata(&path) {
            Ok(metadata) => metadata,
            Err(e) if e.kind() == io::ErrorKind::NotFound => continue,
            Err(e) => return Err(e),
        };
        if !metadata.file_type().is_file() {
            return Err(io::Error::other("diagnostic input is not a regular file"));
        }
        archive.start_file(name, options)?;
        redactor.copy(File::open(path)?, &mut archive)?;
    }
    archive.start_file("status.txt", options)?;
    redactor.copy(status.as_bytes(), &mut archive)?;
    archive.finish()?.sync_all()?;
    let (_, path) = temporary.keep().map_err(|e| e.error)?;
    Ok(path)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn review_bundle_redacts_foreign_identities_in_every_member_and_block_split() {
        let temp = tempfile::tempdir().unwrap();
        let target = temp.path().join("game");
        fs::create_dir_all(target.join(".retoque")).unwrap();
        for offset in 65510..65540 {
            let sample = format!(
                "{} /home/other-synthetic-user/model.dll C:\\Users\\OtherSyntheticUser\\model.dll C:\\\\Users\\\\JsonSyntheticUser\\\\file /pfx/drive_c/users/WineSyntheticUser/file /Steam/userdata/76561198000000001/profile /Users/MacSyntheticUser/file", // privacy: allow (synthetic test path)
                "x".repeat(offset)
            );
            for name in ["OptiScaler.log", "OptiScaler.ini", ".retoque/receipt.txt"] {
                fs::write(target.join(name), &sample).unwrap();
            }
            let archive =
                bundle_with_redactor(&target, &sample, &temp.path().join("out"), &Redactor::new(&[], &[])).unwrap();
            let mut zip = zip::ZipArchive::new(File::open(archive).unwrap()).unwrap();
            for index in 0..zip.len() {
                let mut text = String::new();
                zip.by_index(index).unwrap().read_to_string(&mut text).unwrap();
                for private in [
                    "other-synthetic-user",
                    "OtherSyntheticUser",
                    "JsonSyntheticUser",
                    "WineSyntheticUser",
                    "76561198000000001",
                    "MacSyntheticUser",
                ] {
                    assert!(!text.contains(private), "identifier survives in archive member {index}");
                }
                assert!(text.contains("/model.dll") && text.contains("/profile"));
            }
        }
    }

    #[test]
    fn review_streamline_noise_cannot_hide_real_or_severe_errors() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("log");
        let text = "[E] streamlineLogCallback optional plugin unavailable\n".repeat(851)
            + "[E] real model failure\n[E] actual driver failure\n[E] streamlineLogCallback DEVICE_REMOVED 0x887A0005\n";
        fs::write(&path, text).unwrap();
        let summary = summarize(&path).unwrap();
        assert!(summary.errors[0].contains("real model failure"));
        assert!(summary.errors[1].contains("actual driver failure"));
        assert!(summary.errors[2].contains("DEVICE_REMOVED"));
        assert_eq!(summary.error_count, 3);
        assert_eq!(summary.streamline_noise_count, 851);
        assert!(summary.highlights.iter().any(|s| s.contains("real model failure")));
    }

    #[test]
    fn finds_real_configured_names_and_previous_sessions() {
        let temp = tempfile::tempdir().unwrap();
        let target = temp.path();
        fs::write(
            target.join("OptiScaler.ini"),
            "[Log]\nLogFileName=old.log\nLogFileName=ignored\\Custom.txt\nSingleFile=false\n",
        )
        .unwrap();
        for name in [
            "OptiScaler.log",
            "OptiScaler.previous.log",
            "Custom.log",
            "Custom_12.log",
            "Custom_12.previous.log",
            "Custom.previous.log",
            "Custom_bad.log",
            "other.log",
        ] {
            fs::write(target.join(name), b"log").unwrap();
        }
        let latest = target.join("Custom_12.log");
        File::open(&latest)
            .unwrap()
            .set_modified(SystemTime::now() + std::time::Duration::from_secs(60))
            .unwrap();
        let found = find(target);
        assert_eq!(found.len(), 6);
        assert_eq!(found[0], latest);
        fs::write(
            target.join("OptiScaler.ini"),
            format!("[Log]\nLogFileName={}\n", target.join("Custom.txt").display()),
        )
        .unwrap();
        assert_eq!(find(target).len(), 6);
        assert!(find(&target.join("missing")).is_empty());
    }

    #[test]
    fn summarizes_evidence_and_last_coverage() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("log");
        fs::write(&path, concat!(
            "[12:00:00] [I] ReadVersion DLSS v310.9.0 loaded.\n",
            "[12:00:01] [I] NVSDK_NGX_VULKAN_Init_Ext called\n",
            "[12:00:01] [I] DLSS-NR Vulkan: the model initialised on this device\n",
            "[12:00:01] [I] DLSS-NR coverage stage=post calls=3 model_ok=2 model_failed=0 applied_recorded=2 skipped=1 fallback=0\n",
            "[12:00:02] [W] DLSS-NR Vulkan: this route waits: ownership\n",
            "[12:00:03] [E] failed synthetic call\n",
            "12:00:03\tW\twarning\n",
            "12:00:03\tE\tstreamlineLogCallback failed\n",
            "[12:00:04] [I] DLSS-NR gpu timing pending: dispatch_ms=500\n",
            "[12:00:05] [I] DLSS-NR gpu timing confirmed: stage=post carried=false dispatch_ms=2 model_ms=1\n",
            "[12:00:06] [I] DLSS-NR gpu timing confirmed: stage=post carried=true dispatch_ms=4 model_ms=0\n",
            "[12:00:07] [I] DLSS-NR gpu timing confirmed: stage=post carried=false dispatch_ms=NaN model_ms=1\n",
            "[12:00:08] [I] DLSS-NR coverage stage=pre calls=9 model_ok=7 model_failed=1 skipped=1 fallback=0\n"
        )).unwrap();
        let s = summarize(&path).unwrap();
        assert_eq!(s.dlss_version.as_deref(), Some("310.9.0"));
        assert_eq!(s.api.as_deref(), Some("Vulkan"));
        assert!(s.nr_initialized);
        assert_eq!(s.error_count, 1);
        assert_eq!(s.errors.len(), 1);
        assert_eq!(s.streamline_noise_count, 1);
        assert_eq!(s.warnings, 2);
        assert_eq!(s.nr_gpu_ms, Some(3.0));
        assert_eq!(s.timing_samples, 2);
        assert_eq!(s.coverage.as_ref().unwrap().calls, 9);
        assert_eq!(s.coverage.as_ref().unwrap().stage, "pre");
        assert!(s.nr_route.unwrap().contains("stage=pre"));
    }

    #[test]
    fn missing_or_refused_initialization_is_not_success() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("log");
        fs::write(&path, "[I] DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask\n[E] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 0\n[I] Vulkan hook installed\n").unwrap();
        assert!(!summarize(&path).unwrap().nr_initialized);
        assert!(summarize(&temp.path().join("missing")).is_err());
    }

    #[test]
    fn d3d12_initialization_and_route_use_the_actual_running_record() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("log");
        fs::write(
            &path,
            concat!(
                "[I] calling NVNGXProxy::D3D12_Init_Ext\n",
                "[I] DLSS-NR running before upscale: target 640x360, guides 640x360 (preset 0, intensity 1, style 0)\n",
                "[I] DLSS-NR created before the upscaler; waiting for an observed present before evaluation\n",
            ),
        )
        .unwrap();
        let summary = summarize(&path).unwrap();
        assert!(summary.nr_initialized);
        assert_eq!(summary.api.as_deref(), Some("D3D12"));
        assert!(summary.nr_route.unwrap().contains("waiting for an observed present"));
        assert_eq!(summary.nr_gpu_ms, None);
    }

    #[test]
    fn huge_and_invalid_lines_do_not_unbound_memory() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("log");
        let mut file = File::create(&path).unwrap();
        for _ in 0..1000 {
            file.write_all(b"[E] synthetic failure\n").unwrap();
        }
        file.write_all(&vec![b'x'; 1024 * 1024]).unwrap();
        file.write_all(b"\xff\n[W] end\n").unwrap();
        let s = summarize(&path).unwrap();
        assert_eq!(s.errors.len(), MAX_ERRORS);
        assert_eq!(s.error_count, 1000);
        assert_eq!(s.highlights.len(), MAX_HIGHLIGHTS);
        assert_eq!(s.highlights_omitted, 800);
        assert_eq!(s.truncated_lines, 1);
        assert_eq!(s.warnings, 1);
    }

    #[test]
    fn bundle_has_only_expected_members_and_redacts_every_input() {
        let temp = tempfile::tempdir().unwrap();
        let target = temp.path().join("game");
        fs::create_dir_all(target.join(".retoque")).unwrap();
        let sample = "/synthetic/home/test-person/data test-person Z:\\synthetic\\home\\test-person\\data C:\\Users\\test-person\\data"; // privacy: allow (synthetic test path)
        for name in ["OptiScaler.log", "OptiScaler.ini", ".retoque/receipt.txt"] {
            fs::write(target.join(name), sample).unwrap();
        }
        fs::write(target.join("private.txt"), b"excluded").unwrap();
        // The latest may be a previous-session file; the archive uses a stable, path-free member name.
        fs::write(target.join("OptiScaler.log"), b"older").unwrap();
        let latest = target.join("OptiScaler.previous.log");
        fs::write(&latest, sample).unwrap();
        File::open(latest)
            .unwrap()
            .set_modified(SystemTime::now() + std::time::Duration::from_secs(60))
            .unwrap();
        let redactor = Redactor::new(
            &["/synthetic/home/test-person".into(), "C:\\Users\\test-person".into()], // privacy: allow (synthetic test path)
            &["test-person".into()],
        );
        let path = bundle_with_redactor(&target, sample, &temp.path().join("out"), &redactor).unwrap();
        let mut zip = zip::ZipArchive::new(File::open(&path).unwrap()).unwrap();
        assert_eq!(zip.len(), 4);
        for name in ["OptiScaler.log", "OptiScaler.ini", ".retoque/receipt.txt", "status.txt"] {
            let mut text = String::new();
            zip.by_name(name).unwrap().read_to_string(&mut text).unwrap();
            assert_eq!(text, "~/data <user> ~\\data ~\\data");
        }
        let second = bundle_with_redactor(&target, "", &temp.path().join("out"), &redactor).unwrap();
        assert_ne!(path, second);
    }

    #[test]
    fn redacts_across_read_boundaries_and_json_escaped_paths() {
        let r = Redactor::new(&["C:\\Users\\test-person".into()], &["test-person".into()]); // privacy: allow (synthetic test path)
        for offset in 65510..65540 {
            let input = format!("{}C:\\\\Users\\\\test-person\\\\file test-person", "x".repeat(offset)); // privacy: allow (synthetic test path)
            let mut out = Vec::new();
            r.copy(input.as_bytes(), &mut out).unwrap();
            assert_eq!(
                String::from_utf8(out).unwrap(),
                format!("{}~\\\\file <user>", "x".repeat(offset))
            );
        }
    }

    #[test]
    fn redaction_keeps_words_and_handles_tiny_reads_and_long_identifiers() {
        struct Tiny<'a>(&'a [u8]);
        impl Read for Tiny<'_> {
            fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
                let count = out.len().min(3);
                self.0.read(&mut out[..count])
            }
        }
        let redactor = Redactor::new(&["/home/al".into()], &["al".into()]);
        for separator in ["/", r"\/", r"\", r"\\", r"\\\\"] {
            let input = format!("{separator}Users{separator}Foreign User{separator}file homeostasis userdataHome");
            let mut out = Vec::new();
            redactor.copy(Tiny(input.as_bytes()), &mut out).unwrap();
            assert_eq!(
                String::from_utf8(out).unwrap(),
                format!("{separator}Users{separator}<user>{separator}file homeostasis userdataHome")
            );
        }
        let input = r#"al also al.exe pal /home/alex/file C:\Users\Other User\file {"path":"C:\\Users\\Another User\\file"} /Steam/userdata/76561198000000001/profile"#; // privacy: allow (synthetic test path)
        let mut out = Vec::new();
        redactor.copy(Tiny(input.as_bytes()), &mut out).unwrap();
        assert_eq!(
            String::from_utf8(out).unwrap(),
            r#"<user> also al.exe pal /home/<user>/file C:\Users\<user>\file {"path":"C:\\Users\\<user>\\file"} /Steam/userdata/<steam-id>/profile"#
        );
        let input = format!("/home/{}/model.dll", "u".repeat(200_000));
        let mut out = Vec::new();
        redactor.copy(input.as_bytes(), &mut out).unwrap();
        assert_eq!(out, b"/home/<user>/model.dll");
    }

    #[test]
    fn failed_bundle_removes_its_partial_archive() {
        let temp = tempfile::tempdir().unwrap();
        fs::create_dir(temp.path().join("OptiScaler.ini")).unwrap();
        let out = temp.path().join("out");
        assert!(bundle(temp.path(), "status", &out).is_err());
        assert_eq!(fs::read_dir(out).unwrap().count(), 0);
    }

    #[test]
    fn bundle_missing_optional_files_still_produces_status() {
        let temp = tempfile::tempdir().unwrap();
        let path = bundle(temp.path(), "status", &temp.path().join("out")).unwrap();
        let mut zip = zip::ZipArchive::new(File::open(path).unwrap()).unwrap();
        assert_eq!(zip.len(), 1);
        assert_eq!(zip.by_index(0).unwrap().name(), "status.txt");
    }
}

/// Evidence from observed production messages, never a guess from an error-level line alone.
#[derive(Clone, Debug, PartialEq, Eq, serde::Serialize)]
pub struct LogDiagnosis {
    pub id: &'static str,
    pub title_key: &'static str,
    pub why_key: &'static str,
    pub evidence_line: String,
    pub repair: Option<crate::doctor::Repair>,
}
// Sources: DlssNrFeature_Vk.cpp init/drain failure; DlssNr_Dx12_Model.cpp proxy probe;
// DlssNr_Dx12.cpp coverage emission. pre is retained for the older summary fixtures.
fn signal_kinds(line: &str) -> Vec<u8> {
    let mut kinds = Vec::new();
    if line.contains("DLSS-NR direct: ") && line.contains(" would not load (error ") {
        kinds.push(5);
    }
    if line.contains("DLSS-NR Vulkan: the device was lost while draining it for a resize") {
        kinds.push(0);
    }
    if let Some((_, rest)) = line.split_once("DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned ")
        && rest
            .split_whitespace()
            .next()
            .and_then(|s| s.parse::<i64>().ok())
            .is_some_and(|result| result != 1)
    {
        kinds.push(1);
    }
    if let Some((_, rest)) = line.split_once("DLSS-NR coverage stage=")
        && let Some(cov) = coverage(&format!("stage={rest}"))
    {
        if cov.model_failed > 0 {
            kinds.push(2);
        }
        if ["before", "pre"].contains(&cov.stage.as_str()) && cov.fallback > 0 {
            kinds.push(3);
        }
    }
    if line.contains("DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask") {
        kinds.push(4);
    }
    kinds
}
/// Redacts known home/userdata path forms plus identities explicitly supplied by the caller.
/// Pure; no environment or filesystem access. Not a general-purpose secret scanner.
pub fn redact_line(line: &str, homes: &[String], users: &[String]) -> String {
    let mut out = Vec::new();
    Redactor::new(homes, users)
        .copy(line.as_bytes(), &mut out)
        .expect("Vec write cannot fail");
    String::from_utf8_lossy(&out).into_owned()
}
/// Historical observations in this summary, not a statement that the game is running now.
/// A failed evaluate is not necessarily a loading failure; compatibility corroborates a possible cause.
pub fn diagnose(
    summary: &Summary,
    runtime: crate::recommend::Runtime,
    compatibility: crate::model_compat::Compatibility,
    driver_nvngx_present: Option<bool>,
) -> Vec<LogDiagnosis> {
    use crate::{doctor::setting, model_compat::Compatibility};
    let mut results = Vec::new();
    let mut seen = [false; 6];
    for line in summary
        .diagnostic_lines
        .iter()
        .chain(&summary.errors)
        .chain(&summary.highlights)
    {
        for kind in signal_kinds(line) {
            if seen[kind as usize] {
                continue;
            }
            if kind == 4 && (!runtime.is_wine() || driver_nvngx_present != Some(false) || summary.nr_evaluated) {
                continue;
            }
            seen[kind as usize] = true;
            let (id, title_key, why_key, repair) = match kind {
                0 => (
                    "vulkan_device_lost",
                    "log.vulkan.title",
                    "log.vulkan.why",
                    Some(setting("DlssNr", "Enabled", "false")),
                ),
                1 => ("ngx_init_failed", "log.ngx.title", "log.ngx.why", None),
                2 => (
                    "model_failed",
                    "log.model.title",
                    if compatibility == Compatibility::Incompatible {
                        "log.model.incompatible"
                    } else {
                        "log.model.why"
                    },
                    None,
                ),
                3 => (
                    "pre_stage_fallback",
                    "log.pre.title",
                    "log.pre.why",
                    Some(setting("DlssNr", "Stage", "0")),
                ),
                5 => (
                    "model_load_failed",
                    "log.load.title",
                    if compatibility == Compatibility::Incompatible {
                        "log.model.incompatible"
                    } else {
                        "log.load.why"
                    },
                    None,
                ),
                _ => ("wine_nvngx_missing", "log.nvngx.title", "log.nvngx.why", None),
            };
            results.push(LogDiagnosis {
                id,
                title_key,
                why_key,
                evidence_line: redact_line(line, &[], &[]),
                repair,
            });
        }
    }
    results
}

#[cfg(test)]
mod diagnosis_tests {
    use super::*;
    use crate::{model_compat::Compatibility, recommend::Runtime};
    fn summary(text: &str) -> Summary {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("OptiScaler.log");
        fs::write(&path, text).unwrap();
        summarize(&path).unwrap()
    }
    #[test]
    fn each_rule_positive_negative_and_compatibility_crosscheck() {
        for (positive, negative, id) in [
            (
                "[E] DLSS-NR direct: selected.dll would not load (error 126)",
                "[I] DLSS-NR direct: selected.dll loaded",
                "model_load_failed",
            ),
            (
                "[E] DLSS-NR Vulkan: the device was lost while draining it for a resize; its objects are abandoned",
                "[I] Vulkan hook installed",
                "vulkan_device_lost",
            ),
            (
                "[E] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 0",
                "[I] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 1",
                "ngx_init_failed",
            ),
            (
                "[I] DLSS-NR coverage stage=before calls=2 model_ok=0 model_failed=1 skipped=0 fallback=0",
                "[I] DLSS-NR coverage stage=before calls=2 model_ok=2 model_failed=0 skipped=0 fallback=0",
                "model_failed",
            ),
            (
                "[I] DLSS-NR coverage stage=before calls=2 model_ok=0 model_failed=0 skipped=0 fallback=2",
                "[I] DLSS-NR coverage stage=after calls=2 model_ok=0 model_failed=0 skipped=0 fallback=2",
                "pre_stage_fallback",
            ),
            (
                "[I] DLSS-NR proxy probe: the driver's nvngx is not initialised here, nothing to ask",
                "[I] DLSS-NR: model loader is direct",
                "wine_nvngx_missing",
            ),
        ] {
            let good = diagnose(&summary(positive), Runtime::Proton, Compatibility::Unknown, Some(false));
            assert_eq!(good.len(), 1, "{id}");
            assert_eq!(good[0].id, id);
            assert!(
                diagnose(&summary(negative), Runtime::Proton, Compatibility::Unknown, Some(false)).is_empty(),
                "{id}"
            );
            if id == "model_failed" || id == "model_load_failed" {
                assert_eq!(
                    diagnose(
                        &summary(positive),
                        Runtime::Proton,
                        Compatibility::Incompatible,
                        Some(false)
                    )[0]
                    .why_key,
                    "log.model.incompatible"
                );
            }
            if id == "wine_nvngx_missing" {
                for runtime in [Runtime::Unknown, Runtime::Windows] {
                    assert!(diagnose(&summary(positive), runtime, Compatibility::Unknown, Some(false)).is_empty());
                }
                for exists in [None, Some(true)] {
                    assert!(diagnose(&summary(positive), Runtime::Proton, Compatibility::Unknown, exists).is_empty());
                }
            }
        }
    }
    #[test]
    fn diagnostic_quota_redaction_and_cpu_gpu_distinction() {
        let mut text = "[E] unrelated\n".repeat(250);
        text.push_str("[E] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 0 at /home/fixtureperson/model.dll\n"); // privacy: allow
        text.push_str("[E] DLSS-NR Vulkan: NVSDK_NGX_VULKAN_Init_Ext returned 0\n");
        let s = summary(&text);
        let results = diagnose(&s, Runtime::Unknown, Compatibility::Unknown, None);
        assert_eq!(results.len(), 1);
        assert!(!results[0].evidence_line.contains("fixtureperson"));
        let initialized = summary("[I] DLSS-NR Vulkan: feature up at 1920x1080\n");
        assert!(initialized.nr_initialized && !initialized.nr_evaluated && initialized.nr_gpu_ms.is_none());
        let evaluated =
            summary("[I] DLSS-NR coverage stage=before calls=2 model_ok=2 model_failed=0 skipped=0 fallback=0\n");
        assert!(evaluated.nr_initialized && evaluated.nr_evaluated && evaluated.nr_gpu_ms.is_none());
        let timed = summary("[I] DLSS-NR gpu timing confirmed: stage=before carried=false dispatch_ms=1 model_ms=1\n");
        assert!(timed.nr_initialized && timed.nr_evaluated && timed.nr_gpu_ms == Some(1.0));
    }
}
