//! Retoque's desktop app as a library, so tests can drive the real window (egui_kittest) without a display.
//! `main.rs` only opens the native window.

pub mod app;
pub mod compare;
pub mod i18n;
pub mod jobs;
pub mod model;
pub mod theme;
pub mod widgets;

pub use app::RetoqueApp;

pub mod autotune;
