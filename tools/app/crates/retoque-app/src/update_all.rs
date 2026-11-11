//! Library-wide updates: each row owns the same sealed install Plan as the game page. No batch defaults,
//! implicit downloads, automatic replanning or parallel game writes.
use crate::app::*;
use retoque_core::{Status, receipt::Record};
use std::{path::Path, sync::mpsc::Sender};

#[derive(Default)]
pub(crate) struct State {
    pub open: bool,
    pub preparing: bool,
    pub running: bool,
    pub finished: bool,
    pub rows: Vec<Row>,
    pub error: Option<AppError>,
    focus: Option<egui::Id>,
}
#[derive(Clone)]
pub struct Row {
    pub(crate) name: String,
    pub(crate) target: PathBuf,
    current: Build,
    new: Build,
    pub(crate) plan: Option<Plan>,
    pub(crate) result: RowResult,
}
#[derive(Clone, Default)]
struct Build {
    version: Option<String>,
    hash: Option<String>,
}
impl Build {
    fn label(&self, lang: Lang) -> &str {
        self.version
            .as_deref()
            .unwrap_or(t(lang, if self.hash.is_some() { "st.build" } else { "ver.unknown" }))
    }
}
#[derive(Clone)]
pub struct Skip {
    pub(crate) key: &'static str,
    detail: Option<AppError>,
}
impl Skip {
    fn new(key: &'static str) -> Self {
        Self { key, detail: None }
    }
    fn error(error: AppError) -> Self {
        let key = match &error.kind {
            crate::error::ErrorKind::Core(error) => match error.as_ref() {
                retoque_core::Error::Refused(refusals)
                    if refusals.iter().any(|r| matches!(r, Refusal::GameRunning { .. })) =>
                {
                    "update_all.running"
                }
                retoque_core::Error::Refused(_) => "update_all.refused",
                _ => "update_all.unknown",
            },
            _ => "update_all.unknown",
        };
        Self {
            key,
            detail: Some(error),
        }
    }
}
#[derive(Clone)]
pub enum RowResult {
    Ready,
    Updating,
    Updated,
    Skipped(Skip),
    Failed(AppError),
}
pub enum Event {
    Prepared(Vec<Row>),
    Started(usize),
    Completed(usize, RowResult),
    Done,
}

/// Receipt-owned choices, never Preferences. Existing INI bytes survive the normal install path. In particular,
/// `auto`/absent NR is not converted to enabled, and a deliberately disabled ReShade loader stays disabled.
pub(crate) fn update_options(target: &Path, source: &Path, status: &Status, ini: &Ini) -> Option<InstallOptions> {
    let receipt = status.receipt.as_ref()?;
    let proxy = crate::controller::first_proxy(receipt)?;
    Some(InstallOptions {
        game_dir: target.to_owned(),
        source_dir: source.to_owned(),
        proxy,
        enable_nr: status.nr_enabled == Some(true),
        keep_reshade: ini
            .get("Plugins", "LoadReshade")
            .is_some_and(|v| v.eq_ignore_ascii_case("true") || v == "1")
            && receipt
                .records
                .iter()
                .any(|r| matches!(r, Record::Rename { to, .. } if to.to_ascii_lowercase().ends_with("reshade64.dll"))),
        copy_nvngx: receipt
            .records
            .iter()
            .any(|r| matches!(r, Record::File { path, .. } if path.eq_ignore_ascii_case("_nvngx.dll"))),
        replace_foreign: false,
        allow_second_proxy: false,
    })
}

