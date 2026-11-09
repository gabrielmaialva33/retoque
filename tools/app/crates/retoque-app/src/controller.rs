use crate::app::*;

impl RetoqueApp {
    pub(crate) fn tr(&self, key: &'static str) -> &'static str {
        t(self.lang, key)
    }

    pub(crate) fn rescan(&mut self) {
        if self.scanning {
            self.rescan_pending = true;
            return;
        }
        self.rescan_pending = false;
        self.scanning = true;
        let manual = self.saved.manual.clone();
        let libraries = self.saved.libraries.clone();
        self.jobs
            .spawn("discover", None, move |_| model::discover(manual, libraries));
    }

    pub(crate) fn selected_game(&self) -> Option<&Game> {
        let id = self.selected.as_ref()?;
        self.library_state.games.iter().find(|g| &g.id == id)
    }

    pub(crate) fn target_for(&self, game: &Game) -> Option<PathBuf> {
        let picked = self.saved.targets.get(&game.id);
        picked
            .filter(|p| game.targets.iter().any(|t| &t.path == *p))
            .cloned()
            .or_else(|| game.targets.first().map(|t| t.path.clone()))
    }

    pub(crate) fn open_target(&mut self, target: Option<PathBuf>) {
        self.library_state.invalidate();
        self.jobs.watch(target.clone());
        self.jobs.advance_epoch();
        self.review = None;
        if let Some(old) = self.view.target.clone() {
            self.drafts.insert(old, self.view.edits.clone());
        }
        self.review_open = false;
        let edits = target
            .as_ref()
            .and_then(|t| self.drafts.get(t))
            .cloned()
            .unwrap_or_default();
        let mut options = InstallOptions {
            proxy: "dxgi.dll".into(),
            enable_nr: true,
            copy_nvngx: cfg!(not(windows)),
            ..Default::default()
        };
        if let Some(t) = &target {
            options.game_dir = t.clone();
        }
        self.view = GameView {
            target: target.clone(),
            options,
            edits,
            ..Default::default()
        };
        if let Some(t) = target {
            self.load_target(t);
        }
    }

    pub(crate) fn load_target(&mut self, t: PathBuf) {
        self.jobs.advance_epoch();
        self.autotune_refresh(t.clone());
        self.view.inspection = None;
        self.view.doctor = None;
        self.view.intelligence.latest_log = None;
        self.view.intelligence.log_diagnoses.clear();
        self.view.intelligence.recommendations = Default::default();
        let (a, b, c) = (t.clone(), t.clone(), t);
        self.jobs.spawn("status", None, move |_| model::status(a));
        self.jobs.spawn("ini", None, move |_| model::read_ini(b));
        self.jobs.spawn("logs", None, move |_| model::logs(c));
    }

    pub(crate) fn busy(&self) -> bool {
        self.view
            .target
            .as_ref()
            .is_some_and(|t| self.writing.keys().any(|p| t.starts_with(p) || p.starts_with(t)))
    }

    pub(crate) fn install_inputs(&self) -> Option<InstallOptions> {
        let target = self.view.target.clone()?;
        let mut options = self.view.options.clone();
        options.game_dir = target;
        options.source_dir = self.payload.clone()?;
        Some(options)
    }

    pub(crate) fn request_plan(&self, kind: PlanKind, inputs: PlanInputs) {
        if let Some(target) = &self.view.target {
            self.cmd_tx.send(AppCommand::RequestPlan(target.clone(), kind, inputs));
        }
    }

    pub(crate) fn start_plan(&mut self, kind: PlanKind, inputs: PlanInputs) {
        let Some(target) = self.view.target.clone() else { return };
        if kind == PlanKind::Uninstall && self.selected_game().is_some_and(|g| g.install_dir != target) {
            self.dlss_restore();
            return;
        }
        self.review_open = true;
        self.origin_focus = self.context.memory(|m| m.focused());
        self.next_request += 1;
        let request = self.next_request;
        self.view.request = request;
        self.review = None;
        self.view.applied = None;
        self.view.inputs = Some(inputs.clone());
        let (options, edits) = match inputs {
            PlanInputs::Install(o) => (o, Vec::new()),
            PlanInputs::Uninstall => (
                InstallOptions {
                    game_dir: target.clone(),
                    ..Default::default()
                },
                Vec::new(),
            ),
            PlanInputs::Settings(e) => (InstallOptions::default(), e),
        };
        self.jobs.spawn("plan", None, move |_| {
            model::plan(kind, request, target, options, edits)
        });
    }

    pub(crate) fn apply_plan(&mut self, kind: PlanKind, plan: Plan, ctx: egui::Context) {
        let Some(target) = self.view.target.clone() else { return };
        if self.writing.contains_key(&target) {
            return;
        }
        let edits = match &self.view.inputs {
            Some(PlanInputs::Settings(e)) => e.clone(),
            _ => Vec::new(),
        };
        self.review_open = false;
        self.restore_focus();
        self.jobs.begin_write(&target);
        self.writing.insert(target.clone(), (0, plan.steps().len()));
        self.write_kinds.insert(target.clone(), kind);
        let t = target.clone();
        let history_root = self.intuitive.history_root.clone();
        self.jobs.spawn("apply", Some(target), move |tx| {
            crate::history::apply_recorded(history_root.as_deref(), kind, plan, t, edits, tx, &ctx)
        });
    }

    pub(crate) fn receive(&mut self) {
        if self.usage_refreshed.is_none_or(|at| at.elapsed().as_secs() >= 30) {
            self.refresh_model_usage();
        }
        let mut intelligence_changed = false;
        for outcome in self.jobs.drain() {
            intelligence_changed |= matches!(
                &outcome,
                Outcome::Status { .. }
                    | Outcome::Ini { .. }
                    | Outcome::Logs { .. }
                    | Outcome::ModelInspection(_)
                    | Outcome::Catalog { .. }
                    | Outcome::Auto(_)
            );
            self.library_state.invalidate();
            let current = self.view.target.clone();
            let ours = |t: &Path| current.as_deref() == Some(t);
            match outcome {
                Outcome::DropClassified { request, paths } => self.intuitive.drop.receive(request, paths),
                Outcome::HistoryWritten(Err(error)) => {
                    self.toast(self.tr("undo.save_failed").into(), Some(error), true)
                }
                Outcome::Doctor(assessment) => self.receive_assessment(*assessment),
                Outcome::DoctorBadge {
                    generation,
                    target,
                    count,
                } if generation == self.library_generation && !ours(&target) => {
                    self.doctor_badges.insert(target, count);
                }
                Outcome::Watch { target, result, .. } => self.receive_watch(target, result),
                Outcome::Payload(path, result) if self.payload.as_ref() == Some(&path) => {
                    self.payload_build = Some(result)
                }
                Outcome::Library(generation, path, status) if generation == self.library_generation => {
                    if self.view.target.as_ref() != Some(&path) || self.view.status.is_none() {
                        let target = path.clone();
                        let inspected = status.clone();
                        self.jobs.submit(
                            crate::jobs::Lane::Fast,
                            false,
                            "library-doctor",
                            Some(target.clone()),
                            move |_| {
                                // Status only checks the adjacent model; it cannot prove an explicit ModelFile missing.
                                let unknown_model = retoque_data::doctor::ModelInspection::default();
                                let mut inputs = retoque_data::doctor::Inputs::new(&inspected);
                                inputs.model = Some(&unknown_model);
                                Outcome::DoctorBadge {
                                    generation,
                                    target,
                                    count: retoque_data::doctor::diagnose(&inputs).len(),
                                }
                            },
                        );
                        self.library_state.status.insert(path, *status);
                    }
                }
                Outcome::LibraryDone(generation) if generation == self.library_generation => {
                    self.library_pending = false
                }
                Outcome::ModelInspection(inspection) => self.view.inspection = Some(inspection),
                Outcome::ModelUsage(usage) => self.model_usage = usage,
                Outcome::Auto(event) => {
                    let refresh = matches!(*event, crate::autotune::Event::Facts { .. });
                    self.autotune_event(*event);
                    if refresh {
                        self.lsfg_recheck();
                    }
                }
                Outcome::Dlss(event) => self.dlss_event(event),
                Outcome::Lsfg(event) => self.lsfg_event(event),
                Outcome::Discovered { games, skipped, art } => {
                    if self.rescan_pending {
                        self.scanning = false;
                        self.rescan();
                        continue;
                    }
                    self.library_state.games = games;
                    self.library_state.art = art;
                    self.skipped = skipped;
                    self.scanning = false;
                    self.inspect_library();
                    if let Some(needle) = self.pending_select.take()
                        && let Some(id) = self
                            .library_state
                            .games
                            .iter()
                            .find(|g| g.name.to_lowercase().contains(&needle))
                            .map(|g| g.id.clone())
                    {
                        self.selected = Some(id);
                        let target = self.selected_game().and_then(|g| self.target_for(g));
                        self.open_target(target);
                    }
                    if self
                        .selected
                        .as_ref()
                        .is_some_and(|id| !self.library_state.games.iter().any(|g| &g.id == id))
                    {
                        self.selected = None;
                        self.open_target(None);
                    }
                }
                Outcome::Status { target, result } if ours(&target) => {
                    if let Ok(s) = &result {
                        self.library_state.status.insert(target.clone(), s.clone());
                        if let Some(proxy) = s.receipt.as_ref().and_then(first_proxy) {
                            self.view.options.proxy = proxy;
                        }
                        if let Some(nr) = s.nr_enabled {
                            self.view.options.enable_nr = nr || s.receipt.is_none();
                        }
                    }
                    self.view.status = Some(result);
                }
                Outcome::Ini { target, result } if ours(&target) => {
                    self.view.ini = Some(result.map(|o| o.map(|(_, b)| Ini::parse(&b))));
                    self.view.ini_values = self
                        .view
                        .ini
                        .as_ref()
                        .and_then(|r| r.as_ref().ok())
                        .and_then(|i| i.as_ref())
                        .map(crate::state::settings::IniValues::read)
                        .unwrap_or_default();
                    self.refresh_model_inspection();
                }
                Outcome::Plan {
                    target,
                    kind,
                    request,
                    result,
                    groups,
                } if ours(&target) && request == self.view.request => {
                    let installed = matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_some());
                    let title = self.tr(match kind {
                        PlanKind::Install if installed => "shell.review_update",
                        PlanKind::Install => "shell.review_install",
                        PlanKind::Uninstall => "shell.review_remove",
                        PlanKind::Settings => "set.save",
                    });
                    let title = if self.intuitive.undo_request == Some(request) {
                        self.tr("undo.title")
                    } else {
                        title
                    };
                    let game = self.selected_game().map(|g| g.name.as_str()).unwrap_or("");
                    self.review = Some(result.map(|plan| ReviewSession {
                        title: format!("{title} · {game}"),
                        kind: ReviewKind::Standard(kind),
                        plans: vec![plan],
                        groups: vec![groups],
                        request_id: request,
                    }));
                }
                Outcome::Progress { target, done } => {
                    if let Some(p) = self.writing.get_mut(&target) {
                        *p = (done.index + 1, done.total);
                    }
                }
                Outcome::Applied {
                    target,
                    kind,
                    edits,
                    result,
                } => {
                    self.finish_compound_history(&target, result.is_ok());
                    // The folder is free again whether or not the user is still looking at it.
                    self.jobs.end_write(&target);
                    if let Some(roots) = self.tuning.write_roots.remove(&target) {
                        for root in roots {
                            self.writing.remove(&root);
                        }
                    }
                    self.writing.remove(&target);
                    self.write_kinds.remove(&target);
                    let game_name = self
                        .library_state
                        .games
                        .iter()
                        .find(|g| g.targets.iter().any(|t| t.path == target))
                        .map(|g| g.name.clone())
                        .unwrap_or_else(|| short_path(&target));
                    if self.tuning.writes.remove(&target) {
                        self.toast(
                            format!(
                                "{} · {game_name}",
                                self.tr(if result.is_ok() { "auto.done" } else { "shell.failed" })
                            ),
                            result.as_ref().err().cloned(),
                            result.is_err(),
                        );
                    } else {
                        self.notify_action(kind, &game_name, &result);
                    }
                    if result.is_ok()
                        && let Some(draft) = self.drafts.get_mut(&target)
                    {
                        clear_applied(draft, &edits);
                    }
                    if ours(&target) {
                        if result.is_ok() {
                            // Only what this plan carried leaves the pending list; later edits stay.
                            for (section, key, value) in &edits {
                                let k = (section.clone(), key.clone());
                                if self.view.edits.get(&k) == Some(value) {
                                    self.view.edits.remove(&k);
                                }
                            }
                        }
                        self.view.applied = Some((kind, result));
                        self.load_target(target);
                    }
                    if kind != PlanKind::Settings {
                        self.rescan();
                    }
                }
                Outcome::Logs {
                    latest,
                    target,
                    files,
                    result,
                } if ours(&target) => {
                    if latest {
                        self.view.intelligence.latest_log = result.as_ref().ok().and_then(|s| s.as_ref()).cloned();
                    }
                    self.view.logs = Some((files, result));
                }
                Outcome::Bundle { target, result } if ours(&target) => {
                    self.toast(
                        self.tr(if result.is_ok() {
                            "shell.bundle_done"
                        } else {
                            "shell.failed"
                        })
                        .into(),
                        result.as_ref().err().cloned(),
                        result.is_err(),
                    );
                    self.view.bundle = Some(result);
                }
                Outcome::Catalog { result } => {
                    if self.catalog.is_some()
                        && result.as_ref().is_ok_and(|models| {
                            self.catalog
                                .as_ref()
                                .and_then(|r| r.as_ref().ok())
                                .is_some_and(|old| old != models)
                        })
                    {
                        self.toast(self.tr("shell.catalog_changed").into(), None, false);
                    }
                    self.catalog = Some(result);
                    self.usage_refreshed = None;
                }
                Outcome::Failed { job, target, message } => {
                    self.toast(self.tr("shell.failed").into(), Some(message.clone().into()), true);
                    self.error = Some(format!("{} ({job}): {message}", self.tr("job.failed")).into());
                    if job == "dlss-download" {
                        self.tuning.dlss.download_failed();
                    }
                    if job == "autotune-plan" {
                        self.review = Some(Err(message.clone().into()));
                    }
                    if job == "library-status" {
                        self.library_pending = false;
                    }
                    if job == "discover" {
                        self.scanning = false;
                        if self.rescan_pending {
                            self.rescan();
                        }
                    }
                    // A write that died part-way may have changed the folder: release it and read it again.
                    if matches!(job, "apply" | "autotune-apply")
                        && let Some(t) = target
                    {
                        self.finish_compound_history(&t, false);
                        self.tuning.writes.remove(&t);
                        self.jobs.end_write(&t);
                        if let Some(roots) = self.tuning.write_roots.remove(&t) {
                            for root in roots {
                                self.writing.remove(&root);
                            }
                        }
                        self.writing.remove(&t);
                        self.write_kinds.remove(&t);
                        if ours(&t) {
                            self.load_target(t);
                        }
                    }
                }
                // A result for a folder the user has left.
                _ => {}
            }
        }
        if intelligence_changed {
            self.refresh_intelligence();
        }
    }

