//! Typed core/data failures survive the worker boundary; presentation never parses error prose.
use retoque_core::{Error as CoreError, Refusal};
use std::{path::PathBuf, sync::Arc};

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Remedy {
    CloseRunningProcesses(Vec<(u32, String)>),
    ChoosePayloadDir,
    OpenFolder(PathBuf),
    ReplanStale,
    KeepOrReplaceForeign,
}
#[derive(Clone, Debug)]
pub enum ErrorKind {
    Core(Arc<CoreError>),
    Io {
        path: Option<PathBuf>,
        source: Arc<std::io::Error>,
    },
    Message,
    /// Not a failure: progress or "nothing to do" (`warn: false`, muted) or something left out (`warn: true`).
    Notice {
        warn: bool,
    },
}
#[derive(Clone, Debug, thiserror::Error)]
#[error("{message}")]
pub struct AppError {
    message: String,
    pub kind: ErrorKind,
    pub target: Option<PathBuf>,
    /// The length of the context at the start of `message` ("Not fully applied; completed operations 1/3"),
    /// already in the user's language; 0 when there is none. A length, not a second string: `ApplyFailure` carries
    /// this error and must stay small.
    context: usize,
}
impl AppError {
    /// Information shown where errors are shown, but muted: progress, or that there is nothing to do.
    pub fn info(text: impl Into<String>) -> Self {
        Self::notice(text.into(), false)
    }
    /// Something was left out or must be done first: shown in the warning colour, never as a failure.
    pub fn warning(text: impl Into<String>) -> Self {
        Self::notice(text.into(), true)
    }
    fn notice(message: String, warn: bool) -> Self {
        Self {
            target: None,
            context: 0,
            message,
            kind: ErrorKind::Notice { warn },
        }
    }
    /// A failure, as opposed to a notice.
    pub fn is_failure(&self) -> bool {
        !matches!(self.kind, ErrorKind::Notice { .. })
    }
    /// The sentence the user reads, in their language: no Rust `Debug` output, job names, exit codes or English OS
    /// text. The raw message stays in the app log and under "Technical details" (`details`).
    pub fn user_text(&self, lang: crate::i18n::Lang) -> String {
        use crate::i18n::{diagnostic, t};
        let io = |path: Option<&std::path::Path>| match path {
            Some(path) => t(lang, "error.io").replace("{file}", &crate::format::short_path(path)),
            None => t(lang, "error.io_any").to_owned(),
        };
        let base = match &self.kind {
            ErrorKind::Notice { .. } => self.base_message().to_owned(),
            ErrorKind::Message => diagnostic(lang, self.base_message()),
            ErrorKind::Core(error) => match error.as_ref() {
                CoreError::Refused(refusals) => refusals
                    .iter()
                    .map(|r| {
                        let (key, detail) = crate::format::refusal_text(lang, r);
                        format!("{} {detail}", t(lang, key))
                    })
                    .collect::<Vec<_>>()
                    .join("\n"),
                CoreError::Io { path, .. } => io(Some(path)),
                CoreError::StalePlan(_) => t(lang, "inst.stale").to_owned(),
                CoreError::Other(message) => diagnostic(lang, message),
            },
            ErrorKind::Io { path, source } => {
                // Our own I/O errors carry a sentence the app may know; an OS error never reaches the user.
                let known = diagnostic(lang, self.base_message());
                if path.is_none() && (known != self.base_message() || source.get_ref().is_some()) {
                    known
                } else {
                    io(path.as_deref())
                }
            }
        };
        match self.message.get(..self.context).filter(|c| !c.is_empty()) {
            Some(context) => format!("{context}: {base}"),
            None => base,
        }
    }
    /// The technical text for "Technical details", when it says more than `user_text`.
    pub fn details(&self, lang: crate::i18n::Lang) -> Option<String> {
        (self.is_failure() && self.user_text(lang) != self.message).then(|| self.message.clone())
    }
    fn base_message(&self) -> &str {
        match self.context {
            0 => &self.message,
            n => self.message.get(n + 2..).unwrap_or(&self.message),
        }
    }
    pub fn io(path: PathBuf, source: std::io::Error) -> Self {
        Self {
            target: None,
            context: 0,
            message: format!("{}: {source}", crate::format::display_path(&path)),
            kind: ErrorKind::Io {
                path: Some(path),
                source: Arc::new(source),
            },
        }
    }
    pub fn remedies(&self) -> Vec<Remedy> {
        match &self.kind {
            ErrorKind::Core(error) => match error.as_ref() {
                CoreError::Refused(refusals) => refusals.iter().flat_map(remedies_for_refusal).collect(),
                CoreError::StalePlan(_) => vec![Remedy::ReplanStale],
                CoreError::Io { path, .. } => vec![Remedy::OpenFolder(folder(path))],
                _ => vec![],
            },
            ErrorKind::Io { path: Some(path), .. } => vec![Remedy::OpenFolder(folder(path))],
            _ => vec![],
        }
    }
    pub fn for_target(mut self, target: PathBuf) -> Self {
        self.target = Some(target);
        self
    }
    pub fn with_context(mut self, prefix: String) -> Self {
        self.message = format!("{prefix}: {}", self.message);
        self.context = match self.context {
            0 => prefix.len(),
            inner => prefix.len() + 2 + inner,
        };
        self
    }
}
fn folder(path: &std::path::Path) -> PathBuf {
    path.parent().unwrap_or(path).to_owned()
}
pub fn remedies_for_refusal(refusal: &Refusal) -> Vec<Remedy> {
    match refusal {
        Refusal::GameRunning { processes } => vec![Remedy::CloseRunningProcesses(processes.clone())],
        Refusal::Foreign { .. } => vec![Remedy::KeepOrReplaceForeign],
        // Usage may have many causes. Payload selection is exposed by the missing-payload card explicitly.
        _ => vec![],
    }
}
impl From<CoreError> for AppError {
    fn from(e: CoreError) -> Self {
        Self {
            target: None,
            context: 0,
            message: e.to_string(),
            kind: ErrorKind::Core(Arc::new(e)),
        }
    }
}
impl From<std::io::Error> for AppError {
    fn from(e: std::io::Error) -> Self {
        Self {
            target: None,
            context: 0,
            message: e.to_string(),
            kind: ErrorKind::Io {
                path: None,
                source: Arc::new(e),
            },
        }
    }
}
impl From<String> for AppError {
    fn from(message: String) -> Self {
        Self {
            target: None,
            context: 0,
            message,
            kind: ErrorKind::Message,
        }
    }
}
impl From<&str> for AppError {
    fn from(message: &str) -> Self {
        message.to_owned().into()
    }
}
impl std::ops::Deref for AppError {
    type Target = str;
    fn deref(&self) -> &str {
        &self.message
    }
}
pub(crate) fn buttons(
    ui: &mut egui::Ui,
    error: &AppError,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    for remedy in error.remedies() {
        button(ui, remedy, error.target.clone(), lang, commands);
    }
}
pub(crate) fn button(
    ui: &mut egui::Ui,
    remedy: Remedy,
    target: Option<PathBuf>,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    let key = match &remedy {
        Remedy::CloseRunningProcesses(_) => "remedy.processes",
        Remedy::ChoosePayloadDir => "app.payload.choose",
        Remedy::OpenFolder(_) => "log.open_folder",
        Remedy::ReplanStale => "remedy.replan",
        Remedy::KeepOrReplaceForeign => "remedy.foreign",
    };
    if ui.button(crate::i18n::t(lang, key)).clicked() {
        commands.send(crate::commands::AppCommand::Remedy(remedy, target));
    }
}

