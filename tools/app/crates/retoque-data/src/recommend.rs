//! Pure proposals, never writes. Hardware identity is not runtime qualification; callers must resolve Needs.

use crate::{
    components::ComponentKind,
    gpu::{Arch, Gpu, Vendor},
};
use serde::Serialize;
use std::path::Path;

/// Unknown never silently means native Windows.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize)]
pub enum Runtime {
    #[default]
    Unknown,
    Windows,
    Proton,
    Wine,
}
impl Runtime {
    pub fn is_wine(self) -> bool {
        matches!(self, Self::Proton | Self::Wine)
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
pub enum Confidence {
    /// Reserved for feature-specific runtime evidence, never inferred from an API or NR initialization.
    VerifiedInLastSession,
    StaticMatchUntested,
    ExperimentalUnderProton,
}
impl Confidence {
    pub fn key(self) -> &'static str {
        match self {
            Self::VerifiedInLastSession => "confidence.verified",
            Self::StaticMatchUntested => "confidence.static",
            Self::ExperimentalUnderProton => "confidence.proton",
        }
    }
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct FactEvidence {
    pub key: &'static str,
    pub value: String,
}
impl FactEvidence {
    pub fn new(key: &'static str, value: impl Into<String>) -> Self {
        Self {
            key,
            value: value.into(),
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq, Serialize)]
pub struct GameFacts {
    pub runtime: Runtime,
    /// Explicit API from the most recent log; hook installation alone is not evidence.
    pub last_session_api: Option<String>,
    pub has_dlss: bool,
    pub has_dlssg: bool,
    pub installed: bool,
    /// A recognized OptiScaler proxy exists, but core found no valid receipt.
    pub untracked: bool,
    pub proxy: Option<String>,
    pub sm86_installed: bool,
    pub sm86_legacy: bool,
    pub lsfg_enabled: Option<bool>,
}

/// Combine already inspected facts without filesystem access. Only core's recognized proxies prove installation;
/// an arbitrary proxy filename or a stale receipt alone does not. Components are IDs from installed_components.
/// Missing status remains unknown to the caller, which must suppress proposals when inspection failed.
pub fn facts_for(
    game: &retoque_discovery::Game,
    target: &retoque_discovery::TargetDir,
    status: Option<&retoque_core::Status>,
    components: &[String],
) -> GameFacts {
    let has = |name: &str| {
        target
            .dlss
            .iter()
            .chain(&game.dlss_anywhere)
            .any(|s| s.eq_ignore_ascii_case(name))
    };
    let loader = |name: &str| target.loaders.iter().any(|s| s.eq_ignore_ascii_case(name));
    let proxy = status.and_then(|s| s.optiscaler_proxies.first()).cloned();
    let installed = proxy.is_some();
    let sm86_legacy = loader("dlssg_for_sm86-legacy");
    GameFacts {
        has_dlss: has("nvngx_dlss.dll"),
        has_dlssg: has("nvngx_dlssg.dll"),
        installed,
        untracked: installed && status.is_some_and(|s| s.receipt.is_none()),
        proxy,
        sm86_installed: !components.is_empty() || loader("dlssg_for_sm86") || sm86_legacy,
        sm86_legacy,
        lsfg_enabled: None,
        runtime: match game.runtime {
            retoque_discovery::Runtime::Windows => Runtime::Windows,
            retoque_discovery::Runtime::Proton { .. } => Runtime::Proton,
            retoque_discovery::Runtime::Wine { .. } => Runtime::Wine,
            _ => Runtime::Unknown,
        },
        last_session_api: None,
    }
}

/// Use the caller's cached/read INI; never read it on the GUI thread as part of fact construction.
pub fn facts_for_with_ini(
    game: &retoque_discovery::Game,
    target: &retoque_discovery::TargetDir,
    status: Option<&retoque_core::Status>,
    components: &[String],
    ini: Option<&retoque_core::ini::Ini>,
) -> GameFacts {
    let mut facts = facts_for(game, target, status, components);
    facts.lsfg_enabled = ini.and_then(|ini| ini.get("LSFG", "Enabled")).and_then(|value| {
        match value.trim().to_ascii_lowercase().as_str() {
            "true" | "1" => Some(true),
            "false" | "0" => Some(false),
            _ => None,
        }
    });
    facts
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub enum Action {
    SetIni {
        section: String,
        key: String,
        value: String,
    },
    InstallComponent(ComponentKind),
    RemoveComponent(ComponentKind),
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub struct Recommendation {
    pub id: &'static str,
    pub title_key: &'static str,
    pub why_key: &'static str,
    pub actions: Vec<Action>,
    pub needs: Option<Need>,
    pub evidence: Vec<FactEvidence>,
    pub confidence: Confidence,
    /// Additional qualification text from messages::text; keeps existing GUI why keys stable.
    pub caveat_key: Option<&'static str>,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
pub enum Need {
    Component(ComponentKind),
    LosslessScaling,
    RetoqueInstalled,
}

impl Recommendation {
    /// The GUI can omit this card when its primary Install/Update banner already offers the same plan.
    pub fn is_primary_install_action(&self) -> bool {
        matches!(self.needs, Some(Need::RetoqueInstalled))
    }
}

/// Needs remain unresolved here: in particular LosslessScaling includes a Ready shader-cache check by the UI.
/// This function has no filesystem access and cannot claim assets or converted shaders are available.
pub fn recommend(gpu: Option<&Gpu>, game: &GameFacts) -> Vec<Recommendation> {
    if !game.installed {
        return vec![Recommendation {
            id: "install_retoque",
            title_key: "rec.install.title",
            why_key: "rec.install.why",
            actions: vec![],
            needs: Some(Need::RetoqueInstalled),
            evidence: evidence(gpu, game),
            confidence: Confidence::StaticMatchUntested,
            caveat_key: None,
        }];
    }
    if game.untracked {
        return vec![Recommendation {
            id: "adopt_retoque",
            title_key: "rec.adopt.title",
            why_key: "rec.adopt.why",
            actions: vec![],
            needs: Some(Need::RetoqueInstalled),
            evidence: evidence(gpu, game),
            confidence: Confidence::StaticMatchUntested,
            caveat_key: None,
        }];
    }
    let Some(gpu) = gpu else {
        return vec![];
    };
    if gpu.vendor != Vendor::Nvidia {
        return vec![];
    }
    if matches!(gpu.arch, Arch::Ampere | Arch::Turing)
        && game.has_dlssg
        && (!game.sm86_installed || game.sm86_legacy)
        && game.last_session_api.as_deref().is_none_or(|api| api == "D3D12")
    {
        let mut actions = vec![Action::InstallComponent(ComponentKind::DlssgSm86)];
        actions.extend(ini_actions(lsfg_off_edits()));
        return vec![Recommendation {
            id: if game.sm86_legacy { "sm86_update" } else { "sm86_dlssg" },
            title_key: if game.sm86_legacy {
                "rec.sm86_update.title"
            } else {
                "rec.sm86.title"
            },
            why_key: if game.sm86_legacy {
                "rec.sm86_update.why"
            } else {
                "rec.sm86.why"
            },
            actions,
            needs: Some(Need::Component(ComponentKind::DlssgSm86)),
            evidence: {
                let mut facts = evidence(Some(gpu), game);
                facts.push(FactEvidence::new(
                    "evidence.sm86_docs",
                    "dlssg_for_sm86 0.4.1 README.en.md:92-98; MANUAL.en.html:141",
                ));
                facts
            },
            caveat_key: game.runtime.is_wine().then_some("confidence.sm86_proton"),
            confidence: if game.runtime.is_wine() {
                Confidence::ExperimentalUnderProton
            } else {
                Confidence::StaticMatchUntested
            },
        }];
    }
    if matches!(gpu.arch, Arch::Turing | Arch::Ampere | Arch::Ada | Arch::Blackwell)
        && !game.has_dlssg
        && !game.sm86_installed
        && !game.sm86_legacy
        && game.lsfg_enabled != Some(true)
    {
        let mut edits = lsfg_edits(2, None, false);
        edits.extend(lsfg_input_edits(game));
        return vec![Recommendation {
            id: "lsfg_x2",
            title_key: "rec.lsfg.title",
            why_key: "rec.lsfg.why",
            actions: ini_actions(edits),
            needs: Some(Need::LosslessScaling),
            evidence: evidence(Some(gpu), game),
            caveat_key: None,
            confidence: if game.runtime.is_wine() {
                Confidence::ExperimentalUnderProton
            } else {
                Confidence::StaticMatchUntested
            },
        }];
    }
    vec![]
}

fn ini_actions(edits: Vec<(String, String, String)>) -> Vec<Action> {
    edits
        .into_iter()
        .map(|(section, key, value)| Action::SetIni { section, key, value })
        .collect()
}
fn edit(section: &str, key: &str, value: &str) -> (String, String, String) {
    (section.into(), key.into(), value.into())
}

/// Game-independent settings. Invalid multipliers produce no edits; callers should offer only x2/x3/x4.
/// None preserves the existing DllPath; ShaderDir and FGInput are always preserved.
pub fn lsfg_edits(multiplier: u8, lossless_dir: Option<&Path>, wine: bool) -> Vec<(String, String, String)> {
    if !(2..=4).contains(&multiplier) {
        return vec![];
    }
    let mut edits = vec![
        edit("FrameGen", "Enabled", "true"),
        edit("FrameGen", "FGOutput", "fsrfg"),
        edit("LSFG", "Enabled", "true"),
        edit("LSFG", "Multiplier", &multiplier.to_string()),
    ];
    if let Some(dir) = lossless_dir {
        let value = if wine {
            retoque_core::wine_path(dir)
        } else {
            dir.to_string_lossy().into_owned()
        };
        edits.push(edit("LSFG", "DllPath", &value));
    }
    edits
}

pub fn lsfg_input_edits(game: &GameFacts) -> Vec<(String, String, String)> {
    if game.has_dlss {
        vec![edit("FrameGen", "FGInput", "upscaler")]
    } else {
        vec![
            edit("FrameGen", "FGInput", "synthesized"),
            edit("FrameGen", "SynthesizedMotion", "true"),
        ]
    }
}

/// Disable app frame generation and restore automatic routing, leaving the game's own FG provider to the game.
/// This is an explicit off plan, not a reconstruction of settings that predated LSFG.
pub fn lsfg_off_edits() -> Vec<(String, String, String)> {
    vec![
        edit("LSFG", "Enabled", "false"),
        edit("FrameGen", "Enabled", "false"),
        edit("FrameGen", "FGInput", "auto"),
        edit("FrameGen", "FGOutput", "auto"),
    ]
}

#[cfg(test)]
mod tests {
    use super::*;
    fn gpu(arch: Arch) -> Gpu {
        Gpu {
            name: "Synthetic GPU".into(),
            vendor: Vendor::Nvidia,
            arch,
            compute_cap: None,
            driver: None,
            vram_mib: None,
        }
    }
    fn fixture() -> (
        retoque_discovery::Game,
        retoque_discovery::TargetDir,
        retoque_core::Status,
    ) {
        use retoque_discovery::{Game, Runtime, Store, TargetDir};
        let target = TargetDir {
            path: "synthetic-game/Binaries/Win64".into(),
            executables: vec!["game.exe".into()],
            score: 0,
            dlss: vec![],
            has_receipt: false,
            proxies_present: vec![],
            loaders: vec![],
        };
        let game = Game {
            id: "synthetic:game".into(),
            name: "Synthetic game".into(),
            store: Store::Manual,
            install_dir: "synthetic-game".into(),
            runtime: Runtime::Windows,
            targets: vec![target.clone()],
            dlss_anywhere: vec![],
            registered: true,
        };
        (game, target, retoque_core::Status::default())
    }

    fn receipt() -> retoque_core::receipt::Receipt {
        retoque_core::receipt::Receipt {
            status: retoque_core::receipt::ReceiptStatus::Complete,
            updated: String::new(),
            records: vec![retoque_core::receipt::Record::Proxy {
                name: "dxgi.dll".into(),
            }],
        }
    }

    #[test]
    fn shared_facts_use_verified_proxies_and_all_discovered_dlss() {
        let (mut game, mut target, mut status) = fixture();
        target.dlss = vec!["NVNGX_DLSS.DLL".into()];
        game.dlss_anywhere = vec!["NvNgX_DlSsG.DlL".into()];
        target.proxies_present = vec!["version.dll".into()];
        target.has_receipt = true;
        status.receipt = Some(receipt());
        let facts = facts_for(&game, &target, Some(&status), &[]);
        assert!(facts.has_dlss && facts.has_dlssg);
        assert!(
            !facts.installed,
            "a stale receipt or foreign proxy is not an installed Retoque"
        );
        assert!(!facts_for(&game, &target, None, &[]).installed);
        status.optiscaler_proxies = vec!["winmm.dll".into()];
        let facts = facts_for(&game, &target, Some(&status), &[]);
        assert!(facts.installed && !facts.untracked);
        assert_eq!(facts.proxy.as_deref(), Some("winmm.dll"));
        status.receipt = None;
        let facts = facts_for(&game, &target, Some(&status), &[]);
        assert!(facts.installed && facts.untracked);
        let adoption = recommend(None, &facts);
        assert_eq!(adoption[0].id, "adopt_retoque");
        assert!(adoption[0].actions.is_empty() && adoption[0].is_primary_install_action());
        target.dlss = vec!["sl.interposer.dll".into(), "nvngx_dlssd.dll".into()];
        game.dlss_anywhere.clear();
        let facts = facts_for(&game, &target, Some(&status), &[]);
        assert!(
            !facts.has_dlss && !facts.has_dlssg,
            "interposer alone proves neither feature"
        );
    }

    #[test]
    fn ini_variant_shares_boolean_parsing_without_io() {
        let (game, target, status) = fixture();
        assert_eq!(
            facts_for_with_ini(&game, &target, Some(&status), &[], None).lsfg_enabled,
            None
        );
        for (value, expected) in [
            ("true", Some(true)),
            ("TRUE", Some(true)),
            ("1", Some(true)),
            ("false", Some(false)),
            ("False", Some(false)),
            ("0", Some(false)),
            ("auto", None),
            ("invalid", None),
            ("", None),
        ] {
            let ini = retoque_core::ini::Ini::parse(format!("[lsfg]\nEnabled=  {value}  \n").as_bytes());
            let facts = facts_for_with_ini(&game, &target, Some(&status), &[], Some(&ini));
            assert_eq!(facts.lsfg_enabled, expected, "{value}");
            assert_eq!(facts_for(&game, &target, Some(&status), &[]).lsfg_enabled, None);
        }
    }

    #[test]
    fn field_scenarios_offer_only_adoption_until_tracked() {
        for (name, target_dlss, install_dlss, loader, lsfg, installed) in [
            (
                "Mortal Shell II",
                vec![],
                vec!["nvngx_dlss.dll", "nvngx_dlssg.dll"],
                Some("dlssg_for_sm86"),
                "auto",
                true,
            ),
            (
                "Dying Light The Beast",
                vec!["nvngx_dlss.dll", "nvngx_dlssg.dll"],
                vec![],
                Some("dlssg_for_sm86"),
                "auto",
                true,
            ),
            ("MISERY", vec![], vec![], None, "true", true),
            ("Hellraiser", vec![], vec![], None, "auto", false),
        ] {
            let (mut game, mut target, mut status) = fixture();
            game.name = name.into();
            game.dlss_anywhere = install_dlss.into_iter().map(String::from).collect();
            target.dlss = target_dlss.into_iter().map(String::from).collect();
            target.loaders = loader.into_iter().map(String::from).collect();
            if installed {
                status.optiscaler_proxies.push("dxgi.dll".into());
            }
            let ini = retoque_core::ini::Ini::parse(format!("[LSFG]\nEnabled={lsfg}\n").as_bytes());
            let facts = facts_for_with_ini(&game, &target, Some(&status), &[], Some(&ini));
            let result = recommend(Some(&gpu(Arch::Ampere)), &facts);
            assert_eq!(result.len(), 1, "{name}");
            assert_eq!(
                result[0].id,
                if installed { "adopt_retoque" } else { "install_retoque" },
                "{name}"
            );
            assert!(result[0].actions.is_empty() && result[0].is_primary_install_action());
            if installed {
                status.receipt = Some(receipt());
                let tracked = facts_for_with_ini(&game, &target, Some(&status), &[], Some(&ini));
                assert!(recommend(Some(&gpu(Arch::Ampere)), &tracked).is_empty(), "{name}");
            }
        }
    }

    #[test]
    fn foreign_owned_and_legacy_sm86_block_lsfg_and_legacy_can_update() {
        let (mut game, mut target, mut status) = fixture();
        status.optiscaler_proxies.push("dxgi.dll".into());
        status.receipt = Some(receipt());
        for (loaders, components, legacy) in [
            (vec!["DLSSG_FOR_SM86"], vec![], false),
            (vec!["dlssg_for_sm86-legacy"], vec![], true),
            (vec![], vec!["synthetic-sm86-component-id"], false),
            (vec!["reshade", "special_k"], vec!["synthetic-sm86-component-id"], false),
        ] {
            target.loaders = loaders.into_iter().map(String::from).collect();
            let components: Vec<String> = components.into_iter().map(String::from).collect();
            game.dlss_anywhere.clear();
            let facts = facts_for(&game, &target, Some(&status), &components);
            assert!(facts.sm86_installed);
            assert_eq!(facts.sm86_legacy, legacy);
            assert!(recommend(Some(&gpu(Arch::Ampere)), &facts).is_empty());
            game.dlss_anywhere.push("nvngx_dlssg.dll".into());
            let facts = facts_for(&game, &target, Some(&status), &components);
            let result = recommend(Some(&gpu(Arch::Ampere)), &facts);
            if legacy {
                assert_eq!(result[0].id, "sm86_update");
                assert_eq!(result[0].needs, Some(Need::Component(ComponentKind::DlssgSm86)));
                assert!(!result[0].is_primary_install_action());
                assert_eq!(result[0].actions[0], Action::InstallComponent(ComponentKind::DlssgSm86));
                assert!(result[0].actions.contains(&Action::SetIni {
                    section: "LSFG".into(),
                    key: "Enabled".into(),
                    value: "false".into(),
                }));
                assert!(recommend(Some(&gpu(Arch::Ada)), &facts).is_empty());
            } else {
                assert!(result.is_empty());
            }
        }
        target.loaders = vec!["reshade".into(), "special_k".into()];
        assert!(!facts_for(&game, &target, Some(&status), &[]).sm86_installed);
    }

    #[test]
    fn architecture_and_game_matrix_never_combines_providers() {
        for arch in [
            Arch::Turing,
            Arch::Ampere,
            Arch::Ada,
            Arch::Blackwell,
            Arch::OlderNvidia,
            Arch::Unknown,
        ] {
            for installed in [false, true] {
                for has_dlss in [false, true] {
                    for has_dlssg in [false, true] {
                        for sm86_installed in [false, true] {
                            let facts = GameFacts {
                                installed,
                                has_dlss,
                                has_dlssg,
                                sm86_installed,
                                ..Default::default()
                            };
                            let r = recommend(Some(&gpu(arch)), &facts);
                            let expected = if !installed {
                                Some("install_retoque")
                            } else if matches!(arch, Arch::Turing | Arch::Ampere) && has_dlssg && !sm86_installed {
                                Some("sm86_dlssg")
                            } else if matches!(arch, Arch::Turing | Arch::Ampere | Arch::Ada | Arch::Blackwell)
                                && !has_dlssg
                                && !sm86_installed
                            {
                                Some("lsfg_x2")
                            } else {
                                None
                            };
                            assert_eq!(r.first().map(|r| r.id), expected, "{arch:?} {facts:?}");
                            assert!(r.len() <= 1);
                            if let Some(r) = r.first() {
                                assert!(
                                    r.needs.is_some(),
                                    "pure recommendation cannot resolve runtime prerequisites"
                                );
                                if r.id == "sm86_dlssg" {
                                    assert!(!r.actions.contains(&Action::SetIni {
                                        section: "LSFG".into(),
                                        key: "Enabled".into(),
                                        value: "true".into()
                                    }));
                                }
                            }
                        }
                    }
                }
            }
        }
        assert!(
            recommend(
                None,
                &GameFacts {
                    installed: true,
                    ..Default::default()
                }
            )
            .is_empty()
        );
        assert!(
            recommend(
                Some(&gpu(Arch::Ampere)),
                &GameFacts {
                    installed: true,
                    lsfg_enabled: Some(true),
                    ..Default::default()
                }
            )
            .is_empty()
        );
    }
    #[test]
    fn lsfg_edits_match_schema_and_preserve_unrelated_settings() {
        for n in [2, 3, 4] {
            let edits = lsfg_edits(n, Some(Path::new("/synthetic/LS")), true);
            assert!(
                edits
                    .iter()
                    .any(|(s, k, v)| s == "LSFG" && k == "DllPath" && v.starts_with("Z:"))
            );
            assert!(
                !edits
                    .iter()
                    .any(|(_, k, _)| matches!(k.as_str(), "ShaderDir" | "FGInput"))
            );
            for (section, key, value) in edits
                .into_iter()
                .chain(lsfg_off_edits())
                .chain(lsfg_input_edits(&GameFacts::default()))
            {
                if let Some(setting) = crate::settings::all()
                    .iter()
                    .find(|s| s.section == section && s.key == key)
                {
                    assert!(setting.accepts(&value), "invalid {section}.{key}={value}");
                }
            }
        }
        for n in [0, 1, 5, 255] {
            assert!(lsfg_edits(n, None, false).is_empty());
        }
        assert_eq!(
            lsfg_input_edits(&GameFacts {
                has_dlss: true,
                ..Default::default()
            }),
            vec![edit("FrameGen", "FGInput", "upscaler")]
        );
        let ini = include_str!("../../../../../OptiScaler.ini");
        assert!(ini.contains("SynthesizedMotion=auto"));
    }
}

/// NR model rule shared by the model picker and autotune; no writes and no guessed GPU architecture.
/// A candidate is suggested only for a recognized model with matching CUDA binary targets.
pub fn nr_model<'a>(
    current: &crate::model_compat::ModelCompatibility,
    gpu: Option<&Gpu>,
    catalog: &'a [crate::models::Model],
) -> (crate::model_compat::Compatibility, Option<&'a crate::models::Model>) {
    use crate::model_compat::Compatibility;
    let state = current.for_gpu(gpu);
    let alternative = if state == Compatibility::Incompatible {
        catalog
            .iter()
            .find(|m| m.compatibility.for_gpu(gpu) == Compatibility::Compatible)
    } else {
        None
    };
    (state, alternative)
}

#[cfg(test)]
mod nr_tests {
    use super::*;
    use crate::{
        model_compat::{Compatibility, ModelCompatibility},
        models::Model,
    };
    #[test]
    fn nr_rule_suggests_only_recognized_matching_catalog_models() {
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let current = ModelCompatibility {
            archs: vec![120],
            known: true,
        };
        let mut candidate = Model {
            sha256: "synthetic".into(),
            version: None,
            size: 64,
            label: "Synthetic compatible".into(),
            path: "model.dll".into(),
            compatibility: ModelCompatibility {
                archs: vec![75, 86, 89, 120],
                known: false,
            },
        };
        assert!(nr_model(&current, Some(&gpu), &[candidate.clone()]).1.is_none());
        candidate.compatibility.known = true;
        let catalog = [candidate];
        let (state, suggested) = nr_model(&current, Some(&gpu), &catalog);
        assert_eq!(state, Compatibility::Incompatible);
        assert_eq!(suggested.unwrap().sha256, "synthetic");
        assert!(nr_model(&current, None, &catalog).1.is_none());
        assert!(
            nr_model(&ModelCompatibility::default(), Some(&gpu), &catalog)
                .1
                .is_none()
        );
    }
}

fn evidence(gpu: Option<&Gpu>, game: &GameFacts) -> Vec<FactEvidence> {
    let mut facts = vec![
        FactEvidence::new("evidence.installed", game.installed.to_string()),
        FactEvidence::new("evidence.tracked", (game.installed && !game.untracked).to_string()),
        FactEvidence::new("evidence.runtime", format!("{:?}", game.runtime)),
        FactEvidence::new("evidence.dlss", game.has_dlss.to_string()),
        FactEvidence::new("evidence.dlssg", game.has_dlssg.to_string()),
        FactEvidence::new("evidence.sm86", game.sm86_installed.to_string()),
        FactEvidence::new("evidence.sm86_legacy", game.sm86_legacy.to_string()),
        FactEvidence::new("evidence.lsfg", format!("{:?}", game.lsfg_enabled)),
    ];
    if let Some(gpu) = gpu {
        facts.push(FactEvidence::new(
            "evidence.gpu",
            format!("{:?} {:?} {:?}", gpu.vendor, gpu.arch, gpu.compute_cap),
        ));
    }
    if let Some(api) = &game.last_session_api {
        facts.push(FactEvidence::new("evidence.api", api));
    }
    facts
}

#[cfg(test)]
mod confidence_tests {
    use super::*;
    #[test]
    fn sm86_runtime_and_api_are_evidence_not_qualification() {
        let gpu = crate::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        for runtime in [Runtime::Unknown, Runtime::Windows, Runtime::Proton, Runtime::Wine] {
            for api in [None, Some("D3D12"), Some("D3D11"), Some("Vulkan")] {
                let facts = GameFacts {
                    installed: true,
                    has_dlssg: true,
                    runtime,
                    last_session_api: api.map(str::to_owned),
                    ..Default::default()
                };
                let result = recommend(Some(&gpu), &facts);
                if api.is_some_and(|a| a != "D3D12") {
                    assert!(result.is_empty());
                    continue;
                }
                assert_eq!(
                    result[0].confidence,
                    if runtime.is_wine() {
                        Confidence::ExperimentalUnderProton
                    } else {
                        Confidence::StaticMatchUntested
                    }
                );
                assert!(result[0].evidence.iter().any(|e| e.key == "evidence.sm86_docs"));
                assert_eq!(
                    result[0].evidence.iter().any(|e| e.key == "evidence.api"),
                    api.is_some()
                );
                if runtime.is_wine() {
                    assert_eq!(result[0].caveat_key, Some("confidence.sm86_proton"));
                }
            }
        }
    }
}