pub(crate) fn prepare(games: Vec<(String, PathBuf)>, source: &Path) -> Vec<Row> {
    let new = Build {
        version: retoque_core::pe_version::file_version(&source.join("OptiScaler.dll")),
        hash: retoque_core::status(source).ok().and_then(|s| s.build_sha256),
    };
    let mut seen: Vec<PathBuf> = Vec::new();
    games
        .into_iter()
        .map(|(name, target)| {
            let mut row = Row {
                name,
                target,
                current: Build::default(),
                new: new.clone(),
                plan: None,
                result: RowResult::Skipped(Skip::new("update_all.unknown")),
            };
            let result = (|| -> Result<Option<Plan>, Skip> {
                let status = retoque_core::status(&row.target).map_err(|e| Skip {
                    key: "update_all.unknown",
                    detail: Some(e.into()),
                })?;
                row.current = Build {
                    version: status.proxy_version.clone(),
                    hash: status.build_sha256.clone(),
                };
                match status.receipt.as_ref().map(|r| r.status) {
                    Some(ReceiptStatus::Complete) => {}
                    Some(_) => return Err(Skip::new("update_all.partial")),
                    None => return Err(Skip::new("update_all.foreign")),
                }
                let (Some(old), Some(new)) = (&row.current.hash, &new.hash) else {
                    return Err(Skip::new("update_all.unknown"));
                };
                if old == new {
                    return Err(Skip::new("update_all.current"));
                }
                let (_, bytes) = retoque_core::read_ini(&row.target)
                    .map_err(|e| Skip::error(e.into()))?
                    .ok_or_else(|| Skip::new("update_all.unknown"))?;
                let options = update_options(&row.target, source, &status, &Ini::parse(&bytes))
                    .ok_or_else(|| Skip::new("update_all.unknown"))?;
                // This is exactly the per-game update planning entrypoint, including all core refusals.
                let Outcome::Plan { result, .. } =
                    model::plan(PlanKind::Install, 0, row.target.clone(), options, vec![])
                else {
                    unreachable!()
                };
                let plan = result.map_err(Skip::error)?;
                if !plan.refusals().is_empty() {
                    return Err(Skip::error(
                        retoque_core::Error::Refused(plan.refusals().to_vec()).into(),
                    ));
                }
                if seen
                    .iter()
                    .any(|root| plan.game_dir().starts_with(root) || root.starts_with(plan.game_dir()))
                {
                    return Err(Skip::new("update_all.overlap"));
                }
                seen.push(plan.game_dir().to_owned());
                Ok(Some(plan))
            })();
            match result {
                Ok(plan) => {
                    row.plan = plan;
                    row.result = RowResult::Ready;
                }
                Err(skip) => row.result = RowResult::Skipped(skip),
            }
            row
        })
        .collect()
}

pub(crate) fn apply_batch(plans: Vec<(usize, Plan)>, tx: &Sender<Outcome>, ctx: &egui::Context) -> Outcome {
    for (index, plan) in plans {
        let _ = tx.send(Outcome::UpdateAll(Event::Started(index)));
        ctx.request_repaint();
        // Preflight each reviewed plan immediately before its turn: a game may have started since the dialog.
        // A refusal here wrote nothing. A failure after apply began may be partial and is never called skipped.
        let result = match retoque_core::validate_plan(&plan) {
            Err(error @ retoque_core::Error::Refused(_)) => RowResult::Skipped(Skip::error(error.into())),
            Err(error) => RowResult::Failed(error.into()),
            Ok(()) => {
                let target = plan.game_dir().to_owned();
                match model::apply(PlanKind::Install, plan, target, vec![], tx, ctx) {
                    Outcome::Applied { result: Ok(()), .. } => RowResult::Updated,
                    Outcome::Applied { result: Err(error), .. } => RowResult::Failed(error),
                    _ => unreachable!(),
                }
            }
        };
        let _ = tx.send(Outcome::UpdateAll(Event::Completed(index, result)));
        ctx.request_repaint();
    }
    Outcome::UpdateAll(Event::Done)
}

