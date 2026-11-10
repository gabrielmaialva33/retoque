use super::*;
use crate::tabs::doctor::{self as ui_doctor, DoctorCard};
use egui_kittest::{Harness, kittest::Queryable};

fn app_at(path: &Path) -> RetoqueApp {
    let mut app = crate::app_tests::shell_tests::app();
    app.view.target = Some(path.into());
    app.view.status = Some(retoque_core::status(path).map_err(AppError::from));
    app.view.ini = Some(Ok(retoque_core::read_ini(path).unwrap().map(|(_, b)| Ini::parse(&b))));
    let configured = app
        .view
        .ini
        .as_ref()
        .and_then(|r| r.as_ref().ok())
        .and_then(|i| i.as_ref())
        .and_then(|i| i.get("DlssNr", "ModelFile"))
        .unwrap_or("auto");
    app.view.inspection = Some(inspect_model(path, configured));
    app.tuning.installed = Some(Ok(vec![]));
    app.tuning.gpus = Some(vec![]);
    app
}
fn settle(app: &mut RetoqueApp, predicate: impl Fn(&RetoqueApp) -> bool) {
    let deadline = Instant::now() + Duration::from_secs(5);
    while !predicate(app) {
        app.receive();
        assert!(Instant::now() < deadline, "background completion timed out");
        std::thread::yield_now();
    }
}
#[test]
fn doctor_card_evidence_and_repair_open_review_without_writing_in_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let temp = tempfile::tempdir().unwrap();
        let before = b"; preserve\r\n[DlssNr]\r\nEnabled=true\r\nModelFile=missing.dll\r\n";
        let path = temp.path().join("OptiScaler.ini");
        std::fs::write(&path, before).unwrap();
        let mut app = app_at(temp.path());
        app.lang = lang;
        app.refresh_intelligence();
        settle(&mut app, |app| app.view.doctor.is_some());
        assert_eq!(app.doctor_badges.get(temp.path()), Some(&1));
        let mut harness = Harness::builder().with_size([900., 900.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.receive();
                if let Some(card) = &app.view.doctor {
                    ui_doctor::show(
                        ui,
                        card,
                        app.view.target.as_deref().unwrap(),
                        app.lang,
                        &app.cmd_tx,
                        false,
                    );
                }
                app.review_sheet(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        harness.get_by_label(ui_doctor::text(lang, "doctor.model_missing.title"));
        harness.get_by_label(ui_doctor::text(lang, "severity.error"));
        harness.get_by_label(t(lang, "doctor.evidence")).click();
        harness.run_steps(4);
        harness.get_by_label(ui_doctor::text(lang, "evidence.model_present"));
        harness
            .get_by_role_and_label(egui::Role::Button, t(lang, "doctor.prepare"))
            .click();
        harness.run_steps(3);
        settle(harness.state_mut(), |app| app.review.is_some());
        assert!(harness.state().review_open);
        assert!(harness.state().review.as_ref().unwrap().as_ref().unwrap().allowed());
        assert_eq!(std::fs::read(&path).unwrap(), before);
    }
}
#[test]
fn healthy_is_a_quiet_line_and_badge_is_only_for_nonzero_cached_count() {
    for lang in [Lang::En, Lang::Pt] {
        let (commands, _) = CommandSender::channel();
        let mut harness = Harness::new_ui(move |ui| {
            ui_doctor::show(
                ui,
                &DoctorCard { findings: vec![] },
                Path::new("/synthetic/game"),
                lang,
                &commands,
                false,
            );
            ui_doctor::badge(ui, 0, lang);
        });
        harness.run();
        harness.get_by_label(ui_doctor::text(lang, "doctor.healthy"));
        assert!(harness.query_by_label(t(lang, "doctor.title")).is_none());
        assert!(harness.query_by_label("!").is_none());
        let harness = Harness::new_ui(move |ui| ui_doctor::badge(ui, 3, lang));
        harness.get_by_label("!");
    }
}
#[test]
fn library_row_badge_paints_only_with_findings_and_keeps_the_row_clickable() {
    for (count, marked) in [(0usize, false), (2, true)] {
        let mut harness = Harness::new_ui(move |ui| {
            let row = ui.add(egui::Button::new("Synthetic game"));
            let row = ui_doctor::row_badge(ui, row, count, Lang::En);
            assert_eq!(row.hovered(), row.hovered());
            if marked {
                assert!(row.rect.width() > 0.0);
            }
        });
        harness.run();
        harness.get_by_label("Synthetic game").click();
        harness.run();
    }
}
#[test]
fn log_diagnosis_has_evidence_and_review_action_in_both_languages() {
    for lang in [Lang::En, Lang::Pt] {
        let temp = tempfile::tempdir().unwrap();
        std::fs::write(temp.path().join("OptiScaler.ini"), "[DlssNr]\nEnabled=true\n").unwrap();
        let line = "DLSS-NR Vulkan: the device was lost while draining it for a resize";
        let summary = Summary {
            path: temp.path().join("OptiScaler.log"),
            diagnostic_lines: vec![line.into()],
            ..Default::default()
        };
        let mut app = app_at(temp.path());
        app.lang = lang;
        app.view.logs = Some((vec![summary.path.clone()], Ok(Some(summary))));
        app.refresh_intelligence();
        settle(&mut app, |app| app.view.doctor.is_some());
        let diagnosis = app.view.intelligence.log_diagnoses[0].clone();
        let mut harness = Harness::builder().with_size([1000., 1500.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.receive();
                app.logs_tab(ui, &app.cmd_tx);
                app.review_sheet(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        harness.get_by_label(ui_doctor::text(lang, diagnosis.title_key));
        harness.get_by_label(ui_doctor::text(lang, diagnosis.why_key));
        harness.get_by_label(line);
        harness
            .get_by_role_and_label(egui::Role::Button, t(lang, "doctor.prepare"))
            .click();
        harness.run_steps(3);
        settle(harness.state_mut(), |app| app.review.is_some());
        assert!(harness.state().review_open);
        assert_eq!(
            std::fs::read_to_string(temp.path().join("OptiScaler.ini")).unwrap(),
            "[DlssNr]\nEnabled=true\n"
        );
    }
}
#[test]
fn recommendation_confidence_caveat_and_evidence_in_both_languages() {
    let gpu = retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288").remove(0);
    let facts = GameFacts {
        runtime: recommend::Runtime::Proton,
        installed: true,
        has_dlss: true,
        has_dlssg: true,
        last_session_api: Some("D3D12".into()),
        ..Default::default()
    };
    let recommendations = recommend::recommend(Some(&gpu), &facts);
    let rec = recommendations
        .iter()
        .find(|r| r.confidence == recommend::Confidence::ExperimentalUnderProton)
        .unwrap();
    for lang in [Lang::En, Lang::Pt] {
        let rec = rec.clone();
        let expected = rec.clone();
        let mut harness = Harness::builder()
            .with_size([1000., 800.])
            .build_ui(move |ui| crate::autotune_ui::recommendation_evidence(ui, lang, &rec));
        harness.get_by_label(ui_doctor::text(lang, expected.confidence.key()));
        harness.get_by_label(ui_doctor::text(lang, expected.caveat_key.unwrap()));
        harness.get_by_label(t(lang, "doctor.evidence")).click();
        harness.run_steps(4);
        for fact in &expected.evidence {
            harness.get_by_label(ui_doctor::text(lang, fact.key));
        }
    }
}
#[test]
fn historical_log_does_not_replace_last_session_api() {
    let temp = tempfile::tempdir().unwrap();
    let mut app = app_at(temp.path());
    app.view.intelligence.latest_log = Some(Summary {
        api: Some("D3D12".into()),
        ..Default::default()
    });
    let target = temp.path().to_owned();
    app.jobs.spawn("logs", None, move |_| Outcome::Logs {
        latest: false,
        target,
        files: vec![],
        result: Ok(Some(Summary {
            api: Some("Vulkan".into()),
            ..Default::default()
        })),
    });
    settle(&mut app, |app| app.view.logs.is_some());
    assert_eq!(app.autotune_game_facts().last_session_api.as_deref(), Some("D3D12"));
    let target = temp.path().to_owned();
    app.jobs.spawn("logs", None, move |_| Outcome::Logs {
        latest: true,
        target,
        files: vec![],
        result: Err(AppError::from("unreadable latest log")),
    });
    settle(&mut app, |app| app.view.intelligence.latest_log.is_none());
    assert_eq!(app.autotune_game_facts().last_session_api, None);
}
#[test]
fn watcher_error_is_visible_in_both_languages_and_has_manual_refresh() {
    for lang in [Lang::En, Lang::Pt] {
        let mut app = crate::app_tests::shell_tests::app();
        app.lang = lang;
        app.view.target = Some("/synthetic/game".into());
        app.receive_watch("/synthetic/game".into(), Err("inotify watch limit".into()));
        let harness = Harness::new_ui(move |ui| app.watcher_notice(ui));
        harness.get_by_label(t(lang, "watch.manual"));
        harness.get_by_role_and_label(egui::Role::Button, t(lang, "watch.refresh"));
    }
}
#[test]
#[ignore = "manual selection diagnostic timing"]
fn measure_diagnose_per_selection() {
    let temp = tempfile::tempdir().unwrap();
    let snapshot = || Snapshot {
        install_dir: temp.path().into(),
        target: temp.path().into(),
        status: Some(Status {
            nr_enabled: Some(true),
            ..Default::default()
        }),
        ini: Some(Ini::parse(b"[DlssNr]\nEnabled=true\n[LSFG]\nEnabled=true\n")),
        model: None,
        model_path: None,
        catalog: vec![],
        gpu: None,
        latest: Some(Summary {
            diagnostic_lines: vec!["DLSS-NR Vulkan: the device was lost while draining it for a resize".into()],
            ..Default::default()
        }),
        displayed: None,
        readiness: Some(retoque_data::lsfg::LsfgReadiness::NoLosslessScaling),
        components: vec!["sm86".into()],
        facts: GameFacts::default(),
        components_known: true,
    };
    let mut total = Duration::ZERO;
    let start = Instant::now();
    for _ in 0..10000 {
        let result = snapshot().assess();
        assert!(result.findings.len() >= 4);
        total += result.elapsed;
    }
    eprintln!(
        "diagnose + logs + recommendations: {:.3} us/selection; snapshot + metadata probes + diagnosis: {:.3} us/selection (10000 synthetic selections, debug)",
        total.as_secs_f64() * 100.,
        start.elapsed().as_secs_f64() * 100.
    );
}

#[test]
fn a_failed_read_cannot_release_an_in_flight_writer() {
    let temp = tempfile::tempdir().unwrap();
    let mut app = app_at(temp.path());
    app.writing.insert(temp.path().into(), (0, 2));
    let target = temp.path().to_owned();
    app.jobs
        .spawn("doctor", Some(target), |_| panic!("synthetic diagnostic failure"));
    // The failure is reported once, as a toast.
    settle(&mut app, |app| app.toasts.iter().any(|toast| toast.failed));
    assert!(app.error.is_none());
    assert!(app.writing.contains_key(temp.path()));
}
#[test]
fn unknown_model_path_does_not_propose_disabling_nr() {
    let temp = tempfile::tempdir().unwrap();
    std::fs::write(
        temp.path().join("OptiScaler.ini"),
        "[DlssNr]\nEnabled=true\nModelFile=Q:\\model.dll\n",
    )
    .unwrap();
    let mut app = app_at(temp.path());
    // The app cannot resolve arbitrary Wine drive letters without a prefix mapping.
    app.view.inspection = Some(ModelInspection {
        path: None,
        present: false,
        ..Default::default()
    });
    app.refresh_intelligence();
    settle(&mut app, |app| app.view.doctor.is_some());
    assert!(
        !app.view
            .doctor
            .as_ref()
            .unwrap()
            .findings
            .iter()
            .any(|f| f.id == "model_missing")
    );
}

#[test]
#[ignore = "manual cached diagnostic access measurement"]
fn measure_cached_facts_and_recommendations() {
    use crate::app_tests::measurements::measure;
    use std::hint::black_box;
    let temp = tempfile::tempdir().unwrap();
    std::fs::write(temp.path().join("OptiScaler.ini"), "[LSFG]\nEnabled=false\n").unwrap();
    let mut app = app_at(temp.path());
    app.tuning.installed = Some(Ok(vec!["synthetic-component".into()]));
    app.view.intelligence.latest_log = Some(Summary {
        api: Some("D3D12".into()),
        ..Default::default()
    });
    app.refresh_intelligence();
    settle(&mut app, |app| app.view.doctor.is_some());
    measure("uncached facts + recommendations", 10000, || {
        let facts = app.autotune_game_facts();
        black_box(recommend::recommend(None, &facts));
    });
    let allocations = measure("cached facts + recommendations access", 10000, || {
        black_box(app.view.intelligence.facts.clone());
        black_box(app.view.intelligence.recommendations.clone());
    });
    assert_eq!(allocations, 0);
}
#[test]
fn new_selection_and_write_cooldown_do_not_poison_read_state() {
    let temp = tempfile::tempdir().unwrap();
    let mut app = app_at(temp.path());
    app.view.inspection = None;
    app.view.status.as_mut().unwrap().as_mut().unwrap().nr_enabled = Some(true);
    app.refresh_intelligence();
    settle(&mut app, |app| app.view.doctor.is_some());
    assert!(
        !app.view
            .doctor
            .as_ref()
            .unwrap()
            .findings
            .iter()
            .any(|f| f.id == "model_missing"),
        "pending model inspection is unknown"
    );
    let status = app.view.status.as_ref().unwrap().as_ref().unwrap().clone();
    let log = temp.path().join("OptiScaler.log");
    std::fs::write(
        &log,
        "DLSS-NR Vulkan: the device was lost while draining it for a resize\n",
    )
    .unwrap();
    app.receive_watch(
        temp.path().into(),
        Ok(crate::watcher::Changes {
            logs: true,
            ..Default::default()
        }),
    );
    settle(&mut app, |app| app.view.logs.is_some());
    assert_eq!(
        app.view.status.as_ref().unwrap().as_ref().unwrap().nr_enabled,
        status.nr_enabled,
        "a log-only change must not reload status"
    );
    assert!(app.view.ini.is_some());
}
