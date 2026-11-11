//! The app's data beside the install engine: the user's NR model catalog, the settings the app edits, log
//! summaries and diagnostic bundles (docs/superpowers/specs/2026-11-05-retoque-app.md).

pub mod captures;
pub mod logs;
pub mod measure;
pub mod models;
pub mod session;
pub mod settings;

pub mod components;
pub mod gpu;
pub mod lsfg;
pub mod recommend;

pub mod app_data;
pub mod model_compat;
pub mod model_facts;
pub mod model_fetch;

pub mod doctor;
pub mod messages;

pub mod lsfg_prepare;
pub mod payload_fetch;
pub mod sm86;

pub mod dlss;
mod fetch;
pub mod reshade;
