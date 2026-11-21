//! `Retoque-Setup-<version>.exe`: unpack the app bundle appended to this executable into a fresh private folder,
//! run `retoque-app.exe --setup <folder>` from there (it opens the install or update review), wait for it and remove
//! the folder. See lib.rs for the bundle layout and docs/releases/APP-UPDATE.md for the release flow.

#![cfg_attr(windows, windows_subsystem = "windows")]

use std::{fs, io, process::ExitCode};

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(Failure::Setup(error)) => {
            fail(Text::SetupFailed, &error.to_string());
            ExitCode::FAILURE
        }
        Err(Failure::App(detail)) => {
            fail(Text::AppFailed, &detail);
            ExitCode::FAILURE
        }
    }
}

enum Failure {
    Setup(io::Error),
    App(String),
}
impl From<io::Error> for Failure {
    fn from(error: io::Error) -> Self {
        Failure::Setup(error)
    }
}

fn run() -> Result<(), Failure> {
    let exe = std::env::current_exe()?;
    let bundle = retoque_setup::read_bundle(&mut fs::File::open(&exe)?)?;
    // A fresh folder only this user can reach (it is created under the user's own temporary folder).
    let dir = tempfile::Builder::new().prefix("retoque-setup-").tempdir()?;
    let app = bundle.extract(dir.path())?;
    // The app reads its own copies from the folder; do not hold the archive in memory while it runs.
    drop(bundle);
    let status = std::process::Command::new(&app)
        .arg("--setup")
        .arg(dir.path())
        .current_dir(dir.path())
        .status()?;
    // Best effort: a file still held by another process only leaves the folder behind in the temporary folder.
    let _ = dir.close();
    if status.success() {
        Ok(())
    } else {
        Err(Failure::App(status.to_string()))
    }
}

#[derive(Clone, Copy)]
enum Text {
    SetupFailed,
    AppFailed,
}

/// English, or Brazilian Portuguese when the user's language is Portuguese. The app itself has every language.
fn text(which: Text, portuguese: bool) -> &'static str {
    match (which, portuguese) {
        (Text::SetupFailed, false) => {
            "The Retoque setup could not start. Nothing was installed. Download the setup again from the official releases and try again. Details:"
        }
        (Text::SetupFailed, true) => {
            "Não foi possível iniciar o instalador do Retoque. Nada foi instalado. Baixe o instalador de novo nas versões oficiais e tente novamente. Detalhes:"
        }
        (Text::AppFailed, false) => "The Retoque app closed with an error during setup. Details:",
        (Text::AppFailed, true) => "O app do Retoque fechou com um erro durante a instalação. Detalhes:",
    }
}

fn portuguese() -> bool {
    sys_locale::get_locale().is_some_and(|l| l.to_ascii_lowercase().starts_with("pt"))
}

#[cfg(windows)]
fn fail(which: Text, detail: &str) {
    use std::os::windows::ffi::OsStrExt;
    use windows::{
        Win32::UI::WindowsAndMessaging::{MB_ICONERROR, MB_OK, MessageBoxW},
        core::PCWSTR,
    };
    let wide = |s: &str| -> Vec<u16> { std::ffi::OsStr::new(s).encode_wide().chain(Some(0)).collect() };
    let body = wide(&format!("{}\n\n{detail}", text(which, portuguese())));
    let title = wide("Retoque");
    unsafe {
        MessageBoxW(
            None,
            PCWSTR(body.as_ptr()),
            PCWSTR(title.as_ptr()),
            MB_OK | MB_ICONERROR,
        );
    }
}

#[cfg(not(windows))]
fn fail(which: Text, detail: &str) {
    eprintln!("{}\n\n{detail}", text(which, portuguese()));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn both_languages_end_with_the_details_prompt() {
        for which in [Text::SetupFailed, Text::AppFailed] {
            assert!(text(which, false).ends_with("Details:"));
            assert!(text(which, true).ends_with("Detalhes:"));
        }
    }
}