    pub(crate) fn shell_shortcuts(&mut self, ctx: &egui::Context) {
        if self.intuitive_shortcuts(ctx) {
            return;
        }
        if self.review_open || self.folder_dialog.is_some() {
            return;
        }
        if ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::F)) {
            self.focus_search = true;
            self.sidebar_visible = true;
        }
        if !self.busy() && ctx.input_mut(|i| i.consume_key(egui::Modifiers::COMMAND, egui::Key::Enter)) {
            let partial = matches!(&self.view.status,Some(Ok(s)) if install_state(s,None)==InstallState::Partial);
            if partial {
                self.request_plan(PlanKind::Uninstall, PlanInputs::Uninstall);
            } else if let Some(options) = self.install_inputs() {
                self.request_plan(PlanKind::Install, PlanInputs::Install(options));
            }
        }
    }

    pub(crate) fn hash_payload(&mut self) {
        self.library_state.invalidate();
        self.payload_build = None;
        let Some(path) = self.payload.clone() else { return };
        self.jobs.spawn("payload", None, move |_| {
            use sha2::{Digest, Sha256};
            use std::io::Read;
            let result = (|| {
                let mut file = std::fs::File::open(path.join("OptiScaler.dll"))?;
                let mut hash = Sha256::new();
                let mut bytes = [0; 65536];
                loop {
                    let count = file.read(&mut bytes)?;
                    if count == 0 {
                        break;
                    }
                    hash.update(&bytes[..count]);
                }
                Ok::<_, std::io::Error>(format!("{:x}", hash.finalize()))
            })()
            .map_err(AppError::from);
            Outcome::Payload(path, result)
        });
    }

    pub(crate) fn inspect_library(&mut self) {
        self.library_state.invalidate();
        self.library_generation += 1;
        self.library_pending = true;
        self.library_state.status.clear();
        self.usage_refreshed = None;
        let generation = self.library_generation;
        let paths: Vec<_> = self
            .library_state
            .games
            .iter()
            .filter_map(|g| self.target_for(g))
            .collect();
        let ctx = self.context.clone();
        self.jobs.spawn("library-status", None, move |tx| {
            for path in paths {
                if let Ok(status) = retoque_core::status(&path) {
                    let _ = tx.send(Outcome::Library(generation, path, Box::new(status)));
                    ctx.request_repaint();
                }
            }
            Outcome::LibraryDone(generation)
        });
    }

    pub(crate) fn refresh_model_inspection(&mut self) {
        self.view.inspection = None;
        let Some(target) = self.view.target.clone() else { return };
        let configured = self
            .view
            .ini
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .and_then(|i| i.as_ref())
            .and_then(|i| i.get("DlssNr", "ModelFile"))
            .unwrap_or("auto")
            .to_owned();
        self.jobs.spawn("model-inspection", None, move |_| {
            Outcome::ModelInspection(inspect_model(&target, &configured))
        });
    }

    pub(crate) fn refresh_model_usage(&mut self) {
        self.usage_refreshed = Some(std::time::Instant::now());
        let Some(Ok(models)) = &self.catalog else { return };
        let models: Vec<_> = models.iter().map(|m| m.path.clone()).collect();
        let targets: std::collections::HashSet<_> = self
            .library_state
            .games
            .iter()
            .filter_map(|g| self.target_for(g))
            .collect();
        self.jobs.spawn("model-usage", None, move |_| {
            let mut usage: HashMap<_, usize> = models.into_iter().map(|p| (p, 0)).collect();
            for target in targets {
                if let Ok(Some((_, bytes))) = retoque_core::read_ini(&target) {
                    let ini = Ini::parse(&bytes);
                    if let Some(path) = model_host_path(&target, ini.get("DlssNr", "ModelFile").unwrap_or("auto"))
                        && let Some(count) = usage.get_mut(&path)
                    {
                        *count += 1;
                    }
                }
            }
            Outcome::ModelUsage(usage)
        });
    }

    pub(crate) fn select_id(&mut self, id: String) {
        self.cmd_tx.send(AppCommand::SelectGame(id));
    }

    pub(crate) fn handle_commands(&mut self) {
        while let Ok(command) = self.cmd_rx.try_recv() {
            match command {
                AppCommand::SelectGame(id) => self.select_game(id),
                AppCommand::UndoSettings(target) => self.start_undo(target),
                AppCommand::PrepareRepair(target, repair) => self.prepare_repair(target, repair),
                AppCommand::RefreshSelected => {
                    if let Some(target) = self.view.target.clone()
                        && !self.busy()
                    {
                        self.jobs.watch(Some(target.clone()));
                        self.load_target(target.clone());
                        self.compare.invalidate_captures(&target);
                    }
                }
                AppCommand::RequestPlan(target, kind, inputs) => {
                    if self.view.target.as_ref() == Some(&target) {
                        self.start_plan(kind, inputs);
                    }
                }
                AppCommand::RescanLibrary => self.rescan(),
                AppCommand::Remedy(remedy, target) => self.remediate(remedy, target),
                AppCommand::SwitchTab(tab) => self.tab = tab,
                AppCommand::OpenFolder(path) => model::open_folder(&path),
                AppCommand::RefreshModel => self.refresh_model_inspection(),
                AppCommand::ImportModel(path) => self.jobs.spawn("import", None, move |_| model::catalog_import(path)),
                AppCommand::RemoveModel(sha) => self.jobs.spawn("remove", None, move |_| model::catalog_remove(sha)),
                AppCommand::Bundle => {
                    if !self.busy()
                        && let Some(target) = self.view.target.clone()
                    {
                        let status = format!(
                            "{:#?}\nversion_mismatch={}",
                            self.view.status,
                            self.view
                                .status
                                .as_ref()
                                .and_then(|s| s.as_ref().ok())
                                .is_some_and(Status::version_mismatch)
                        );
                        self.view.bundle = None;
                        self.jobs.spawn("bundle", None, move |_| {
                            model::bundle(target, status, model::bundle_dir())
                        });
                    }
                }
                AppCommand::ReadLogs(selected) => {
                    if let Some(target) = self.view.target.clone() {
                        let files = self
                            .view
                            .logs
                            .as_ref()
                            .map(|(files, _)| files.clone())
                            .unwrap_or_default();
                        self.view.logs = None;
                        self.jobs.spawn("logs", None, move |_| match selected {
                            None => model::logs(target),
                            Some(path) => Outcome::Logs {
                                latest: false,
                                target,
                                files,
                                result: retoque_data::logs::summarize(&path).map(Some).map_err(AppError::from),
                            },
                        });
                    }
                }
                AppCommand::CancelReview => {
                    self.review_open = false;
                    self.restore_focus();
                }
                AppCommand::ConfirmReview(request) => self.confirm_review(request),
            }
        }
    }

    pub(crate) fn select_game(&mut self, id: String) {
        if self.selected.as_ref() != Some(&id) {
            self.selected = Some(id);
            let target = self.selected_game().and_then(|g| self.target_for(g));
            self.open_target(target);
        }
    }

    pub(crate) fn restore_focus(&mut self) {
        self.restore_frames = 2;
        self.context.request_repaint();
    }

    pub(crate) fn review_stale(&self) -> bool {
        matches!(&self.review, Some(Ok(r)) if matches!(r.kind, ReviewKind::Standard(PlanKind::Install)))
            && self.view.inputs != self.install_inputs().map(PlanInputs::Install)
    }

    pub(crate) fn confirm_review(&mut self, request: u64) {
        if !self.review_open || self.review_stale() || self.busy() {
            return;
        }
        let Some(Ok(review)) = self.review.as_ref() else { return };
        if review.request_id != request || !review.allowed() {
            return;
        }
        let review = review.clone();
        match review.kind {
            ReviewKind::Standard(kind) => {
                self.apply_plan(kind, review.plans.into_iter().next().unwrap(), self.context.clone())
            }
            ReviewKind::Autotune(request) => {
                let Some(target) = self.view.target.clone() else { return };
                self.capture_compound_history(&target, &review.plans);
                self.autotune_apply(crate::autotune::Prepared {
                    target,
                    request: *request,
                    plans: review.plans,
                    groups: review.groups,
                });
            }
        }
    }

    pub(crate) fn review_sheet(&mut self, ctx: &egui::Context) {
        if !self.review_open {
            if self.restore_frames > 0 {
                if let Some(id) = self.origin_focus {
                    ctx.memory_mut(|m| m.request_focus(id));
                }
                self.restore_frames -= 1;
                ctx.request_repaint();
            }
            return;
        }
        let modal = egui::Modal::new(egui::Id::unique("plan_review"))
            .frame(
                egui::Frame::new()
                    .fill(theme::CARD)
                    .stroke(egui::Stroke::new(1.0, theme::CARD_EDGE))
                    .corner_radius(12)
                    .inner_margin(20),
            )
            .show(ctx, |ui| {
                ui.set_width((ctx.content_rect().width() - 64.0).clamp(280.0, 740.0));
                match &self.review {
                    Some(Ok(review)) => {
                        let installed = matches!(&self.view.status, Some(Ok(s)) if s.receipt.is_some());
                        let label = self.tr(match review.kind {
                            ReviewKind::Autotune(ref r)
                                if matches!(r.dlss, Some(crate::dlss_ui::Change::Restore { .. })) =>
                            {
                                "inst.uninstall"
                            }
                            ReviewKind::Autotune(_) => "auto.confirm",
                            ReviewKind::Standard(PlanKind::Install) if installed => "inst.update",
                            ReviewKind::Standard(PlanKind::Install) => "inst.install",
                            ReviewKind::Standard(PlanKind::Uninstall) => "inst.uninstall",
                            ReviewKind::Standard(PlanKind::Settings) => "set.apply",
                        });
                        if self.intuitive.undo_request == Some(review.request_id) {
                            ui.label(self.tr("undo.auto_note"));
                        }
                        crate::modals::review::show(
                            ui,
                            review,
                            self.lang,
                            self.review_stale(),
                            self.busy(),
                            label,
                            &self.cmd_tx,
                        );
                    }
                    other => {
                        if let Some(Err(error)) = other {
                            ui.colored_label(theme::BAD, error);
                            crate::error::buttons(ui, error, self.lang, &self.cmd_tx);
                        } else {
                            ui.spinner();
                            ui.label(self.tr("shell.preparing"));
                        }
                        if ui.button(self.tr("cancel")).clicked() {
                            self.cmd_tx.send(AppCommand::CancelReview);
                        }
                    }
                }
            });
        if modal.should_close() {
            self.cmd_tx.send(AppCommand::CancelReview);
        }
    }

    pub(crate) fn notify_action(&mut self, kind: PlanKind, game: &str, result: &Result<(), AppError>) {
        let key = match (kind, result.is_ok()) {
            (_, false) => "shell.failed",
            (PlanKind::Install, true) => "shell.installed_done",
            (PlanKind::Uninstall, true) => "shell.removed_done",
            (PlanKind::Settings, true) => "shell.settings_done",
        };
        self.toast(
            format!("{} · {game}", self.tr(key)),
            result
                .as_ref()
                .err()
                .cloned()
                .or_else(|| Some(self.tr("shell.done_details").into())),
            result.is_err(),
        );
    }

    pub(crate) fn toast(&mut self, message: String, detail: Option<AppError>, failed: bool) {
        self.toasts.push(ActionToast {
            message,
            detail,
            failed,
            created: std::time::Instant::now(),
            details_open: false,
        });
        if self.toasts.len() > 3 {
            self.toasts.remove(0);
        }
    }
}
pub(crate) fn first_proxy(r: &retoque_core::receipt::Receipt) -> Option<String> {
    r.records.iter().find_map(|rec| match rec {
        retoque_core::receipt::Record::Proxy { name } => Some(name.clone()),
        _ => None,
    })
}

