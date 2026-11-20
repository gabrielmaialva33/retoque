//! Explicit, read-only legacy reach diagnostic. No discovery, renderer or installer entrypoint.
//! Raw paths and process inventories never enter the serializable report.
mod platform;
#[cfg(test)]
mod tests;

use crate::pe_version::{self, Machine};
use serde::Serialize;
use sha2::{Digest, Sha256};
use std::{
    fs::{File, OpenOptions},
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

pub const TIME_LIMIT: Duration = Duration::from_secs(2);
pub const OUTPUT_LIMIT: usize = 64 * 1024;
pub const INVENTORY_LIMIT: usize = 1024;
pub const READ_LIMIT: usize = 1024 * 1024;
const HASH_LIMIT: u64 = 256 * 1024 * 1024;

#[derive(Clone, Debug, Default)]
pub struct Request {
    pub executable: Option<PathBuf>,
    pub launcher_only: bool,
    pub classify_launcher: Option<fn(&Path) -> bool>,
    pub pid: Option<u32>,
    /// Only an explicitly named launcher's file; never follow a process tree.
    pub launcher: Option<PathBuf>,
    pub log: Option<PathBuf>,
    pub runtime: Runtime,
    pub mode: Mode,
    pub output: DesiredOutput,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Runtime {
    Windows,
    Proton,
    Linux,
    #[default]
    Unknown,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    Windowed,
    Borderless,
    Exclusive,
    #[default]
    Unknown,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum DesiredOutput {
    Overlay,
    InGame,
    #[default]
    Unknown,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Architecture {
    X86,
    X64,
    Arm64,
    #[default]
    Unknown,
}
impl From<Machine> for Architecture {
    fn from(m: Machine) -> Self {
        match m {
            Machine::X86 => Self::X86,
            Machine::X64 => Self::X64,
            Machine::Arm64 => Self::Arm64,
            Machine::Unknown => Self::Unknown,
        }
    }
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Format {
    Pe,
    Elf,
    #[default]
    Unknown,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Fact {
    Observed,
    #[default]
    Unknown,
}
#[derive(Clone, Debug, Default, Serialize)]
pub struct Image {
    pub sha256: Option<String>,
    pub architecture: Architecture,
    pub format: Format,
    pub header_observation: Fact,
    pub imports_complete: bool,
    #[serde(skip)]
    imports: Vec<String>,
}
#[derive(Clone, Debug, Default)]
struct Process {
    image_path: Option<PathBuf>,
    architecture: Architecture,
    identity: Option<String>,
    stable: bool,
    modules: Vec<String>,
    modules_complete: bool,
    matches_executable: bool,
    issue: Option<&'static str>,
    geometry: Option<[u32; 2]>,
}
#[derive(Clone, Debug, Default, Serialize)]
pub struct LogEvidence {
    pub file_sha256: Option<String>,
    pub complete: bool,
    pub vulkan_creation: Fact,
    pub vulkan_present: Fact,
    pub d3d11_bridge_creation: Fact,
    pub d3d12_present: Fact,
    /// Existing logs contain no binding to the inspected executable's digest/start identity.
    pub current_process_binding: Fact,
}
#[derive(Clone, Debug, Default)]
struct Inventory {
    launcher_only: bool,
    executable: Image,
    launcher: Option<Image>,
    process: Process,
    log: LogEvidence,
    capture_available: Option<bool>,
    timed_out: bool,
    issues: Vec<&'static str>,
}
#[derive(Debug, Serialize)]
pub struct ApiEvidence {
    pub api: &'static str,
    pub module: Fact,
    pub import: Fact,
    pub observed_basenames: Vec<&'static str>,
}
#[derive(Debug, Serialize)]
pub struct Route {
    pub route: &'static str,
    pub state: &'static str,
    pub reason: &'static str,
    pub key: Option<&'static str>,
}
#[derive(Debug, Serialize)]
pub struct Report {
    pub schema: u8,
    pub host_os: &'static str,
    pub requested_runtime: Runtime,
    pub requested_mode: Mode,
    pub desired_output: DesiredOutput,
    pub executable: Image,
    pub launcher: Option<Image>,
    pub process_architecture: Architecture,
    pub process_identity_sha256: Option<String>,
    pub process_identity: Fact,
    pub selected_executable_in_process: Fact,
    pub modules_complete: bool,
    pub inspection_complete: bool,
    pub renderer_decision: &'static str,
    pub window_mode_observation: Fact,
    pub window_geometry_observation: Fact,
    pub window_client_size: Option<[u32; 2]>,
    pub capture_api_available: Option<bool>,
    pub apis: Vec<ApiEvidence>,
    pub log: LogEvidence,
    pub routes: Vec<Route>,
    pub issues: Vec<&'static str>,
    pub elapsed_ms: u64,
}

const APIS: &[(&str, &[&str])] = &[
    ("d3d9", &["d3d9.dll"]),
    ("d3d10", &["d3d10.dll", "d3d10_1.dll", "d3d10core.dll"]),
    ("d3d11", &["d3d11.dll"]),
    ("d3d12", &["d3d12.dll", "d3d12core.dll"]),
    ("vulkan", &["vulkan-1.dll", "winevulkan.dll", "libvulkan.so.1"]),
    ("opengl", &["opengl32.dll", "libgl.so.1", "libglx.so.0", "libegl.so.1"]),
    ("wined3d", &["wined3d.dll", "wined3d.dll.so"]),
];

fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}
fn basename(s: &str) -> &str {
    s.rsplit(['/', '\\']).next().unwrap_or("")
}
fn observed(v: bool) -> Fact {
    if v { Fact::Observed } else { Fact::Unknown }
}

trait Source {
    fn snapshot(&mut self, request: &Request, deadline: Instant) -> Inventory;
}
struct Local;
impl Source for Local {
    fn snapshot(&mut self, request: &Request, deadline: Instant) -> Inventory {
        let mut inventory = Inventory::default();
        if let Some(pid) = request.pid {
            inventory.process = platform::process(pid, request.executable.as_deref(), deadline);
        }
        let exe = request
            .executable
            .as_deref()
            .or(inventory.process.image_path.as_deref());
        if let Some(exe) = exe {
            inventory.launcher_only = request.classify_launcher.is_some_and(|check| check(exe));
            inventory.executable = image(exe, deadline);
            if let Some(log) = &request.log {
                // Do not accept a log from another game, linked file or directory scan.
                let same_parent = exe
                    .parent()
                    .and_then(|p| p.canonicalize().ok())
                    .zip(log.parent().and_then(|p| p.canonicalize().ok()))
                    .is_some_and(|(a, b)| a == b);
                if same_parent {
                    inventory.log = read_log(log, deadline);
                } else {
                    inventory.issues.push("probe.log_scope");
                }
            }
        }
        if let Some(launcher) = &request.launcher {
            inventory.launcher = Some(image(launcher, deadline));
        }
        inventory.capture_available = platform::capture_available();
        // Recheck identity after all I/O, not merely during enumeration.
        if let Some(pid) = request.pid {
            let identity = platform::identity(pid);
            inventory.process.stable &= identity.is_some() && identity == inventory.process.identity;
        }
        inventory.timed_out = Instant::now() >= deadline;
        inventory
    }
}

/// None does exactly zero I/O. The sole product caller is the explicit CLI subcommand.
pub fn inspect(request: Option<&Request>) -> Option<Report> {
    inspect_with(request, &mut Local)
}
fn inspect_with(request: Option<&Request>, source: &mut impl Source) -> Option<Report> {
    let request = request?;
    let start = Instant::now();
    let inventory = source.snapshot(request, start + TIME_LIMIT);
    Some(report(request, inventory, start.elapsed()))
}
/// CLI deadline fallback. It contains no facts collected by an unfinished worker.
pub fn timeout_report(request: &Request) -> Report {
    report(
        request,
        Inventory {
            timed_out: true,
            ..Default::default()
        },
        TIME_LIMIT,
    )
}
fn report(request: &Request, mut raw: Inventory, elapsed: Duration) -> Report {
    if raw.timed_out || elapsed >= TIME_LIMIT {
        raw = Inventory {
            issues: vec!["probe.budget"],
            ..Default::default()
        };
    }
    if request.launcher_only || raw.launcher_only {
        raw.issues.push("probe.launcher_only");
    }
    if request.pid.is_none() {
        raw.issues.push("probe.no_process");
    } else if !raw.process.stable {
        // Do not mix evidence belonging to a stale/reused PID with the requested image.
        if request.executable.is_none() {
            raw.executable = Image::default();
        }
        raw.process.modules.clear();
        raw.process.modules_complete = false;
        raw.process.architecture = Architecture::Unknown;
        raw.process.geometry = None;
        raw.process.matches_executable = false;
        raw.issues.push("probe.identity");
    }
    if let Some(issue) = raw.process.issue {
        raw.issues.push(issue);
    }
    if request.pid.is_some() && !raw.process.matches_executable {
        raw.process.modules.clear();
        raw.process.modules_complete = false;
        raw.issues.push("probe.mismatch");
    }
    if raw.executable.header_observation == Fact::Unknown || raw.executable.sha256.is_none() {
        raw.issues.push("probe.image_unknown");
    }
    if !raw.executable.imports_complete {
        raw.issues.push("probe.imports_partial");
    }
    if !raw.process.modules_complete {
        raw.issues.push("probe.modules_partial");
    }
    if request.mode == Mode::Unknown {
        raw.issues.push("probe.mode_unknown");
    }
    if request.runtime == Runtime::Unknown {
        raw.issues.push("probe.runtime_unknown");
    }
    if request.output == DesiredOutput::Unknown {
        raw.issues.push("probe.output_unknown");
    }
    raw.issues.push("probe.window_unknown");
    raw.issues.push("probe.history");
    if raw.process.modules.len() > INVENTORY_LIMIT {
        raw.process.modules.truncate(INVENTORY_LIMIT);
        raw.process.modules_complete = false;
        raw.issues.push("probe.budget");
    }
    let apis: Vec<_> = APIS
        .iter()
        .map(|(api, names)| {
            let has = |values: &[String], name: &str| values.iter().any(|s| basename(s).eq_ignore_ascii_case(name));
            ApiEvidence {
                api,
                module: observed(names.iter().any(|n| has(&raw.process.modules, n))),
                import: observed(names.iter().any(|n| has(&raw.executable.imports, n))),
                observed_basenames: names
                    .iter()
                    .copied()
                    .filter(|n| has(&raw.process.modules, n) || has(&raw.executable.imports, n))
                    .collect(),
            }
        })
        .collect();
    let complete = raw.executable.header_observation == Fact::Observed
        && raw.executable.sha256.is_some()
        && raw.executable.imports_complete
        && raw.process.stable
        && raw.process.architecture != Architecture::Unknown
        && raw.process.modules_complete
        && raw.process.matches_executable
        && request.pid.is_some()
        && !request.launcher_only
        && !raw.launcher_only;
    let mut result = Report {
        schema: 1,
        host_os: std::env::consts::OS,
        requested_runtime: request.runtime,
        requested_mode: request.mode,
        desired_output: request.output,
        executable: raw.executable,
        launcher: raw.launcher,
        process_architecture: raw.process.architecture,
        process_identity_sha256: raw.process.identity.filter(|_| raw.process.stable),
        process_identity: observed(raw.process.stable),
        selected_executable_in_process: observed(raw.process.matches_executable),
        modules_complete: raw.process.modules_complete,
        inspection_complete: complete,
        renderer_decision: if complete {
            "probe.no_verdict"
        } else {
            "probe.incomplete"
        },
        window_mode_observation: Fact::Unknown,
        window_geometry_observation: observed(raw.process.geometry.is_some()),
        window_client_size: raw.process.geometry,
        capture_api_available: raw.capture_available,
        apis,
        log: raw.log,
        routes: Vec::new(),
        issues: raw.issues,
        elapsed_ms: elapsed.as_millis().min(u64::MAX as u128) as u64,
    };
    result.routes = routes(&result);
    result.issues.sort();
    result.issues.dedup();
    result
}
fn routes(r: &Report) -> Vec<Route> {
    let api = |name| {
        r.apis
            .iter()
            .any(|a| a.api == name && (a.module == Fact::Observed || a.import == Fact::Observed))
    };
    let x64 = r.executable.architecture == Architecture::X64 && r.executable.format == Format::Pe;
    let candidate = |condition| if condition { "candidate" } else { "unknown" };
    let blocked = r.executable.architecture == Architecture::X86;
    vec![
        Route {
            route: "windows_capture",
            state: if matches!(r.requested_runtime, Runtime::Proton | Runtime::Linux)
                || r.desired_output == DesiredOutput::InGame
                || r.requested_mode == Mode::Exclusive
                || r.capture_api_available == Some(false)
            {
                "unavailable"
            } else {
                candidate(
                    r.requested_runtime == Runtime::Windows
                        && r.capture_api_available == Some(true)
                        && r.executable.header_observation == Fact::Observed
                        && r.executable.sha256.is_some()
                        && matches!(r.requested_mode, Mode::Windowed | Mode::Borderless)
                        && r.desired_output == DesiredOutput::Overlay,
                )
            },
            reason: "probe.route.capture",
            key: None,
        },
        Route {
            route: "windows_d3d10",
            state: "unavailable",
            reason: "probe.route.d3d10",
            key: None,
        },
        Route {
            route: "windows_opengl",
            state: "unavailable",
            reason: "probe.route.gl",
            key: None,
        },
        Route {
            route: "x86_in_process",
            state: "unavailable",
            reason: "probe.route.x86",
            key: None,
        },
        Route {
            route: "d3d10_dxvk",
            state: if blocked {
                "unavailable"
            } else {
                candidate(x64 && api("d3d10") && api("vulkan") && r.requested_runtime == Runtime::Proton)
            },
            reason: "probe.route.dxvk",
            key: Some("[DlssNr] Enabled=true; VulkanPresentHost=true"),
        },
        Route {
            route: "d3d_wined3d",
            state: "unavailable",
            reason: "probe.route.wined3d",
            key: None,
        },
        Route {
            route: "native_linux_gl",
            state: "unavailable",
            reason: "probe.route.native_gl",
            key: None,
        },
        Route {
            route: "d3d11_bridge",
            state: if blocked {
                "unavailable"
            } else {
                candidate(x64 && api("d3d11"))
            },
            reason: "probe.route.d3d11",
            key: Some("[DlssNr] Enabled=true; HookMethod=2"),
        },
        Route {
            route: "d3d12_present",
            state: if blocked {
                "unavailable"
            } else {
                candidate(x64 && api("d3d12"))
            },
            reason: "probe.route.d3d12",
            key: Some("[DlssNr] Enabled=true; HookMethod=2"),
        },
        Route {
            route: "vulkan_present",
            state: if blocked {
                "unavailable"
            } else {
                candidate(x64 && api("vulkan"))
            },
            reason: "probe.route.vulkan",
            key: Some("[DlssNr] Enabled=true; VulkanPresentHost=true"),
        },
    ]
}

fn open_regular(path: &Path) -> Option<File> {
    if !crate::safety::unlinked(path) {
        return None;
    }
    let mut options = OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::OpenOptionsExt;
        options.custom_flags(0x00200000);
    }
    let file = options.open(path).ok()?;
    (file.metadata().ok()?.is_file() && crate::safety::unlinked(path)).then_some(file)
}
fn image(path: &Path, deadline: Instant) -> Image {
    let Some(mut file) = open_regular(path) else {
        return Image::default();
    };
    let Ok(before) = file.metadata() else {
        return Image::default();
    };
    let mut image = Image::default();
    if let Some(header) = pe_version::read_header(&mut file) {
        if header.dll {
            return image;
        }
        image.architecture = header.machine.into();
        image.format = Format::Pe;
        image.header_observation = observed(image.architecture != Architecture::Unknown);
        if let Some((imports, complete)) = pe_version::read_imports(&mut file) {
            image.imports = imports;
            image.imports_complete = complete;
        }
    } else {
        let mut header = [0; 64];
        if file.seek(SeekFrom::Start(0)).is_ok() {
            let n = file.read(&mut header).unwrap_or(0);
            image.architecture = elf_architecture(&header[..n]);
            if image.architecture != Architecture::Unknown {
                image.format = Format::Elf;
                image.header_observation = Fact::Observed;
            }
        }
    }
    if before.len() <= HASH_LIMIT && file.seek(SeekFrom::Start(0)).is_ok() {
        let mut hash = Sha256::new();
        let mut buf = [0u8; 65536];
        let mut read = 0u64;
        loop {
            if Instant::now() >= deadline {
                break;
            }
            let Ok(n) = file.read(&mut buf) else {
                break;
            };
            if n == 0 {
                if read == before.len() {
                    image.sha256 = Some(format!("{:x}", hash.finalize()));
                }
                break;
            }
            read += n as u64;
            if read > HASH_LIMIT || read > before.len() {
                break;
            }
            hash.update(&buf[..n]);
        }
    }
    if !file
        .metadata()
        .is_ok_and(|after| after.len() == before.len() && after.modified().ok() == before.modified().ok())
    {
        return Image::default();
    }
    image
}
fn elf_architecture(b: &[u8]) -> Architecture {
    if b.len() < 20 || &b[..4] != b"\x7fELF" || b[5] != 1 || b[6] != 1 {
        return Architecture::Unknown;
    }
    let size = match b[4] {
        1 => 52,
        2 => 64,
        _ => return Architecture::Unknown,
    };
    if b.len() < size {
        return Architecture::Unknown;
    }
    if b[20..24] != [1, 0, 0, 0] {
        return Architecture::Unknown;
    }
    let ehsize = if size == 52 { 40 } else { 52 };
    if u16::from_le_bytes([b[ehsize], b[ehsize + 1]]) as usize != size || b[16] != 2 && b[16] != 3 || b[17] != 0 {
        return Architecture::Unknown;
    }
    match (b[4], u16::from_le_bytes([b[18], b[19]])) {
        (1, 3) => Architecture::X86,
        (2, 62) => Architecture::X64,
        (2, 183) => Architecture::Arm64,
        _ => Architecture::Unknown,
    }
}
fn read_log(path: &Path, deadline: Instant) -> LogEvidence {
    let Some(mut f) = open_regular(path) else {
        return LogEvidence::default();
    };
    let Ok(meta) = f.metadata() else {
        return LogEvidence::default();
    };
    // Head only: bounded and no misleading claim that a tail digest hashes the file.
    let mut b = Vec::new();
    if f.by_ref().take(READ_LIMIT as u64 + 1).read_to_end(&mut b).is_err() || Instant::now() >= deadline {
        return LogEvidence::default();
    }
    let complete = b.len() <= READ_LIMIT && meta.len() == b.len() as u64;
    let hash = complete.then(|| digest(&b));
    b.truncate(READ_LIMIT);
    let mut evidence = log_evidence(&b);
    evidence.complete = complete;
    evidence.file_sha256 = hash;
    evidence
}
fn log_evidence(b: &[u8]) -> LogEvidence {
    let mut e = LogEvidence::default();
    for line in b.split(|c| *c == b'\n').filter(|line| line.len() <= 4096) {
        let Ok(line) = std::str::from_utf8(line) else {
            continue;
        };
        // Do not export a raw log line, address, title or path.
        e.vulkan_creation = observed(
            e.vulkan_creation == Fact::Observed
                || line.contains("DLSS-NR Vulkan present host: device ") && line.contains(" created by "),
        );
        e.vulkan_present = observed(
            e.vulkan_present == Fact::Observed
                || line.contains("DLSS-NR Vulkan present host (cold NR): window ")
                    && line.contains(" (first present); presents 1,"),
        );
        e.d3d11_bridge_creation = observed(
            e.d3d11_bridge_creation == Fact::Observed
                || line.contains("Dx11wDx12SC ") && line.contains(" created, real: "),
        );
        e.d3d12_present = observed(
            e.d3d12_present == Fact::Observed
                || line.contains(
                    "DLSS-NR present: the pass runs at the game's own Present, ahead of its frame generation, on the ",
                ),
        );
    }
    e
}
