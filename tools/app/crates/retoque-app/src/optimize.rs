//! Compose one review without writes. Prerequisites touch app data only and must finish before `prepare`.
use super::{ComponentChange, Request, components, recommend, sm86};
use crate::{error::AppError, model::Edit};
use std::path::PathBuf;

pub fn first_run_eligible(gpus: &[retoque_data::gpu::Gpu]) -> bool {
    retoque_data::gpu::primary(gpus).is_some_and(|gpu| {
        gpu.vendor == retoque_data::gpu::Vendor::Nvidia
            && gpu
                .name
                .as_bytes()
                .windows(6)
                .any(|name| name.eq_ignore_ascii_case(b"RTX 20") || name.eq_ignore_ascii_case(b"RTX 30"))
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Prerequisite {
    Sm86,
    LsfgShaders,
    NrModel,
}
impl Prerequisite {
    pub fn unavailable_key(self) -> &'static str {
        match self {
            Self::Sm86 => "sm86.download_help",
            Self::LsfgShaders => "optimize.lsfg_unavailable",
            Self::NrModel => "nr_fetch.unavailable",
        }
    }
}
#[derive(Default)]
pub struct Selection<'a> {
    pub install: Option<retoque_core::InstallOptions>,
    pub recommendations: &'a [recommend::Recommendation],
    pub component: Option<&'a components::Component>,
    pub installed_ids: &'a [String],
    pub model_edit: Option<Edit>,
    pub need_nr_model: bool,
    pub include_experimental: bool,
    pub choose_lsfg: bool,
    pub lsfg_ready: bool,
    pub lsfg_edits: Vec<Edit>,
}
pub struct Composition {
    pub request: Request,
    pub prerequisites: Vec<Prerequisite>,
    pub experimental_omitted: bool,
}
pub fn recommendations_for_optimize(
    current: &[recommend::Recommendation],
    gpu: Option<&retoque_data::gpu::Gpu>,
    facts: &recommend::GameFacts,
    has_install_inputs: bool,
) -> Vec<recommend::Recommendation> {
    let needs_install = current.iter().any(|r| r.is_primary_install_action())
        || facts.untracked
        || (!facts.installed && has_install_inputs);
    if !needs_install {
        return current.to_vec();
    }
    let mut out = if current.is_empty() {
        recommend::recommend(gpu, facts)
    } else {
        current.to_vec()
    };
    if !out.iter().any(|r| !r.is_primary_install_action()) {
        let mut post_install = facts.clone();
        post_install.installed = true;
        post_install.untracked = false;
        out.extend(recommend::recommend(gpu, &post_install));
    }
    out
}
pub fn compose(selection: Selection<'_>) -> Result<Composition, &'static str> {
    let mut request = Request::default();
    let mut prerequisites = Vec::new();
    let mut experimental_omitted = false;
    let mut lsfg_omitted = false;
    let mut need_install = false;
    let mut adopt_legacy = false;
    for rec in selection.recommendations {
        // ReShade/add-ons are always an explicit user operation, never a one-click prerequisite.
        if rec.needs
            == Some(recommend::Need::Component(
                retoque_data::components::ComponentKind::ReShade,
            ))
        {
            continue;
        }
        if rec.is_primary_install_action() {
            need_install = true;
            adopt_legacy |= rec.id == "adopt_retoque";
            continue;
        }
        if rec.confidence == recommend::Confidence::ExperimentalUnderProton && !selection.include_experimental {
            experimental_omitted = true;
            continue;
        }
        if rec.needs == Some(recommend::Need::LosslessScaling) {
            if !selection.choose_lsfg {
                lsfg_omitted = true;
                continue;
            }
            if !selection.lsfg_ready {
                prerequisites.push(Prerequisite::LsfgShaders);
            } else {
                request.edits.extend(selection.lsfg_edits.clone());
            }
            continue;
        }
        for action in &rec.actions {
            match action {
                recommend::Action::SetIni { section, key, value } => {
                    request.edits.push((section.clone(), key.clone(), value.clone()))
                }
                recommend::Action::InstallComponent(_) => {
                    if let Some(c) = selection.component {
                        request.set_component(ComponentChange::Install(c.clone()));
                    } else {
                        prerequisites.push(Prerequisite::Sm86);
                    }
                }
                recommend::Action::RemoveComponent(_) => {
                    if let Some(id) = selection.installed_ids.first() {
                        request.set_component(ComponentChange::Remove(id.clone()));
                    } else {
                        return Err("optimize.identity_missing");
                    }
                }
            }
        }
    }
    if let Some(edit) = selection.model_edit {
        request.edits.push(edit);
    } else if selection.need_nr_model {
        prerequisites.push(Prerequisite::NrModel);
    }
    // Duplicate/conflicting recommendations must never silently change the resulting setting.
    let mut values = std::collections::BTreeMap::new();
    for (section, key, value) in &request.edits {
        let k = (section.to_ascii_lowercase(), key.to_ascii_lowercase());
        if values.insert(k, value).is_some_and(|old| old != value) {
            return Err("optimize.conflict");
        }
    }
    request.edits.dedup();
    let has_post_install_work = request.component.is_some() || !request.edits.is_empty() || !prerequisites.is_empty();
    let install_wanted =
        adopt_legacy || has_post_install_work || (need_install && !experimental_omitted && !lsfg_omitted);
    if (need_install || selection.install.is_some()) && install_wanted {
        if let Some(options) = selection.install {
            request.set_install(options);
        } else if need_install {
            return Err("optimize.install_first");
        }
    }
    Ok(Composition {
        request,
        prerequisites,
        experimental_omitted,
    })
}
#[derive(Clone, Copy)]
pub enum Consent {
    Download,
    Check,
}
pub struct Pending {
    pub target: PathBuf,
    pub generation: u64,
    pub review_request: u64,
    pub request: Request,
}
pub struct CachedRecs {
    pub target: Option<PathBuf>,
    pub generation: u64,
    pub recs_ptr: usize,
    pub facts_ptr: usize,
    pub has_install_inputs: bool,
    pub combined: std::sync::Arc<Vec<recommend::Recommendation>>,
}
#[derive(Default)]
pub struct UiState {
    pub consent: Option<Consent>,
    pub busy: bool,
    pub downloaded: u64,
    pub release: Option<sm86::Release>,
    pub available: bool,
    pub kit_cache: Option<PathBuf>,
    pub error: Option<AppError>,
    pub record: Option<sm86::Record>,
    pub pending: Option<Pending>,
    pub experimental_for: Option<PathBuf>,
    pub lsfg_for: Option<PathBuf>,
    pub cached_recs: Option<CachedRecs>,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::app::*;
    use crate::autotune::{apply, prepare};
    use egui_kittest::{Harness, kittest::Queryable};
    use std::{
        fs,
        sync::Arc,
        time::{Duration, Instant},
    };
    fn only_nr_profile(edits: &[(String, String, String)]) -> bool {
        !edits.is_empty()
            && edits
                .iter()
                .all(|(section, key, _)| recommend::RTX30_NR.iter().any(|(s, k, _)| s == section && k == key))
    }
    fn recs(runtime: recommend::Runtime, dlssg: bool) -> Vec<recommend::Recommendation> {
        let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        recommend::recommend(
            Some(&gpu),
            &recommend::GameFacts {
                installed: true,
                has_dlssg: dlssg,
                runtime,
                ..Default::default()
            },
        )
    }
    fn component(root: &Path) -> components::Component {
        let source = root.join("source");
        for file in [
            "version.dll",
            "dlssg_sm86.ini",
            "dlssg_sm86/backend/sm86_backend.dll",
            "dlssg_sm86/kernels/fg_kernels.dll",
            "dlssg_sm86/runtime/nvngx_dlssg.dll",
            "dlssg_sm86/xefg/xefg_kernels.dll",
        ] {
            let path = source.join(file);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, b"MZ synthetic dlssg_sm86").unwrap();
        }
        components::ComponentCatalog::open(Some(&root.join("catalog")))
            .unwrap()
            .import_dir(&source, None)
            .unwrap()
    }
    #[test]
    fn first_run_is_only_for_one_detected_rtx_20_or_30() {
        for (name, cap, expected) in [
            ("RTX 2060", "7.5", true),
            ("RTX 3060", "8.6", true),
            ("GTX 1660", "7.5", false),
            ("RTX 4090", "8.9", false),
            ("A100", "8.0", false),
        ] {
            let gpus = retoque_data::gpu::parse_nvidia_smi_csv(&format!("{name},{cap},1,12288"));
            assert_eq!(first_run_eligible(&gpus), expected);
            assert!(!first_run_eligible(&[gpus[0].clone(), gpus[0].clone()]));
        }
        assert!(!first_run_eligible(&[]));
    }

    #[test]
    fn composition_resolves_one_request_or_explicit_prerequisites() {
        let native = recs(recommend::Runtime::Windows, true);
        let result = compose(Selection {
            recommendations: &native,
            ..Default::default()
        })
        .unwrap();
        assert_eq!(result.prerequisites, [Prerequisite::Sm86]);
        let proton = recs(recommend::Runtime::Proton, true);
        let result = compose(Selection {
            recommendations: &proton,
            ..Default::default()
        })
        .unwrap();
        // Experimental sm86 is left out under Proton; the RTX 30 Neural Rendering profile is not experimental.
        assert!(result.request.component.is_none() && result.prerequisites.is_empty());
        assert!(only_nr_profile(&result.request.edits), "{:?}", result.request.edits);
        assert!(result.experimental_omitted);
        assert_eq!(
            compose(Selection {
                recommendations: &proton,
                include_experimental: true,
                ..Default::default()
            })
            .unwrap()
            .prerequisites,
            [Prerequisite::Sm86]
        );
        let lsfg = recs(recommend::Runtime::Windows, false);
        assert!(only_nr_profile(
            &compose(Selection {
                recommendations: &lsfg,
                ..Default::default()
            })
            .unwrap()
            .request
            .edits
        ));
        assert_eq!(
            compose(Selection {
                recommendations: &lsfg,
                choose_lsfg: true,
                ..Default::default()
            })
            .unwrap()
            .prerequisites,
            [Prerequisite::LsfgShaders]
        );
        let edits = recommend::lsfg_edits(2, Some(Path::new("/synthetic/LS")), false);
        let c = compose(Selection {
            recommendations: &lsfg,
            choose_lsfg: true,
            lsfg_ready: true,
            lsfg_edits: edits.clone(),
            ..Default::default()
        })
        .unwrap();
        assert!(c.prerequisites.is_empty());
        assert_eq!(c.request.edits[..edits.len()], edits[..]);
        assert!(only_nr_profile(&c.request.edits[edits.len()..]));
        assert_eq!(
            compose(Selection {
                need_nr_model: true,
                ..Default::default()
            })
            .unwrap()
            .prerequisites,
            [Prerequisite::NrModel]
        );
    }
    #[test]
    fn combined_component_settings_and_model_do_not_write_until_confirmed() {
        let temp = tempfile::tempdir().unwrap();
        let c = component(temp.path());
        let game = temp.path().join("game");
        fs::create_dir(&game).unwrap();
        let before = b"; unchanged\r\n[LSFG]\r\nEnabled=true\r\n[DlssNr]\r\nModelFile=old.dll\r\n";
        fs::write(game.join("OptiScaler.ini"), before).unwrap();
        let recs = recs(recommend::Runtime::Windows, true);
        let composed = compose(Selection {
            recommendations: &recs,
            component: Some(&c),
            model_edit: Some(("DlssNr".into(), "ModelFile".into(), "synthetic.dll".into())),
            ..Default::default()
        })
        .unwrap();
        let prepared = prepare(&game, composed.request).unwrap();
        assert_eq!(prepared.plans.len(), 2);
        assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before);
        assert!(!game.join("version.dll").exists());
        assert!(!game.join(".retoque").exists());
        apply(&prepared, &mut |_| {}).unwrap();
        let ini = retoque_core::ini::Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap());
        assert_eq!(ini.get("LSFG", "Enabled"), Some("false"));
        assert_eq!(ini.get("DlssNr", "ModelFile"), Some("synthetic.dll"));
        assert!(game.join("version.dll").is_file());
    }
    #[test]
    fn automatic_button_opens_one_read_only_review_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let component = component(temp.path());
            let game = temp.path().join("game");
            fs::create_dir(&game).unwrap();
            let before = b"[LSFG]\nEnabled=true\n[DlssNr]\nModelFile=old.dll\n";
            fs::write(game.join("OptiScaler.ini"), before).unwrap();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some(game.clone());
            app.view.intelligence.recommendations = Arc::new(recs(recommend::Runtime::Windows, true));
            app.view.intelligence.facts = Arc::new(recommend::GameFacts {
                installed: true,
                has_dlssg: true,
                runtime: recommend::Runtime::Windows,
                ..Default::default()
            });
            app.tuning.catalog = Some(Ok(vec![component]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.view.inspection = Some(ModelInspection {
                compatibility: retoque_data::model_compat::ModelCompatibility {
                    known: true,
                    archs: vec![120],
                },
                ..Default::default()
            });
            app.catalog = Some(Ok(vec![Model {
                sha256: "synthetic".into(),
                version: None,
                size: 2,
                label: "Synthetic".into(),
                path: temp.path().join("model.dll"),
                compatibility: retoque_data::model_compat::ModelCompatibility {
                    known: true,
                    archs: vec![86],
                },
            }]));
            let mut h = Harness::builder().with_size([900., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.optimize_controls(ui);
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert_eq!(r.plans.len(), 2);
            assert!(r.allowed());
            assert_eq!(r.title, t(lang, "optimize.title"));
            assert!(
                r.plans
                    .iter()
                    .flat_map(|p| p.steps())
                    .any(|s| matches!(s,retoque_core::Step::SetIni {key,..} if key == "ModelFile"))
            );
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before);
            assert!(!game.join("version.dll").exists());
            assert!(!game.join(".retoque").exists());
        }
    }
    #[test]
    fn first_run_offer_and_cancel_never_contact_network_or_write_games() {
        for lang in [Lang::En, Lang::Pt] {
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.tuning.catalog = Some(Ok(vec![]));
            let mut h = Harness::builder().with_size([1000., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.sm86_dialog(ui.ctx());
                },
                app,
            );
            h.get_by_label(t(lang, "sm86.consent_title"));
            assert!(!h.state().jobs.busy());
            h.get_by_role_and_label(egui::Role::Button, t(lang, "sm86.later"))
                .click();
            h.run_steps(3);
            assert!(h.state().saved.sm86_offer_seen);
            assert!(h.state().tuning.sm86.consent.is_none());
            assert!(!h.state().jobs.busy());
        }
    }
    #[cfg(not(windows))]
    #[test]
    fn optimize_prerequisite_fetches_nr_model_in_app_data_then_opens_single_review_in_both_languages() {
        use crate::tabs::model::{NrConsent, synthetic_model_fixture_for_optimize};
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let component = component(temp.path());
            let game = temp.path().join("game");
            fs::create_dir(&game).unwrap();
            let before = b"[LSFG]\nEnabled=true\n[DlssNr]\nEnabled=true\nModelFile=auto\n";
            fs::write(game.join("OptiScaler.ini"), before).unwrap();
            let (entry, fetcher) = synthetic_model_fixture_for_optimize();
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.view.target = Some(game.clone());
            app.view.ini = Some(Ok(Some(retoque_core::ini::Ini::parse(before))));
            app.view.intelligence.recommendations = Arc::new(recs(recommend::Runtime::Windows, true));
            app.view.intelligence.facts = Arc::new(recommend::GameFacts {
                installed: true,
                has_dlssg: true,
                runtime: recommend::Runtime::Windows,
                ..Default::default()
            });
            app.tuning.catalog = Some(Ok(vec![component]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            // Model missing beside the game and empty local catalog: Prerequisite::NrModel must run first.
            app.view.inspection = Some(ModelInspection {
                path: Some(game.join("nvngx_dlssnr.dll")),
                present: false,
                ..Default::default()
            });
            app.catalog = Some(Ok(vec![]));
            app.nr_fetch.entries = vec![entry];
            app.nr_fetch.catalog_root = Some(temp.path().join("nr-catalog"));
            app.nr_fetch.fetcher = Some(fetcher);
            let mut h = Harness::builder().with_size([900., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.optimize_controls(ui);
                    app.nr_fetch_dialog(ui.ctx());
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            assert!(matches!(h.state().nr_fetch.consent, Some(NrConsent::Download(_))));
            assert!(h.state().review.is_none());
            h.get_by_label(t(lang, "nr_fetch.consent_title"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "nr_fetch.confirm"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert_eq!(r.plans.len(), 2);
            assert!(r.allowed());
            assert_eq!(r.title, t(lang, "optimize.title"));
            assert!(
                r.plans
                    .iter()
                    .flat_map(|p| p.steps())
                    .any(|s| matches!(s, retoque_core::Step::SetIni { key, .. } if key == "ModelFile"))
            );
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before);
            assert!(!game.join("version.dll").exists());
        }
    }
    fn payload_dir(root: &Path) -> PathBuf {
        let dir = root.join("payload");
        fs::create_dir_all(&dir).unwrap();
        fs::write(
            dir.join("OptiScaler.dll"),
            b"MZ OptiScaler working as new synthetic payload",
        )
        .unwrap();
        fs::write(
            dir.join("nvngx.dll_dlssnr.dll"),
            b"MZ dlssnr_call_ new synthetic forwarder",
        )
        .unwrap();
        fs::write(
            dir.join("OptiScaler.ini"),
            b"; payload ini\r\n[LSFG]\r\nEnabled=true\r\n[DlssNr]\r\nEnabled=false\r\nModelFile=auto\r\n",
        )
        .unwrap();
        dir
    }
    #[test]
    fn legacy_install_with_sm86_and_model_produces_one_review_applies_in_order_and_recovers_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let payload = payload_dir(temp.path());
            let component = component(temp.path());
            let game = temp.path().join("mortal-shell-ii");
            fs::create_dir(&game).unwrap();
            let old_proxy = b"MZ OptiScaler working as legacy script proxy";
            let old_fwd = b"MZ dlssnr_call_ legacy script forwarder";
            let before_ini =
                b"; legacy\r\n[LSFG]\r\nEnabled=true\r\n[DlssNr]\r\nEnabled=false\r\nModelFile=old.dll\r\n";
            fs::write(game.join("dxgi.dll"), old_proxy).unwrap();
            fs::write(game.join("nvngx.dll_dlssnr.dll"), old_fwd).unwrap();
            fs::write(game.join("OptiScaler.ini"), before_ini).unwrap();

            let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
            let facts = recommend::GameFacts {
                installed: true,
                untracked: true,
                proxy: Some("dxgi.dll".into()),
                has_dlss: true,
                has_dlssg: true,
                runtime: recommend::Runtime::Windows,
                ..Default::default()
            };
            let initial_recs = recommend::recommend(Some(&gpu), &facts);
            assert_eq!(initial_recs.len(), 1);
            assert_eq!(initial_recs[0].id, "adopt_retoque");

            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.payload = Some(payload.clone());
            app.view.target = Some(game.clone());
            app.view.options.proxy = "dxgi.dll".into();
            app.view.options.enable_nr = true;
            app.view.options.copy_nvngx = false;
            app.view.status = Some(Ok(retoque_core::status(&game).unwrap()));
            app.view.ini = Some(Ok(Some(retoque_core::ini::Ini::parse(before_ini))));
            app.view.intelligence.facts = Arc::new(facts);
            app.view.intelligence.recommendations = Arc::new(initial_recs);
            app.tuning.catalog = Some(Ok(vec![component.clone()]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(vec![gpu]);
            app.view.inspection = Some(ModelInspection {
                compatibility: retoque_data::model_compat::ModelCompatibility {
                    known: true,
                    archs: vec![120],
                },
                ..Default::default()
            });
            app.catalog = Some(Ok(vec![Model {
                sha256: "synthetic".into(),
                version: None,
                size: 2,
                label: "Synthetic".into(),
                path: temp.path().join("model.dll"),
                compatibility: retoque_data::model_compat::ModelCompatibility {
                    known: true,
                    archs: vec![86],
                },
            }]));

            let mut h = Harness::builder().with_size([900., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.optimize_controls(ui);
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert_eq!(r.plans.len(), 3, "expected install first, then sm86, then settings");
            assert!(r.allowed());
            assert_eq!(r.title, t(lang, "optimize.title"));
            // Plan 0: Retoque registration/install plan with backups of the legacy proxy and forwarder.
            assert!(
                r.plans[0]
                    .steps()
                    .iter()
                    .any(|s| matches!(s, retoque_core::Step::Backup { path, .. } if path == "dxgi.dll"))
            );
            assert!(
                r.plans[0]
                    .steps()
                    .iter()
                    .any(|s| matches!(s, retoque_core::Step::Backup { path, .. } if path == "nvngx.dll_dlssnr.dll"))
            );
            // Plan 1: sm86 component install plan.
            assert!(
                r.plans[1]
                    .steps()
                    .iter()
                    .any(|s| matches!(s, retoque_core::Step::Copy { to, .. } if to == "version.dll"))
            );
            // Plan 2: post-install settings & NR model plan.
            assert!(
                r.plans[2]
                    .steps()
                    .iter()
                    .any(|s| matches!(s, retoque_core::Step::SetIni { key, .. } if key == "ModelFile"))
            );
            // Nothing written before confirmation.
            assert_eq!(fs::read(game.join("dxgi.dll")).unwrap(), old_proxy);
            assert_eq!(fs::read(game.join("nvngx.dll_dlssnr.dll")).unwrap(), old_fwd);
            assert_eq!(fs::read(game.join("OptiScaler.ini")).unwrap(), before_ini);
            assert!(!game.join("version.dll").exists());
            assert!(!game.join(".retoque").exists());

            // Verify applied order on the synthetic game: install first, then sm86, then settings.
            let ReviewKind::Autotune(ref req) = r.kind else {
                panic!("expected compound review");
            };
            let prepared = crate::autotune::Prepared {
                target: game.clone(),
                request: (**req).clone(),
                plans: r.plans.clone(),
                groups: r.groups.clone(),
            };
            let n0 = prepared.plans[0].steps().len();
            let n1 = prepared.plans[1].steps().len();
            let n2 = prepared.plans[2].steps().len();
            let mut applied_steps = Vec::new();
            apply(&prepared, &mut |done| {
                assert_eq!(done.index, applied_steps.len());
                assert_eq!(done.total, n0 + n1 + n2);
                if done.index == n0 {
                    // Right when plan 1 starts, plan 0 (install) must already have written the receipt and proxy,
                    // while version.dll from plan 1 is not copied yet.
                    assert!(game.join(".retoque/receipt.txt").is_file());
                    assert_eq!(
                        fs::read(game.join("dxgi.dll")).unwrap(),
                        fs::read(payload.join("OptiScaler.dll")).unwrap()
                    );
                    assert!(!game.join("version.dll").exists());
                }
                if done.index == n0 + n1 {
                    // Right when plan 2 (settings) starts, plan 1 (sm86) must already have written version.dll,
                    // while ModelFile is still old.dll.
                    assert!(game.join("version.dll").is_file());
                    let ini = retoque_core::ini::Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap());
                    assert_eq!(ini.get("DlssNr", "ModelFile"), Some("old.dll"));
                }
                applied_steps.push(done.step);
            })
            .unwrap();
            assert_eq!(applied_steps.len(), n0 + n1 + n2);
            let status = retoque_core::status(&game).unwrap();
            let receipt = status.receipt.expect("v1 receipt written");
            assert_eq!(receipt.status, retoque_core::receipt::ReceiptStatus::Complete);
            assert_eq!(retoque_core::installed_components(&game).unwrap(), vec![component.id]);
            let ini = retoque_core::ini::Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap());
            assert_eq!(ini.get("DlssNr", "Enabled"), Some("true"));
            assert_eq!(ini.get("LSFG", "Enabled"), Some("false"));
            assert!(ini.get("DlssNr", "ModelFile").unwrap().contains("model.dll"));
        }
    }
    #[test]
    fn uninstalled_game_plans_settings_against_post_install_ini_and_mid_apply_failure_is_recoverable() {
        let temp = tempfile::tempdir().unwrap();
        let payload = payload_dir(temp.path());
        let c = component(temp.path());
        let game = temp.path().join("uninstalled-game");
        fs::create_dir(&game).unwrap();
        let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let facts = recommend::GameFacts {
            installed: false,
            untracked: false,
            has_dlss: true,
            has_dlssg: true,
            runtime: recommend::Runtime::Windows,
            ..Default::default()
        };
        let recs = recommendations_for_optimize(&recommend::recommend(Some(&gpu), &facts), Some(&gpu), &facts, true);
        let composed = compose(Selection {
            install: Some(retoque_core::InstallOptions {
                game_dir: game.clone(),
                proxy: "dxgi.dll".into(),
                source_dir: payload.clone(),
                enable_nr: true,
                copy_nvngx: false,
                ..Default::default()
            }),
            recommendations: &recs,
            component: Some(&c),
            model_edit: Some(("DlssNr".into(), "ModelFile".into(), "synthetic.dll".into())),
            ..Default::default()
        })
        .unwrap();
        // OptiScaler.ini does not exist in `game` yet; prepare must still seal all 3 plans cleanly.
        assert!(!game.join("OptiScaler.ini").exists());
        let prepared = prepare(&game, composed.request).unwrap();
        assert_eq!(prepared.plans.len(), 3);
        assert!(prepared.plans.iter().all(|p| p.refusals().is_empty()));
        assert!(!game.join("OptiScaler.ini").exists());
        assert!(!game.join(".retoque").exists());

        // Inject a failure in the middle of plan 1 (after plan 0 completed and plan 1 wrote its first step,
        // before plan 1 finishes or plan 2 starts) and verify the partial receipt state is recoverable.
        let n0 = prepared.plans[0].steps().len();
        let err = apply(&prepared, &mut |done| {
            if done.index == n0 {
                let conflict = game.join("dlssg_sm86/xefg");
                fs::create_dir_all(&conflict).unwrap();
                fs::write(conflict.join("xefg_kernels.dll"), b"MZ unexpected foreign file").unwrap();
            }
        })
        .unwrap_err();
        assert_eq!(err.completed, 1);
        let status = retoque_core::status(&game).unwrap();
        let partial_receipt = status.receipt.expect("partial v1 receipt preserved");
        assert_eq!(partial_receipt.status, retoque_core::receipt::ReceiptStatus::Partial);
        let ini = retoque_core::ini::Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap());
        assert_eq!(ini.get("DlssNr", "ModelFile"), Some("auto"));
        fs::remove_dir_all(game.join("dlssg_sm86")).unwrap();
        let uninstall = retoque_core::plan_uninstall(&game).unwrap();
        assert!(uninstall.refusals().is_empty());
        retoque_core::apply(&uninstall.into_verified().unwrap(), &mut |_| {}).unwrap();
        assert!(!game.join(".retoque/receipt.txt").exists());
    }
    #[test]
    fn uninstalled_game_settings_are_never_planned_against_the_payload_folder() {
        // On Windows the app runs from the payload folder: a plan aimed there is refused as "in use" by the running-file
        // check (Rafael's RTX 3060, 2026-11-10, Grounded 2), and could only ever describe the payload's own INI.
        let temp = tempfile::tempdir().unwrap();
        let payload = payload_dir(temp.path());
        let payload_ini = fs::read(payload.join("OptiScaler.ini")).unwrap();
        let c = component(temp.path());
        let game = temp.path().join("fresh-game");
        fs::create_dir(&game).unwrap();
        let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
        let facts = recommend::GameFacts {
            installed: false,
            has_dlss: true,
            has_dlssg: true,
            runtime: recommend::Runtime::Windows,
            ..Default::default()
        };
        let recs = recommendations_for_optimize(&recommend::recommend(Some(&gpu), &facts), Some(&gpu), &facts, true);
        let composed = compose(Selection {
            install: Some(retoque_core::InstallOptions {
                game_dir: game.clone(),
                proxy: "dxgi.dll".into(),
                source_dir: payload.clone(),
                enable_nr: true,
                copy_nvngx: false,
                ..Default::default()
            }),
            recommendations: &recs,
            component: Some(&c),
            model_edit: Some(("DlssNr".into(), "ModelFile".into(), "synthetic.dll".into())),
            ..Default::default()
        })
        .unwrap();
        let prepared = prepare(&game, composed.request).unwrap();
        let canonical_payload = payload.canonicalize().unwrap();
        for plan in &prepared.plans {
            assert!(plan.refusals().is_empty(), "{:?}", plan.refusals());
            assert!(
                !plan.game_dir().starts_with(&payload) && !plan.game_dir().starts_with(&canonical_payload),
                "a plan is aimed at the payload folder: {}",
                plan.game_dir().display()
            );
        }
        apply(&prepared, &mut |_| {}).unwrap();
        let ini = retoque_core::ini::Ini::parse(&fs::read(game.join("OptiScaler.ini")).unwrap());
        assert_eq!(ini.get("DlssNr", "ModelFile"), Some("synthetic.dll"));
        assert_eq!(ini.get("DlssNr", "Enabled"), Some("true"));
        assert_eq!(
            fs::read(payload.join("OptiScaler.ini")).unwrap(),
            payload_ini,
            "payload INI changed"
        );
    }
    #[test]
    fn foreign_proxy_refusal_in_install_plan_is_shown_and_blocks_whole_review_in_both_languages() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let payload = payload_dir(temp.path());
            let component = component(temp.path());
            let game = temp.path().join("foreign-proxy-game");
            fs::create_dir(&game).unwrap();
            let foreign_bytes = b"MZ unknown foreign proxy dll";
            fs::write(game.join("dxgi.dll"), foreign_bytes).unwrap();

            let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
            let facts = recommend::GameFacts {
                installed: false,
                untracked: false,
                has_dlss: true,
                has_dlssg: true,
                runtime: recommend::Runtime::Windows,
                ..Default::default()
            };
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.payload = Some(payload);
            app.view.target = Some(game.clone());
            app.view.options.proxy = "dxgi.dll".into();
            app.view.options.enable_nr = true;
            app.view.options.copy_nvngx = false;
            app.view.status = Some(Ok(retoque_core::status(&game).unwrap()));
            app.view.intelligence.facts = Arc::new(facts.clone());
            app.view.intelligence.recommendations = Arc::new(recommend::recommend(Some(&gpu), &facts));
            app.tuning.catalog = Some(Ok(vec![component]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(vec![gpu]);

            let mut h = Harness::builder().with_size([1000., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.optimize_controls(ui);
                    app.review_sheet(ui.ctx());
                    app.handle_commands();
                },
                app,
            );
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            h.run_steps(3);
            let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert!(!r.allowed(), "foreign proxy refusal must block the whole review");
            assert!(
                r.plans[0].refusals().iter().any(
                    |refusal| matches!(refusal, retoque_core::Refusal::Foreign { path, .. } if path == "dxgi.dll")
                )
            );
            assert!(h.query_by_label_contains(t(lang, "ref.foreign")).is_some());
            assert!(
                h.query_by_role_and_label(egui::Role::Button, t(lang, "auto.confirm"))
                    .is_none()
            );
            assert_eq!(fs::read(game.join("dxgi.dll")).unwrap(), foreign_bytes);
            assert!(!game.join(".retoque").exists());
        }
    }
    #[test]
    fn proton_sm86_stays_opt_in_experimental_and_cached_recommendations_do_not_allocate_per_frame() {
        for lang in [Lang::En, Lang::Pt] {
            let temp = tempfile::tempdir().unwrap();
            let payload = payload_dir(temp.path());
            let component = component(temp.path());
            let game = temp.path().join("proton-legacy-game");
            fs::create_dir(&game).unwrap();
            fs::write(game.join("dxgi.dll"), b"MZ OptiScaler working as legacy proxy").unwrap();
            fs::write(game.join("nvngx.dll_dlssnr.dll"), b"MZ dlssnr_call_ legacy forwarder").unwrap();
            fs::write(game.join("OptiScaler.ini"), b"[LSFG]\nEnabled=true\n").unwrap();

            let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
            let facts = recommend::GameFacts {
                installed: false,
                untracked: false,
                has_dlss: true,
                has_dlssg: true,
                runtime: recommend::Runtime::Proton,
                ..Default::default()
            };
            let mut app = crate::app_tests::shell_tests::app();
            app.lang = lang;
            app.payload = Some(payload);
            app.view.target = Some(game.clone());
            app.view.options.proxy = "dxgi.dll".into();
            app.view.options.enable_nr = true;
            app.view.options.copy_nvngx = false;
            app.view.status = Some(Ok(retoque_core::status(&game).unwrap()));
            app.view.intelligence.facts = Arc::new(facts.clone());
            app.view.intelligence.recommendations = Arc::new(recommend::recommend(Some(&gpu), &facts));
            app.tuning.catalog = Some(Ok(vec![component]));
            app.tuning.installed = Some(Ok(vec![]));
            app.tuning.gpus = Some(vec![gpu.clone()]);

            // Warm cache once, then verify repeated calls allocate 0 times on the UI thread.
            let _ = app.optimize_recommendations();
            let allocations =
                crate::app_tests::measurements::measure("cached optimize_recommendations access", 1000, || {
                    std::hint::black_box(app.optimize_recommendations());
                });
            assert_eq!(
                allocations, 0,
                "cached optimize_recommendations must not allocate per frame"
            );

            let mut h = Harness::builder().with_size([1000., 900.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.receive();
                    app.optimize_controls(ui);
                },
                app,
            );
            // Without opting in, experimental Proton sm86 is omitted on an uninstalled game: the review installs
            // Retoque alone ("Install and optimize" always installs) and says what was left out.
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            {
                // Retoque, then the RTX 30 Neural Rendering profile; nothing of sm86 without opting in.
                let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
                assert_eq!(r.plans.len(), 2);
                for plan in &r.plans {
                    for step in plan.steps() {
                        assert!(!matches!(step, retoque_core::Step::Copy { to, .. } if to.starts_with("dlssg_sm86")));
                        assert!(!matches!(step, retoque_core::Step::SetIni { section, .. }
                            if section == "DLSSG" || section == "FrameGen"));
                    }
                }
            }
            assert_eq!(
                h.state().tuning.sm86.error.as_ref().map(|e| e.to_string()).as_deref(),
                Some(t(lang, "optimize.experimental_omitted"))
            );
            // Closing the review: the app reloads the game's facts; the harness restores the same ones.
            h.state_mut().review = None;
            h.state_mut().view.intelligence.facts = Arc::new(facts.clone());
            h.state_mut().view.intelligence.recommendations = Arc::new(recommend::recommend(Some(&gpu), &facts));
            h.run_steps(2);

            // Opting in via the checkbox includes install + sm86 + settings in one review.
            h.get_by_label(t(lang, "optimize.experimental")).click();
            h.run_steps(2);
            h.get_by_role_and_label(egui::Role::Button, t(lang, "optimize.title"))
                .click();
            h.run_steps(3);
            let deadline = Instant::now() + Duration::from_secs(5);
            while h.state().review.is_none() {
                h.state_mut().receive();
                assert!(Instant::now() < deadline);
                std::thread::yield_now();
            }
            let r = h.state().review.as_ref().unwrap().as_ref().unwrap();
            assert_eq!(r.plans.len(), 3);
            assert!(r.allowed());
        }
    }
}