/// The one way a failure is shown inside a view: a tinted card with the sentence, what to do next, and the
/// technical text folded under "Technical details". A notice is drawn as a muted or warning line instead.
pub(crate) fn card(
    ui: &mut egui::Ui,
    error: impl std::borrow::Borrow<AppError>,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    let error = error.borrow();
    if !error.is_failure() {
        notice(ui, error);
        return;
    }
    crate::widgets::tinted_card(ui, crate::theme::ERROR_CARD, |ui| body(ui, error, lang, commands));
}

/// A card's contents without the card: for places that already have a frame (a modal, a popup).
pub(crate) fn body(
    ui: &mut egui::Ui,
    error: &AppError,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    crate::applog::shown_error(&error.to_string());
    ui.add(egui::Label::new(egui::RichText::new(error.user_text(lang)).color(crate::theme::TEXT_STRONG)).wrap());
    let remedies = error.remedies();
    if !remedies.is_empty() {
        ui.add_space(4.0);
        ui.horizontal_wrapped(|ui| {
            for remedy in remedies {
                button(ui, remedy, error.target.clone(), lang, commands);
            }
        });
    }
    details(ui, error, lang);
}

/// The raw message under "Technical details", when it says more than the sentence.
pub(crate) fn details(ui: &mut egui::Ui, error: &AppError, lang: crate::i18n::Lang) {
    if let Some(details) = error.details(lang) {
        let title = crate::i18n::t(lang, "technical.details");
        crate::widgets::collapsible(
            ui,
            ("error-details", &error.message),
            title,
            crate::widgets::Fold::Details,
            |ui| {
                ui.add(egui::Label::new(egui::RichText::new(details).small().monospace()).wrap());
            },
        );
    }
}

