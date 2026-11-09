//! Pure health checks. Repairs are proposals; callers must build and review a fresh core Plan.
use crate::{
    gpu::Gpu,
    logs,
    lsfg::LsfgReadiness,
    model_compat::{Compatibility, ModelCompatibility},
    models::Model,
    recommend::{self, Action, FactEvidence, Runtime},
};
use retoque_core::{Status, ini::Ini, receipt::ReceiptStatus};
use serde::Serialize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub enum Severity {
    Info,
    Warning,
    Error,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub enum Repair {
    InstallReShade,
    UpdateDlss,
    RestoreDlss,
    Settings(Vec<Action>),
    Reinstall,
    /// Reconcile through core's uninstall/recovery planner, preserving its refusals.
    ReconcileInstallation,
    RemoveComponent {
        id: String,
    },
    /// Fetch a compatible community NR model into the app catalog, then review selecting it for the game.
    FetchCompatibleModel {
        sha256: String,
    },
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Finding {
    pub id: &'static str,
    pub severity: Severity,
    pub title_key: &'static str,
    pub why_key: &'static str,
    pub evidence: Vec<FactEvidence>,
    pub repair: Option<Repair>,
}
#[derive(Clone, Debug, Default)]
pub struct ModelInspection {
    /// None means an unresolved Wine drive or failed inspection, not an absent model.
    pub present: Option<bool>,
    pub compatibility: ModelCompatibility,
}
/// All inputs belong to the selected game and inspection generation. None means unknown.
pub struct Inputs<'a> {
    pub status: &'a Status,
    pub ini: Option<&'a Ini>,
    pub pending_journal: bool,
    pub model: Option<&'a ModelInspection>,
    pub catalog: &'a [Model],
    pub gpu: Option<&'a Gpu>,
    pub log: Option<&'a logs::Summary>,
    pub lsfg: Option<&'a LsfgReadiness>,
    /// Installed sm86 component IDs, or a detected external sm86 loader via sm86_present.
    pub components: &'a [String],
    pub sm86_present: bool,
    pub runtime: Runtime,
    /// Evidence from the game's launch configuration, never the manager's own environment.
    pub override_present: Option<bool>,
    pub proxy_loaded_last_session: Option<bool>,
    pub driver_nvngx_present: Option<bool>,
    pub allow_model_fetch: bool,
}
impl<'a> Inputs<'a> {
    pub fn new(status: &'a Status) -> Self {
        Self {
            status,
            ini: None,
            pending_journal: false,
            model: None,
            catalog: &[],
            gpu: None,
            log: None,
            lsfg: None,
            components: &[],
            sm86_present: false,
            runtime: Runtime::Unknown,
            override_present: None,
            proxy_loaded_last_session: None,
            driver_nvngx_present: None,
            allow_model_fetch: false,
        }
    }
}
pub(crate) fn setting(section: &str, key: &str, value: &str) -> Repair {
    Repair::Settings(vec![Action::SetIni {
        section: section.into(),
        key: key.into(),
        value: value.into(),
    }])
}
fn enabled(ini: Option<&Ini>, section: &str) -> Option<bool> {
    ini.and_then(|i| i.get(section, "Enabled"))
        .and_then(|s| match s.trim().to_ascii_lowercase().as_str() {
            "true" | "1" => Some(true),
            "false" | "0" => Some(false),
            _ => None,
        })
}
pub fn diagnose(input: &Inputs<'_>) -> Vec<Finding> {
    let mut findings = Vec::new();
    let mut add = |id, severity, title_key, why_key, evidence, repair| {
        findings.push(Finding {
            id,
            severity,
            title_key,
            why_key,
            evidence,
            repair,
        })
    };
    let state = input.status;
    if input.pending_journal
        || state
            .receipt
            .as_ref()
            .is_some_and(|r| r.status != ReceiptStatus::Complete)
    {
        add(
            "unfinished_install",
            Severity::Error,
            "doctor.unfinished.title",
            "doctor.unfinished.why",
            vec![
                FactEvidence::new(
                    "evidence.receipt",
                    format!("{:?}", state.receipt.as_ref().map(|r| &r.status)),
                ),
                FactEvidence::new("evidence.pending", input.pending_journal.to_string()),
            ],
            Some(Repair::ReconcileInstallation),
        );
    }
    if state.version_mismatch() {
        add(
            "version_mismatch",
            Severity::Warning,
            "doctor.version.title",
            "doctor.version.why",
            vec![
                FactEvidence::new(
                    "evidence.proxy_version",
                    state.proxy_version.as_deref().unwrap_or_default(),
                ),
                FactEvidence::new(
                    "evidence.forwarder_version",
                    state.forwarder_version.as_deref().unwrap_or_default(),
                ),
            ],
            Some(Repair::Reinstall),
        );
    }
    // Only use status' adjacent-model check for auto; it knows nothing about ModelFile overrides.
    let configured = input.ini.and_then(|i| i.get("DlssNr", "ModelFile"));
    let present = input.model.map(|m| m.present).unwrap_or_else(|| {
        configured
            .filter(|s| !s.trim().is_empty() && !s.eq_ignore_ascii_case("auto"))
            .map_or(Some(state.model_present), |_| None)
    });
    if enabled(input.ini, "DlssNr").or(state.nr_enabled) == Some(true) && present == Some(false) {
        let mut evidence = vec![
            FactEvidence::new("evidence.nr_enabled", "true"),
            FactEvidence::new("evidence.model_present", "false"),
        ];
        let catalog_alt = input
            .catalog
            .iter()
            .find(|m| m.compatibility.for_gpu(input.gpu) == Compatibility::Compatible);
        let repair = if input.allow_model_fetch && input.runtime != Runtime::Unknown {
            if let Some(m) = catalog_alt {
                evidence.push(FactEvidence::new("evidence.catalog_model", &m.sha256));
                Some(setting(
                    "DlssNr",
                    "ModelFile",
                    &crate::models::Catalog::model_file_value(m, input.runtime.is_wine()),
                ))
            } else if let Some(remote) = crate::model_fetch::pick_pinned_for_gpu(input.gpu) {
                evidence.push(FactEvidence::new("evidence.community_model", &remote.sha256));
                Some(Repair::FetchCompatibleModel { sha256: remote.sha256 })
            } else {
                Some(setting("DlssNr", "Enabled", "false"))
            }
        } else {
            Some(setting("DlssNr", "Enabled", "false"))
        };
        add(
            "model_missing",
            Severity::Error,
            "doctor.model_missing.title",
            "doctor.model_missing.why",
            evidence,
            repair,
        );
    }
    if let Some(model) = input.model.filter(|m| m.present == Some(true)) {
        let (compatibility, alternative) = recommend::nr_model(&model.compatibility, input.gpu, input.catalog);
        if compatibility == Compatibility::Incompatible {
            let mut evidence = vec![
                FactEvidence::new("evidence.model_targets", format!("{:?}", model.compatibility.archs)),
                FactEvidence::new(
                    "evidence.gpu",
                    format!("{:?}", input.gpu.map(|g| (&g.vendor, g.compute_cap))),
                ),
            ];
            if let Some(m) = alternative {
                evidence.push(FactEvidence::new("evidence.catalog_model", &m.sha256));
            }
            let repair = if let Some(m) = alternative.filter(|_| input.runtime != Runtime::Unknown) {
                Some(setting(
                    "DlssNr",
                    "ModelFile",
                    &crate::models::Catalog::model_file_value(m, input.runtime.is_wine()),
                ))
            } else if input.allow_model_fetch
                && input.runtime != Runtime::Unknown
                && let Some(remote) = crate::model_fetch::pick_pinned_for_gpu(input.gpu)
            {
                evidence.push(FactEvidence::new("evidence.community_model", &remote.sha256));
                Some(Repair::FetchCompatibleModel { sha256: remote.sha256 })
            } else {
                None
            };
            add(
                "model_incompatible",
                Severity::Error,
                "doctor.model_incompatible.title",
                "doctor.model_incompatible.why",
                evidence,
                repair,
            );
        }
    }
    if enabled(input.ini, "LSFG") == Some(true) {
        if let Some(readiness) = input.lsfg.filter(|r| **r != LsfgReadiness::Ready) {
            add(
                "lsfg_not_ready",
                Severity::Error,
                "doctor.lsfg_ready.title",
                "doctor.lsfg_ready.why",
                vec![FactEvidence::new(
                    "evidence.lsfg_ready",
                    match readiness {
                        LsfgReadiness::NoLosslessScaling => "missing_assets",
                        _ => "missing_shader_cache",
                    },
                )],
                Some(setting("LSFG", "Enabled", "false")),
            );
        }
        let output = input.ini.and_then(|i| i.get("FrameGen", "FGOutput"));
        if !output.is_some_and(|s| s.trim().eq_ignore_ascii_case("fsrfg")) {
            add(
                "lsfg_output",
                Severity::Warning,
                "doctor.lsfg_output.title",
                "doctor.lsfg_output.why",
                vec![FactEvidence::new("evidence.fg_output", output.unwrap_or("auto"))],
                Some(setting("FrameGen", "FGOutput", "fsrfg")),
            );
        }
        if input.sm86_present || !input.components.is_empty() {
            add(
                "fg_conflict",
                Severity::Error,
                "doctor.fg_conflict.title",
                "doctor.fg_conflict.why",
                vec![
                    FactEvidence::new("evidence.sm86", "true"),
                    FactEvidence::new("evidence.lsfg", "true"),
                ],
                Some(Repair::Settings(
                    recommend::lsfg_off_edits()
                        .into_iter()
                        .map(|(section, key, value)| Action::SetIni { section, key, value })
                        .collect(),
                )),
            );
        }
    }
    if input.runtime.is_wine()
        && input.override_present == Some(false)
        && input.proxy_loaded_last_session != Some(true)
        && !state.optiscaler_proxies.is_empty()
        && let Some(option) = &state.launch_option
    {
        add(
            "launch_override",
            Severity::Warning,
            "doctor.launch.title",
            "doctor.launch.why",
            vec![
                FactEvidence::new("evidence.launch_option", option),
                FactEvidence::new("evidence.override", "false"),
            ],
            None,
        );
    }
    if let Some(log) = input.log {
        let compatibility = input
            .model
            .map_or(Compatibility::Unknown, |m| m.compatibility.for_gpu(input.gpu));
        for diagnosis in logs::diagnose(log, input.runtime, compatibility, input.driver_nvngx_present) {
            findings.push(Finding {
                id: diagnosis.id,
                severity: Severity::Warning,
                title_key: diagnosis.title_key,
                why_key: diagnosis.why_key,
                evidence: vec![FactEvidence::new("evidence.log", diagnosis.evidence_line)],
                repair: diagnosis.repair,
            });
        }
    }
    findings
}

/// Pure DLSS diagnostics. An unknown version never proves that a DLL is outdated.
pub fn diagnose_dlss(files: &[crate::dlss::GameDll], root_incomplete: bool) -> Vec<Finding> {
    let mut findings = Vec::new();
    if root_incomplete {
        findings.push(Finding {
            id: "dlss_root_incomplete",
            severity: Severity::Error,
            title_key: "doctor.dlss_root.title",
            why_key: "doctor.dlss_root.why",
            evidence: vec![],
            repair: Some(Repair::RestoreDlss),
        });
    }
    let old = files
        .iter()
        .filter(|d| d.version.as_deref().is_some_and(crate::dlss::older))
        .collect::<Vec<_>>();
    if !old.is_empty() {
        findings.push(Finding {
            id: "dlss_outdated",
            severity: Severity::Info,
            title_key: "doctor.dlss.title",
            why_key: "doctor.dlss.why",
            evidence: old
                .iter()
                .map(|d| {
                    FactEvidence::new(
                        "evidence.dlss_file_version",
                        format!("{}: {}", d.path, d.version.as_deref().unwrap_or_default()),
                    )
                })
                .collect(),
            repair: Some(Repair::UpdateDlss),
        });
    }
    findings
}

/// Static add-on candidates are evidence of a missing prerequisite, not proof of a successful load.
pub fn diagnose_reshade(snapshot: &crate::reshade::Snapshot) -> Vec<Finding> {
    crate::reshade::recommendation(snapshot)
        .map(|r| Finding {
            id: "reshade_addons",
            severity: Severity::Warning,
            title_key: r.title_key,
            why_key: r.why_key,
            evidence: r.evidence,
            repair: Some(Repair::InstallReShade),
        })
        .into_iter()
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use retoque_core::receipt::Receipt;
    fn has(input: &Inputs<'_>, id: &str) -> bool {
        diagnose(input).iter().any(|f| f.id == id)
    }
    #[test]
    fn unfinished_receipt_and_pending_positive_negative() {
        let mut status = Status::default();
        assert!(diagnose(&Inputs::new(&status)).is_empty());
        for state in [
            ReceiptStatus::Complete,
            ReceiptStatus::Partial,
            ReceiptStatus::UninstallIncomplete,
        ] {
            let incomplete = state != ReceiptStatus::Complete;
            status.receipt = Some(Receipt {
                status: state,
                updated: String::new(),
                records: vec![],
            });
            assert_eq!(has(&Inputs::new(&status), "unfinished_install"), incomplete);
        }
        status.receipt = None;
        let mut input = Inputs::new(&status);
        input.pending_journal = true;
        assert!(has(&input, "unfinished_install"));
    }
    #[test]
    fn version_mismatch_requires_two_known_versions() {
        for (proxy, forwarder, expected) in [
            (None, None, false),
            (Some("1"), None, false),
            (Some("1"), Some("1"), false),
            (Some("1"), Some("2"), true),
        ] {
            let status = Status {
                proxy_version: proxy.map(str::to_owned),
                forwarder_version: forwarder.map(str::to_owned),
                ..Default::default()
            };
            assert_eq!(has(&Inputs::new(&status), "version_mismatch"), expected);
        }
    }
    #[test]
    fn model_missing_respects_override_unknown_and_disabled() {
        let status = Status {
            nr_enabled: Some(true),
            model_present: false,
            ..Default::default()
        };
        assert!(has(&Inputs::new(&status), "model_missing"));
        let ini = Ini::parse(b"[DlssNr]\nModelFile=selected.dll\nEnabled=true\n");
        let mut input = Inputs::new(&status);
        input.ini = Some(&ini);
        assert!(!has(&input, "model_missing"));
        for present in [None, Some(true), Some(false)] {
            let model = ModelInspection {
                present,
                ..Default::default()
            };
            let mut case = Inputs::new(&status);
            case.ini = Some(&ini);
            case.model = Some(&model);
            assert_eq!(has(&case, "model_missing"), present == Some(false));
        }
        let ini = Ini::parse(b"[DlssNr]\nEnabled=false\n");
        let mut input = Inputs::new(&status);
        input.ini = Some(&ini);
        assert!(!has(&input, "model_missing"));
    }
    #[test]
    fn incompatible_model_offers_only_static_compatible_catalog_alternative() {
        let status = Status::default();
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let mut model = ModelInspection {
            present: Some(true),
            compatibility: ModelCompatibility {
                known: true,
                archs: vec![120],
            },
        };
        let mut candidate = Model {
            sha256: "synthetic-hash".into(),
            version: None,
            size: 64,
            label: "Fixture".into(),
            path: "/synthetic/model.dll".into(),
            compatibility: ModelCompatibility {
                known: false,
                archs: vec![86],
            },
        };
        for known in [false, true] {
            candidate.compatibility.known = known;
            let catalog = [candidate.clone()];
            let mut input = Inputs::new(&status);
            input.model = Some(&model);
            input.gpu = Some(&gpu);
            input.catalog = &catalog;
            input.runtime = Runtime::Proton;
            let result = diagnose(&input);
            assert_eq!(result[0].id, "model_incompatible");
            assert_eq!(result[0].repair.is_some(), known);
            if known {
                assert!(
                    matches!(&result[0].repair, Some(Repair::Settings(actions)) if matches!(&actions[0], Action::SetIni {value, ..} if value.starts_with("Z:")))
                );
            }
        }
        for (present, known, arch) in [
            (None, true, 120),
            (Some(false), true, 120),
            (Some(true), false, 120),
            (Some(true), true, 86),
        ] {
            model.present = present;
            model.compatibility.known = known;
            model.compatibility.archs = vec![arch];
            let mut input = Inputs::new(&status);
            input.model = Some(&model);
            input.gpu = Some(&gpu);
            assert!(!has(&input, "model_incompatible"));
        }
    }
    #[test]
    fn lsfg_rules_positive_negative_and_unknown_readiness() {
        let status = Status::default();
        for active in [true, false] {
            for output in ["fsrfg", "auto"] {
                let ini = Ini::parse(format!("[LSFG]\nEnabled={active}\n[FrameGen]\nFGOutput={output}\n").as_bytes());
                for readiness in [
                    None,
                    Some(LsfgReadiness::Ready),
                    Some(LsfgReadiness::NoLosslessScaling),
                    Some(LsfgReadiness::NeedsShaderCache { hint: String::new() }),
                ] {
                    for sm86 in [false, true] {
                        let mut input = Inputs::new(&status);
                        input.ini = Some(&ini);
                        input.lsfg = readiness.as_ref();
                        input.sm86_present = sm86;
                        assert_eq!(
                            has(&input, "lsfg_not_ready"),
                            active && readiness.as_ref().is_some_and(|r| *r != LsfgReadiness::Ready)
                        );
                        assert_eq!(has(&input, "lsfg_output"), active && output != "fsrfg");
                        assert_eq!(has(&input, "fg_conflict"), active && sm86);
                    }
                }
            }
        }
        let ini = Ini::parse(b"[LSFG]\nEnabled=true\n[FrameGen]\nFGOutput=fsrfg\n");
        let components = vec!["synthetic-sm86-id".to_owned()];
        let mut input = Inputs::new(&status);
        input.ini = Some(&ini);
        input.components = &components;
        assert!(has(&input, "fg_conflict"));
    }
    #[test]
    fn launch_override_requires_positive_missing_evidence() {
        let status = Status {
            optiscaler_proxies: vec!["dxgi.dll".into()],
            launch_option: Some("WINEDLLOVERRIDES=\"dxgi=n,b\" %command%".into()),
            ..Default::default()
        };
        for runtime in [Runtime::Unknown, Runtime::Windows, Runtime::Proton, Runtime::Wine] {
            for present in [None, Some(false), Some(true)] {
                for loaded in [None, Some(false), Some(true)] {
                    let mut input = Inputs::new(&status);
                    input.runtime = runtime;
                    input.override_present = present;
                    input.proxy_loaded_last_session = loaded;
                    assert_eq!(
                        has(&input, "launch_override"),
                        runtime.is_wine() && present == Some(false) && loaded != Some(true)
                    );
                }
            }
        }
        let empty = Status::default();
        let mut input = Inputs::new(&empty);
        input.runtime = Runtime::Proton;
        input.override_present = Some(false);
        assert!(!has(&input, "launch_override"));
    }
    #[test]
    fn missing_or_incompatible_model_offers_community_fetch_when_enabled() {
        let status = Status {
            nr_enabled: Some(true),
            model_present: false,
            ..Default::default()
        };
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let mut input = Inputs::new(&status);
        input.gpu = Some(&gpu);
        input.runtime = Runtime::Proton;
        input.allow_model_fetch = true;
        let findings = diagnose(&input);
        assert_eq!(findings[0].id, "model_missing");
        assert_eq!(
            findings[0].repair,
            Some(Repair::FetchCompatibleModel {
                sha256: "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed".into()
            })
        );

        let incompat = ModelInspection {
            present: Some(true),
            compatibility: ModelCompatibility {
                known: true,
                archs: vec![120],
            },
        };
        let default_status = Status::default();
        let mut input = Inputs::new(&default_status);
        input.model = Some(&incompat);
        input.gpu = Some(&gpu);
        input.runtime = Runtime::Windows;
        input.allow_model_fetch = true;
        let findings = diagnose(&input);
        assert_eq!(findings[0].id, "model_incompatible");
        assert_eq!(
            findings[0].repair,
            Some(Repair::FetchCompatibleModel {
                sha256: "cdfa6c1ec784867182164c7836f324f087b5c9f374d0c9fe46f54999feabf3ed".into()
            })
        );
    }
}
