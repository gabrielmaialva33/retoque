use crate::app::*;
#[cfg(all(test, not(windows)))]
mod tests {
    use super::*;

    #[test]
    pub(crate) fn setting_control_is_named_and_interactive_for_accessibility() {
        use egui_kittest::{Harness, kittest::Queryable};
        let setting = settings::all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Stage")
            .unwrap();
        let mut harness = Harness::new_ui_state(
            |ui, value| {
                let label = ui.label(setting.label_en);
                value_widget(ui, setting, value, "stage-test", Lang::En).labelled_by(label.id);
            },
            "auto".to_owned(),
        );
        harness
            .get_by_role_and_label(egui::Role::ComboBox, setting.label_en)
            .click();
        harness.run();
        harness.get_by_label("Before the upscaler (experimental)").click();
        harness.run();
        assert_eq!(harness.state(), "1");
    }

    #[test]
    pub(crate) fn model_file_values_map_back_to_host_paths() {
        assert_eq!(host_path("Z:\\home\\u\\m.dll"), Some(PathBuf::from("/home/u/m.dll"))); // privacy: allow (synthetic test path)
        assert_eq!(host_path("/abs/m.dll"), Some(PathBuf::from("/abs/m.dll")));
        assert_eq!(host_path("C:\\x.dll"), None);
        assert_eq!(host_path("nvngx_dlssnr.dll"), None);
    }
}
#[cfg(test)]
mod shell_tests {
    use super::*;
    use egui_kittest::{
        Harness,
        kittest::{NodeT as _, Queryable},
    };

    #[test]
    pub(crate) fn review_groups_existing_copy_as_replace_without_backup() {
        let copy = Step::Copy {
            from: "payload/OptiScaler.dll".into(),
            to: "winmm.dll".into(),
        };
        assert_eq!(step_group(&copy, std::slice::from_ref(&copy), true), 1);
        assert_eq!(step_group(&copy, std::slice::from_ref(&copy), false), 0);
    }

    pub(crate) fn app() -> RetoqueApp {
        let context = egui::Context::default();
        let (cmd_tx, cmd_rx) = CommandSender::channel();
        RetoqueApp {
            tuning: crate::autotune::UiState::default(),
            folder_dialog: None,
            folder_focus: None,
            folder_restore: 0,
            compare: crate::compare::CompareView::new(),
            saved: Saved::default(),
            lang: Lang::En,
            jobs: TaskHub::new(context.clone()),
            skipped: vec![],
            scanning: false,
            library_state: Default::default(),
            rescan_pending: false,
            selected: None,
            tab: Tab::Install,
            payload: None,
            view: GameView::default(),
            catalog: None,
            error: None,
            writing: HashMap::new(),
            write_kinds: HashMap::new(),
            next_request: 0,
            pending_select: None,
            drafts: HashMap::new(),
            context,
            cmd_tx,
            cmd_rx,
            model_usage: HashMap::new(),
            usage_refreshed: None,
            payload_build: None,
            library_generation: 0,
            library_pending: false,
            review_open: false,
            review: None,
            origin_focus: None,
            sidebar_visible: true,
            focus_search: false,
            library_has_focus: false,
            restore_frames: 0,
            toasts: vec![],
            process_notice: None,
        }
    }