/// A notice (progress, nothing to do, something left out) in its colour; a failure in the error card.
pub(crate) fn notice(ui: &mut egui::Ui, error: &AppError) {
    let color = match error.kind {
        ErrorKind::Notice { warn: true } => crate::theme::WARN,
        ErrorKind::Notice { warn: false } => crate::theme::MUTED,
        _ => crate::theme::BAD,
    };
    ui.add(egui::Label::new(egui::RichText::new(&error.message).color(color)).wrap());
}

/// One line for a bar: the sentence (cut to the line, whole on hover), what to do next, and the card under a
/// "Details" button. The bar's other controls stay where they are.
pub(crate) fn summary(
    ui: &mut egui::Ui,
    error: &AppError,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    crate::applog::shown_error(&error.to_string());
    let text = error.user_text(lang);
    let line = text.lines().next().unwrap_or_default().to_owned();
    let color = if error.is_failure() {
        crate::theme::BAD
    } else {
        crate::theme::MUTED
    };
    // At most half the bar, so the controls after it keep their place.
    let width = (ui.available_width() * 0.5).max(160.0);
    ui.scope(|ui| {
        ui.set_max_width(width);
        ui.add(egui::Label::new(egui::RichText::new(line).color(color)).truncate())
            .on_hover_text(&text);
    });
    buttons(ui, error, lang, commands);
    if error.is_failure() {
        ui.menu_button(crate::i18n::t(lang, "shell.details"), |ui| {
            ui.set_max_width(420.0);
            body(ui, error, lang, commands);
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn typed_context_keeps_pids_io_kind_and_stale_remedy() {
        let error = AppError::from(CoreError::Refused(vec![Refusal::GameRunning {
            processes: vec![(42, "synthetic-game".into())],
        }]));
        assert_eq!(
            error.remedies(),
            [Remedy::CloseRunningProcesses(vec![(42, "synthetic-game".into())])]
        );
        let error = AppError::io(
            "/synthetic/game/file".into(),
            std::io::Error::from(std::io::ErrorKind::PermissionDenied),
        );
        assert!(
            matches!(error.kind, ErrorKind::Io { ref source, .. } if source.kind() == std::io::ErrorKind::PermissionDenied)
        );
        assert_eq!(error.remedies(), [Remedy::OpenFolder("/synthetic/game".into())]);
        let error = AppError::from(CoreError::StalePlan("unchanged CLI wording".into()));
        assert_eq!(error.remedies(), [Remedy::ReplanStale]);
    }

    #[test]
    fn user_text_has_no_debug_output_exit_codes_or_os_text() {
        use crate::i18n::{Lang, t};
        for lang in [Lang::En, Lang::Pt] {
            let refused = AppError::from(CoreError::Refused(vec![Refusal::GameRunning {
                processes: vec![(4242, "Game.exe".into())],
            }]));
            let text = refused.user_text(lang);
            assert_eq!(text, format!("{} Game.exe (4242)", t(lang, "ref.running")));
            for internal in ["refused", "GameRunning", "processes", "exit", "saída"] {
                assert!(!text.contains(internal), "{text}");
            }
            // The raw form stays available under the details fold.
            assert!(refused.details(lang).is_some_and(|d| d.contains("GameRunning")));

            let os = std::io::Error::from_raw_os_error(13);
            let os_text = os.to_string();
            let io = AppError::io("/synthetic/game/OptiScaler.ini".into(), os);
            let text = io.user_text(lang);
            assert_eq!(
                text,
                t(lang, "error.io").replace("{file}", "/synthetic/game/OptiScaler.ini")
            );
            assert!(!text.contains(&os_text));
            assert!(io.details(lang).is_some_and(|d| d.contains(&os_text)));
            let bare = AppError::from(std::io::Error::from_raw_os_error(13));
            assert_eq!(bare.user_text(lang), t(lang, "error.io_any"));

            // Our own sentences are translated, and the context of a partial apply is kept in front.
            let partial = AppError::from(CoreError::Refused(vec![Refusal::SecondProxy {
                name: "winmm.dll".into(),
            }]))
            .with_context("1/3".into());
            assert_eq!(
                partial.user_text(lang),
                format!("1/3: {} winmm.dll", t(lang, "ref.second"))
            );
        }
    }

    #[test]
    fn notices_are_not_failures() {
        let info = AppError::info("Nothing to do");
        let warning = AppError::warning("Left out");
        assert!(!info.is_failure() && !warning.is_failure());
        assert!(matches!(warning.kind, ErrorKind::Notice { warn: true }));
        assert_eq!(info.user_text(crate::i18n::Lang::Pt), "Nothing to do");
        assert!(info.details(crate::i18n::Lang::En).is_none());
        assert!(AppError::from("failed").is_failure());
    }
}
