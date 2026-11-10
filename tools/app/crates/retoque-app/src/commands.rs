//! UI intent queue. The shell drains it once, after rendering the complete frame.
use std::sync::mpsc::{Receiver, Sender, channel};

use crate::app::PlanInputs;
use crate::model::PlanKind;

pub(crate) enum AppCommand {
    SelectGame(String),
    UndoSettings(std::path::PathBuf),
    PrepareRepair(std::path::PathBuf, retoque_data::doctor::Repair),
    RefreshSelected,
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
    PromptFetchCompatibleModel(Option<std::path::PathBuf>),
    PromptFetchModel(retoque_data::model_fetch::RemoteModel, Option<std::path::PathBuf>),
    PromptRefreshModelCatalog,
    DismissNrModelOffer,
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

/// Stable actions for the command palette; labels share the ordinary UI's translations and say what happens.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PaletteAction {
    RescanLibrary,
    /// The game page's main action, under the banner's name for the open game (install, register, update or only
    /// optimize).
    Optimize,
    ReviewUninstall,
    ReviewSettings,
    UndoSettings,
}

impl PaletteAction {
    pub const ALL: [Self; 5] = [
        Self::RescanLibrary,
        Self::Optimize,
        Self::ReviewUninstall,
        Self::ReviewSettings,
        Self::UndoSettings,
    ];

    /// The label's key; `Optimize` gives its plainest name, the shell shows the banner's current one instead.
    pub fn label_key(self) -> &'static str {
        match self {
            Self::RescanLibrary => "app.refresh",
            Self::Optimize => "shell.optimize",
            Self::ReviewUninstall => "palette.uninstall",
            Self::ReviewSettings => "palette.review_settings",
            Self::UndoSettings => "undo.title",
        }
    }
}
