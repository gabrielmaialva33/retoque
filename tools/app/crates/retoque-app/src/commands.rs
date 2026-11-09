//! UI intent queue. The shell drains it once, after rendering the complete frame.
use std::sync::mpsc::{Receiver, Sender, channel};

use crate::app::PlanInputs;
use crate::model::PlanKind;

pub(crate) enum AppCommand {
    SelectGame(String),
    UndoSettings(std::path::PathBuf),
    RequestPlan(std::path::PathBuf, PlanKind, PlanInputs),
    RescanLibrary,
    ConfirmReview(u64),
    CancelReview,
    Remedy(crate::error::Remedy, Option<std::path::PathBuf>),
    SwitchTab(crate::app::Tab),
    OpenFolder(std::path::PathBuf),
    Bundle,
    ReadLogs(Option<std::path::PathBuf>),
    RefreshModel,
    ImportModel(std::path::PathBuf),
    RemoveModel(String),
}

#[derive(Clone)]
pub(crate) struct CommandSender(Sender<AppCommand>);

impl CommandSender {
    pub(crate) fn channel() -> (Self, Receiver<AppCommand>) {
        let (tx, rx) = channel();
        (Self(tx), rx)
    }

    pub(crate) fn send(&self, command: AppCommand) {
        let _ = self.0.send(command);
    }
}

/// Stable actions for the command palette; labels share the ordinary UI's translations.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaletteAction {
    RescanLibrary,
    ReviewInstall,
    ReviewUninstall,
    ReviewSettings,
    UndoSettings,
}

impl PaletteAction {
    pub const ALL: [Self; 5] = [
        Self::RescanLibrary,
        Self::ReviewInstall,
        Self::ReviewUninstall,
        Self::ReviewSettings,
        Self::UndoSettings,
    ];

    pub fn label(self, lang: crate::i18n::Lang) -> &'static str {
        crate::i18n::t(
            lang,
            match self {
                Self::RescanLibrary => "app.refresh",
                Self::ReviewInstall => "kind.install",
                Self::ReviewUninstall => "kind.uninstall",
                Self::ReviewSettings => "kind.settings",
                Self::UndoSettings => "undo.title",
            },
        )
    }
}