impl RetoqueApp {
    pub(crate) fn remediate(&mut self, remedy: Remedy, target: Option<PathBuf>) {
        match remedy {
            Remedy::OpenFolder(path) => model::open_folder(&path),
            Remedy::ChoosePayloadDir => {
                if let Some(dir) = rfd::FileDialog::new().pick_folder() {
                    if model::is_payload(&dir) {
                        self.saved.payload = Some(dir.clone());
                        self.payload = Some(dir);
                        self.hash_payload();
                        self.error = None;
                    } else {
                        self.error = Some(format!("{}: {}", dir.display(), self.tr("app.payload.none")).into());
                    }
                }
            }
            Remedy::CloseRunningProcesses(processes) => self.process_notice = Some(processes),
            Remedy::KeepOrReplaceForeign if target == self.view.target => {
                self.review_open = false;
                self.tab = Tab::Install;
                self.restore_focus();
            }
            Remedy::ReplanStale if target.is_some() && target == self.view.target => {
                if let Some(Ok(review)) = self.review.clone()
                    && let ReviewKind::Autotune(request) = review.kind
                {
                    self.autotune_request(review.title, *request);
                } else if let Some(inputs) = self.view.inputs.clone() {
                    let kind = match &inputs {
                        PlanInputs::Install(_) => PlanKind::Install,
                        PlanInputs::Uninstall => PlanKind::Uninstall,
                        PlanInputs::Settings(_) => PlanKind::Settings,
                    };
                    let inputs = if kind == PlanKind::Install {
                        self.install_inputs().map(PlanInputs::Install)
                    } else {
                        Some(inputs)
                    };
                    if let Some(inputs) = inputs {
                        self.start_plan(kind, inputs);
                    }
                }
            }
            _ => {}
        }
    }
    pub(crate) fn show_process_notice(&mut self, ctx: &egui::Context) {
        let Some(processes) = &self.process_notice else { return };
        let response = egui::Modal::new(egui::Id::unique("running_processes")).show(ctx, |ui| {
            ui.heading(self.tr("remedy.close_processes"));
            for (pid, name) in processes {
                ui.label(format!("{name} · PID {pid}"));
            }
            ui.button(self.tr("cancel")).clicked()
        });
        if response.inner || response.should_close() {
            self.process_notice = None;
        }
    }
}