impl RetoqueApp {
    pub(crate) fn update_all_button(&mut self, ui: &mut egui::Ui) {
        if self.library_state.derived.updates == 0 {
            return;
        }
        if ui
            .add_enabled(
                self.writing.is_empty() && self.payload.is_some(),
                theme::primary(self.tr("update_all.open")),
            )
            .clicked()
        {
            self.start_update_all();
        }
    }
    fn start_update_all(&mut self) {
        if self.library_state.update_all.preparing || self.library_state.update_all.running {
            return;
        }
        let Some(source) = self.payload.clone() else {
            return;
        };
        // Match the library badge, honoring the chosen target when it has that state.
        // Include non-update installs as well, so incomplete/unregistered/unknown ones are explicitly accounted for.
        self.refresh_library_cache();
        let payload = self
            .payload_build
            .as_ref()
            .and_then(|r| r.as_ref().ok())
            .map(String::as_str);
        let games = self
            .library_state
            .games
            .iter()
            .enumerate()
            .filter(|(i, _)| self.library_state.derived.states[*i] != InstallState::Missing)
            .filter_map(|(i, g)| {
                let expected = self.library_state.derived.states[i];
                let matches = |target: &&retoque_discovery::TargetDir| {
                    crate::state::library::single_target_install_state(target, &self.library_state.status, payload)
                        == expected
                };
                let preferred = self.target_for(g);
                let target = g
                    .targets
                    .iter()
                    .find(|t| Some(&t.path) == preferred.as_ref())
                    .filter(matches)
                    .or_else(|| g.targets.iter().find(matches))?;
                Some((g.name.clone(), target.path.clone()))
            })
            .collect();
        self.library_state.update_all = State {
            open: true,
            preparing: true,
            focus: self.context.memory(|m| m.focused()),
            ..Default::default()
        };
        self.jobs.spawn("update-all-plan", None, move |_| {
            Outcome::UpdateAll(Event::Prepared(prepare(games, &source)))
        });
    }
    fn confirm_update_all(&mut self) {
        let state = &mut self.library_state.update_all;
        if state.preparing || state.running || state.finished {
            return;
        }
        let mut plans = Vec::new();
        for (index, row) in state.rows.iter_mut().enumerate() {
            if !matches!(row.result, RowResult::Ready) {
                continue;
            }
            let Some(plan) = row.plan.take() else {
                continue;
            };
            if !plan.refusals().is_empty() {
                row.result = RowResult::Skipped(Skip::error(
                    retoque_core::Error::Refused(plan.refusals().to_vec()).into(),
                ));
                continue;
            }
            if self
                .writing
                .keys()
                .any(|p| p.starts_with(plan.game_dir()) || plan.game_dir().starts_with(p))
            {
                row.result = RowResult::Skipped(Skip::new("update_all.busy"));
                continue;
            }
            // Reserve every queued target as well as the active one: navigation cannot start a competing writer.
            row.target = plan.game_dir().to_owned();
            self.jobs.begin_write(&row.target);
            self.writing.insert(row.target.clone(), (0, plan.steps().len()));
            plans.push((index, plan));
        }
        state.running = true;
        let ctx = self.context.clone();
        self.jobs
            .spawn("update-all-apply", None, move |tx| apply_batch(plans, tx, &ctx));
    }
    pub(crate) fn update_all_event(&mut self, event: Event) {
        match event {
            Event::Prepared(rows) => {
                self.library_state.update_all.preparing = false;
                self.library_state.update_all.rows = rows;
            }
            Event::Started(index) => self.library_state.update_all.rows[index].result = RowResult::Updating,
            Event::Completed(index, result) => {
                let row = &mut self.library_state.update_all.rows[index];
                row.result = result;
                self.jobs.end_write(&row.target);
                self.writing.remove(&row.target);
            }
            Event::Done => {
                self.library_state.update_all.running = false;
                self.library_state.update_all.finished = true;
                let rows = &self.library_state.update_all.rows;
                let updated = rows.iter().filter(|r| matches!(r.result, RowResult::Updated)).count();
                let skipped = rows
                    .iter()
                    .filter(|r| matches!(r.result, RowResult::Skipped(_)))
                    .count();
                let failed = rows.iter().filter(|r| matches!(r.result, RowResult::Failed(_))).count();
                let message = self
                    .tr("update_all.done")
                    .replace("{updated}", &updated.to_string())
                    .replace("{skipped}", &skipped.to_string())
                    .replace("{failed}", &failed.to_string());
                self.toast(message, None, failed > 0);
                self.inspect_library();
                if let Some(target) = self.view.target.clone() {
                    self.load_target(target);
                }
            }
        }
    }
    pub(crate) fn update_all_failed(&mut self, job: &str, error: AppError) {
        let state = &mut self.library_state.update_all;
        state.preparing = false;
        state.error = Some(error.clone());
        if job == "update-all-apply" {
            for row in &mut state.rows {
                if matches!(row.result, RowResult::Ready | RowResult::Updating) {
                    row.result = RowResult::Failed(error.clone());
                    self.jobs.end_write(&row.target);
                    self.writing.remove(&row.target);
                }
            }
            self.update_all_event(Event::Done);
        }
    }
    pub(crate) fn update_all_modal(&mut self, ctx: &egui::Context) {
        let state = &self.library_state.update_all;
        if !state.open {
            return;
        }
        let lang = self.lang;
        let mut confirm = false;
        let mut close = false;
        let busy = state.preparing || state.running;
        let response = egui::Modal::new(egui::Id::unique("update_all_review"))
            .frame(widgets::modal_frame())
            .show(ctx, |ui| {
                ui.set_width(640.0_f32.min(ctx.content_rect().width() - 48.0));
                ui.heading(t(lang, "update_all.title"));
                ui.label(t(lang, "update_all.note"));
                if state.preparing {
                    widgets::busy(ui, lang, t(lang, "shell.checking"), None, None);
                }
                if state.running {
                    let done = state
                        .rows
                        .iter()
                        .filter(|r| !matches!(r.result, RowResult::Ready | RowResult::Updating))
                        .count();
                    let label = t(lang, "update_all.progress")
                        .replace("{done}", &done.to_string())
                        .replace("{total}", &state.rows.len().to_string());
                    ui.add(egui::ProgressBar::new(done as f32 / state.rows.len().max(1) as f32).text(label));
                }
                if let Some(error) = &state.error {
                    crate::error::body(ui, error, lang, &self.cmd_tx);
                }
                egui::ScrollArea::vertical()
                    .max_height((ctx.content_rect().height() - 310.0).max(160.0))
                    .show(ui, |ui| {
                        for (index, row) in state.rows.iter().enumerate() {
                            ui.push_id(index, |ui| {
                                ui.label(RichText::new(&row.name).strong());
                                ui.label(format!("{} -> {}", row.current.label(lang), row.new.label(lang)));
                                let (key, color) = match &row.result {
                                    RowResult::Ready => ("update_all.ready", theme::TEXT),
                                    RowResult::Updating => ("shell.progress_update", theme::MUTED),
                                    RowResult::Updated => ("shell.updated_done", theme::GOOD),
                                    RowResult::Skipped(_) => ("update_all.skipped", theme::WARN),
                                    RowResult::Failed(_) => ("shell.failed", theme::BAD),
                                };
                                ui.colored_label(color, t(lang, key));
                                if matches!(row.result, RowResult::Updating)
                                    && let Some((done, total)) = self.writing.get(&row.target)
                                {
                                    ui.add(
                                        egui::ProgressBar::new(*done as f32 / (*total).max(1) as f32)
                                            .text(format!("{done} {} {total}", t(lang, "progress.of"))),
                                    );
                                }
                                if let RowResult::Skipped(skip) = &row.result {
                                    ui.label(t(lang, skip.key));
                                }
                                if let RowResult::Failed(error) = &row.result {
                                    crate::error::body(ui, error, lang, &self.cmd_tx);
                                }
                                // Build hashes, paths, raw refusals and the exact reviewed changes are inspectable details.
                                widgets::collapsible(
                                    ui,
                                    "update_details",
                                    &format!("{}: {}", t(lang, "shell.details"), row.name),
                                    widgets::Fold::Details,
                                    |ui| {
                                        ui.label(crate::format::display_path(&row.target));
                                        ui.label(format!(
                                            "{} -> {}",
                                            row.current.hash.as_deref().unwrap_or(t(lang, "ver.unknown")),
                                            row.new.hash.as_deref().unwrap_or(t(lang, "ver.unknown"))
                                        ));
                                        if let RowResult::Skipped(Skip {
                                            detail: Some(error), ..
                                        }) = &row.result
                                        {
                                            ui.label(error.user_text(lang));
                                        }
                                        if let Some(plan) = &row.plan {
                                            for note in plan.notes() {
                                                ui.label(diagnostic(lang, note));
                                            }
                                            for step in plan.steps() {
                                                let (verb, text, _) = step_text(lang, step);
                                                ui.label(format!("{verb}: {text}"));
                                            }
                                        }
                                    },
                                );
                                ui.add_space(8.0);
                            });
                        }
                    });
                ui.separator();
                ui.horizontal(|ui| {
                    let cancel_button = ui.add_enabled(
                        !busy,
                        egui::Button::new(t(
                            lang,
                            if state.finished || state.error.is_some() {
                                "close"
                            } else {
                                "cancel"
                            },
                        )),
                    );
                    close = cancel_button.clicked();
                    if !state.finished {
                        let count = state
                            .rows
                            .iter()
                            .filter(|r| matches!(r.result, RowResult::Ready))
                            .count();
                        let label = t(
                            lang,
                            if count == 1 {
                                "update_all.confirm_one"
                            } else {
                                "update_all.confirm"
                            },
                        )
                        .replace("{count}", &count.to_string());
                        confirm = ui
                            .add_enabled(!busy && count > 0 && state.error.is_none(), theme::primary(&label))
                            .clicked();
                    }
                });
            });
        if confirm {
            self.confirm_update_all();
        } else if !busy && (close || response.should_close()) {
            self.library_state.update_all.open = false;
            if let Some(focus) = self.library_state.update_all.focus.take() {
                ctx.memory_mut(|m| m.request_focus(focus));
            }
        }
    }
}
