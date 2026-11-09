//! The app's data beside the install engine: the user's NR model catalog, the settings the app edits, log
//! summaries and diagnostic bundles (docs/superpowers/specs/2026-11-05-retoque-app.md).

pub mod captures;
pub mod logs;
pub mod models;
pub mod settings;

pub mod components;
pub mod gpu;
pub mod lsfg;
pub mod recommend;

pub mod model_compat;

pub mod doctor;
pub mod messages;

pub mod lsfg_prepare;
