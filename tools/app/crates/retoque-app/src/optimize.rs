//! Compose one review without writes. Prerequisites touch app data only and must finish before `prepare`.
use super::{ComponentChange, Request, components, recommend, sm86};
use crate::{error::AppError, model::Edit};
use std::path::PathBuf;

pub fn first_run_eligible(gpus: &[retoque_data::gpu::Gpu]) -> bool {
    gpus.len() == 1
        && gpus[0].vendor == retoque_data::gpu::Vendor::Nvidia
        && gpus[0]
            .name
            .as_bytes()
            .windows(6)
            .any(|name| name.eq_ignore_ascii_case(b"RTX 20") || name.eq_ignore_ascii_case(b"RTX 30"))
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
pub fn compose(selection: Selection<'_>) -> Result<Composition, &'static str> {
    let mut request = Request::default();
    let mut prerequisites = Vec::new();
    let mut experimental_omitted = false;
    for rec in selection.recommendations {
        if rec.is_primary_install_action() {
            return Err("optimize.install_first");
        }
        if rec.confidence == recommend::Confidence::ExperimentalUnderProton && !selection.include_experimental {
            experimental_omitted = true;
            continue;
        }
        if rec.needs == Some(recommend::Need::LosslessScaling) {
            if !selection.choose_lsfg {
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
                        request.component = Some(ComponentChange::Install(c.clone()));
                    } else {
                        prerequisites.push(Prerequisite::Sm86);
                    }
                }
                recommend::Action::RemoveComponent(_) => {
                    if let Some(id) = selection.installed_ids.first() {
                        request.component = Some(ComponentChange::Remove(id.clone()));
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
        assert!(
            result.request.component.is_none() && result.request.edits.is_empty() && result.prerequisites.is_empty()
        );
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
        assert!(
            compose(Selection {
                recommendations: &lsfg,
                ..Default::default()
            })
            .unwrap()
            .request
            .edits
            .is_empty()
        );
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
        assert_eq!(c.request.edits, edits);
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
}