    #[test]
    pub(crate) fn nr_warning_suggestion_and_unknown_are_visible_in_both_languages() {
        use retoque_data::model_compat::ModelCompatibility;
        for lang in [Lang::En, Lang::Pt] {
            let mut app = app();
            app.lang = lang;
            app.view.target = Some("/synthetic/nr-game".into());
            app.tuning.gpus = Some(retoque_data::gpu::parse_nvidia_smi_csv("RTX 3060,8.6,1,12288"));
            app.catalog = Some(Ok(vec![retoque_data::models::Model {
                sha256: "synthetic".into(),
                version: None,
                size: 64,
                label: "Compatible fixture".into(),
                path: "/synthetic/model.dll".into(),
                compatibility: ModelCompatibility {
                    archs: vec![86],
                    known: true,
                },
            }]));
            let mut h = Harness::builder().with_size([900., 800.]).build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    app.context = ui.ctx().clone();
                    app.view.inspection = Some(ModelInspection {
                        present: true,
                        compatibility: ModelCompatibility {
                            archs: vec![120],
                            known: true,
                        },
                        ..Default::default()
                    });
                    app.nr_compatibility_notice(ui);
                    app.model_compatibility_label(ui, &ModelCompatibility::default());
                },
                app,
            );
            h.run_steps(2);
            h.get_by_label(t(lang, "mdl.incompatible"));
            h.get_by_label(t(lang, "mdl.compat_unknown"));
            h.get_by_role_and_label(egui::Role::Button, t(lang, "mdl.use_suggested"));
            assert!(h.query_by_label_contains("Compatible fixture").is_some());
        }
    }

    #[test]
    pub(crate) fn version_mismatch_warns_but_missing_resource_is_unknown() {
        let mut app = app();
        app.view.status = Some(Ok(Status {
            optiscaler_proxies: vec!["dxgi.dll".into()],
            proxy_version: Some("1.2.3.4".into()),
            forwarder_version: Some("1.2.3.5".into()),
            ..Default::default()
        }));
        let mut h = Harness::builder()
            .with_size([1100., 400.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.status_chips(ui), app);
        h.run_steps(2);
        h.get_by_label(t(Lang::En, "ver.mismatch"));
        h.state_mut()
            .view
            .status
            .as_mut()
            .unwrap()
            .as_mut()
            .unwrap()
            .forwarder_version = None;
        h.run_steps(2);
        assert!(h.query_by_label(t(Lang::En, "ver.mismatch")).is_none());
        assert!(h.query_by_label_contains("NR forwarder").is_none());
        assert!(h.query_by_label_contains("OptiScaler 1.2.3.4").is_some());
    }

    #[test]
    pub(crate) fn root_changes_queue_a_fresh_scan_instead_of_racing_in_flight_discovery() {
        let mut app = app();
        app.scanning = true;
        app.rescan();
        assert!(app.rescan_pending);
        assert!(!app.jobs.busy());
        app.rescan();
        assert!(app.rescan_pending);
        assert!(!app.jobs.busy());
    }

    #[test]
    pub(crate) fn add_folder_asks_kind_and_manager_lists_both_kinds() {
        let mut app = app();
        app.saved.manual.push("/synthetic/Single".into());
        app.saved.libraries.push("/synthetic/Several".into());
        let mut h = Harness::builder().with_size([930., 900.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                if ui.button(t(app.lang, "lib.add_folder")).clicked() {
                    app.folders_open(FolderDialog::Add);
                }
                app.folders_button(ui);
                app.folders_dialog(ui.ctx());
            },
            app,
        );
        h.get_by_role_and_label(egui::Role::Button, "Add folder…")
            .click_accesskit();
        h.run_steps(3);
        h.get_by_role_and_label(egui::Role::Button, "One game");
        h.get_by_role_and_label(egui::Role::Button, "A folder with several games");
        h.key_press(egui::Key::Escape);
        h.run_steps(3);
        assert!(h.state().folder_dialog.is_none());
        h.get_by_role_and_label(egui::Role::Button, "Your folders")
            .click_accesskit();
        h.run_steps(3);
        h.get_by_label("Individual games and emulators");
        h.get_by_label("Folders with several games");
        h.get_by_label("/synthetic/Single");
        h.get_by_label("/synthetic/Several");
        h.get_by_role_and_label(egui::Role::Button, "Close").click_accesskit();
        h.run_steps(3);
        assert!(h.state().folder_dialog.is_none());
    }

    #[test]
    pub(crate) fn unregistered_library_row_keeps_name_and_shows_hint() {
        let mut app = app();
        app.library_state.games.push(Game {
            id: "orphan".into(),
            name: "Unlisted game".into(),
            store: Store::Epic,
            install_dir: "/synthetic/game".into(),
            runtime: Runtime::Unknown,
            targets: vec![],
            dlss_anywhere: vec![],
            registered: false,
        });
        let h = Harness::new_ui_state(|ui, app: &mut RetoqueApp| app.library(ui), app);
        h.get_by_role_and_label(egui::Role::Button, "Unlisted game");
        h.get_by_label(t(Lang::En, "folders.unregistered"));
    }

    #[test]
    pub(crate) fn gpu_header_names_architecture_memory_and_driver() {
        use crate::autotune::gpu::{Arch, Gpu, Vendor};
        let mut app = app();
        app.tuning.gpus = Some(vec![Gpu {
            name: "RTX 3060".into(),
            vendor: Vendor::Nvidia,
            arch: Arch::Ampere,
            compute_cap: Some((8, 6)),
            driver: Some("617.14".into()),
            vram_mib: Some(12288),
        }]);
        let harness = Harness::new_ui_state(|ui, app: &mut RetoqueApp| app.autotune_gpu(ui), app);
        harness.get_by_label("RTX 3060 · Ampere · 12 GB");
    }

    #[test]
    pub(crate) fn lsfg_is_gated_by_readiness_and_review_does_not_write() {
        use crate::autotune::lsfg::LsfgReadiness;
        let temp = tempfile::tempdir().unwrap();
        let ini = temp.path().join("OptiScaler.ini");
        let before = "[LSFG]\nEnabled=false\nMultiplier=2\n[FrameGen]\nEnabled=false\n";
        std::fs::write(&ini, before).unwrap();
        let mut app = app();
        app.view.target = Some(temp.path().into());
        app.view.ini = Some(Ok(Some(Ini::parse(before.as_bytes()))));
        app.view.status = Some(Ok(Status {
            receipt: Some(retoque_core::receipt::Receipt {
                status: ReceiptStatus::Complete,
                updated: String::new(),
                records: vec![],
            }),
            ..Default::default()
        }));
        app.tuning.installed = Some(Ok(vec![]));
        app.tuning.readiness = Some(LsfgReadiness::NoLosslessScaling);
        let mut harness = Harness::builder().with_size([930., 1200.]).build_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.receive();
                app.autotune_lsfg(ui);
                app.review_sheet(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        assert!(
            harness
                .get_by_role_and_label(egui::Role::Button, "x2")
                .accesskit_node()
                .is_disabled()
        );
        harness.state_mut().tuning.readiness = Some(LsfgReadiness::Ready);
        harness.run_steps(3);
        harness
            .get_by_role_and_label(egui::Role::Button, "x2")
            .click_accesskit();
        for _ in 0..100 {
            harness.step();
            if harness.state().review.is_some() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(5));
        }
        assert!(harness.state().review.is_some());
        assert!(harness.state().review_open);
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
        harness.key_press(egui::Key::Escape);
        harness.run_steps(3);
        assert!(!harness.state().review_open);
        harness.state_mut().tuning.installed = Some(Ok(vec!["component".into()]));
        harness.run_steps(3);
        assert!(
            harness
                .get_by_role_and_label(egui::Role::Button, "x2")
                .accesskit_node()
                .is_disabled()
        );
        assert_eq!(std::fs::read_to_string(&ini).unwrap(), before);
    }

    #[test]
    pub(crate) fn states_distinguish_updates_current_and_partial_cleanup() {
        let mut status = Status::default();
        assert_eq!(install_state(&status, Some("new")), InstallState::Missing);
        status.receipt = Some(retoque_core::receipt::Receipt {
            status: ReceiptStatus::Complete,
            updated: String::new(),
            records: vec![],
        });
        status.build_sha256 = Some("old".into());
        assert_eq!(install_state(&status, None), InstallState::Installed);
        assert_eq!(install_state(&status, Some("new")), InstallState::Update);
        assert_eq!(install_state(&status, Some("old")), InstallState::Current);
        status.receipt.as_mut().unwrap().status = ReceiptStatus::Partial;
        assert_eq!(install_state(&status, Some("old")), InstallState::Partial);
    }

    #[test]
    pub(crate) fn drafts_survive_target_switches_and_only_applied_values_are_cleared() {
        let temp = tempfile::tempdir().unwrap();
        let first = temp.path().join("first");
        let second = temp.path().join("second");
        std::fs::create_dir_all(&first).unwrap();
        std::fs::create_dir_all(&second).unwrap();
        let mut app = app();
        app.open_target(Some(first.clone()));
        let key = ("DlssNr".into(), "Stage".into());
        app.view.edits.insert(key.clone(), "1".into());
        app.open_target(Some(second.clone()));
        assert!(app.view.edits.is_empty());
        app.view.edits.insert(key.clone(), "0".into());
        app.open_target(Some(first));
        assert_eq!(app.view.edits.get(&key).map(String::as_str), Some("1"));
        let edits = vec![("DlssNr".into(), "Stage".into(), "0".into())];
        clear_applied(&mut app.view.edits, &edits);
        assert_eq!(app.view.edits.len(), 1);
        clear_applied(app.drafts.get_mut(&second).unwrap(), &edits);
        assert!(app.drafts[&second].is_empty());
    }

    #[test]
    pub(crate) fn search_focus_arrow_navigation_and_modal_escape_restore_focus() {
        let mut app = app();
        app.focus_search = false;
        for name in ["First", "Second"] {
            app.library_state.games.push(Game {
                dlss_anywhere: Vec::new(),
                registered: true,
                id: name.into(),
                name: name.into(),
                store: Store::Manual,
                install_dir: PathBuf::new(),
                runtime: Runtime::Unknown,
                targets: vec![],
            });
        }
        let mut harness = Harness::new_ui_state(
            |ui, app: &mut RetoqueApp| {
                app.context = ui.ctx().clone();
                app.shell_shortcuts(ui.ctx());
                app.search_field(ui);
                app.library_keyboard(ui);
                app.handle_commands();
                app.review_sheet(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        harness.key_press_modifiers(egui::Modifiers::COMMAND, egui::Key::F);
        harness.run_steps(3);
        assert!(
            harness
                .state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
        harness.get_by_role_and_label(egui::Role::TextInput, "Search games");
        harness.key_press(egui::Key::ArrowDown);
        harness.run_steps(3);
        assert_eq!(harness.state().selected.as_deref(), Some("First"));
        harness.key_press(egui::Key::ArrowDown);
        harness.run_steps(3);
        assert_eq!(harness.state().selected.as_deref(), Some("Second"));
        harness.state_mut().review_open = true;
        harness.state_mut().origin_focus = Some(egui::Id::unique("library_search"));
        harness.run_steps(3);
        harness.key_press(egui::Key::Escape);
        harness.run_steps(3);
        assert!(!harness.state().review_open);
        assert!(
            harness
                .state()
                .context
                .memory(|m| m.has_focus(egui::Id::unique("library_search")))
        );
    }
}
#[cfg(test)]
mod tab_content_tests {
    use super::*;

    #[test]
    pub(crate) fn model_inspection_uses_current_file_and_never_falls_back_for_a_named_missing_file() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("nvngx_dlssnr.dll");
        // Cross a streaming chunk boundary with the version signature.
        let mut bytes = vec![0; 65532];
        bytes.extend([0xbd, 0x04, 0xef, 0xfe, 0, 0, 1, 0]);
        bytes.extend(((310_u32 << 16) | 9).to_le_bytes());
        bytes.extend((16_u32 << 16).to_le_bytes());
        std::fs::write(&path, bytes).unwrap();
        let found = inspect_model(temp.path(), "auto");
        assert!(found.present);
        assert_eq!(found.version.as_deref(), Some("310.9.16.0"));
        assert!(!inspect_model(temp.path(), "missing.dll").present);
        std::fs::rename(&path, temp.path().join("renamed.dll")).unwrap();
        assert_eq!(inspect_model(temp.path(), "renamed.dll").version, found.version);
        assert!(!inspect_model(temp.path(), "auto").present);
        assert_eq!(model_host_path(temp.path(), "Z:relative.dll"), None);
    }

    #[test]
    pub(crate) fn log_verdict_does_not_promote_initialization_to_evaluation() {
        let mut summary = Summary {
            api: Some("Vulkan".into()),
            nr_initialized: true,
            ..Default::default()
        };
        assert!(log_verdict(Lang::En, &summary).starts_with("NR initialized; evaluation is not confirmed"));
        summary.coverage = Some(retoque_data::logs::Coverage {
            stage: "post".into(),
            model_ok: 3,
            ..Default::default()
        });
        assert_eq!(
            log_verdict(Lang::En, &summary),
            "NR evaluation recorded on Vulkan after the upscaler · 0 errors"
        );
        assert!(log_verdict(Lang::Pt, &summary).contains("depois da ampliação"));
    }

    #[cfg(not(windows))]
    #[test]
    pub(crate) fn boolean_control_can_restore_automatic_without_writing_an_ini() {
        use egui_kittest::{Harness, kittest::Queryable};
        let setting = settings::all()
            .iter()
            .find(|s| s.section == "DlssNr" && s.key == "Enabled")
            .unwrap();
        let mut harness = Harness::new_ui_state(
            |ui, value| {
                value_widget(ui, setting, value, "bool-test", Lang::En);
            },
            "true".to_owned(),
        );
        // Native egui buttons stay in the tab order and activate with Space.
        harness.key_down(egui::Key::Tab);
        harness.run_steps(1);
        harness.key_up(egui::Key::Tab);
        harness.run_steps(1);
        harness.key_down(egui::Key::Space);
        harness.run_steps(1);
        harness.key_up(egui::Key::Space);
        harness.run_steps(1);
        assert_eq!(harness.state(), "auto");
        harness.get_by_label("Off").click_accesskit();
        harness.run();
        assert_eq!(harness.state(), "false");
        harness.get_by_label("Automatic").click_accesskit();
        harness.run();
        assert_eq!(harness.state(), "auto");
    }
}

mod measurements {
    use super::*;
    use std::{
        alloc::{GlobalAlloc, Layout, System},
        cell::Cell,
        hint::black_box,
        time::Instant,
    };
    thread_local! { static COUNTING: Cell<bool> = const { Cell::new(false) }; static ALLOCATIONS: Cell<usize> = const { Cell::new(0) }; }
    struct Counter;
    fn count() {
        COUNTING.with(|enabled| {
            if enabled.get() {
                ALLOCATIONS.with(|n| n.set(n.get() + 1));
            }
        });
    }
    // Only this test thread is counted; worker allocations never pollute a frame measurement.
    unsafe impl GlobalAlloc for Counter {
        unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
            count();
            unsafe { System.alloc(layout) }
        }
        unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
            count();
            unsafe { System.alloc_zeroed(layout) }
        }
        unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, size: usize) -> *mut u8 {
            count();
            unsafe { System.realloc(ptr, layout, size) }
        }
        unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
            unsafe { System.dealloc(ptr, layout) }
        }
    }
    #[global_allocator]
    static ALLOCATOR: Counter = Counter;
    fn measure(name: &str, repeats: usize, mut f: impl FnMut()) -> usize {
        f();
        ALLOCATIONS.with(|n| n.set(0));
        COUNTING.with(|enabled| enabled.set(true));
        let start = Instant::now();
        for _ in 0..repeats {
            f();
        }
        let elapsed = start.elapsed();
        COUNTING.with(|enabled| enabled.set(false));
        let allocations = ALLOCATIONS.with(Cell::get);
        println!(
            "MEASURE {name}: {:.3} us/iteration, {:.2} allocations/iteration ({repeats} iterations)",
            elapsed.as_secs_f64() * 1e6 / repeats as f64,
            allocations as f64 / repeats as f64
        );
        allocations
    }
    #[test]
    #[ignore = "manual timed UI/derived-state measurement"]
    fn frame_cost_library_and_settings() {
        let mut app = shell_tests::app();
        for i in 0..500 {
            app.library_state.games.push(Game {
                id: i.to_string(),
                name: format!("Synthetic Game {i}"),
                store: Store::Manual,
                runtime: Runtime::Unknown,
                install_dir: PathBuf::new(),
                targets: vec![],
                registered: true,
                dlss_anywhere: vec![],
            });
        }
        app.library_state.search = "Game".into();
        app.refresh_library_cache();
        measure("library legacy filter 500 games", 2000, || {
            let needle = app.library_state.search.to_lowercase();
            let visible: Vec<_> = app
                .library_state
                .games
                .iter()
                .filter(|g| needle.is_empty() || g.name.to_lowercase().contains(&needle))
                .collect();
            let groups: (Vec<_>, Vec<_>) = visible.into_iter().partition(|_| false);
            black_box(groups);
        });
        let allocations = measure("library cached filter 500 games", 2000, || {
            app.refresh_library_cache();
            black_box(&app.library_state.derived);
        });
        assert_eq!(allocations, 0, "unchanged library projection must allocate nothing");
        let mut text = "[Unrelated]\n".to_owned();
        for i in 0..1000 {
            text.push_str(&format!("key{i}=value{i}\n"));
        }
        for section in ["DlssNr", "Upscalers", "FrameGen", "LSFG"] {
            text.push_str(&format!("[{section}]\n"));
            for setting in settings::all().iter().filter(|s| s.section == section) {
                text.push_str(&format!("{}=auto\n", setting.key));
            }
        }
        let ini = Ini::parse(text.as_bytes());
        let values = crate::state::settings::IniValues::read(&ini);
        measure("settings legacy INI lookup", 2000, || {
            for s in settings::all() {
                black_box(ini.get(s.section, s.key));
            }
        });
        let allocations = measure("settings cached INI lookup", 2000, || {
            for s in settings::all() {
                black_box(values.get(s.section, s.key));
            }
        });
        assert_eq!(allocations, 0, "cached settings lookups must allocate nothing");
        // Full view figures include egui layout/text allocations; they are not a claim of zero-allocation frames.
        app.library_state.games.truncate(100);
        app.library_state.invalidate();
        let mut h = egui_kittest::Harness::builder()
            .with_size([1200., 900.])
            .build_ui_state(|ui, app: &mut RetoqueApp| app.home_grid(ui), app);
        h.run_steps(5);
        measure("library home view 100 games", 50, || {
            h.step();
        });
        let mut app = shell_tests::app();
        app.view.target = Some("/synthetic/game".into());
        app.view.ini_values = values;
        app.view.ini = Some(Ok(Some(ini)));
        let mut h = egui_kittest::Harness::builder()
            .with_size([1200., 900.])
            .with_accessibility_check(false)
            .build_ui_state(
                |ui, app: &mut RetoqueApp| {
                    let mut edits = std::mem::take(&mut app.view.edits);
                    app.settings_tab(ui, &mut edits, &app.cmd_tx);
                    app.view.edits = edits;
                },
                app,
            );
        h.run_steps(5);
        measure("settings view", 50, || {
            h.step();
        });
    }
}

