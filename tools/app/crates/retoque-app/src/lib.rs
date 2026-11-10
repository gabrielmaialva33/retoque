//! Retoque's desktop app as a library, so tests can drive the real window (egui_kittest) without a display.
//! `main.rs` only opens the native window.

pub mod app;
pub mod applog;
pub mod compare;
pub mod i18n;
pub mod jobs;
pub mod leftovers_ui;
pub mod model;
pub mod theme;
pub mod widgets;

pub use app::RetoqueApp;

pub mod autotune;

pub mod commands;

pub(crate) mod modals;

#[cfg(test)]
mod app_tests;
pub(crate) mod autotune_ui;
pub(crate) mod controller;
pub(crate) mod folders_ui;
pub(crate) mod format;
pub(crate) mod shell;
pub(crate) mod tabs;

pub(crate) mod state;

pub mod error;

pub(crate) mod drop_import;
pub(crate) mod fuzzy;
pub(crate) mod history;
pub(crate) mod intelligence;
pub(crate) mod intuitive;
#[cfg(test)]
mod intuitive_tests;
pub(crate) mod watcher;

pub(crate) mod lsfg_ui;

mod dlss_ui;
pub(crate) mod payload_ui;

pub(crate) mod reshade_ui;
