use crate::app::*;

pub(crate) fn refusal_text(lang: Lang, r: &Refusal) -> (&'static str, String) {
    match r {
        Refusal::GameRunning { processes } => (
            "ref.running",
            processes
                .iter()
                .map(|(pid, n)| format!("{n} ({pid})"))
                .collect::<Vec<_>>()
                .join(", "),
        ),
        Refusal::Foreign { path, what } => {
            let kind = match what {
                retoque_core::Foreign::ReShade => t(lang, "ref.reshade"),
                retoque_core::Foreign::Unknown => t(lang, "ref.unknown_file"),
            };
            ("ref.foreign", format!("{path} ({kind})"))
        }
        Refusal::SecondProxy { name } => ("ref.second", name.clone()),
        Refusal::Receipt(s) => ("ref.receipt", diagnostic(lang, s)),
        Refusal::Usage(s) => ("ref.usage", diagnostic(lang, s)),
    }
}

pub(crate) fn step_text(lang: Lang, step: &Step) -> (&'static str, String, Color32) {
    match step {
        Step::Copy { from, to } => {
            let name = from
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_default();
            (
                t(lang, "step.copy"),
                if name == *to {
                    to.clone()
                } else {
                    format!("{to}  <- {name}")
                },
                theme::GOOD,
            )
        }
        Step::Backup { path, copy } => (t(lang, "step.backup"), format!("{path}  -> {copy}"), theme::MUTED),
        Step::Rename { from, to } => (t(lang, "step.rename"), format!("{from}  -> {to}"), theme::WARN),
        Step::SetIni {
            file,
            section,
            key,
            value,
            old,
        } => {
            let before = old.as_deref().unwrap_or(t(lang, "value.unset"));
            (
                t(lang, "step.ini"),
                format!("{file}: [{section}] {key}: {before} -> {value}"),
                theme::ACCENT,
            )
        }
        Step::UnsetIni { file, section, key, .. } => (
            t(lang, "step.remove"),
            format!("[{section}] {key} ({file})"),
            theme::ACCENT,
        ),
        Step::Remove { path } => (t(lang, "step.remove"), path.clone(), theme::BAD),
        Step::Restore { copy, to } => (t(lang, "step.restore"), format!("{to}  <- {copy}"), theme::WARN),
        Step::Keep { path, why } => (
            t(lang, "step.keep"),
            format!("{path} ({})", diagnostic(lang, why)),
            theme::MUTED,
        ),
    }
}

pub(crate) fn plan_kind_name(lang: Lang, kind: PlanKind) -> &'static str {
    match kind {
        PlanKind::Install => t(lang, "kind.install"),
        PlanKind::Uninstall => t(lang, "kind.uninstall"),
        PlanKind::Settings => t(lang, "kind.settings"),
    }
}

pub(crate) fn store_name(lang: Lang, store: Store) -> &'static str {
    match store {
        Store::Steam => t(lang, "store.steam"),
        Store::Heroic => t(lang, "store.heroic"),
        Store::Lutris => t(lang, "store.lutris"),
        Store::Epic => t(lang, "store.epic"),
        Store::Gog => t(lang, "store.gog"),
        Store::Manual => t(lang, "store.manual"),
        Store::Xbox => t(lang, "store.xbox"),
    }
}

pub(crate) fn runtime_short(lang: Lang, runtime: &Runtime) -> &'static str {
    match runtime {
        Runtime::Windows => t(lang, "runtime.windows"),
        Runtime::Proton { .. } => t(lang, "runtime.proton"),
        Runtime::Wine { .. } => t(lang, "runtime.wine"),
        Runtime::NativeLinux => t(lang, "runtime.linux"),
        Runtime::Unknown => t(lang, "unknown"),
    }
}

pub(crate) fn runtime_text(lang: Lang, runtime: &Runtime) -> String {
    match runtime {
        Runtime::Windows => t(lang, "runtime.windows").into(),
        Runtime::Proton { app_id, .. } => format!("{} · {app_id}", t(lang, "runtime.proton")),
        Runtime::Wine { .. } => t(lang, "runtime.wine").into(),
        Runtime::NativeLinux => t(lang, "runtime.linux").into(),
        Runtime::Unknown => t(lang, "unknown").into(),
    }
}

pub(crate) fn display_path(p: impl AsRef<Path>) -> String {
    let s = p.as_ref().display().to_string();
    if let Some(rest) = s.strip_prefix(r"\\?\") {
        if let Some(unc) = rest.strip_prefix(r"UNC\").or_else(|| rest.strip_prefix(r"unc\")) {
            return format!(r"\\{unc}");
        }
        return rest.to_owned();
    }
    s
}

pub(crate) fn short_path(p: &Path) -> String {
    let clean = display_path(p);
    if let Some(home) = dirs::home_dir() {
        let home_clean = display_path(&home);
        if let Ok(rest) = Path::new(&clean).strip_prefix(Path::new(&home_clean)) {
            return if rest.as_os_str().is_empty() {
                "~".into()
            } else {
                format!("~/{}", rest.display())
            };
        }
    }
    clean
}

pub(crate) fn relative_to(p: &Path, base: &Path, root: &str) -> String {
    let clean_p = display_path(p);
    let clean_base = display_path(base);
    match Path::new(&clean_p).strip_prefix(Path::new(&clean_base)) {
        Ok(rest) if rest.as_os_str().is_empty() => root.into(),
        Ok(rest) => rest.display().to_string(),
        Err(_) => short_path(Path::new(&clean_p)),
    }
}