#[test]
fn remedies_show_pids_in_both_languages_without_starting_a_write() {
    use egui_kittest::kittest::Queryable;
    for lang in [Lang::En, Lang::Pt] {
        let mut app = shell_tests::app();
        app.lang = lang;
        let error = AppError::from(retoque_core::Error::Refused(vec![Refusal::GameRunning {
            processes: vec![(42, "synthetic-game".into())],
        }]));
        let mut h = egui_kittest::Harness::new_ui_state(
            move |ui, app: &mut RetoqueApp| {
                crate::error::card(ui, &error, app.lang, &app.cmd_tx);
                app.show_process_notice(ui.ctx());
                app.handle_commands();
            },
            app,
        );
        h.get_by_role_and_label(egui::Role::Button, t(lang, "remedy.processes"))
            .click_accesskit();
        h.run_steps(3);
        h.get_by_label("synthetic-game · PID 42");
        assert!(h.state().writing.is_empty());
        assert!(h.state().review.is_none());
    }
}

#[test]
fn command_for_previous_target_cannot_open_a_review_on_current_target() {
    let mut app = shell_tests::app();
    app.view.target = Some("/synthetic/second".into());
    app.cmd_tx.send(AppCommand::RequestPlan(
        "/synthetic/first".into(),
        PlanKind::Uninstall,
        PlanInputs::Uninstall,
    ));
    app.handle_commands();
    assert!(!app.review_open);
    assert!(app.review.is_none());
    assert!(!app.jobs.busy());
}
