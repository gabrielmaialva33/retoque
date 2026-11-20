//! RetoqueWindowHost protocol 1, independent of Windows and egui.
//! The host supports status/stop only. Settings belong to a new session, never a pretend live command.

/// What a launch copies into its session folder. The host is absent from payloads before 0.3.1.
pub const HOST_FILES: [&str; 3] = ["RetoqueWindowHost.exe", "OptiScaler.dll", "nvngx.dll_dlssnr.dll"];

/// True when the selected Retoque files can start "NR on any window"; otherwise the app offers no control for it.
pub fn payload_has_host(payload: &std::path::Path) -> bool {
    HOST_FILES.iter().all(|name| payload.join(name).is_file())
}
use serde::Deserialize;
use std::{
    io::{self, BufRead},
    path::PathBuf,
};

pub const MAX_LINE: usize = 64 * 1024;
pub const MAX_WINDOWS: usize = 512;

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Source {
    Window(String),
    Countdown(u8),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Output {
    #[default]
    Preview,
    Overlay,
}
#[derive(Clone, Debug, PartialEq)]
pub struct Settings {
    pub model: PathBuf,
    pub source: Source,
    pub strength: u32,
    pub scale: u32,
    pub motion: bool,
    pub protect_ui: bool,
    pub compare: u8,
    pub output: Output,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            model: PathBuf::new(),
            source: Source::Countdown(5),
            strength: 100,
            scale: 50,
            motion: false,
            protect_ui: true,
            compare: 0,
            output: Output::Preview,
        }
    }
}
impl Settings {
    pub fn validate(&self) -> Result<(), &'static str> {
        let model = self.model.to_str().ok_or("invalid model path")?;
        if !self.model.is_absolute() || model.contains(['\r', '\n', '\0']) {
            return Err("invalid model path");
        }
        match &self.source {
            Source::Window(h) if !valid_handle(h) => return Err("invalid window"),
            Source::Countdown(n) if !(1..=60).contains(n) => return Err("invalid countdown"),
            _ => {}
        }
        if self.strength > 100 || !(25..=100).contains(&self.scale) || self.compare > 2 {
            return Err("invalid processing settings");
        }
        Ok(())
    }
    pub fn arguments(&self) -> Vec<String> {
        let mut args = vec!["--run".into()];
        match &self.source {
            Source::Window(h) => args.extend(["--window".into(), h.clone()]),
            Source::Countdown(n) => args.extend(["--countdown".into(), n.to_string()]),
        }
        if self.output == Output::Overlay {
            args.push("--overlay".into());
        }
        args
    }
    pub fn ini(&self) -> Result<Vec<u8>, &'static str> {
        self.validate()?;
        let mut ini =
            retoque_core::ini::Ini::parse(include_bytes!("../../../../../../retoque/window_host/window-host.ini"));
        for (key, value) in [
            ("ModelFile", self.model.to_str().unwrap().to_owned()),
            ("TransferStrength", format!("{:.2}", self.strength as f64 / 100.0)),
            ("WorkingScale", format!("{:.2}", self.scale as f64 / 100.0)),
            ("SynthMotion", self.motion.to_string()),
            ("UiProtection", self.protect_ui.to_string()),
            ("Compare", self.compare.to_string()),
        ] {
            ini.set("DlssNr", key, &value);
        }
        Ok(ini.to_bytes())
    }
}
fn valid_handle(h: &str) -> bool {
    h.strip_prefix("0x").is_some_and(|v| {
        !v.is_empty()
            && v.len() <= 16
            && v.bytes().all(|b| b.is_ascii_hexdigit())
            && u64::from_str_radix(v, 16).is_ok_and(|n| n != 0)
    })
}

