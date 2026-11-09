use retoque_core::{Error, ini::Ini};
use retoque_data::{
    doctor::{self, Inputs, ModelInspection},
    logs, lsfg,
    messages::{self, Language},
    model_compat,
    recommend::Runtime,
};
use std::{
    fs,
    path::{Path, PathBuf},
};

fn io_error(error: impl std::fmt::Display) -> Error {
    Error::Other(error.to_string())
}
// Drive mappings other than Z: need a known Wine prefix. Never call those paths missing.
fn host_path(game: &Path, value: &str) -> Option<PathBuf> {
    let value = value.trim();
    if cfg!(windows) {
        return Some(game.join(value));
    }
    if value.starts_with("\\\\") || value.starts_with("//") {
        return None;
    }
    let normalized = value.replace('\\', "/");
    if normalized.get(..3).is_some_and(|s| s.eq_ignore_ascii_case("z:/")) {
        return Some(PathBuf::from(&normalized[2..]));
    }
    if normalized.contains(':') || normalized.starts_with('/') && !value.starts_with('/') {
        return None;
    }
    Some(game.join(normalized))
}
fn inspect(game: &Path, ini: Option<&Ini>) -> Result<ModelInspection, Error> {
    let value = ini.and_then(|i| i.get("DlssNr", "ModelFile")).unwrap_or("auto");
    let path = if value.trim().is_empty() || value.trim().eq_ignore_ascii_case("auto") {
        Some(game.join("nvngx_dlssnr.dll"))
    } else {
        host_path(game, value)
    };
    let Some(path) = path else {
        return Ok(ModelInspection::default());
    };
    let metadata = match fs::symlink_metadata(&path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(ModelInspection {
                present: Some(false),
                ..Default::default()
            });
        }
        Err(error) => return Err(io_error(error)),
    };
    if !metadata.is_file() {
        return Ok(ModelInspection::default());
    }
    let (mut file, _) = retoque_discovery::io::open_regular_file(&path).map_err(io_error)?;
    Ok(ModelInspection {
        present: Some(true),
        compatibility: model_compat::inspect(&mut file).map_err(io_error)?,
    })
}
pub fn run(game: &Path, json: bool) -> Result<(), Error> {
    let pending = match fs::symlink_metadata(game.join(".retoque/pending-receipt.txt")) {
        Ok(_) => true,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => false,
        Err(error) => return Err(io_error(error)),
    };
    let state = match retoque_core::status(game) {
        Ok(state) => state,
        Err(_) if pending => {
            // Even an orphaned or malformed journal is a finding. Do not guess the rest of the state.
            let unknown = retoque_core::Status::default();
            let mut inputs = Inputs::new(&unknown);
            inputs.pending_journal = true;
            let mut findings = doctor::diagnose(&inputs);
            findings[0].why_key = "doctor.journal_unreadable.why";
            return show(&findings, json);
        }
        Err(error) => return Err(error),
    };
    let ini = retoque_core::read_ini(game)?.map(|(_, bytes)| Ini::parse(&bytes));
    let model = inspect(game, ini.as_ref())?;
    let log = logs::find(game)
        .first()
        .map(|p| logs::summarize(p))
        .transpose()
        .map_err(io_error)?;
    let components = retoque_core::installed_components(game)?;
    let gpus = retoque_data::gpu::detect();
    // Multiple adapters do not identify which one this game uses.
    let gpu = (gpus.len() == 1).then(|| &gpus[0]);
    let lsfg_enabled = ini
        .as_ref()
        .and_then(|i| i.get("LSFG", "Enabled"))
        .is_some_and(|s| s.trim().eq_ignore_ascii_case("true") || s.trim() == "1");
    let configured_dll_dir = ini
        .as_ref()
        .and_then(|i| i.get("LSFG", "DllPath"))
        .filter(|s| !s.trim().is_empty() && !s.eq_ignore_ascii_case("auto"));
    let dll_dir = configured_dll_dir.and_then(|s| host_path(game, s));
    let readiness = (lsfg_enabled && (configured_dll_dir.is_none() || dll_dir.is_some())).then(|| {
        lsfg::lsfg_readiness_for_game(
            dll_dir.as_deref(),
            ini.as_ref().and_then(|i| i.get("LSFG", "ShaderDir")),
            Some(game),
            None,
        )
    });
    let mut inputs = Inputs::new(&state);
    inputs.ini = ini.as_ref();
    inputs.model = Some(&model);
    inputs.log = log.as_ref();
    inputs.components = &components;
    inputs.gpu = gpu;
    inputs.lsfg = readiness.as_ref();
    // A Linux manager does not prove that the selected game uses Proton or that its override is absent.
    inputs.runtime = if cfg!(windows) {
        Runtime::Windows
    } else {
        Runtime::Unknown
    };
    inputs.pending_journal = pending;
    let findings = doctor::diagnose(&inputs);
    show(&findings, json)
}
fn show(findings: &[doctor::Finding], json: bool) -> Result<(), Error> {
    let language = if std::env::var("RETOQUE_LANG")
        .or_else(|_| std::env::var("LANG"))
        .unwrap_or_default()
        .to_ascii_lowercase()
        .starts_with("pt")
    {
        Language::Portuguese
    } else {
        Language::English
    };
    if json {
        println!("{}", serde_json::to_string_pretty(findings).map_err(io_error)?);
    } else {
        if findings.is_empty() {
            println!("{}", messages::text("doctor.healthy", language));
        }
        for finding in findings {
            println!(
                "[{}] {}: {}",
                messages::text(
                    match finding.severity {
                        doctor::Severity::Warning => "severity.warning",
                        doctor::Severity::Error => "severity.error",
                    },
                    language
                ),
                finding.id,
                messages::text(finding.title_key, language)
            );
            println!("  {}", messages::text(finding.why_key, language));
            for evidence in &finding.evidence {
                println!(
                    "  {}: {}",
                    messages::text(evidence.key, language),
                    logs::redact_line(&evidence.value, &[], &[])
                );
            }
            if finding.repair.is_some() {
                println!("  {}", messages::text("doctor.review", language));
            }
        }
    }
    if !findings.is_empty() {
        std::process::exit(6);
    }
    Ok(())
}
