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
}
#[derive(Clone, Debug, thiserror::Error)]
#[error("{message}")]
pub struct AppError {
    message: String,
    pub kind: ErrorKind,
    pub target: Option<PathBuf>,
}
impl AppError {
    pub fn io(path: PathBuf, source: std::io::Error) -> Self {
        Self {
            target: None,
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
            message: e.to_string(),
            kind: ErrorKind::Core(Arc::new(e)),
        }
    }
}
impl From<std::io::Error> for AppError {
    fn from(e: std::io::Error) -> Self {
        Self {
            target: None,
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
impl From<&AppError> for egui::WidgetText {
    fn from(error: &AppError) -> Self {
        error.to_string().into()
    }
}
impl From<AppError> for egui::WidgetText {
    fn from(error: AppError) -> Self {
        error.to_string().into()
    }
}

impl From<&AppError> for egui::RichText {
    fn from(error: &AppError) -> Self {
        egui::RichText::new(error.to_string())
    }
}
impl From<AppError> for egui::RichText {
    fn from(error: AppError) -> Self {
        egui::RichText::new(error.to_string())
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
    if ui.small_button(crate::i18n::t(lang, key)).clicked() {
        commands.send(crate::commands::AppCommand::Remedy(remedy, target));
    }
}

pub(crate) fn card(
    ui: &mut egui::Ui,
    error: impl std::borrow::Borrow<AppError>,
    lang: crate::i18n::Lang,
    commands: &crate::commands::CommandSender,
) {
    let error = error.borrow();
    ui.colored_label(crate::theme::BAD, error);
    buttons(ui, error, lang, commands);
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
}