pub(crate) fn host_path(value: &str) -> Option<PathBuf> {
    let v = value.trim();
    if cfg!(windows) {
        // Only an absolute path can be checked from here; a relative one is resolved by OptiScaler, not us.
        let p = PathBuf::from(v);
        return p.is_absolute().then_some(p);
    }
    if let Some(rest) = v.strip_prefix("Z:").or_else(|| v.strip_prefix("z:")) {
        return Some(PathBuf::from(rest.replace('\\', "/")));
    }
    v.starts_with('/').then(|| PathBuf::from(v))
}

pub(crate) fn install_state(status: &Status, payload: Option<&str>) -> InstallState {
    match status.receipt.as_ref().map(|r| r.status) {
        Some(ReceiptStatus::Partial | ReceiptStatus::UninstallIncomplete) => InstallState::Partial,
        Some(ReceiptStatus::Complete) => match (status.build_sha256.as_deref(), payload) {
            (Some(installed), Some(available)) if installed == available => InstallState::Current,
            (Some(_), Some(_)) => InstallState::Update,
            _ => InstallState::Installed,
        },
        None if !status.optiscaler_proxies.is_empty() => InstallState::Untracked,
        None => InstallState::Missing,
    }
}

pub(crate) fn clear_applied(draft: &mut BTreeMap<(String, String), String>, edits: &[Edit]) {
    for (section, key, value) in edits {
        let key = (section.clone(), key.clone());
        if draft.get(&key) == Some(value) {
            draft.remove(&key);
        }
    }
}

pub(crate) fn step_group(step: &Step, steps: &[Step], destination_exists: bool) -> usize {
    match step {
        Step::Keep { .. } => 2,
        Step::Remove { .. } => 3,
        Step::SetIni { .. } | Step::UnsetIni { .. } => 4,
        Step::Backup { .. } | Step::Rename { .. } | Step::Restore { .. } => 1,
        Step::Copy { to, .. }
            if destination_exists
                || steps
                    .iter()
                    .any(|s| matches!(s, Step::Backup { path, .. } | Step::Remove { path } if path == to)) =>
        {
            1
        }
        Step::Copy { .. } => 0,
    }
}

// A relative ModelFile belongs to the proxy/executable folder, never to the app working directory.
pub(crate) fn model_host_path(target: &Path, configured: &str) -> Option<PathBuf> {
    // Builds before 2026-11-10 wrote the catalog path in Windows' verbatim form (\\?\C:\...): the same file.
    let plain = display_path(Path::new(configured.trim()));
    let value = plain.as_str();
    if value.is_empty() || value.eq_ignore_ascii_case("auto") {
        Some(target.join("nvngx_dlssnr.dll"))
    } else if value.contains(':') || value.starts_with(['/', '\\']) {
        if value.contains(':') && !value.as_bytes().get(2).is_some_and(|b| matches!(b, b'/' | b'\\')) {
            return None;
        }
        host_path(value)
    } else {
        Some(target.join(value.replace('\\', "/")))
    }
}

pub(crate) fn inspect_model(target: &Path, configured: &str) -> ModelInspection {
    use std::io::Read;
    let path = model_host_path(target, configured);
    let present = path.as_ref().is_some_and(|p| p.is_file());
    let version = path.as_ref().filter(|_| present).and_then(|path| {
        let mut file = std::fs::File::open(path)
            .ok()?
            .take(retoque_data::model_compat::MAX_MODEL_BYTES);
        // The model may be hundreds of MB. Keep only a chunk and enough overlap for version records.
        let mut block = [0; 65536];
        let mut tail = Vec::new();
        let mut version = None;
        loop {
            let count = file.read(&mut block).ok()?;
            if count == 0 {
                break;
            }
            tail.extend_from_slice(&block[..count]);
            if tail.windows(8).any(|s| s == [0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0]) {
                version = retoque_data::models::pe_file_version(&tail);
            }
            if tail.len() > 15 {
                tail.drain(..tail.len() - 15);
            }
        }
        version
    });
    let compatibility = path
        .as_ref()
        .filter(|_| present)
        .and_then(|p| {
            let mut file = std::fs::File::open(p).ok()?;
            retoque_data::model_compat::inspect(&mut file).ok()
        })
        .unwrap_or_default();
    ModelInspection {
        path,
        present,
        version,
        compatibility,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_verbatim_model_file_is_the_same_path() {
        let target = Path::new("/game");
        assert_eq!(
            model_host_path(target, r"\\?\D:\Retoque\models\nvngx_dlssnr.dll"),
            model_host_path(target, r"D:\Retoque\models\nvngx_dlssnr.dll")
        );
        assert_eq!(model_host_path(target, "auto"), Some(target.join("nvngx_dlssnr.dll")));
    }

    #[test]
    fn display_path_and_short_path_strip_windows_verbatim_prefixes() {
        assert_eq!(
            display_path(Path::new(r"\\?\F:\Sparta\MortalShell2\Binaries\Win64")),
            r"F:\Sparta\MortalShell2\Binaries\Win64"
        );
        assert_eq!(
            display_path(Path::new(r"\\?\UNC\server\share\Games\Win64")),
            r"\\server\share\Games\Win64"
        );
        assert_eq!(
            short_path(Path::new(r"\\?\F:\Sparta\MortalShell2\Binaries\Win64")),
            r"F:\Sparta\MortalShell2\Binaries\Win64"
        );
        assert_eq!(
            relative_to(
                Path::new(r"\\?\F:\Sparta\MortalShell2"),
                Path::new(r"F:\Sparta\MortalShell2"),
                "root"
            ),
            "root"
        );
    }
}