#[derive(Clone, Debug, Deserialize)]
pub struct Window {
    pub hwnd: String,
    pub pid: u32,
    pub title: String,
}
#[derive(Clone, Debug, Deserialize)]
#[serde(tag = "event")]
pub enum Event {
    #[serde(rename = "window")]
    Window(Window),
    #[serde(rename = "catalog")]
    Catalog { windows: usize },
    #[serde(rename = "countdown")]
    Countdown { seconds: u8 },
    #[serde(rename = "started")]
    Started {
        protocol: u32,
        nr: bool,
        output: String,
        disengage_hotkey: String,
    },
    #[serde(rename = "nr")]
    Nr { running: bool, status: String },
    #[serde(rename = "paused")]
    Paused { reason: String },
    #[serde(rename = "resumed")]
    Resumed,
    #[serde(rename = "status")]
    Status {
        rev: Option<u64>,
        state: String,
        overlay_visible: Option<bool>,
    },
    #[serde(rename = "refused")]
    Refused { stage: String, reason: String, exit: i32 },
    #[serde(rename = "command-refused")]
    CommandRefused {
        #[serde(default)]
        reason: String,
    },
    #[serde(rename = "restart-required")]
    RestartRequired { reason: String },
    #[serde(rename = "stopped")]
    Stopped { reason: String, exit: i32 },
    #[serde(rename = "stream")]
    Stream,
    #[serde(rename = "resized")]
    Resized,
    #[serde(rename = "frame")]
    Frame,
}
/// Reject an oversized event before allocating past MAX_LINE, even without a newline.
pub fn read_event(reader: &mut impl BufRead) -> io::Result<Option<Event>> {
    let mut line = Vec::new();
    loop {
        let buf = reader.fill_buf()?;
        if buf.is_empty() {
            if line.is_empty() {
                return Ok(None);
            }
            return Err(io::Error::new(io::ErrorKind::InvalidData, "truncated host event"));
        }
        let n = buf.iter().position(|b| *b == b'\n').map_or(buf.len(), |i| i + 1);
        if line.len() + n > MAX_LINE {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "host event overflow"));
        }
        let complete = buf[n - 1] == b'\n';
        line.extend_from_slice(&buf[..n]);
        reader.consume(n);
        if complete {
            break;
        }
    }
    let event: Event = serde_json::from_slice(&line).map_err(|e| io::Error::new(io::ErrorKind::InvalidData, e))?;
    if let Event::Window(w) = &event
        && (!valid_handle(&w.hwnd) || w.pid == 0 || w.title.len() > 4096)
    {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "invalid catalog window"));
    }
    Ok(Some(event))
}
#[derive(Clone, Copy)]
pub enum Command {
    Status,
    Stop,
}
pub fn command(command: Command, revision: u64) -> String {
    let cmd = match command {
        Command::Status => "status",
        Command::Stop => "stop",
    };
    format!("{{\"cmd\":\"{cmd}\",\"rev\":{revision}}}\n")
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Phase {
    #[default]
    Idle,
    Catalog,
    Starting,
    Capturing,
    Running,
    Paused,
    Stopping,
    Stopped,
    Failed,
    RestartRequired,
}
impl Phase {
    pub fn key(self) -> &'static str {
        match self {
            Self::Idle => "any.idle",
            Self::Catalog => "any.catalog_busy",
            Self::Starting => "any.starting",
            Self::Capturing => "any.capturing",
            Self::Running => "any.running",
            Self::Paused => "any.paused",
            Self::Stopping => "any.stopping",
            Self::Stopped => "any.stopped",
            Self::Failed => "any.failed",
            Self::RestartRequired => "any.restart_state",
        }
    }
}
#[derive(Clone, Debug, Default)]
pub struct State {
    pub phase: Phase,
    pub revision: u64,
    pub requested: Option<Settings>,
    /// Snapshot of the launch configuration, only after a valid started event. Not a per-setting runtime echo.
    pub applied: Option<Settings>,
    pub confirmed_revision: Option<u64>,
    pub windows: Vec<Window>,
    pub catalog_complete: bool,
    pub countdown: Option<u8>,
    pub problem: Option<Problem>,
    pub detail: String,
    pub overlay: bool,
    pub nr_running: bool,
}
impl State {
    pub fn begin(&mut self, request: Option<Settings>) {
        let revision = self.revision + 1;
        *self = Self {
            phase: if request.is_some() {
                Phase::Starting
            } else {
                Phase::Catalog
            },
            requested: request,
            revision,
            ..Default::default()
        };
    }
    pub fn fail(&mut self, problem: Problem, detail: impl Into<String>) {
        self.phase = Phase::Failed;
        self.problem = Some(problem);
        self.detail = detail.into();
    }
    pub fn accept(&mut self, event: Event) {
        if matches!(self.phase, Phase::Failed | Phase::RestartRequired) {
            return;
        }
        match event {
            Event::Window(w) if self.phase == Phase::Catalog => {
                if self.windows.len() == MAX_WINDOWS {
                    self.fail(Problem::Protocol, "catalog overflow");
                } else {
                    self.windows.push(w);
                }
            }
            Event::Catalog { windows } if self.phase == Phase::Catalog => {
                if windows != self.windows.len() {
                    self.fail(Problem::Protocol, "incomplete catalog");
                } else {
                    self.catalog_complete = true;
                    self.phase = Phase::Idle;
                }
            }
            Event::Started {
                protocol, nr, output, ..
            } => {
                if protocol != 1
                    || !nr
                    || self.requested.is_none()
                    || output
                        != if self.requested.as_ref().is_some_and(|s| s.output == Output::Overlay) {
                            "overlay"
                        } else {
                            "preview"
                        }
                {
                    self.fail(Problem::Protocol, "incompatible started event");
                    return;
                }
                self.overlay = output == "overlay";
                if self.phase != Phase::Stopping {
                    self.applied = self.requested.clone();
                    self.phase = Phase::Capturing;
                }
            }
            Event::Nr { running, status } => {
                self.nr_running = running;
                self.detail = status;
                if matches!(self.phase, Phase::Capturing | Phase::Running) {
                    self.phase = if running { Phase::Running } else { Phase::Capturing };
                }
            }
            Event::Countdown { seconds } => self.countdown = Some(seconds),
            Event::Paused { reason } if self.phase != Phase::Stopping => {
                self.phase = Phase::Paused;
                self.problem = Some(Problem::reason("pause", &reason));
                self.detail = reason;
            }
            Event::Resumed if self.phase != Phase::Stopping => {
                self.phase = if self.nr_running {
                    Phase::Running
                } else {
                    Phase::Capturing
                };
                self.problem = None;
            }
            Event::Status { rev, .. } => {
                if rev == Some(self.revision) {
                    self.confirmed_revision = rev;
                }
            }
            Event::Refused { stage, reason, .. } => self.fail(Problem::reason(&stage, &reason), reason),
            Event::CommandRefused { reason } => self.fail(Problem::Protocol, reason),
            Event::RestartRequired { reason } => {
                self.phase = Phase::RestartRequired;
                self.problem = Some(Problem::Restart);
                self.detail = reason;
            }
            Event::Stopped { reason, exit } => {
                if exit == 7 {
                    self.phase = Phase::RestartRequired;
                    self.problem = Some(Problem::Restart);
                } else if exit != 0 {
                    self.fail(Problem::reason("", &reason), reason.clone());
                } else {
                    self.phase = Phase::Stopped;
                    self.problem = None;
                }
                self.detail = reason;
            }
            _ => {}
        }
    }
    pub fn exited(&mut self, code: Option<i32>, user_stop: bool) {
        if matches!(self.phase, Phase::Failed | Phase::RestartRequired) {
            return;
        }
        if code == Some(7) {
            self.phase = Phase::RestartRequired;
            self.problem = Some(Problem::Restart);
        } else if user_stop {
            self.phase = Phase::Stopped;
            self.problem = None;
        } else if code != Some(0) || !(self.catalog_complete || self.phase == Phase::Stopped) {
            self.fail(Problem::Crash, format!("host exited: {code:?}"));
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Problem {
    Protocol,
    Crash,
    Slow,
    Files,
    Cursor,
    Exclusive,
    Topmost,
    Hotkey,
    Dpi,
    Capture,
    Protected,
    Source,
    Adapter,
    Nr,
    Output,
    Device,
    Restart,
    Pause,
    Unknown,
}
impl Problem {
    pub fn key(self) -> &'static str {
        match self {
            Self::Protocol => "any.err_protocol",
            Self::Crash => "any.err_crash",
            Self::Slow => "any.err_slow",
            Self::Files => "any.err_files",
            Self::Cursor => "any.err_cursor",
            Self::Exclusive => "any.err_exclusive",
            Self::Topmost => "any.err_topmost",
            Self::Hotkey => "any.err_hotkey",
            Self::Dpi => "any.err_dpi",
            Self::Capture => "any.err_capture",
            Self::Protected => "any.err_protected",
            Self::Source => "any.err_source",
            Self::Adapter => "any.err_adapter",
            Self::Nr => "any.err_nr",
            Self::Output => "any.err_output",
            Self::Device => "any.err_device",
            Self::Restart => "any.restart",
            Self::Pause => "any.err_pause",
            Self::Unknown => "any.err_unknown",
        }
    }
    pub fn reason(stage: &str, reason: &str) -> Self {
        let r = reason.to_lowercase();
        if r.contains("cursor-capture") {
            Self::Cursor
        } else if r.contains("exclusive-fullscreen") {
            Self::Exclusive
        } else if r.contains("source-topmost") {
            Self::Topmost
        } else if r.contains("ctrl+alt+f10") {
            Self::Hotkey
        } else if r.contains("dpi") {
            Self::Dpi
        } else if r.contains("affinity") || r.contains("protected") {
            Self::Protected
        } else {
            match stage {
                "pause" => Self::Pause,
                "source" => Self::Source,
                "adapter" => Self::Adapter,
                "capture" | "winrt" => Self::Capture,
                "nr" => Self::Nr,
                "overlay" | "preview" => Self::Output,
                "devices" | "transport" => Self::Device,
                _ => Self::Unknown,
            }
        }
    }
}
